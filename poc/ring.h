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

#ifndef RING_H
#define RING_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

// A single-producer / single-consumer ring, written by one audio callback and read by another.
//
// THE CURSORS ARE ABSOLUTE FRAME COUNTS, NOT BUFFER INDICES. That is taken from JUCE's
// AudioIODeviceCombiner (juce_CoreAudio_mac.cpp), and it is the single most useful idea in that
// file. Two consequences follow that head/tail indices do not give:
//
//   - Overflow and underrun become plain arithmetic on two monotonic numbers, rather than the
//     usual "is the gap wrapped or not" case analysis that is so easy to get subtly wrong.
//   - The fill depth is meaningful before either side has ever run. There is no ambiguous
//     "empty or full?" state at start-up, which is exactly when the latency is not yet known.
//
// A uint64 frame count at 96 kHz wraps after about six million years, so wrap-around is not
// handled and does not need to be.
//
// JUCE's own comment (line 1870 there) explains why its AbstractFifo could not be used for this:
// a generic SPSC ring cannot recover from under/overflow lock-free without either overwriting or
// reading stale data. The same reasoning applies here, so recovery is explicit - see ring_read().

typedef struct {
    float *          buffer;       // interleaved, frames * channels
    uint32_t         frames;       // capacity in frames
    uint32_t         channels;
    _Atomic uint64_t writePos;     // absolute frames written by the producer
    _Atomic uint64_t readPos;      // absolute frames consumed by the consumer
    _Atomic uint32_t overflows;    // producer found no room
    _Atomic uint32_t underflows;   // consumer found too little data
} tRing;

bool     ring_init(tRing * ring, uint32_t frames, uint32_t channels);
void     ring_free(tRing * ring);
void     ring_reset(tRing * ring);

// Producer side. Returns false and counts an overflow if the write would overlap unread data,
// in which case nothing is written - dropping a block is better than tearing one.
bool     ring_write(tRing * ring, const float * src, uint32_t frames);

// Consumer side. Returns false and counts an underflow if fewer than 'frames' are available,
// in which case dst is zero-filled so the caller always has something to hand its device.
bool     ring_read(tRing * ring, float * dst, uint32_t frames);

// Frames written but not yet read. This is the quantity the drift loop steers.
uint64_t ring_fill(const tRing * ring);

// Force the fill depth to exactly 'targetFrames' by moving the READ cursor, discarding whatever
// is surplus. Consumer side only - it writes readPos, so calling it from the producer would put
// two threads on one cursor.
//
// This is how the bridge starts, and how it recovers. Waiting for the ring to fill to the
// setpoint sounds like the natural way to prime it, but it races the two devices: a device that
// is slow to deliver its first callback - a DisplayPort output takes about 160 ms - lets the
// other one run far past the setpoint first, handing the drift loop an opening error it then
// needs minutes to walk off at a few hundred ppm. Snapping the cursor makes the opening error
// zero by construction.
void     ring_resync(tRing * ring, uint32_t targetFrames);

#endif // RING_H
