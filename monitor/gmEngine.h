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

#ifndef GM_ENGINE_H
#define GM_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GM_UID_LEN    (256)

typedef struct {
    char     inUid[GM_UID_LEN];
    uint32_t inFirst;        // first device channel, 0-based
    uint32_t inChannels;     // 1 mono, 2 stereo
    char     outUid[GM_UID_LEN];
    uint32_t outFirst;
    uint32_t outChannels;
    uint32_t frames;         // device buffer to ask for - honoured only where nothing else holds the device
    double   safetyMs;       // added to the smallest setpoint that clears both blocks
    double   trim;           // linear gain, 0..2
} tGmConfig;

typedef enum {
    eGmStopped = 0,
    eGmRunning,
    eGmWaiting,              // a chosen device is not present; starts by itself when it is
    eGmFailed,
} tGmState;

typedef struct {
    tGmState state;
    char     message[160];
    float    inPeak[2];
    float    outPeak[2];
    double   latencyMs;      // input to output, every term the devices and the bridge report
    double   bridgeMs;       // of which the ring and the resampler - the part the Safety and Buffer rows move
    double   fillFrames;
    double   setpointFrames;
    double   driftPpm;
    uint32_t underruns;
    uint32_t resyncs;
    uint32_t inFrames;       // the buffers actually in force
    uint32_t outFrames;
    bool     inShared;       // another client held the device, so its buffer was left alone
    bool     outShared;
    double   inRate;
    double   outRate;
} tGmStatus;

// All on the main thread.
void gm_engine_start(const tGmConfig * config);
void gm_engine_stop(void);
void gm_engine_set_trim(double trim);
// notes §1 - call a few times a second: follows devices going and coming back
void gm_engine_poll(void);
void gm_engine_status(tGmStatus * out);

#endif // GM_ENGINE_H
