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
// Notes: Docs/code-notes/resampler.h.md - "// notes §k" refers there.

#ifndef RESAMPLER_H
#define RESAMPLER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

// notes §1

// Overridable at build time so the self-test can sweep them; see do-poc.
#ifndef RESAMPLER_TAPS
#define RESAMPLER_TAPS      (64)    // filter length; group delay is half this, in input frames
#endif

#ifndef RESAMPLER_PHASES
#define RESAMPLER_PHASES    (512)   // fractional-delay resolution, linearly interpolated between
#endif

// notes §2
#ifndef RESAMPLER_GUARD
#define RESAMPLER_GUARD     (0.95)
#endif

typedef struct {
    float *  coef;            // (RESAMPLER_PHASES + 1) * RESAMPLER_TAPS, phase-major
    float *  buffer;          // interleaved input history + pending input
    uint32_t capacity;        // frames
    uint32_t count;           // valid frames in buffer
    uint32_t channels;
    double   pos;             // fractional read position, in frames, within buffer
    double   nominalRatio;    // input frames per output frame, before drift correction
    uint32_t overruns;        // push() beyond capacity; indicates a sizing bug, not a clock issue
} tResampler;

bool     resampler_init(tResampler * rs, uint32_t channels, double nominalRatio, uint32_t maxOutFrames);
void     resampler_free(tResampler * rs);
void     resampler_reset(tResampler * rs);

// How many further input frames must be pushed before outFrames can be produced at this ratio.
uint32_t resampler_needed(const tResampler * rs, uint32_t outFrames, double ratio);

void     resampler_push(tResampler * rs, const float * in, uint32_t frames);
void     resampler_process(tResampler * rs, float * out, uint32_t outFrames, double ratio);

// Constant group delay through the filter, in input frames. Part of the reported latency.
double   resampler_latency_frames(void);

#ifdef __cplusplus
}
#endif

#endif // RESAMPLER_H
