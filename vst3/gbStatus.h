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

#ifndef GB_STATUS_H
#define GB_STATUS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// Live figures the processor publishes and the editor reads.
//
// ONE OF THESE PER PLUG-IN INSTANCE, not one per process. It used to be a single global, and with
// two instances in a set the editors read whichever processor wrote last - so a panel showing the
// microphone reported that it was capturing a Kronos. Two answers, one of them a lie.
//
// The processor claims a slot on construction and tells its controller which one through
// IConnectionPoint, the channel VST3 provides for exactly this - the two are separate registered
// classes precisely so a host MAY keep them apart, and nothing else bridges them.
//
// Messages carry the slot number ONCE. The meters and drift figures are then read straight out of
// this shared structure, because a message per frame per instance would be a great deal of
// allocation on a UI timer for numbers that are only ever advisory.
//
// Everything here is written by the audio or worker thread and read by the UI thread, so it is all
// atomic and none of it is a pointer. The device name is the exception - a fixed buffer copied
// under no lock at all, on the grounds that the worst case is a torn string in a readout that
// refreshes thirty times a second.
typedef struct {
    atomic_bool     active;
    char            deviceName[128];

    // The saved device is named in the project but is not plugged in. Distinct from "nothing
    // selected": one is a plug-in waiting for hardware it has been told to use, the other is one
    // that has never been told anything, and answering the first with the second is what let a
    // missing USB interface fall through to whatever sat at slot 0 - a microphone.
    //
    // waitingName is written once, before the flag is raised, and read only while it is up, so it
    // needs no more protection than deviceName above.
    atomic_int      waitingForDevice;
    char            waitingName[128];
    atomic_int      deviceRate;
    atomic_int      deviceFrames;
    atomic_int      latencySamples;
    atomic_int      measuredSamples;    // the hardware round trip, 0 until measured

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
    _Atomic double  driftPpm;

    _Atomic float   peakLeft;
    _Atomic float   peakRight;
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
