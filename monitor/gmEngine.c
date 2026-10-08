/*
 * GenBridge Monitor - route an input pair to an output pair with as little delay as two clocks allow.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
// Notes: Docs/code-notes/gmEngine.c.md - "// notes §k" refers there.

// notes §2

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device.h"
#include "drift.h"
#include "gmEngine.h"
#include "resampler.h"
#include "ring.h"

#define GM_MARGIN          (1.25)   // notes §3
#define GM_MAX_CALLBACK    (4096)   // frames a device may hand over in one call

typedef struct {
    tRing            ring;
    tResampler       resampler;
    tDrift           drift;
    tDeviceStream    in;
    tDeviceStream    out;
    bool             inOpen;
    bool             outOpen;
    AudioObjectID    inId;
    AudioObjectID    outId;
    uint32_t         inChannels;
    uint32_t         outChannels;
    double           inRate;
    double           outRate;
    double           ratio;              // input frames per output frame, as the devices report it
    double           setpoint;
    float *          pull;               // ring -> resampler staging
    uint32_t         pullCapacity;
    float *          mix;                // resampler -> output mapping staging
    uint32_t         inFrames;
    uint32_t         outFrames;
    uint32_t         inRestoreFrames;    // notes §4 - nonzero when we changed it and must put it back
    uint32_t         outRestoreFrames;
    bool             inShared;
    bool             outShared;
    double           latencyMs;
    double           bridgeMs;
    _Atomic double   trim;
    _Atomic bool     needResync;
    _Atomic bool     started;
    _Atomic uint32_t resyncs;
    _Atomic float    inPeak[2];
    _Atomic float    outPeak[2];
    _Atomic double   fill;
    _Atomic double   driftPpm;
} tGmEngine;

static tGmEngine * gEngine     = NULL;
static tGmConfig   gConfig;
static tGmState    gState      = eGmStopped;
static char        gMessage[160];
static _Atomic bool gListChanged = false;
static bool        gWatching   = false;

// ---------------------------------------------------------------------------------------------
// Real time. No allocation, locks or logging.
// ---------------------------------------------------------------------------------------------

// notes §5
static void hold_peak(_Atomic float * peak, float value) {
    float was = atomic_load(peak);

    if (value > was) {
        atomic_store(peak, value);
    }
}

static void input_callback(void * user, const float * input, float * output, uint32_t frames) {
    (void)output;
    tGmEngine * e    = (tGmEngine *)user;
    float       p[2] = { 0.0f, 0.0f };

    for (uint32_t i = 0; i < frames; i++) {
        for (uint32_t c = 0; c < e->inChannels; c++) {
            float v = fabsf(input[(i * e->inChannels) + c]);

            if (v > p[c]) {
                p[c] = v;
            }
        }
    }
    hold_peak(&e->inPeak[0], p[0]);
    hold_peak(&e->inPeak[1], (e->inChannels > 1) ? p[1] : p[0]);

    if (!ring_write(&e->ring, input, frames)) {
        atomic_store(&e->needResync, true);   // only the output side may move the read cursor
    }
}

static void output_callback(void * user, const float * input, float * output, uint32_t frames) {
    (void)input;
    tGmEngine * e = (tGmEngine *)user;

    if (frames > GM_MAX_CALLBACK) {
        memset(output, 0, (size_t)frames * e->outChannels * sizeof(float));
        return;
    }

    if (atomic_load(&e->needResync)) {
        if (ring_fill(&e->ring) < (uint64_t)e->setpoint) {
            memset(output, 0, (size_t)frames * e->outChannels * sizeof(float));
            return;
        }
        ring_resync(&e->ring, (uint32_t)e->setpoint);
        resampler_reset(&e->resampler);
        drift_reset(&e->drift);

        if (atomic_load(&e->started)) {
            atomic_fetch_add(&e->resyncs, 1);
        }
        atomic_store(&e->started, true);
        atomic_store(&e->needResync, false);
    }

    double   fill       = (double)ring_fill(&e->ring);
    double   correction = drift_update(&e->drift, fill, (double)frames / e->outRate);
    double   ratio      = e->ratio * (1.0 + correction);
    uint32_t needed     = resampler_needed(&e->resampler, frames, ratio);

    if (needed > e->pullCapacity) {
        needed = e->pullCapacity;
    }

    if (needed > 0) {
        if (!ring_read(&e->ring, e->pull, needed)) {
            atomic_store(&e->needResync, true);
        }
        resampler_push(&e->resampler, e->pull, needed);
    }
    resampler_process(&e->resampler, e->mix, frames, ratio);

    // notes §6
    float gain = (float)atomic_load(&e->trim);
    float p[2] = { 0.0f, 0.0f };

    for (uint32_t i = 0; i < frames; i++) {
        float l = e->mix[i * e->inChannels] * gain;
        float r = (e->inChannels > 1) ? (e->mix[(i * e->inChannels) + 1] * gain) : l;

        if (e->outChannels == 1) {
            float m = (e->inChannels > 1) ? (0.5f * (l + r)) : l;

            output[i] = m;
            p[0]      = fmaxf(p[0], fabsf(m));
            p[1]      = p[0];
        } else {
            output[i * 2]       = l;
            output[(i * 2) + 1] = r;
            p[0]                = fmaxf(p[0], fabsf(l));
            p[1]                = fmaxf(p[1], fabsf(r));
        }
    }
    hold_peak(&e->outPeak[0], p[0]);
    hold_peak(&e->outPeak[1], p[1]);

    atomic_store(&e->fill, drift_filtered_fill(&e->drift));
    atomic_store(&e->driftPpm, drift_measured_ppm(&e->drift));
}

// ---------------------------------------------------------------------------------------------
// Main thread
// ---------------------------------------------------------------------------------------------

static void list_changed(void * user) {
    (void)user;
    atomic_store(&gListChanged, true);
}

static bool find_uid(const char * uid, bool input, tDeviceInfo * found) {
    tDeviceInfo list[DEVICE_MAX];
    uint32_t    count = device_enumerate(list, DEVICE_MAX);

    if ((uid == NULL) || (uid[0] == '\0')) {
        return false;
    }

    for (uint32_t i = 0; i < count; i++) {
        if ((strcmp(list[i].uid, uid) == 0) && ((input ? list[i].inputChannels : list[i].outputChannels) > 0)) {
            *found = list[i];
            return true;
        }
    }

    return false;
}

// notes §4
static uint32_t claim_buffer(AudioObjectID id, uint32_t wanted, bool * shared, uint32_t * restore) {
    uint32_t current = device_buffer_frames(id);
    uint32_t lo      = 0;
    uint32_t hi      = 0;

    *restore = 0;
    *shared  = device_is_running_somewhere(id);

    if (*shared || (wanted == 0) || (wanted == current)) {
        return current;
    }

    if (device_buffer_frame_range(id, &lo, &hi)) {
        wanted = (wanted < lo) ? lo : ((wanted > hi) ? hi : wanted);
    }

    if (device_set_buffer_frames(id, wanted)) {
        *restore = current;
    }

    return device_buffer_frames(id);
}

static void release(void) {
    tGmEngine * e = gEngine;

    if (e == NULL) {
        return;
    }

    if (e->inOpen) {
        device_stop(&e->in);
        device_close(&e->in);
    }

    if (e->outOpen) {
        device_stop(&e->out);
        device_close(&e->out);
    }

    // notes §4 - put back only what nobody else is using now: a client that arrived after us is running at
    // our size, and changing it under them is the very thing the claim refused to do
    if ((e->inRestoreFrames > 0) && !device_is_running_somewhere(e->inId)) {
        device_set_buffer_frames(e->inId, e->inRestoreFrames);
    }

    if ((e->outRestoreFrames > 0) && (e->outId != e->inId) && !device_is_running_somewhere(e->outId)) {
        device_set_buffer_frames(e->outId, e->outRestoreFrames);
    }

    ring_free(&e->ring);
    resampler_free(&e->resampler);
    free(e->pull);
    free(e->mix);
    free(e);
    gEngine = NULL;
}

static void fail(const char * text) {
    release();
    gState = eGmFailed;
    snprintf(gMessage, sizeof(gMessage), "%s", text);
}

static void open_engine(void) {
    tDeviceInfo inInfo;
    tDeviceInfo outInfo;

    release();

    bool haveIn  = find_uid(gConfig.inUid, true, &inInfo);
    bool haveOut = find_uid(gConfig.outUid, false, &outInfo);

    if ((gConfig.inUid[0] == '\0') || (gConfig.outUid[0] == '\0')) {
        gState = eGmStopped;
        snprintf(gMessage, sizeof(gMessage), "%s", "choose an input and an output");
        return;
    }

    if (!haveIn || !haveOut) {
        gState = eGmWaiting;
        snprintf(gMessage, sizeof(gMessage), "waiting for the %s device", !haveIn ? "input" : "output");
        return;
    }

    uint32_t inCh  = (gConfig.inChannels > 1) ? 2u : 1u;
    uint32_t outCh = (gConfig.outChannels > 1) ? 2u : 1u;

    if ((gConfig.inFirst + inCh) > inInfo.inputChannels) {
        fail("the input channels are beyond what the device has");
        return;
    }

    if ((gConfig.outFirst + outCh) > outInfo.outputChannels) {
        fail("the output channels are beyond what the device has");
        return;
    }

    tGmEngine * e = (tGmEngine *)calloc(1, sizeof(tGmEngine));

    if (e == NULL) {
        fail("out of memory");
        return;
    }
    gEngine        = e;
    e->inId        = inInfo.id;
    e->outId       = outInfo.id;
    e->inChannels  = inCh;
    e->outChannels = outCh;

    // notes §4 - one device in both roles is one buffer setting, so it is claimed once
    e->inFrames  = claim_buffer(e->inId, gConfig.frames, &e->inShared, &e->inRestoreFrames);
    e->outFrames = (e->outId == e->inId) ? e->inFrames
                   : claim_buffer(e->outId, gConfig.frames, &e->outShared, &e->outRestoreFrames);

    if (e->outId == e->inId) {
        e->outShared = e->inShared;
    }
    e->inRate  = device_sample_rate(e->inId);
    e->outRate = device_sample_rate(e->outId);

    if ((e->inRate <= 0.0) || (e->outRate <= 0.0) || (e->inFrames == 0) || (e->outFrames == 0)) {
        fail("could not read the devices' rate or buffer");
        return;
    }
    e->ratio = e->inRate / e->outRate;

    // notes §3
    double floor = ((double)e->outFrames * e->ratio) + (double)e->inFrames + (2.0 * RESAMPLER_TAPS);

    e->setpoint = (floor * GM_MARGIN) + ((gConfig.safetyMs / 1000.0) * e->inRate);

    uint32_t ringFrames = (uint32_t)(e->setpoint * 8.0) + (4u * GM_MAX_CALLBACK);

    e->pullCapacity = (uint32_t)((double)GM_MAX_CALLBACK * e->ratio * 1.5) + (4u * RESAMPLER_TAPS);
    e->pull         = (float *)calloc((size_t)e->pullCapacity * inCh, sizeof(float));
    e->mix          = (float *)calloc((size_t)GM_MAX_CALLBACK * inCh, sizeof(float));

    if ((e->pull == NULL) || (e->mix == NULL) || !ring_init(&e->ring, ringFrames, inCh)
        || !resampler_init(&e->resampler, inCh, e->ratio, GM_MAX_CALLBACK)) {
        fail("out of memory");
        return;
    }
    tDriftConfig drift = drift_default_config();

    drift_init(&e->drift, &drift, e->inRate, e->setpoint);
    atomic_store(&e->trim, gConfig.trim);
    atomic_store(&e->needResync, true);

    // notes §7
    e->bridgeMs  = 1000.0 * ((e->setpoint + resampler_latency_frames()) / e->inRate);
    e->latencyMs = e->bridgeMs
                   + (1000.0 * ((((double)device_latency_frames(e->inId, true) + (double)e->inFrames) / e->inRate)
                                + (((double)e->outFrames + (double)device_latency_frames(e->outId, false)) / e->outRate)));

    if (!device_open(&e->in, e->inId, true, gConfig.inFirst, inCh, GM_MAX_CALLBACK, input_callback, e)) {
        fail("could not open the input");
        return;
    }
    e->inOpen = true;

    if (!device_open(&e->out, e->outId, false, gConfig.outFirst, outCh, GM_MAX_CALLBACK, output_callback, e)) {
        fail("could not open the output");
        return;
    }
    e->outOpen = true;

    if (!device_start(&e->in) || !device_start(&e->out)) {
        fail("a device would not start");
        return;
    }
    gState = eGmRunning;
    snprintf(gMessage, sizeof(gMessage), "%s -> %s", inInfo.name, outInfo.name);
}

void gm_engine_start(const tGmConfig * config) {
    gConfig = *config;

    if (!gWatching) {
        gWatching = device_watch_list(list_changed, NULL);
    }
    open_engine();
}

void gm_engine_stop(void) {
    release();
    gState = eGmStopped;
    snprintf(gMessage, sizeof(gMessage), "%s", "stopped");
}

void gm_engine_set_trim(double trim) {
    gConfig.trim = trim;

    if (gEngine != NULL) {
        atomic_store(&gEngine->trim, trim);
    }
}

void gm_engine_poll(void) {
    if (!atomic_exchange(&gListChanged, false)) {
        return;
    }
    tDeviceInfo info;

    if (gState == eGmWaiting) {
        open_engine();
    } else if ((gState == eGmRunning)
               && (!find_uid(gConfig.inUid, true, &info) || !find_uid(gConfig.outUid, false, &info))) {
        release();
        gState = eGmWaiting;
        snprintf(gMessage, sizeof(gMessage), "%s", "a device went away - waiting for it");
    }
}

void gm_engine_status(tGmStatus * out) {
    memset(out, 0, sizeof(*out));
    out->state = gState;
    snprintf(out->message, sizeof(out->message), "%s", gMessage);

    tGmEngine * e = gEngine;

    if (e == NULL) {
        return;
    }

    // notes §5 - read and decay: each status read is one meter frame
    for (int c = 0; c < 2; c++) {
        out->inPeak[c]  = atomic_exchange(&e->inPeak[c], 0.0f);
        out->outPeak[c] = atomic_exchange(&e->outPeak[c], 0.0f);
    }
    out->latencyMs      = e->latencyMs;
    out->bridgeMs       = e->bridgeMs;
    out->fillFrames     = atomic_load(&e->fill);
    out->setpointFrames = e->setpoint;
    out->driftPpm       = atomic_load(&e->driftPpm);
    out->underruns      = atomic_load(&e->ring.underflows);
    out->resyncs        = atomic_load(&e->resyncs);
    out->inFrames       = e->inFrames;
    out->outFrames      = e->outFrames;
    out->inShared       = e->inShared;
    out->outShared      = e->outShared;
    out->inRate         = e->inRate;
    out->outRate        = e->outRate;
}
