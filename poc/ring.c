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

#include <stdlib.h>
#include <string.h>

#include "ring.h"

bool ring_init(tRing * ring, uint32_t frames, uint32_t channels) {
    memset(ring, 0, sizeof(*ring));

    ring->buffer = (float *)calloc((size_t)frames * channels, sizeof(float));

    if (ring->buffer == NULL) {
        return false;
    }

    ring->frames   = frames;
    ring->channels = channels;

    atomic_store(&ring->writePos, 0);
    atomic_store(&ring->readPos, 0);

    return true;
}

void ring_free(tRing * ring) {
    free(ring->buffer);
    ring->buffer = NULL;
}

void ring_reset(tRing * ring) {
    memset(ring->buffer, 0, (size_t)ring->frames * ring->channels * sizeof(float));

    atomic_store(&ring->writePos, 0);
    atomic_store(&ring->readPos, 0);
}

uint64_t ring_fill(const tRing * ring) {
    // Load the read cursor first. If the producer runs between the two loads the fill is
    // under-reported rather than over-reported, which is the safe direction for both callers:
    // the producer sees less room than it has, the consumer sees less data than it has.
    uint64_t read  = atomic_load(&ring->readPos);
    uint64_t write = atomic_load(&ring->writePos);

    return (write >= read) ? (write - read) : 0;
}

// Copy 'frames' frames between an interleaved caller buffer and the ring, splitting at the wrap.
// 'toRing' selects the direction; everything else about the two cases is identical, and writing
// them separately is how the wrap arithmetic ends up correct in one copy and wrong in the other.
static void copy_wrapped(tRing * ring, uint64_t startFrame, float * external, uint32_t frames, bool toRing) {
    uint32_t channels = ring->channels;
    uint32_t pos      = (uint32_t)(startFrame % ring->frames);
    uint32_t done     = 0;

    while (done < frames) {
        uint32_t run = ring->frames - pos;

        if (run > frames - done) {
            run = frames - done;
        }

        float * ringAt     = ring->buffer + (size_t)pos * channels;
        float * externalAt = external + (size_t)done * channels;
        size_t  bytes      = (size_t)run * channels * sizeof(float);

        if (toRing) {
            memcpy(ringAt, externalAt, bytes);
        } else {
            memcpy(externalAt, ringAt, bytes);
        }

        done += run;
        pos   = (pos + run) % ring->frames;
    }
}

bool ring_write(tRing * ring, const float * src, uint32_t frames) {
    uint64_t write = atomic_load(&ring->writePos);
    uint64_t read  = atomic_load(&ring->readPos);

    // Room is capacity minus what the consumer has not taken yet. Refusing the whole block on
    // overflow rather than writing part of it keeps the cursor arithmetic exact: a partial write
    // would leave the fill depth lying about how much contiguous audio is really there.
    if ((write - read) + frames > ring->frames) {
        atomic_fetch_add(&ring->overflows, 1);
        return false;
    }

    copy_wrapped(ring, write, (float *)src, frames, true);

    // Publish only after the data is in place, or the consumer can read frames that are still
    // being written. Release pairs with the acquire in ring_read().
    atomic_store_explicit(&ring->writePos, write + frames, memory_order_release);

    return true;
}

bool ring_read(tRing * ring, float * dst, uint32_t frames) {
    uint64_t read  = atomic_load(&ring->readPos);
    uint64_t write = atomic_load_explicit(&ring->writePos, memory_order_acquire);

    if (write - read < frames) {
        // Underrun. Hand the device silence rather than stale audio, and do NOT advance the read
        // cursor - the drift loop then sees the fill depth it actually has and pulls the ratio to
        // refill, instead of chasing a cursor that has run past the data.
        memset(dst, 0, (size_t)frames * ring->channels * sizeof(float));
        atomic_fetch_add(&ring->underflows, 1);
        return false;
    }

    copy_wrapped(ring, read, dst, frames, false);

    atomic_store_explicit(&ring->readPos, read + frames, memory_order_release);

    return true;
}

void ring_resync(tRing * ring, uint32_t targetFrames) {
    uint64_t write = atomic_load_explicit(&ring->writePos, memory_order_acquire);
    uint64_t target = (uint64_t)targetFrames;

    // Never place the read cursor before the start of the stream, or ahead of the write cursor:
    // either would make the fill depth a huge number when the unsigned subtraction wraps.
    if (target > write) {
        target = write;
    }

    if (target > ring->frames) {
        target = ring->frames;
    }

    atomic_store_explicit(&ring->readPos, write - target, memory_order_release);
}
