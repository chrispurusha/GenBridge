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
// Notes: Docs/code-notes/gbStatus.h.md - "// notes §k" refers there.

#ifndef GB_STATUS_H
#define GB_STATUS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// notes §1
typedef struct {
    atomic_bool     active;
    char            deviceName[128];

    // notes §2
    atomic_int      waitingForDevice;
    char            waitingName[128];
    atomic_int      deviceRate;
    atomic_int      deviceFrames;
    atomic_int      latencySamples;
    atomic_int      measuredSamples;    // the hardware round trip, 0 until measured

    // The spread behind that average: the fastest and slowest round trip of the run, and how many
    // of them counted. An average with no range beside it cannot be told from a lucky single shot.
    atomic_int      measuredLow;
    atomic_int      measuredHigh;
    atomic_int      measuredTrips;

    // Live progress of a run in flight: which trip, and which phase of it. The panel can say
    // "trip 3 of 5" instead of going quiet for three seconds, and a run that stalls says where.
    atomic_int      measureTripNow;
    atomic_int      measurePhase;

    // Note events taken from the host, and note events that reached a MIDI destination. "Notes are
    // not getting through" has three quite different causes and they look identical from outside;
    // this says which half of the journey to look at.
    atomic_int      eventsIn;
    atomic_int      eventsOut;

    // notes §3
    atomic_int      actualSamples;

    // notes §4
    atomic_int      deviceShared;

    // The reported total, broken into what it is made of. All in HOST frames, so they add up.
    atomic_int      ringSamples;
    atomic_int      deviceSamples;
    atomic_int      filterSamples;
    atomic_int      offsetSamples;
    atomic_int      measureFailed;
    atomic_int      measureRanEmpty;   // ran, but the onset beat our own latency - see GB_MEASURE_TOO_EARLY
    atomic_int      offlineRender;     // host asked for faster-than-realtime processing - see setupProcessing      // last attempt could not produce a trustworthy figure
    atomic_int      underruns;
    atomic_int      resyncs;

    _Atomic double  fillFrames;
    _Atomic double  setpointFrames;

    // The setpoint the conservative floor would have chosen, whether or not that is what is in
    // force. Published so the panel can show a manual setting ALONGSIDE the recommendation instead
    // of the plug-in quietly overruling one with the other — see "THE FLOOR IS ADVICE" in gbBridge.c.
    _Atomic double  recommendedFrames;
    _Atomic double  driftPpm;

    _Atomic float   peakLeft;
    _Atomic float   peakRight;

    // notes §5
    atomic_int      hostInputPresent;
} tGbStatus;

#define GB_STATUS_SLOTS    (32)

// Claim and release a slot. A slot that cannot be had returns -1, and callers must cope: the panel
// then simply shows no live figures rather than someone else's.
int         gb_status_claim(void);
void        gb_status_release(int slot);

// NULL for an out-of-range or unclaimed slot.
tGbStatus * gb_status(int slot);

#ifdef __cplusplus
}
#endif

#endif // GB_STATUS_H
