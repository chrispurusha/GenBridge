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
// Notes: Docs/code-notes/gbBridge.c.md - "// notes §k" refers there.


// notes §1

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <CoreAudio/HostTime.h>

#include "gbBridgePrivate.h"
#include "gbDraw.h"
#include "synthlibLog.h"
#include "gbStatus.h"

// FILE-LOCAL, AND FORWARD-DECLARED. A class let its members call each other in
// any order; a C file does not, and these are the ones nothing outside this file
// needs to see.
static uint64_t gb_block_host_time(tGbBridge * self, int32_t frames);
static void gb_capture_callback(void * user, const float * input, float * output, uint32_t frames);
static uint8_t gb_channel_for(tGbBridge * self, int16_t incoming);
static void gb_close_capture(tGbBridge * self);
static void gb_close_capture_locked(tGbBridge * self);
static void gb_device_list_changed(void * user);
static uint64_t gb_host_time_for(tGbBridge * self, uint64_t blockHostTime, int32_t sampleOffset);
static double gb_latency_frames_for_fill(tGbBridge * self, double fillFrames);
static double gb_minimum_setpoint(tGbBridge * self, uint32_t deviceFrames);
static double gb_minimum_setpoint_for(tGbBridge * self, uint32_t hostFrames, uint32_t deviceFrames);
static double gb_minimum_setpoint_for_host(tGbBridge * self, uint32_t hostFrames);
static double gb_now_ms(void);
static void gb_observe_block(tGbBridge * self, int32_t frames);
static void gb_observe_burst(tGbBridge * self, int32_t frames);
static bool gb_open_capture_locked(tGbBridge * self, const tDeviceInfo * info);
static void gb_publish_config_snapshot(tGbBridge * self);
static void gb_publish_latency_breakdown(tGbBridge * self);
static void gb_publish_status(tGbBridge * self, float ** out, int32_t frames, double fill);
static void gb_publish_waiting(tGbBridge * self, bool waiting, const char * name);
static bool gb_resolve_slot(const tDeviceInfo * list, uint32_t count, int slot, tDeviceInfo * out);
static void gb_retune(tGbBridge * self);
static void gb_revert_retune(tGbBridge * self);
static void gb_send_controller(tGbBridge * self, uint32_t id, double value, uint64_t hostTime);
static void gb_send_slot(tGbBridge * self);
static void gb_silence(float ** out, int32_t frames);
static uint32_t gb_snapshot_latency(tGbBridge * self);
static double gb_snapshot_latency_frames(tGbBridge * self);
static void gb_start_worker(tGbBridge * self);
static void gb_stop_worker(tGbBridge * self);
static void gb_worker_loop(tGbBridge * self);

// The worker's own entry point. Declared here because start_worker() hands its address to
// pthread_create() before the function itself appears; a class had no such ordering to mind.
static void * gb_worker_entry(void * arg);

// From a CoreAudio thread: drop the cached list and wake the worker, nothing more.
static void gb_device_list_changed(void * user) {
    tGbBridge * self = (tGbBridge *)user;

    gb_device_list_invalidate();
    gb_request_device(self);
}

// notes §2
void gb_lock_config_from_host(tGbBridge * self, const char * who) {
    double began = gb_now_ms();

    pthread_mutex_lock(&self->configLock);

    double waited = gb_now_ms() - began;

    if (waited >= 20.0) {
        synthlib_log_line("HOST THREAD BLOCKED: %s waited %.0f ms for configLock - a device open was in "
                 "flight. This is what a spinning cursor looks like from in here", who, waited);
    }
}

// notes §3
static void gb_publish_config_snapshot(tGbBridge * self) {
    atomic_store(&self->snapHostRate, self->hostRate);
    atomic_store(&self->snapRatio, self->nominalRatio);
    atomic_store(&self->snapSetpoint, self->setpointFrames);
    atomic_store(&self->snapDeviceLatency, self->deviceLatency);
}

// The snapshot's version of gb_internal_latency_frames(self). Safe from any thread; never touches a
// field the worker can be rewriting.
static double gb_snapshot_latency_frames(tGbBridge * self) {
    // notes §4
    double ratio = atomic_load(&self->snapRatio);

    if (ratio <= 0.0) {
        return 0.0;    // mid-swap, or nothing open: no latency to report rather than inf
    }

    // Before a block has run there is nothing measured yet, so the setpoint is the estimate.
    return (atomic_load(&self->snapSetpoint) + resampler_latency_frames() + (double)atomic_load(&self->snapDeviceLatency))
           / ratio;
}

static uint32_t gb_snapshot_latency(tGbBridge * self) {
    double total = gb_snapshot_latency_frames(self);

    if (self->instrument) {
        total += (atomic_load(&self->offsetMs) / 1000.0) * atomic_load(&self->snapHostRate);
    }

    return (total > 0.0) ? (uint32_t)total : 0;
}

// The pipeline's delay for a given ring occupancy. A sample read out of a ring holding `fill`
// frames entered it `fill` frames ago, so the occupancy IS the delay - the rest is what the
// resampler and the device's own converters add.
static double gb_latency_frames_for_fill(tGbBridge * self, double fillFrames) {
    double inputFrames = fillFrames + resampler_latency_frames() + (double)self->deviceLatency;

    if (self->nominalRatio <= 0.0) {
        return 0.0;
    }

    // Reported in the HOST's frames, and the ring is measured in the device's.
    return inputFrames / self->nominalRatio;
}

double gb_internal_latency_frames(tGbBridge * self) {
    return gb_latency_frames_for_fill(self, self->setpointFrames);
}

// notes §5
double gb_latency_frames_measured(tGbBridge * self, double fillFrames, uint64_t at) {
    if (self->nominalRatio <= 0.0) {
        return 0.0;
    }

    double buffered = (double)self->deviceLatency - (double)self->openDeviceFrames;

    if (buffered < 0.0) {
        buffered = 0.0;
    }

    double inRing = fillFrames + buffered + resampler_latency_frames();

    // notes §6
    double sinceWrite = gb_frames_between(self, atomic_load(&self->lastWriteHostTime), at);
    double sanest     = ((double)self->openDeviceFrames * 4.0) / self->nominalRatio;

    if ((atomic_load(&self->lastWriteHostTime) == 0) || (sinceWrite > sanest)) {
        sinceWrite = 0.0;
    }

    return sinceWrite + (inRing / self->nominalRatio);
}

uint32_t gb_internal_latency(tGbBridge * self) {
    double total = gb_internal_latency_frames(self);

    return (total > 0.0) ? (uint32_t)total : 0;
}

uint32_t gb_report_latency(tGbBridge * self) {
    double total = gb_internal_latency_frames(self);

    // notes §7
    if (self->instrument) {
        total += (atomic_load(&self->offsetMs) / 1000.0) * self->hostRate;
    }

    return (total > 0.0) ? (uint32_t)total : 0;
}

// notes §8
static uint8_t gb_channel_for(tGbBridge * self, int16_t incoming) {
    int forced = atomic_load(&self->midiChannel);

    return (forced <= 0) ? (uint8_t)(incoming & 0x0F) : (uint8_t)((forced - 1) & 0x0F);
}

// notes §9
static uint64_t gb_block_host_time(tGbBridge * self, int32_t frames) {
    uint64_t now   = AudioGetCurrentHostTime();

    // notes §10
    self->blockActualHostTime = now;
    uint64_t start = now;
    double   rate  = atomic_load(&self->eventRate);

    // notes §11
    double gap = gb_frames_between(self, self->lastCallHostTime, now);

    self->blockStartsCycle = (self->lastCallHostTime == 0) || (gap > ((double)self->lastCallFrames * 0.5));
    self->lastCallHostTime = now;
    self->lastCallFrames   = (uint32_t)frames;

    // notes §12
    if (self->nextBlockHostTime > now) {
        start = self->nextBlockHostTime;
    }

    // notes §13
    double cycle = (double)self->hostMaxFrames * 8.0;

    if ((double)self->observedMaxFrames * 2.0 > cycle) {
        cycle = (double)self->observedMaxFrames * 2.0;
    }

    if (cycle < 4096.0) {
        cycle = 4096.0;
    }

    if (rate > 0.0) {
        uint64_t ceiling = now + AudioConvertNanosToHostTime(
                               (uint64_t)((cycle / rate) * 1.0e9));

        if (start > ceiling) {
            start = now;
        }
    }

    // notes §14
    double advance = (double)frames;

    if (rate > 0.0) {
        double ahead = gb_frames_between(self, now, start);
        double allow = (self->observedMaxFrames > 0) ? (double)self->observedMaxFrames : (double)self->hostMaxFrames;

        if (ahead > allow) {
            double shave = (ahead - allow);
            double most  = advance * 0.02;

            advance -= (shave > most) ? most : shave;
        }
    }

    self->nextBlockHostTime = (rate > 0.0)
                        ? start + AudioConvertNanosToHostTime(
                              (uint64_t)((advance / rate) * 1.0e9))
                        : 0;

    self->blockHostTimeNow = start;

    return start;
}

// HOW FAR INTO A CALLBACK AN AVERAGE BLOCK SITS. A host that hands over one block per callback
// has none of this; one that delivers 512 in four 128s has a block starting 0, 128, 256 or 384
// frames in, averaging 192. Audio thread only, and both figures it reads are audio-thread state.
double gb_mean_callback_lead(tGbBridge * self) {
    double burst = (double)self->observedMaxFrames;
    double call  = (double)self->lastCallFrames;

    return (burst > call) ? ((burst - call) / 2.0) : 0.0;
}

// How many of the host's frames separate two host-clock instants. 0 if they are the wrong way
// round, which is what a caller wants everywhere it is used here.
double gb_frames_between(tGbBridge * self, uint64_t from, uint64_t to) {
    double rate = atomic_load(&self->eventRate);

    if ((to <= from) || (rate <= 0.0)) {
        return 0.0;
    }

    return ((double)AudioConvertHostTimeToNanos(to - from) / 1.0e9) * rate;
}

// WHERE AN EVENT BELONGS IN WALL TIME, from where it belongs in the block. Offset 0 is the top
// of this block, which is now; anything later is that many frames into the future and CoreMIDI
// will hold it until then.
static uint64_t gb_host_time_for(tGbBridge * self, uint64_t blockHostTime, int32_t sampleOffset) {
    double rate = atomic_load(&self->eventRate);

    if ((sampleOffset <= 0) || (rate <= 0.0)) {
        return blockHostTime;
    }

    double nanos = ((double)sampleOffset / rate) * 1.0e9;

    return blockHostTime + AudioConvertNanosToHostTime((uint64_t)nanos);
}

// notes §15
static void gb_send_controller(tGbBridge * self, uint32_t id, double value, uint64_t hostTime) {
    uint32_t index      = (uint32_t)(id - (uint32_t)GB_CC_BASE);
    uint8_t  channel    = gb_channel_for(self, (int16_t)((index / GB_CC_PER_CHANNEL) & 0x0F));
    uint32_t controller = index % GB_CC_PER_CHANNEL;
    int      destination = atomic_load(&self->midiDestination);
    uint8_t  message[3];

    if (value < 0.0) {
        value = 0.0;
    } else if (value > 1.0) {
        value = 1.0;
    }

    if (controller == (uint32_t)GB_CC_AFTERTOUCH) {
        // Channel pressure is two bytes, not three.
        message[0] = (uint8_t)(0xD0 | channel);
        message[1] = (uint8_t)((value * 127.0) + 0.5);

        gb_midi_send_at(destination, message, 2, hostTime);
        return;
    }

    if (controller == (uint32_t)GB_CC_PITCHBEND) {
        // FOURTEEN BITS, centred at 8192 - the one controller that is not a 0..127 byte, and
        // the one a keyboard player notices immediately if it is wrong.
        int bend = (int)((value * 16383.0) + 0.5);

        message[0] = (uint8_t)(0xE0 | channel);
        message[1] = (uint8_t)(bend & 0x7F);
        message[2] = (uint8_t)((bend >> 7) & 0x7F);

        gb_midi_send_at(destination, message, 3, hostTime);
        return;
    }

    if (controller > 127) {
        return;
    }

    message[0] = (uint8_t)(0xB0 | channel);
    message[1] = (uint8_t)controller;
    message[2] = (uint8_t)((value * 127.0) + 0.5);

    gb_midi_send_at(destination, message, 3, hostTime);
}

// notes §16
static void gb_observe_burst(tGbBridge * self, int32_t frames) {
    // notes §17
    self->burstBefore = self->blockStartsCycle ? 0 : self->burstFrames;

    if ((self->observedMaxFrames > 0) && (self->burstBefore > self->observedMaxFrames)) {
        self->burstBefore = self->observedMaxFrames;
    }

    if (self->blockStartsCycle) {
        // notes §18
        uint32_t ceiling = (self->hostMaxFrames > 0) ? (self->hostMaxFrames * 4) : 8192;

        if ((self->burstFrames > self->observedMaxFrames) && (self->burstFrames <= ceiling)) {
            self->observedMaxFrames = self->burstFrames;
        }

        self->burstFrames = (uint32_t)frames;
    } else {
        self->burstFrames += (uint32_t)frames;
    }
}

static void gb_observe_block(tGbBridge * self, int32_t frames) {
    // notes §19
    if (self->running && (self->observedMaxFrames > 0) && (self->retuneState != eRetuneRequested)) {
        double needed = gb_minimum_setpoint_for_host(self, self->observedMaxFrames) * GB_AUTO_MARGIN;

        if (needed > (self->setpointFrames + GB_RETUNE_MIN_GAIN)) {
            self->retuneState = eRetuneRequested;
            gb_wake_worker(self);
            return;
        }
    }

    if (self->retuneState != eRetuneWatching) {
        return;
    }

    self->observedFrames += (uint64_t)frames;

    if ((double)self->observedFrames < (GB_SETTLE_SECONDS * self->hostRate)) {
        return;
    }

    double candidate = gb_minimum_setpoint_for_host(self, self->observedMaxFrames) * GB_AUTO_MARGIN;

    // Nothing to gain if the host is using what it declared.
    if (self->observedMaxFrames >= self->hostMaxFrames) {
        self->retuneState = eRetuneSettled;
        return;
    }

    if ((self->setpointFrames - candidate) >= GB_RETUNE_MIN_GAIN) {
        self->retuneState = eRetuneRequested;
        gb_wake_worker(self);
    } else {
        self->retuneState = eRetuneSettled;
    }
}

// Peak-hold with a slow decay, which is what makes a meter readable: a true instantaneous peak
// on a 30 Hz repaint shows almost nothing of a transient.
static void gb_publish_status(tGbBridge * self, float ** out, int32_t frames, double fill) {
    tGbStatus * status = gb_status(self->statusSlot);

    if (status == NULL) {
        return;
    }
    float       peak[GB_CHANNELS] = { 0.0f, 0.0f };

    for (int c = 0; c < GB_CHANNELS; c++) {
        for (int32_t i = 0; i < frames; i++) {
            float magnitude = (out[c][i] < 0.0f) ? -out[c][i] : out[c][i];

            if (magnitude > peak[c]) {
                peak[c] = magnitude;
            }
        }
    }

    float heldLeft  = atomic_load(&status->peakLeft) * 0.85f;
    float heldRight = atomic_load(&status->peakRight) * 0.85f;

    atomic_store(&status->peakLeft, (peak[0] > heldLeft) ? peak[0] : heldLeft);
    atomic_store(&status->peakRight, (peak[1] > heldRight) ? peak[1] : heldRight);

    atomic_store(&status->fillFrames, fill);
    atomic_store(&status->driftPpm, drift_measured_ppm(&self->drift));
    atomic_store(&status->underruns, (int)atomic_load(&self->ring.underflows));
    atomic_store(&status->resyncs, (int)atomic_load(&self->resyncs));
}

// notes §20
void gb_send_latency_changed(tGbBridge * self) {
    gb_send_message(self, "gbLatency", 0);
}

static void gb_send_slot(tGbBridge * self) {
    gb_send_message(self, "gbStatusSlot", self->statusSlot);
}

void gb_send_message(tGbBridge * self, const char * id, int value) {
    // OUT THROUGH THE WRAPPER, because making an IMessage needs the host application object and
    // delivering one needs the IConnectionPoint - two things a plug-in can only touch in C++. What
    // the bridge knows is that it has something to say and what to call it; see tGbHostOps.
    if (self->hostOps.send_message != NULL) {
        self->hostOps.send_message(self->hostOps.user, id, value);
    }
}

static void gb_silence(float ** out, int32_t frames) {
    for (int c = 0; c < GB_CHANNELS; c++) {
        memset(out[c], 0, (size_t)frames * sizeof(float));
    }
}

// notes §21
static void gb_capture_callback(void * user, const float * input, float * output, uint32_t frames) {
    (void)output;

    tGbBridge * self = (tGbBridge *)user;
    const float *     source = input;

    if (self->captureChannels == 1) {
        for (uint32_t i = 0; i < frames; i++) {
            self->widen[(i * 2) + 0] = input[i];
            self->widen[(i * 2) + 1] = input[i];
        }

        source = self->widen;
    }

    // WHEN THE NEWEST FRAME IN THE RING GOT HERE. Without it the ring's occupancy cannot be
    // turned into an age: see gb_latency_frames_measured(self).
    atomic_store(&self->lastWriteHostTime, AudioGetCurrentHostTime());

    if (!ring_write(&self->ring, source, frames)) {
        atomic_store(&self->needResync, true);
    }
}

// notes §22

static void gb_start_worker(tGbBridge * self) {
    if (self->workerActive) {
        return;
    }

    atomic_store(&self->workerQuit, false);
    self->workerActive = (pthread_create(&self->worker, NULL, gb_worker_entry, self) == 0);
}

static void gb_stop_worker(tGbBridge * self) {
    if (!self->workerActive) {
        return;
    }

    atomic_store(&self->workerQuit, true);
    gb_wake_worker(self);
    pthread_join(self->worker, NULL);
    self->workerActive = false;
}

void gb_wake_worker(tGbBridge * self) {
    pthread_mutex_lock(&self->wakeMutex);
    self->wakeFlag = true;
    pthread_cond_signal(&self->wakeCond);
    pthread_mutex_unlock(&self->wakeMutex);
}

static double gb_now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((double)ts.tv_sec * 1000.0) + ((double)ts.tv_nsec / 1.0e6);
}

void gb_request_device(tGbBridge * self) {
    atomic_store(&self->lastDeviceRequestMs, gb_now_ms());
    atomic_store(&self->deviceDirty, true);
    gb_wake_worker(self);
}

// The panel reads this out of the shared block rather than being messaged, like every other live
// figure - see gbStatus.h.
static void gb_publish_waiting(tGbBridge * self, bool waiting, const char * name) {
    tGbStatus * status = gb_status(self->statusSlot);

    if (status == NULL) {
        return;
    }

    // NAME FIRST, FLAG SECOND. The panel only reads the name while the flag is up, so writing
    // them in this order means it can never show "waiting for" against a stale name.
    if (waiting && (name != NULL)) {
        snprintf(status->waitingName, sizeof(status->waitingName), "%s", name);
    }
    atomic_store(&status->waitingForDevice, waiting ? 1 : 0);
}

static void * gb_worker_entry(void * arg) {
    gb_worker_loop((tGbBridge *)arg);
    return NULL;
}

static void gb_worker_loop(tGbBridge * self) {
    while (!atomic_load(&self->workerQuit)) {
        pthread_mutex_lock(&self->wakeMutex);

        while (!self->wakeFlag && !atomic_load(&self->workerQuit)) {
            pthread_cond_wait(&self->wakeCond, &self->wakeMutex);
        }

        self->wakeFlag = false;
        pthread_mutex_unlock(&self->wakeMutex);

        if (atomic_load(&self->workerQuit)) {
            break;
        }

        // notes §23
        while (atomic_load(&self->deviceDirty) && !atomic_load(&self->workerQuit)) {
            double waited = gb_now_ms() - atomic_load(&self->lastDeviceRequestMs);

            if (waited >= GB_DEVICE_SETTLE_MS) {
                break;
            }
            usleep((useconds_t)((GB_DEVICE_SETTLE_MS - waited) * 1000.0));
        }

        if (!atomic_load(&self->workerQuit) && atomic_exchange(&self->deviceDirty, false)) {
            gb_reconfigure(self);
        }

        if (atomic_load(&self->retuneState) == eRetuneRequested) {
            gb_retune(self);
        }

        if (atomic_exchange(&self->revertWanted, false)) {
            gb_revert_retune(self);
        }

        if (atomic_exchange(&self->measurePanic, false)) {
            synthlib_log_line("measure requested");
            gb_send_all_notes_off(self);
        }

        if (atomic_exchange(&self->measureStore, false)) {
            gb_store_measurement(self);
        }

        // notes §24
        while (atomic_load(&self->offsetDirty) && !atomic_load(&self->workerQuit)) {
            double waited = gb_now_ms() - atomic_load(&self->lastOffsetChangeMs);

            if (waited >= GB_OFFSET_SETTLE_MS) {
                break;
            }
            usleep((useconds_t)((GB_OFFSET_SETTLE_MS - waited) * 1000.0));
        }

        if (!atomic_load(&self->workerQuit) && atomic_exchange(&self->offsetDirty, false)) {
            uint32_t nowLatency = gb_snapshot_latency(self);

            if (nowLatency != self->reportedLatency) {
                self->reportedLatency = nowLatency;
                atomic_store(&self->latencyDirty, true);
            }
        }

        // notes §25
        if (atomic_load(&self->latencyDirty)) {
            // notes §26
            double ratio    = (atomic_load(&self->snapRatio) > 0.0) ? atomic_load(&self->snapRatio) : 1.0;
            double perMs    = (atomic_load(&self->snapHostRate) > 0.0) ? (atomic_load(&self->snapHostRate) / 1000.0) : 48.0;
            double ringPart = atomic_load(&self->snapSetpoint) / ratio;
            double devPart  = (double)atomic_load(&self->snapDeviceLatency) / ratio;
            double filtPart = resampler_latency_frames() / ratio;
            double offPart  = (atomic_load(&self->offsetMs) / 1000.0) * atomic_load(&self->snapHostRate);

            synthlib_log_line("latency %u smp (%.1f ms) = pipeline %.0f (setpoint would say %.0f: ring "
                     "%.0f + device %.0f + filter %.0f) + measured %.0f (%.1f ms)",
                     atomic_load(&self->reportedLatency), (double)atomic_load(&self->reportedLatency) / perMs,
                     atomic_load(&self->pipelineAvg), ringPart + devPart + filtPart,
                     ringPart, devPart, filtPart, offPart, offPart / perMs);
        }

        if (atomic_exchange(&self->latencyDirty, false)) {
            // notes §27
            gb_sync_offset_to_pair(self);
            gb_send_latency_changed(self);
            gb_publish_measurement(self);
        }
    }
}

void gb_reconfigure(tGbBridge * self) {
    tDeviceInfo list[DEVICE_MAX];
    uint32_t    count = device_enumerate(list, DEVICE_MAX);
    tDeviceInfo chosen;
    bool        found = false;

    int index = atomic_load(&self->wantedDevice);

    // HOST INPUT OPENS NOTHING, and closes whatever was open. Holding a capture device while the
    // audio is coming from the host would keep hardware nobody is listening to - and on a device
    // that is also the host's own interface, it would keep imposing a buffer size on it.
    if (atomic_load(&self->captureSource) == GB_SOURCE_HOST) {
        pthread_mutex_lock(&self->configLock);
        gb_close_capture_locked(self);
        pthread_mutex_unlock(&self->configLock);

        gb_publish_waiting(self, false, NULL);

        tGbStatus * status = gb_status(atomic_load(&self->statusSlot));

        if (status != NULL) {
            snprintf(status->deviceName, sizeof(status->deviceName), "%s", "Host input");
            atomic_store(&status->active, true);
        }

        // WHAT THE HOST IS TOLD IS THE CORRECTION AND NOTHING ELSE. gb_report_latency() adds the
        // pipeline term to it, and with nothing open that term is already zero - nominalRatio is 0,
        // so gb_internal_latency_frames() returns 0 rather than dividing by it.
        uint32_t nowLatency = gb_report_latency(self);

        if (nowLatency != atomic_load(&self->reportedLatency)) {
            atomic_store(&self->reportedLatency, nowLatency);
            gb_send_latency_changed(self);
        }
        synthlib_log_line("capture source: HOST INPUT - no device opened, latency %u samples", nowLatency);
        return;
    }

    synthlib_log_line("reconfigure: slot %d, saved uid '%s'", index, self->deviceSelector);

    // notes §28
    if (atomic_load(&self->savedDevicePending) && !(self->deviceSelector[0] == '\0')) {
        found = device_find(self->deviceSelector, true, &chosen);

        if (found) {
            atomic_store(&self->savedDevicePending, false);
            gb_publish_waiting(self, false, NULL);

            int slot = gb_slot_for_uid(chosen.uid);

            if (slot >= 0) {
                atomic_store(&self->wantedDevice, slot);
                index = slot;
                gb_send_message(self, "gbDeviceSlot", slot);
            }
            synthlib_log_line("saved device '%s' present - opening it", chosen.name);
        } else {
            // Device not present, don't modify deviceSelector - preserve the saved intent.
            // gb_publish_waiting(self, true, (savedDeviceName[0] == '\0') ? deviceSelector
            //                                               : savedDeviceName);
            synthlib_log_line("saved device '%s' not present - waiting (not modifying deviceSelector)",
                     (self->savedDeviceName[0] == '\0') ? self->deviceSelector : self->savedDeviceName);
        }
    } else if (index >= 0) {
        gb_publish_waiting(self, false, NULL);
        found = gb_resolve_slot(list, count, index, &chosen);
    }

    // Before any parameter has arrived, the saved UID is what a reopened project has to go on.
    // It is no longer a competing selector: the controller resolves the same UID to the same
    // slot and sets the parameter to match, so what arrives next agrees with what opened here.
    if (!found && (index < 0) && !(self->deviceSelector[0] == '\0')) {
        found = device_find(self->deviceSelector, true, &chosen);
        synthlib_log_line("no parameter yet - restoring saved uid: %s", found ? chosen.name : "not present");
    }

    // notes §29
    if (!found && (index < 0)) {
        synthlib_log_line("nothing selected yet - staying idle");
    }

    if (!found) {
        synthlib_log_line("slot %d resolved to nothing - staying closed", index);
    } else {
        synthlib_log_line("slot %d -> '%s'", index, chosen.name);
    }

    // What gb_close_capture_locked(self) must NOT hand back, because we are about to reopen it. Zero
    // when the reconfigure is a genuine device change or a close, where the restore is right.
    self->keepSettingsFor = found ? chosen.id : 0;

    double heldFrom = gb_now_ms();
    pthread_mutex_lock(&self->configLock);
    gb_close_capture_locked(self);
    self->keepSettingsFor = 0;

    if (found) {
        // Recorded only on SUCCESS. Writing it before the attempt meant a device that failed to
        // open still became the thing the project saved and reopened with.
        if (gb_open_capture_locked(self, &chosen)) {
            snprintf(self->deviceSelector, sizeof(self->deviceSelector), "%s", chosen.uid);
            self->appliedDevice  = index;
        } else {
            self->running = false;
            synthlib_log_line("open failed - selection unchanged, staying closed");
        }
    }

    uint32_t nowLatency = self->running ? gb_report_latency(self) : 0;
    double held       = gb_now_ms() - heldFrom;

    pthread_mutex_unlock(&self->configLock);

    // THE OTHER HALF OF THE SPINNING CURSOR. This is how long the worker owned the lock that
    // every host-thread entry point waits for; gb_lock_config_from_host(self) reports the wait from
    // the other side. The two together say whether a beachball was this plug-in's doing.
    if (held >= 20.0) {
        // BROKEN DOWN, because "it held the lock for 1441 ms" says a refactor is needed and not
        // WHICH of the three things inside it to move first.
        synthlib_log_line("reconfigure held configLock for %.0f ms (close+probe %.0f, rate+buffer %.0f, "
                 "open %.0f) - anything the host asked of this plug-in on its own thread waited "
                 "behind it", held,
                 (self->gPhaseIdle > heldFrom) ? (self->gPhaseIdle - heldFrom) : 0.0,
                 (self->gPhaseOpen > self->gPhaseProps) ? (self->gPhaseOpen - self->gPhaseProps) : 0.0,
                 (self->gPhaseOpen > 0.0) ? (gb_now_ms() - self->gPhaseOpen) : 0.0);
    }

    // Told OUTSIDE the lock: restartComponent re-enters the plug-in, and a host is entitled to
    // call straight back into it - including into process(), which trylocks this same mutex.
    if (nowLatency != self->reportedLatency) {
        self->reportedLatency = nowLatency;
        gb_send_latency_changed(self);
        synthlib_log_line("latency now %u samples - asked the host to re-read it", nowLatency);
    }
}

// notes §30
static bool gb_resolve_slot(const tDeviceInfo * list, uint32_t count, int slot, tDeviceInfo * out) {
    uint32_t seen = 1;      // slot 0 is None, so the first real device is slot 1

    if (slot <= 0) {
        return false;       // None: nothing to open, and gb_reconfigure(self) closes what is open
    }

    for (uint32_t i = 0; i < count; i++) {
        if (list[i].inputChannels == 0) {
            continue;
        }

        if ((int)seen == slot) {
            *out = list[i];
            return true;
        }

        seen++;
    }

    return false;
}

// Apply the smaller setpoint the observed block size allows, then tell the host its latency
// moved. Worker thread only.
static void gb_retune(tGbBridge * self) {
    pthread_mutex_lock(&self->configLock);

    if (!self->running) {
        pthread_mutex_unlock(&self->configLock);
        atomic_store(&self->retuneState, eRetuneSettled);
        return;
    }

    double before = self->setpointFrames;

    self->setpointFrames = gb_minimum_setpoint_for_host(self, self->observedMaxFrames) * GB_AUTO_MARGIN;

    drift_set_setpoint(&self->drift, self->setpointFrames);
    gb_publish_config_snapshot(self);
    gb_publish_latency_breakdown(self);

    // The ring holds more than the new setpoint wants, so snap it down rather than waiting for
    // the loop to drain it at a few hundred ppm - which would take minutes.
    atomic_store(&self->needResync, true);

    tGbStatus * status = gb_status(self->statusSlot);

    if (status != NULL) {
        atomic_store(&status->setpointFrames, self->setpointFrames);
    }

    uint32_t nowLatency = gb_report_latency(self);

    pthread_mutex_unlock(&self->configLock);

    atomic_store(&self->retuneState, eRetuneSettled);

    // Reads both ways now: reclaiming frames when the host uses less than it declared, and
    // CLAIMING them when it hands over more per callback than either number suggested.
    synthlib_log_line("retuned %s: host declares %u and takes %u per callback - setpoint %.0f -> %.0f, "
             "latency %u -> %u",
             (self->setpointFrames > before) ? "UP - the ring was smaller than one callback" : "down",
             self->hostMaxFrames, self->observedMaxFrames, before, self->setpointFrames, atomic_load(&self->reportedLatency),
             nowLatency);

    if (nowLatency != self->reportedLatency) {
        self->reportedLatency = nowLatency;

        if (status != NULL) {
            atomic_store(&status->latencySamples, (int)nowLatency);
        }

        gb_send_latency_changed(self);
    }
}

// The pair a measurement belongs to. Both halves matter: the same synth reached over USB and
// over DIN answers at different speeds, and two synths on one interface are not comparable.
tMeasured * gb_measured_for(tGbBridge * self, const char * audioUid, const char * midiDest, bool create) {
    for (uint32_t i = 0; i < self->measuredCount; i++) {
        if ((strncmp(self->measured[i].audioUid, audioUid, DEVICE_UID_LEN) == 0)
            && (strncmp(self->measured[i].midiDest, midiDest, GB_MIDI_NAME_LEN) == 0)) {
            return &self->measured[i];
        }
    }

    if (!create) {
        return NULL;
    }

    uint32_t slot = self->measuredCount;

    if (self->measuredCount < GB_MAX_MEASURED) {
        self->measuredCount++;
    } else {
        memmove(&self->measured[0], &self->measured[1], sizeof(tMeasured) * (GB_MAX_MEASURED - 1));
        slot = GB_MAX_MEASURED - 1;
    }

    memset(&self->measured[slot], 0, sizeof(self->measured[slot]));
    strncpy(self->measured[slot].audioUid, audioUid, DEVICE_UID_LEN - 1);
    strncpy(self->measured[slot].midiDest, midiDest, GB_MIDI_NAME_LEN - 1);

    return &self->measured[slot];
}

void gb_current_midi_name(tGbBridge * self, char * out, unsigned long len) {
    gb_midi_destination_name(atomic_load(&self->midiDestination), out, len);
}

void gb_remember_offset_pair(tGbBridge * self, const char * destination) {
    snprintf(self->offsetUid, sizeof(self->offsetUid), "%s", gb_audio_key(self));
    snprintf(self->offsetDest, sizeof(self->offsetDest), "%s", destination);
}

// WORKER THREAD ONLY. The +/- arrives on the audio thread, which stores the atomic and asks for
// a latency update; the table write happens here instead, because gb_measured_for(self) can allocate a
// slot and memmove the array and that has no business running under a process() call.
void gb_sync_offset_to_pair(tGbBridge * self) {
    // THE KEY IS THE AUDIO SOURCE, which in host-input mode is not a device UID - see gb_audio_key().
    // Testing deviceSelector here meant that in that mode there was no pair to fold the correction
    // into, so a measured or nudged offset was never written down.
    if (!self->instrument || (gb_audio_key(self)[0] == '\0')) {
        return;
    }

    char destination[GB_MIDI_NAME_LEN];

    gb_current_midi_name(self, destination, sizeof(destination));

    tMeasured * entry = gb_measured_for(self, gb_audio_key(self), destination, true);

    if (entry != NULL) {
        entry->offsetMs = atomic_load(&self->offsetMs);
    }

    gb_remember_offset_pair(self, destination);
}

// notes §31
void gb_publish_measurement(tGbBridge * self) {
    gb_publish_latency_breakdown(self);
}

// Every component of the reported figure, so the panel can show where the time actually goes
// rather than one number nobody can argue with.
static void gb_publish_latency_breakdown(tGbBridge * self) {
    tGbStatus * status = gb_status(self->statusSlot);

    if (status == NULL) {
        return;
    }

    // FROM THE SNAPSHOT, not the fields. This is called both with configLock held (from the
    // open and from retune) and without it (from the worker's idle publish), so it cannot take
    // the lock and must not read anything the lock protects.
    double setpoint = atomic_load(&self->snapSetpoint);
    double ratio    = atomic_load(&self->snapRatio);

    if (ratio <= 0.0) {
        ratio = 1.0;
    }

    atomic_store(&status->ringSamples, (int)(setpoint / ratio));
    atomic_store(&status->deviceSamples, (int)((double)atomic_load(&self->snapDeviceLatency) / ratio));
    atomic_store(&status->filterSamples, (int)(resampler_latency_frames() / ratio));
    atomic_store(&status->measuredSamples, (int)self->hardwareSamples);
    atomic_store(&status->measuredLow, atomic_load(&self->measureTripLow));
    atomic_store(&status->measuredHigh, atomic_load(&self->measureTripHigh));
    atomic_store(&status->measuredTrips, atomic_load(&self->measureTripsUsed));
    atomic_store(&status->offsetSamples, (int)((atomic_load(&self->offsetMs) / 1000.0) * atomic_load(&self->snapHostRate)));
    atomic_store(&status->latencySamples, (int)gb_snapshot_latency(self));
    atomic_store(&status->recommendedFrames, self->recommendedSetpoint);
    atomic_store(&status->eventsIn, (int)atomic_load(&self->eventsIn));
    atomic_store(&status->eventsOut, (int)atomic_load(&self->eventsOut));
}

// Put the conservative floor back after a retune turned out to be too tight.
static void gb_revert_retune(tGbBridge * self) {
    pthread_mutex_lock(&self->configLock);

    if (!self->running) {
        pthread_mutex_unlock(&self->configLock);
        return;
    }

    double before = self->setpointFrames;

    self->setpointFrames = gb_minimum_setpoint(self, self->openDeviceFrames) * GB_AUTO_MARGIN;

    drift_set_setpoint(&self->drift, self->setpointFrames);
    atomic_store(&self->needResync, true);

    tGbStatus * status = gb_status(self->statusSlot);

    if (status != NULL) {
        atomic_store(&status->setpointFrames, self->setpointFrames);
    }

    uint32_t nowLatency = gb_report_latency(self);

    pthread_mutex_unlock(&self->configLock);

    synthlib_log_line("retune reverted after an underrun: setpoint %.0f -> %.0f", before, self->setpointFrames);

    if (nowLatency != self->reportedLatency) {
        self->reportedLatency = nowLatency;

        if (status != NULL) {
            atomic_store(&status->latencySamples, (int)nowLatency);
        }

        gb_send_latency_changed(self);
    }
}

static bool gb_open_capture_locked(tGbBridge * self, const tDeviceInfo * info) {
    tDeviceSettings * settings = gb_ensure_settings(self, info->uid);

    // A rate or buffer size chosen since the last open takes precedence over what was stored,
    // and is then stored itself - so the choice survives the next reopen of the project.
    if (atomic_load(&self->wantedRate) > 0.0) {
        settings->rate = atomic_exchange(&self->wantedRate, 0.0);
    }

    if (atomic_load(&self->wantedFrames) > 0) {
        settings->frames = (uint32_t)atomic_exchange(&self->wantedFrames, 0);
    }

    if (atomic_load(&self->wantedChannels) > 0) {
        settings->captureChannels = (uint32_t)atomic_exchange(&self->wantedChannels, 0);
    }

    if (atomic_load(&self->wantedFirstChannel) >= 0) {
        settings->firstChannel = (uint32_t)atomic_exchange(&self->wantedFirstChannel, -1);
    }

    // notes §32
    uint32_t available = info->inputChannels;
    uint32_t first     = settings->firstChannel;
    uint32_t wantCount = settings->captureChannels;

    if (available == 0) {
        synthlib_log_line("device '%s' reports no input channels", info->name);
        return false;
    }

    if (first >= available) {
        synthlib_log_line("first channel %u past the device's %u - using channel 1", first + 1, available);
        first = 0;
    }

    if ((first + wantCount) > available) {
        wantCount = available - first;      // at least 1, since first < available
        synthlib_log_line("only %u channel(s) available from %u - capturing %u",
                 available - first, first + 1, wantCount);
    }

    // notes §33
    if (first != settings->firstChannel) {
        settings->firstChannel = first;
        gb_send_message(self, "gbFirstChannel", (int)first);
    }

    // Same again for the width. A one-channel device cannot give a stereo pair, and the capture
    // callback widens the single channel rather than failing - but the panel must not go on
    // saying "Stereo" over a mono capture.
    if (wantCount != settings->captureChannels) {
        settings->captureChannels = wantCount;
        gb_send_message(self, "gbMode", (int)((wantCount > 1) ? 1 : 0));
    }
    self->captureChannels = wantCount;

    // notes §34
    device_wait_until_idle(info->id, 120);

    self->gPhaseIdle = gb_now_ms();

    bool deviceIsShared = device_is_running_somewhere(info->id);

    if (deviceIsShared && ((settings->rate > 0.0) || (settings->frames > 0))) {
        synthlib_log_line("device '%s' is already running for another client - leaving its rate and"
                 " buffer size alone (asked for %.0f Hz, %u frames). Its buffer is whatever that"
                 " client set, and the ring and reported latency follow it",
                 info->name, settings->rate, settings->frames);
    }

    {
        tGbStatus * status = gb_status(self->statusSlot);

        if (status != NULL) {
            atomic_store(&status->deviceShared, deviceIsShared ? 1 : 0);
        }
    }

    self->gPhaseProps = gb_now_ms();

    // Rate before buffer size: changing the nominal rate can reset the buffer size on some
    // devices, so doing it the other way round silently loses the buffer setting.
    if (!deviceIsShared && (settings->rate > 0.0)) {
        // WHAT IT WAS, so gb_close_capture_locked(self) can hand the device back as it found it.
        // Recorded only when we are actually about to change it, and only for the device we
        // changed - restoring one we never touched would be its own kind of interference.
        if (device_sample_rate(info->id) != settings->rate) {
            self->restoreDevice = info->id;
            self->restoreRate   = device_sample_rate(info->id);
        }
        device_set_sample_rate_and_wait(info->id, settings->rate);
    }

    if (!deviceIsShared && (settings->frames > 0)) {
        uint32_t lowest  = 0;
        uint32_t highest = 0;
        uint32_t wanted  = settings->frames;

        // CLAMPED TO WHAT THE DEVICE ALLOWS. Asking a USB interface for 16 frames is simply
        // refused, and a refusal looks exactly like a device that changed its mind on its own -
        // so ask what it can do and say what happened.
        if (device_buffer_frame_range(info->id, &lowest, &highest)) {
            if (wanted < lowest) {
                synthlib_log_line("device '%s' will not go below %u frames; %u requested",
                         info->name, lowest, settings->frames);
                wanted = lowest;
            } else if ((highest > 0) && (wanted > highest)) {
                wanted = highest;
            }
        }

        if (device_buffer_frames(info->id) != wanted) {
            self->restoreDevice = info->id;
            self->restoreFrames = device_buffer_frames(info->id);
        }
        // notes §35
        if (!device_set_buffer_frames(info->id, wanted) && (self->restoreDevice == info->id)) {
            // Nothing was changed, so there is nothing to hand back on close.
            self->restoreDevice = 0;
            self->restoreFrames = 0;
        }
    }

    double deviceRate = device_sample_rate(info->id);

    if ((deviceRate <= 0.0) || (self->hostRate <= 0.0)) {
        return false;
    }

    uint32_t deviceFrames = device_buffer_frames(info->id);

    // notes §36
    if ((settings->frames > 0) && (deviceFrames != settings->frames)) {
        uint32_t canDo   = 0;
        uint32_t canDoTo = 0;
        bool     ranged  = device_buffer_frame_range(info->id, &canDo, &canDoTo);

        // notes §37
        bool inRange = ranged && (settings->frames >= canDo)
                       && ((canDoTo == 0) || (settings->frames <= canDoTo));

        // notes §38
        const char * culprit = NULL;

        for (uint32_t slot = 0; slot < GB_STATUS_SLOTS; slot++) {
            tGbStatus * other = gb_status(slot);

            if ((other == NULL) || (slot == (uint32_t)self->statusSlot)
                || !atomic_load(&other->active)) {
                continue;
            }

            if (strcmp(other->deviceName, info->name) == 0) {
                culprit = other->deviceName;
                break;
            }
        }

        synthlib_log_line("asked %s for %u frames and it gave %u - %s. Its range is %u..%u. The ring and "
                 "the reported latency follow the %u, which is why they look large against the "
                 "setting on the panel",
                 info->name, settings->frames, deviceFrames,
                 (culprit != NULL)
                 ? "ANOTHER GenBridge IN THIS HOST ALREADY HAS THIS DEVICE OPEN, and the buffer "
                   "belongs to whoever opened it first - set them both the same, or point them "
                   "at different devices"
                 : (inRange ? "the size is one it says it supports, so something outside this "
                    "host already has the device open and the buffer belongs to whoever opened "
                    "it first"
                    : "outside what its driver will do"),
                 ranged ? canDo : 0, ranged ? canDoTo : 0, deviceFrames);
    }

    self->nominalRatio   = deviceRate / self->hostRate;
    self->deviceLatency  = device_latency_frames(info->id, true);
    self->manualSetpoint = (settings->targetMs > 0.0);
    self->setpointFrames = self->manualSetpoint
                     ? ((settings->targetMs / 1000.0) * deviceRate)
                     : 0.0;
    atomic_store(&self->trimGain, settings->trim);

    // notes §39
    bool trustObserved = (atomic_load(&self->retuneState) != eRetuneWatching) && (self->observedMaxFrames > 0);

    uint32_t effectiveHostFrames = trustObserved
                                   ? self->observedMaxFrames
                                   : ((self->observedMaxFrames > self->hostMaxFrames) ? self->observedMaxFrames
                                      : self->hostMaxFrames);
    double   minimum              = gb_minimum_setpoint_for(self, effectiveHostFrames, deviceFrames);

    // notes §40
    self->recommendedSetpoint = minimum * GB_AUTO_MARGIN;

    if (self->setpointFrames <= 0.0) {
        self->setpointFrames = self->recommendedSetpoint;           // auto
    } else if (self->setpointFrames < minimum) {
        synthlib_log_line("setpoint %.0f is below the %.0f frame floor (recommended %.0f) — honouring it;"
                 " watch the underrun count", self->setpointFrames, minimum, self->recommendedSetpoint);
    }

    synthlib_log_line("open %s rate %.0f frames %u ratio %.6f floor %.0f setpoint %.0f devlat %u"
             " -> reported latency %u",
             info->name, deviceRate, deviceFrames, self->nominalRatio, minimum, self->setpointFrames,
             self->deviceLatency,
             (unsigned)((self->setpointFrames + resampler_latency_frames() + (double)self->deviceLatency)
                        / self->nominalRatio));

    uint32_t ringFrames = (uint32_t)(self->setpointFrames * 8.0)
                          + (4 * (deviceFrames + self->hostMaxFrames));

    if (!ring_init(&self->ring, ringFrames, GB_CHANNELS)) {
        return false;
    }

    if (!resampler_init(&self->resampler, GB_CHANNELS, self->nominalRatio, self->hostMaxFrames)) {
        return false;
    }

    self->pullCapacity = (uint32_t)((double)self->hostMaxFrames * self->nominalRatio * 1.5) + (4 * RESAMPLER_TAPS);
    self->pullBuffer   = (float *)calloc((size_t)self->pullCapacity * GB_CHANNELS, sizeof(float));
    self->interleaved  = (float *)calloc((size_t)self->hostMaxFrames * GB_CHANNELS, sizeof(float));
    self->widen        = (float *)calloc((size_t)deviceFrames * 4 * GB_CHANNELS, sizeof(float));

    if ((self->pullBuffer == NULL) || (self->interleaved == NULL) || (self->widen == NULL)) {
        return false;
    }

    tDriftConfig config = drift_default_config();

    drift_init(&self->drift, &config, deviceRate, self->setpointFrames);

    self->gPhaseOpen = gb_now_ms();

    if (!device_open(&self->capture, info->id, true, first, self->captureChannels,
                     deviceFrames * 4, gb_capture_callback, self)) {
        synthlib_log_line("device_open failed for '%s' (%u ch from %u)", info->name, atomic_load(&self->captureChannels),
                 first + 1);
        return false;
    }

    if (!device_start(&self->capture)) {
        return false;
    }

    // notes §41
    atomic_store(&self->ring.underflows, 0);
    atomic_store(&self->ring.overflows, 0);
    atomic_store(&self->resyncs, 0);
    self->primed = false;

    atomic_store(&self->needResync, true);
    self->running = true;

    // What is now genuinely in force. The parameter handlers compare against these, so a
    // re-sent value that changes nothing costs nothing.
    self->appliedRate         = deviceRate;
    self->appliedFrames       = deviceFrames;
    self->appliedChannels     = self->captureChannels;
    self->appliedFirstChannel = settings->firstChannel;
    self->openDeviceFrames    = deviceFrames;

    // Whatever was measured for this device and destination before.
    {
        char destination[GB_MIDI_NAME_LEN];

        gb_current_midi_name(self, destination, sizeof(destination));

        const tMeasured * previous = gb_measured_for(self, info->uid, destination, false);

        self->hardwareSamples = (previous != NULL) ? previous->hardwareSamples : 0;

        // notes §42
        if ((strncmp(self->offsetUid, info->uid, DEVICE_UID_LEN) != 0)
            || (strncmp(self->offsetDest, destination, GB_MIDI_NAME_LEN) != 0)) {
            atomic_store(&self->offsetMs, (previous != NULL) ? previous->offsetMs : 0.0);
            gb_remember_offset_pair(self, destination);
            gb_send_message(self, "gbOffset", (int)lround(atomic_load(&self->offsetMs) * 1000.0));
        }
    }

    // notes §43
    self->observedFrames = 0;

    // notes §44
    atomic_store(&self->retuneState, ((self->observedMaxFrames > 0) || self->manualSetpoint)
                      ? eRetuneSettled : eRetuneWatching);

    tGbStatus * status = gb_status(self->statusSlot);

    snprintf(status->deviceName, sizeof(status->deviceName), "%s", info->name);
    atomic_store(&status->deviceRate, (int)deviceRate);
    atomic_store(&status->deviceFrames, (int)deviceFrames);
    atomic_store(&status->setpointFrames, self->setpointFrames);
    atomic_store(&status->active, true);

    gb_publish_config_snapshot(self);
    gb_publish_latency_breakdown(self);

    return true;
}

static void gb_close_capture_locked(tGbBridge * self) {
    tGbStatus * status = gb_status(self->statusSlot);

    if (status != NULL) {
        // Cleared, not just marked inactive: a stale device name outlives the device and the
        // panel goes on naming something it is no longer connected to.
        atomic_store(&status->active, false);
        atomic_store(&status->deviceRate, 0);
        atomic_store(&status->latencySamples, 0);
        status->deviceName[0] = '\0';
    }

    if (self->running) {
        device_close(&self->capture);
        ring_free(&self->ring);
        resampler_free(&self->resampler);
        self->running = false;
    }

    // notes §45
    self->nominalRatio   = 0.0;
    self->setpointFrames = 0.0;
    self->deviceLatency  = 0;
    gb_publish_config_snapshot(self);

    // notes §46
    if ((self->restoreDevice != 0) && (self->restoreDevice == self->keepSettingsFor)) {
        synthlib_log_line("reopening the same device - leaving its buffer alone rather than restoring "
                 "%u frames and setting it straight back", self->restoreFrames);
    } else if (self->restoreDevice != 0) {
        if (self->restoreFrames > 0) {
            device_set_buffer_frames(self->restoreDevice, self->restoreFrames);
        }

        if (self->restoreRate > 0.0) {
            device_set_sample_rate(self->restoreDevice, self->restoreRate);
        }
        synthlib_log_line("restored device settings: %u frames, %.0f Hz", self->restoreFrames, self->restoreRate);

        self->restoreDevice = 0;
        self->restoreFrames = 0;
        self->restoreRate   = 0.0;
    }

    // notes §47
    self->observedFrames = 0;
    atomic_store(&self->retuneState, (self->observedMaxFrames > 0) ? eRetuneSettled : eRetuneWatching);

    free(self->pullBuffer);
    free(self->interleaved);
    free(self->widen);
    self->pullBuffer  = NULL;
    self->interleaved = NULL;
    self->widen       = NULL;
}

// notes §48
static double gb_minimum_setpoint(tGbBridge * self, uint32_t deviceFrames) {
    return gb_minimum_setpoint_for(self, self->hostMaxFrames, deviceFrames);
}

// TWO NAMES WHERE C++ HAD ONE OVERLOADED, and the shorter one says which frames it means: the
// device's own, as they stand. An overload set is a thing a reader has to resolve; two names are
// not.
static double gb_minimum_setpoint_for_host(tGbBridge * self, uint32_t hostFrames) {
    return gb_minimum_setpoint_for(self, hostFrames, self->openDeviceFrames);
}

static double gb_minimum_setpoint_for(tGbBridge * self, uint32_t hostFrames, uint32_t deviceFrames) {
    return ((double)hostFrames * self->nominalRatio)
           + (double)deviceFrames
           + (2.0 * RESAMPLER_TAPS);
}

static void gb_close_capture(tGbBridge * self) {
    pthread_mutex_lock(&self->configLock);
    gb_close_capture_locked(self);
    pthread_mutex_unlock(&self->configLock);
}
// notes §49

tGbBridge * gb_bridge_create(bool instrument) {
    tGbBridge * self = (tGbBridge *)calloc(1, sizeof(tGbBridge));

    if (self == NULL) {
        return NULL;
    }

    // WHAT IS NOT ZERO. calloc has done the rest, which is what a C++ member initialiser list was
    // doing one field at a time - and the few below are the ones where zero would be a lie: no
    // status slot yet, a ratio that is a DIVISOR, and the "nothing chosen" that -1 means.
    self->instrument = instrument;
    atomic_store(&self->statusSlot, gb_status_claim());
    atomic_store(&self->snapRatio, 1.0);
    atomic_store(&self->retuneState, eRetuneSettled);
    atomic_store(&self->testNote, GB_MEASURE_NOTE);
    atomic_store(&self->needResync, true);
    atomic_store(&self->trimGain, 1.0f);
    atomic_store(&self->wantedDevice, -1);
    atomic_store(&self->wantedFirstChannel, -1);
    atomic_store(&self->captureChannels, GB_CHANNELS);
    self->blockStartsCycle = true;
    self->measureState     = eMeasureIdle;
    self->appliedDevice    = -1;
    self->hostMaxFrames    = 1024;
    self->nominalRatio     = 1.0;

    if (instrument) {
        gb_midi_init();
    }

    pthread_mutex_init(&self->configLock, NULL);
    pthread_mutex_init(&self->wakeMutex, NULL);
    pthread_cond_init(&self->wakeCond, NULL);

    return self;
}

void gb_bridge_destroy(tGbBridge * self) {
    if (self == NULL) {
        return;
    }
    gb_stop_worker(self);
    gb_close_capture(self);
    gb_status_release(atomic_load(&self->statusSlot));
    free(self->stateBlob);       // the blob the host last asked for; see gb_bridge_state()

    pthread_cond_destroy(&self->wakeCond);
    pthread_mutex_destroy(&self->wakeMutex);
    pthread_mutex_destroy(&self->configLock);

    free(self);
}

void gb_bridge_connect(tGbBridge * self, const tGbHostOps * ops) {
    self->hostOps = *ops;

    // The status slot, announced once. The panel reads the meters and the drift figures straight
    // out of that slot rather than being sent a message per frame.
    gb_send_slot(self);
    synthlib_log_line("connected to controller, published status slot %d", atomic_load(&self->statusSlot));
}

void gb_bridge_disconnect(tGbBridge * self) {
    tGbHostOps none = { NULL, NULL };

    self->hostOps = none;
}

void gb_bridge_watch_devices(tGbBridge * self) {
    // Hot-plug. Nothing noticed a device appearing before this, so a plug-in waiting for a saved
    // interface would have waited until the user touched a control - see device_watch_list().
    device_watch_list(gb_device_list_changed, self);
}

void gb_bridge_unwatch_devices(tGbBridge * self) {
    device_unwatch_list(self);
    gb_close_capture(self);
}

// notes §50
void gb_bridge_set_active(tGbBridge * self, bool active) {
    if (active) {
        gb_start_worker(self);
        gb_reconfigure(self);       // synchronous: latency must be known before the host asks
    } else {
        gb_stop_worker(self);
        gb_close_capture(self);
    }
}

// notes §51
void gb_bridge_setup_processing(tGbBridge * self, double sampleRate, int32_t maxBlockFrames,
                                bool offline) {
    // notes §52
    gb_lock_config_from_host(self, "setupProcessing");

    self->hostRate = sampleRate;
    atomic_store(&self->eventRate, sampleRate);

    // notes §53
    if ((self->hostMaxFrames != 0) && ((uint32_t)maxBlockFrames != self->hostMaxFrames)) {
        self->observedMaxFrames = 0;
        self->observedFrames    = 0;
        atomic_store(&self->retuneState, eRetuneWatching);
    }
    self->hostMaxFrames = (uint32_t)maxBlockFrames;

    pthread_mutex_unlock(&self->configLock);

    if (offline != atomic_load(&self->offlineRender)) {
        synthlib_log_line("host set process mode %s%s", offline ? "OFFLINE" : "realtime",
                    offline ? " - a bounce in this mode captures nothing, the device runs in real"
                              " time" : "");
    }

    atomic_store(&self->offlineRender, offline);

    tGbStatus * status = gb_status(atomic_load(&self->statusSlot));

    if (status != NULL) {
        atomic_store(&status->offlineRender, offline ? 1 : 0);
    }
}

void gb_bridge_processing_started(tGbBridge * self) {
    // The block timeline starts again with the transport. Carrying it across a stop would only
    // matter for the first block after one - the model is behind real time by then and re-anchors
    // on its own - but starting clean says what is meant.
    self->nextBlockHostTime = 0;
}

uint32_t gb_bridge_latency(tGbBridge * self) {
    // notes §54
    return atomic_load(&self->reportedLatency);
}

uint64_t gb_bridge_block_begin(tGbBridge * self, int32_t frames) {
    // notes §55
    uint64_t blockHostTime = gb_block_host_time(self, frames);

    // notes §56
    if (gb_capturing(self)) {
        gb_observe_burst(self, frames);
    }

    return blockHostTime;
}

// ONE PARAMETER, ALREADY REDUCED TO ITS LAST POINT. Walking the host's queues is the wrapper's job
// because IParamValueQueue is a C++ interface; deciding what a value MEANS is this one's, and every
// route to a change - the panel, the host's generic control, an automation lane - arrives here.
void gb_bridge_parameter(tGbBridge * self, uint32_t id, double value, int32_t sampleOffset,
                         uint64_t blockHostTime) {
    if ((id >= GB_CC_BASE) && (id < (GB_CC_BASE + GB_CC_COUNT))) {
        if (self->instrument) {
            // The point's own offset, exactly as a note gets its own - a controller move
            // inside a block belongs where the host put it.
            gb_send_controller(self, id, value, gb_host_time_for(self, blockHostTime, sampleOffset));
        }

        return;
    }

    if (id == kParamTrim) {
        atomic_store(&self->trimGain, (float)(value * 2.0));
    } else if (id == kParamDevice) {
        int wanted = gb_device_slot(value);

        if (wanted != atomic_load(&self->wantedDevice)) {
            // notes §57
            if (atomic_exchange(&self->deviceParamSeen, true) == true) {
                atomic_store(&self->savedDevicePending, false);
            }
            atomic_store(&self->wantedDevice, wanted);
            gb_request_device(self);       // signals the worker; opens nothing on this thread
        }
    } else if (id == kParamMidiDest) {
        int wanted = (int)(value * (double)(GB_MIDI_SLOTS - 1) + 0.5);

        atomic_store(&self->midiDestination, wanted);
        // notes §58
    } else if (id == kParamMidiChannel) {
        atomic_store(&self->midiChannel, (int)(value * (double)(GB_CHANNEL_SLOTS - 1) + 0.5));
    } else if (id == kParamTestNote) {
        int note = (int)((value * 127.0) + 0.5);

        atomic_store(&self->testNote, (note < 0) ? 0 : ((note > 127) ? 127 : note));
        return;
    } else if (id == kParamOffsetMs) {
        atomic_store(&self->offsetMs, GB_OFFSET_MIN_MS + (value * (GB_OFFSET_MAX_MS - GB_OFFSET_MIN_MS)));

        // notes §59
        atomic_store(&self->lastOffsetChangeMs, gb_now_ms());
        atomic_store(&self->offsetDirty, true);
        gb_wake_worker(self);
    } else if (id == kParamSource) {
        int wanted = (value < 0.5) ? GB_SOURCE_DEVICE : GB_SOURCE_HOST;

        if (wanted != atomic_load(&self->captureSource)) {
            atomic_store(&self->captureSource, wanted);

            // notes §60
            gb_request_device(self);
        }
    } else if (id == kParamRate) {
        int index = (int)(value * (double)(gGbRateCount - 1) + 0.5);

        if (gGbRates[index] != atomic_load(&self->wantedRate)) {
            atomic_store(&self->wantedRate, gGbRates[index]);
            gb_request_device(self);
        }
    } else if (id == kParamMode) {
        int wanted = (value < 0.5) ? 1 : 2;

        if ((uint32_t)wanted != self->captureChannels) {
            atomic_store(&self->wantedChannels, wanted);
            gb_request_device(self);
        }
    } else if (id == kParamFirstChannel) {
        int wanted = (int)(value * (double)(GB_MAX_FIRST_CHANNEL - 1) + 0.5);

        atomic_store(&self->wantedFirstChannel, wanted);
        gb_request_device(self);
    } else if (id == kParamFrames) {
        int index = (int)(value * (double)(gGbFrameCount - 1) + 0.5);

        if (gGbFrames[index] != atomic_load(&self->wantedFrames)) {
            atomic_store(&self->wantedFrames, gGbFrames[index]);
            gb_request_device(self);
        }
    }
}

// THE MEASURE BUTTON, which needs both halves of what the host delivered - see gbBridge.h.
void gb_bridge_measure_trigger(tGbBridge * self, bool sawPress, bool held) {
    // notes §61
    if (sawPress && !self->measureArmed && (self->measureState == eMeasureIdle)) {
        // notes §62
        gb_start_measurement(self);
    }

    self->measureArmed = held;
}

// notes §63
void gb_bridge_note(tGbBridge * self, tGbNoteKind kind, int16_t channel, int16_t pitch,
                    float value, int32_t sampleOffset, uint64_t blockHostTime) {
    int     destination = atomic_load(&self->midiDestination);
    uint8_t message[3];

    if (kind == eGbNoteOn) {
        int velocity = (int)((value * 127.0f) + 0.5f);

        // A note-on at zero velocity is a note-off on the wire, and some devices treat the two
        // differently. Sending a real note-off is the safer of the two.
        if (velocity <= 0) {
            message[0] = (uint8_t)(0x80 | gb_channel_for(self, channel));
            message[1] = (uint8_t)(pitch & 0x7F);
            message[2] = 64;
        } else {
            message[0] = (uint8_t)(0x90 | gb_channel_for(self, channel));
            message[1] = (uint8_t)(pitch & 0x7F);
            message[2] = (uint8_t)((velocity > 127) ? 127 : velocity);
        }
    } else if (kind == eGbNoteOff) {
        int velocity = (int)((value * 127.0f) + 0.5f);

        message[0] = (uint8_t)(0x80 | gb_channel_for(self, channel));
        message[1] = (uint8_t)(pitch & 0x7F);
        message[2] = (uint8_t)((velocity < 0) ? 0 : ((velocity > 127) ? 127 : velocity));
    } else {
        message[0] = (uint8_t)(0xA0 | gb_channel_for(self, channel));
        message[1] = (uint8_t)(pitch & 0x7F);
        message[2] = (uint8_t)((value * 127.0f) + 0.5f);
    }

    // notes §64
    atomic_fetch_add(&self->eventsIn, 1);

    if (gb_midi_send_at(destination, message, 3,
                        gb_host_time_for(self, blockHostTime, sampleOffset))) {
        atomic_fetch_add(&self->eventsOut, 1);
    }
}

// notes §65
static void gb_render_from_host(tGbBridge * self, float ** out, const float * const * in,
                                int32_t inChannels, int32_t frames, uint64_t blockHostTime) {
    float trim = atomic_load(&self->trimGain);

    for (int c = 0; c < GB_CHANNELS; c++) {
        // A MONO SOURCE FEEDS BOTH SIDES, the same rule the device path applies on the way into the
        // ring - see the widen buffer in gb_capture_callback().
        const float * source = ((in != NULL) && (inChannels > 0))
                               ? in[(c < inChannels) ? c : (inChannels - 1)] : NULL;

        if (source == NULL) {
            memset(out[c], 0, (size_t)frames * sizeof(float));
            continue;
        }

        for (int32_t i = 0; i < frames; i++) {
            out[c][i] = source[i] * trim;
        }
    }

    // WAS THERE ANYTHING IN IT? A cheap test with an early exit, because the answer is the one
    // thing the panel cannot work out for itself and the one thing a user needs told - see
    // hostInputPresent in gbStatus.h.
    bool audible = false;

    for (int c = 0; (c < GB_CHANNELS) && !audible; c++) {
        for (int32_t i = 0; i < frames; i++) {
            if ((out[c][i] > 1.0e-6f) || (out[c][i] < -1.0e-6f)) {
                audible = true;
                break;
            }
        }
    }

    if (audible) {
        self->hostSilentFrames = 0;
    } else {
        self->hostSilentFrames += (uint64_t)frames;
    }

    // THE MEASUREMENT IS UNCHANGED, AND THAT IS THE POINT. It reads the OUTPUT buffer, which now
    // holds the host's input, so it times exactly the same round trip - note out, synth, interface,
    // host, here - without knowing the capture path underneath it has gone.
    gb_run_measurement(self, out, frames, blockHostTime, 0.0);

    tGbStatus * status = gb_status(atomic_load(&self->statusSlot));

    if (status != NULL) {
        // NO RING, SO NO RING FIGURES. A published zero would read as a starved buffer rather than
        // an absent one; the panel greys these, and these are the values it greys.
        atomic_store(&status->fillFrames, 0.0);
        atomic_store(&status->actualSamples,
                     (int)((atomic_load(&self->offsetMs) / 1000.0) * self->hostRate));
        atomic_store(&status->measureTripNow, self->measureTrip);
        atomic_store(&status->measurePhase, (int)self->measureState);

        // TWO SECONDS, so a genuine gap between notes does not read as a broken routing while a
        // routing that never carries anything says so quickly enough to be useful.
        atomic_store(&status->hostInputPresent,
                     ((double)self->hostSilentFrames < (self->hostRate * 2.0)) ? 1 : 0);
    }
    gb_publish_status(self, out, frames, 0.0);
}

void gb_bridge_render(tGbBridge * self, float ** out, const float * const * in, int32_t inChannels,
                      int32_t frames, uint64_t blockHostTime) {
    // THE HOST-INPUT MODE TAKES NO LOCK AND NEEDS NONE. It touches no ring, no resampler and no
    // device - nothing the worker can be rebuilding underneath it - so the trylock below, and the
    // block of silence a failed trylock produces, would be cost for nothing.
    if (atomic_load(&self->captureSource) == GB_SOURCE_HOST) {
        gb_render_from_host(self, out, in, inChannels, frames, blockHostTime);
        return;
    }

    // notes §66
    if (pthread_mutex_trylock(&self->configLock) != 0) {
        gb_silence(out, frames);
        return;
    }

    if (!self->running) {
        gb_silence(out, frames);
        pthread_mutex_unlock(&self->configLock);
        return;
    }

    // Same start-up and recovery rule as the command line bridge: hold silence until there is
    // a setpoint's worth to snap to, then resync so the loop opens with zero error.
    if (atomic_load(&self->needResync)) {
        if (ring_fill(&self->ring) < (uint64_t)self->setpointFrames) {
            gb_silence(out, frames);
            pthread_mutex_unlock(&self->configLock);
            return;
        }

        ring_resync(&self->ring, (uint32_t)self->setpointFrames);
        resampler_reset(&self->resampler);
        drift_reset(&self->drift);
        atomic_store(&self->needResync, false);

        // The first one is the prime, not a fault. Anything after it means the loop lost the
        // buffer and had to be rescued, which is exactly what should never happen.
        if (self->primed) {
            atomic_fetch_add(&self->resyncs, 1);
        }

        self->primed = true;
    }

    gb_observe_block(self, frames);

    double fill       = (double)ring_fill(&self->ring);
    double interval   = (double)frames / self->hostRate;
    double correction = drift_update(&self->drift, fill, interval);
    double ratio      = self->nominalRatio * (1.0 + correction);

    uint32_t needed = resampler_needed(&self->resampler, (uint32_t)frames, ratio);

    if (needed > self->pullCapacity) {
        needed = self->pullCapacity;
    }

    if (needed > 0) {
        if (!ring_read(&self->ring, self->pullBuffer, needed)) {
            atomic_store(&self->needResync, true);
        }

        resampler_push(&self->resampler, self->pullBuffer, needed);
    }

    resampler_process(&self->resampler, self->interleaved, (uint32_t)frames, ratio);

    float trim = atomic_load(&self->trimGain);

    for (int32_t i = 0; i < frames; i++) {
        for (int c = 0; c < GB_CHANNELS; c++) {
            out[c][i] = self->interleaved[(i * GB_CHANNELS) + c] * trim;
        }
    }

    gb_run_measurement(self, out, frames, blockHostTime, fill);

    {
        tGbStatus * status = gb_status(self->statusSlot);

        if (status != NULL) {
            // notes §67
            double actual = gb_latency_frames_measured(self, fill, self->blockActualHostTime)
                            + (double)self->burstBefore;

            if (self->instrument) {
                actual += (atomic_load(&self->offsetMs) / 1000.0) * self->hostRate;
            }

            atomic_store(&status->actualSamples, (int)actual);

            // THE SAME NUMBER THE HOST WILL BE TOLD, averaged. Seeded rather than eased in from
            // zero: a fresh device would otherwise report a latency climbing out of nothing for
            // its first second, and every step of that is a compensation pass.
            double instant = actual - ((atomic_load(&self->offsetMs) / 1000.0) * self->hostRate);
            double prior   = atomic_load(&self->pipelineAvg);

            // notes §68
            double estimate = gb_latency_frames_for_fill(self, self->setpointFrames);
            double allow    = (double)((self->observedMaxFrames > 0) ? self->observedMaxFrames : self->hostMaxFrames)
                              + ((double)self->openDeviceFrames / ((self->nominalRatio > 0.0) ? self->nominalRatio : 1.0));

            if (instant > (estimate + allow)) {
                instant = estimate + allow;
            } else if (instant < (estimate - allow)) {
                instant = (estimate > allow) ? (estimate - allow) : estimate;
            }

            atomic_store(&self->pipelineAvg, (prior > 0.0)
                              ? (prior + ((instant - prior) * GB_PIPELINE_SMOOTH))
                              : instant);

            // notes §69
            atomic_store(&status->measureTripNow, self->measureTrip);
            atomic_store(&status->measurePhase, (int)self->measureState);
        }
    }
    gb_publish_status(self, out, frames, fill);

    pthread_mutex_unlock(&self->configLock);

    return;
}
