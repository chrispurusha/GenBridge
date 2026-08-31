/*
 * GenBridge - bridge any CoreAudio device into a DAW.
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

// Proof of concept for the whole idea: run two independent CoreAudio devices against each other
// and hold the buffer between them at a fixed depth indefinitely, with no xruns.
//
// This is deliberately NOT a plug-in. The hard part of GenBridge is the drift loop, and a loop
// that has to be reloaded into a DAW to be observed is a loop that will not get tuned. Here the
// output device stands in for the DAW's clock - the substitution is honest, because from the
// bridge's point of view a DAW is exactly that: something that consumes blocks on a clock which is
// not the capture device's.
//
// The telemetry is the deliverable. If 'fill' holds its setpoint and 'int' settles on the same
// number as 'raw', the loop is tracking the real crystal offset and the design works.

#include <math.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "device.h"
#include "drift.h"
#include "resampler.h"
#include "ring.h"

#define DEFAULT_CHANNELS     (2)
#define DEFAULT_FRAMES       (256)
#define DEFAULT_TARGET_MS    (40.0)
#define MIN_TARGET_MS        (5.0)

typedef struct {
    tRing            ring;
    tResampler       resampler;
    tDrift           drift;

    double           inRate;
    double           outRate;
    double           nominalRatio;      // input frames per output frame, as the devices report it
    double           workingRatio;      // what the bridge actually uses; differs only under --inject-ppm
    uint32_t         channels;

    float *          pullBuffer;        // staging for ring -> resampler
    uint32_t         pullCapacity;

    double           setpointFrames;

    _Atomic uint64_t framesIn;
    _Atomic uint64_t framesOut;

    // Set by either callback, acted on by the output one. Only the consumer may move the read
    // cursor, so the producer asks rather than does.
    _Atomic bool     needResync;
    _Atomic bool     started;
    _Atomic uint32_t resyncs;
    _Atomic uint64_t syncFramesIn;
    _Atomic uint64_t syncFramesOut;
    uint64_t         syncReadPos;       // consumer-owned, so only the output callback touches it

    _Atomic double   liveFill;
    _Atomic double   liveRawPpm;
    _Atomic double   liveCorrectionPpm;
    _Atomic double   liveMeasuredPpm;
} tBridge;

static volatile sig_atomic_t gRunning = 1;

static void on_signal(int sig) {
    (void)sig;
    gRunning = 0;
}

static double now_seconds(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (double)ts.tv_sec + ((double)ts.tv_nsec * 1e-9);
}

// ---------------------------------------------------------------------------------------------
// The two real-time callbacks. Between them they must not allocate, lock or log.
// ---------------------------------------------------------------------------------------------

static void input_callback(void * user, const float * input, float * output, uint32_t frames) {
    (void)output;

    tBridge * bridge = (tBridge *)user;

    if (!ring_write(&bridge->ring, input, frames)) {
        // Overflow: the consumer has fallen far enough behind that the ring wrapped. Ask for a
        // resync rather than doing it here - readPos belongs to the output callback.
        atomic_store(&bridge->needResync, true);
    }

    atomic_fetch_add(&bridge->framesIn, frames);
}

static void output_callback(void * user, const float * input, float * output, uint32_t frames) {
    (void)input;

    tBridge * bridge   = (tBridge *)user;
    uint32_t  channels = bridge->channels;

    if (atomic_load(&bridge->needResync)) {
        // Hold silence until there is a setpoint's worth to snap to. Resyncing to a ring that is
        // still filling would only have to be done again a moment later.
        if (ring_fill(&bridge->ring) < (uint64_t)bridge->setpointFrames) {
            memset(output, 0, (size_t)frames * channels * sizeof(float));
            return;
        }

        ring_resync(&bridge->ring, (uint32_t)bridge->setpointFrames);
        resampler_reset(&bridge->resampler);
        drift_reset(&bridge->drift);

        // Restart the raw cross-check from here, or the frames discarded by the snap sit in the
        // counters for ever and the ratio they imply is wrong by a constant.
        atomic_store(&bridge->syncFramesIn, atomic_load(&bridge->framesIn));
        atomic_store(&bridge->syncFramesOut, atomic_load(&bridge->framesOut));
        bridge->syncReadPos = atomic_load(&bridge->ring.readPos);
        atomic_store(&bridge->liveRawPpm, 0.0);

        if (atomic_load(&bridge->started)) {
            atomic_fetch_add(&bridge->resyncs, 1);
        }

        atomic_store(&bridge->started, true);
        atomic_store(&bridge->needResync, false);
    }

    double fill       = (double)ring_fill(&bridge->ring);
    double correction = drift_update(&bridge->drift, fill);
    double ratio      = bridge->workingRatio * (1.0 + correction);

    uint32_t needed = resampler_needed(&bridge->resampler, frames, ratio);

    if (needed > bridge->pullCapacity) {
        needed = bridge->pullCapacity;
    }

    if (needed > 0) {
        if (!ring_read(&bridge->ring, bridge->pullBuffer, needed)) {
            // Underrun. ring_read has already handed back silence; ask for a resync so the next
            // block starts from a known depth instead of chasing the hole.
            atomic_store(&bridge->needResync, true);
        }

        resampler_push(&bridge->resampler, bridge->pullBuffer, needed);
    }

    resampler_process(&bridge->resampler, output, frames, ratio);

    uint64_t producedTotal = atomic_load(&bridge->framesOut) + frames;

    atomic_store(&bridge->framesOut, producedTotal);

    // The raw cross-check, computed HERE rather than on the reporting thread. Both quantities are
    // owned by this callback - readPos is moved by ring_read above, framesOut a line ago - so they
    // are mutually consistent. Reading them from another thread instead lets one advance between
    // the two loads, and a single block of skew looks exactly like tens of ppm of drift.
    uint64_t consumed = atomic_load(&bridge->ring.readPos) - bridge->syncReadPos;
    uint64_t produced = producedTotal - atomic_load(&bridge->syncFramesOut);

    if (produced > 0) {
        double appliedRatio = (double)consumed / (double)produced;

        atomic_store(&bridge->liveRawPpm, ((appliedRatio / bridge->nominalRatio) - 1.0) * 1.0e6);
    }

    atomic_store(&bridge->liveFill, fill);
    atomic_store(&bridge->liveCorrectionPpm, drift_correction_ppm(&bridge->drift));
    atomic_store(&bridge->liveMeasuredPpm, drift_measured_ppm(&bridge->drift));
}

// ---------------------------------------------------------------------------------------------

static void list_devices(void) {
    tDeviceInfo list[DEVICE_MAX];
    uint32_t    count = device_enumerate(list, DEVICE_MAX);

    printf("%-40s %6s %6s %9s  %s\n", "NAME", "IN", "OUT", "RATE", "UID");

    for (uint32_t i = 0; i < count; i++) {
        printf("%-40s %6u %6u %9.0f  %s\n",
               list[i].name, list[i].inputChannels, list[i].outputChannels,
               list[i].sampleRate, list[i].uid);
    }
}

static void usage(const char * argv0) {
    printf("usage: %s --in <device> --out <device> [options]\n", argv0);
    printf("       %s --list\n\n", argv0);
    printf("  --in <substring>      capture device (matched against name or UID)\n");
    printf("  --out <substring>     playback device\n");
    printf("  --channels <n>        channels to bridge (default %d)\n", DEFAULT_CHANNELS);
    printf("  --frames <n>          device buffer frames to request (default %d)\n", DEFAULT_FRAMES);
    printf("  --target-ms <ms>      ring setpoint (default %.0f)\n", DEFAULT_TARGET_MS);
    printf("  --bandwidth <hz>      drift loop bandwidth (default %.3f)\n", drift_default_config().bandwidthHz);
    printf("  --max-ppm <ppm>       correction clamp (default %.0f)\n", drift_default_config().maxCorrectionPpm);
    printf("  --seconds <n>         run time; 0 runs until interrupted (default 0)\n");
    printf("  --no-correct          disable the drift loop, to show what it is fixing\n");
    printf("  --inject-ppm <ppm>    lie to the bridge about the rate ratio by this much, so the\n");
    printf("                        loop has a known drift to find. 'integ' must converge on the\n");
    printf("                        negative of it, and 'raw' on zero.\n");
}

int main(int argc, char ** argv) {
    const char * inName    = NULL;
    const char * outName   = NULL;
    uint32_t     channels  = DEFAULT_CHANNELS;
    uint32_t     frames    = DEFAULT_FRAMES;
    double       targetMs  = DEFAULT_TARGET_MS;
    double       runFor    = 0.0;
    double       injectPpm = 0.0;
    bool         correct   = true;
    tDriftConfig driftConfig = drift_default_config();

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--list") == 0)) {
            list_devices();
            return 0;
        } else if ((strcmp(argv[i], "--help") == 0) || (strcmp(argv[i], "-h") == 0)) {
            usage(argv[0]);
            return 0;
        } else if ((strcmp(argv[i], "--no-correct") == 0)) {
            correct = false;
        } else if ((i + 1) < argc) {
            if (strcmp(argv[i], "--in") == 0) {
                inName = argv[++i];
            } else if (strcmp(argv[i], "--out") == 0) {
                outName = argv[++i];
            } else if (strcmp(argv[i], "--channels") == 0) {
                channels = (uint32_t)atoi(argv[++i]);
            } else if (strcmp(argv[i], "--frames") == 0) {
                frames = (uint32_t)atoi(argv[++i]);
            } else if (strcmp(argv[i], "--target-ms") == 0) {
                targetMs = atof(argv[++i]);
            } else if (strcmp(argv[i], "--bandwidth") == 0) {
                driftConfig.bandwidthHz = atof(argv[++i]);
            } else if (strcmp(argv[i], "--max-ppm") == 0) {
                driftConfig.maxCorrectionPpm = atof(argv[++i]);
            } else if (strcmp(argv[i], "--seconds") == 0) {
                runFor = atof(argv[++i]);
            } else if (strcmp(argv[i], "--inject-ppm") == 0) {
                injectPpm = atof(argv[++i]);
            } else {
                fprintf(stderr, "unknown option: %s\n", argv[i]);
                return 1;
            }
        } else {
            fprintf(stderr, "unknown or incomplete option: %s\n", argv[i]);
            return 1;
        }
    }

    if ((inName == NULL) || (outName == NULL)) {
        usage(argv[0]);
        return 1;
    }

    if (targetMs < MIN_TARGET_MS) {
        targetMs = MIN_TARGET_MS;
    }

    tDeviceInfo inInfo;
    tDeviceInfo outInfo;

    if (!device_find(inName, true, &inInfo)) {
        fprintf(stderr, "no input device matching '%s' (try --list)\n", inName);
        return 1;
    }

    if (!device_find(outName, false, &outInfo)) {
        fprintf(stderr, "no output device matching '%s' (try --list)\n", outName);
        return 1;
    }

    if (inInfo.id == outInfo.id) {
        fprintf(stderr, "input and output resolve to the same device (%s).\n", inInfo.name);
        fprintf(stderr, "one device is one clock, so there would be no drift to correct.\n");
        return 1;
    }

    device_set_buffer_frames(inInfo.id, frames);
    device_set_buffer_frames(outInfo.id, frames);

    uint32_t inFrames  = device_buffer_frames(inInfo.id);
    uint32_t outFrames = device_buffer_frames(outInfo.id);

    double inRate  = device_sample_rate(inInfo.id);
    double outRate = device_sample_rate(outInfo.id);

    if ((inRate <= 0.0) || (outRate <= 0.0)) {
        fprintf(stderr, "could not read a sample rate from both devices\n");
        return 1;
    }

    tBridge * bridge = (tBridge *)calloc(1, sizeof(tBridge));

    if (bridge == NULL) {
        return 1;
    }

    bridge->inRate       = inRate;
    bridge->outRate      = outRate;
    bridge->nominalRatio = inRate / outRate;
    bridge->channels     = channels;

    // Synthetic drift. Real hardware may or may not have any - a USB device running synchronous
    // to the host has none at all - so a loop that only ever sees well behaved devices has not
    // been tested. Deliberately mis-stating the ratio gives a drift whose exact size is known in
    // advance, which turns "it held steady" into a check with a right answer.
    bridge->workingRatio = bridge->nominalRatio * (1.0 + (injectPpm * 1.0e-6));

    double setpoint = (targetMs / 1000.0) * inRate;

    // The setpoint has to clear one output block's worth of input plus the filter's reach, or the
    // very first read underruns and the loop spends its life recovering from a hole of its own
    // making.
    double minimumSetpoint = ((double)outFrames * bridge->workingRatio) + (2.0 * RESAMPLER_TAPS);

    if (setpoint < minimumSetpoint) {
        setpoint = minimumSetpoint;
        printf("note: setpoint raised to %.0f frames to clear one output block\n", setpoint);
    }

    uint32_t ringFrames = (uint32_t)(setpoint * 8.0) + (4 * (inFrames + outFrames));

    if (!ring_init(&bridge->ring, ringFrames, channels)) {
        fprintf(stderr, "ring allocation failed\n");
        return 1;
    }

    if (!resampler_init(&bridge->resampler, channels, bridge->workingRatio, outFrames)) {
        fprintf(stderr, "resampler allocation failed\n");
        return 1;
    }

    bridge->pullCapacity = (uint32_t)((double)outFrames * bridge->workingRatio * 1.5) + (4 * RESAMPLER_TAPS);
    bridge->pullBuffer   = (float *)calloc((size_t)bridge->pullCapacity * channels, sizeof(float));

    if (bridge->pullBuffer == NULL) {
        return 1;
    }

    if (!correct) {
        // Everything else identical, loop disabled: the control case that shows the drift is real.
        driftConfig.bandwidthHz      = 0.0;
        driftConfig.maxCorrectionPpm = 0.0;
    }

    drift_init(&bridge->drift, &driftConfig, inRate, setpoint, (double)outFrames / outRate);

    bridge->setpointFrames = setpoint;

    atomic_store(&bridge->needResync, true);
    atomic_store(&bridge->started, false);

    tDeviceStream inStream;
    tDeviceStream outStream;

    if (!device_open(&inStream, inInfo.id, true, channels, inFrames * 4, input_callback, bridge)) {
        return 1;
    }

    if (!device_open(&outStream, outInfo.id, false, channels, outFrames * 4, output_callback, bridge)) {
        return 1;
    }

    printf("\n");
    printf("  in   %s\n", inInfo.name);
    printf("       %.0f Hz, %u frames, latency %u frames\n",
           inRate, inFrames, device_latency_frames(inInfo.id, true));
    printf("  out  %s\n", outInfo.name);
    printf("       %.0f Hz, %u frames, latency %u frames\n",
           outRate, outFrames, device_latency_frames(outInfo.id, false));
    printf("\n");
    printf("  nominal ratio %.9f    setpoint %.0f frames (%.1f ms)    loop %s\n",
           bridge->nominalRatio, setpoint, (setpoint / inRate) * 1000.0,
           correct ? "on" : "OFF (control case)");

    if (injectPpm != 0.0) {
        printf("  injected drift %+.1f ppm - expect integ %+.1f ppm and raw 0.0 ppm\n",
               injectPpm, -injectPpm);
    }
    printf("\n");
    printf("  %7s %10s %9s %10s %10s %10s %6s %6s %6s\n",
           "time", "fill", "err", "corr", "integ", "raw", "under", "over", "resync");

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (!device_start(&inStream) || !device_start(&outStream)) {
        device_close(&inStream);
        device_close(&outStream);
        return 1;
    }

    double start = now_seconds();

    while (gRunning) {
        usleep(1000000);

        double elapsed = now_seconds() - start;

        double fill   = atomic_load(&bridge->liveFill);
        double rawPpm = atomic_load(&bridge->liveRawPpm);

        printf("  %6.1fs %10.1f %9.1f %+9.2fp %+9.2fp %+9.2fp %6u %6u %6u\n",
               elapsed,
               fill,
               fill - setpoint,
               atomic_load(&bridge->liveCorrectionPpm),
               atomic_load(&bridge->liveMeasuredPpm),
               rawPpm,
               atomic_load(&bridge->ring.underflows),
               atomic_load(&bridge->ring.overflows),
               atomic_load(&bridge->resyncs));

        fflush(stdout);

        if ((runFor > 0.0) && (elapsed >= runFor)) {
            break;
        }
    }

    device_stop(&inStream);
    device_stop(&outStream);
    device_close(&inStream);
    device_close(&outStream);

    printf("\n  underruns %u, overflows %u, resyncs %u, resampler overruns %u\n",
           atomic_load(&bridge->ring.underflows),
           atomic_load(&bridge->ring.overflows),
           atomic_load(&bridge->resyncs),
           bridge->resampler.overruns);

    ring_free(&bridge->ring);
    resampler_free(&bridge->resampler);
    free(bridge->pullBuffer);
    free(bridge);

    return 0;
}
