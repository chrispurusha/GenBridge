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

#ifndef RESAMPLER_H
#define RESAMPLER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

// A windowed-sinc polyphase resampler whose ratio may change on every block.
//
// THE VARIABLE RATIO IS THE ENTIRE POINT, and it is what a fixed-ratio converter cannot do.
// Apple's AudioConverter and JUCE's ResamplingAudioSource both want a ratio that is constant, or
// at least rarely changed - JUCE rebuilds its low-pass whenever the ratio moves, behind a
// SpinLock, which is neither cheap nor real-time safe. Here the ratio is expected to wander by a
// few parts per million on every single callback, because that is how a clock drift is cancelled.
//
// This is why the anti-alias cutoff is fixed at construction from the NOMINAL ratio, not from the
// live one. The live ratio differs from nominal by well under 0.1%; moving a filter cutoff by that
// much would change nothing audible while forcing a table rebuild in the audio callback. So the
// table is built once, and the drift correction is carried purely by how fast the read position
// advances through it.
//
// libsoxr's variable-rate engine (soxr_set_io_ratio, the SOXR_VR flag) does the same job with a
// better filter, and is the intended replacement once it is vendored - see README. The arithmetic
// either side of it does not change, which is why this is behind an interface.

// Overridable at build time so the self-test can sweep them; see do-poc.
#ifndef RESAMPLER_TAPS
#define RESAMPLER_TAPS      (64)    // filter length; group delay is half this, in input frames
#endif

#ifndef RESAMPLER_PHASES
#define RESAMPLER_PHASES    (512)   // fractional-delay resolution, linearly interpolated between
#endif

// Passband edge as a fraction of the target Nyquist. A finite filter cannot go from passband to
// stopband instantly, so it needs somewhere to roll off, and giving it none is what leaves imaging
// sitting right below Nyquist.
//
// These two numbers were chosen by measurement, not taste - `genbridge --self-test` prints the
// table they came from. Upsampling THD+N at 0.45 fs, which is where the defect lives:
//
//      taps  guard   THD+N          note
//        32   1.00   -27 dB         no transition band at all: the original bug
//        32   0.95   -50 dB         32 taps cannot reach the stopband in 5% of guard
//        48   0.95   -90 dB
//        64   0.95   -95 dB         chosen
//       128   0.95   -103 dB        2 dB per doubling from here; not worth it
//
// 0.95 keeps the passband above 20 kHz either side of a 44.1/48 conversion, so nothing audible is
// given up in exchange. The cost of 64 taps is twice the multiply-accumulates of 32 and twice the
// group delay - 32 input frames, well under a millisecond - which matters only at high channel
// counts, where a 32-channel device would be doing about 98 M MAC/s.
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
