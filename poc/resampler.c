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

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "resampler.h"

#define HALF_TAPS      (RESAMPLER_TAPS / 2)
#define KAISER_BETA    (9.0)    // ~ -70 dB side lobes, the usual choice for audio-rate conversion

static double bessel_i0(double x) {
    // Series expansion. Converges quickly for the arguments a Kaiser window uses, and this runs
    // once at init rather than per sample, so there is no reason to reach for anything cleverer.
    double sum  = 1.0;
    double term = 1.0;

    for (int i = 1; i < 40; i++) {
        term *= (x / (2.0 * i)) * (x / (2.0 * i));
        sum  += term;

        if (term < sum * 1e-16) {
            break;
        }
    }

    return sum;
}

static double sinc(double x) {
    if (fabs(x) < 1e-12) {
        return 1.0;
    }

    double px = M_PI * x;

    return sin(px) / px;
}

double resampler_latency_frames(void) {
    return (double)HALF_TAPS;
}

static void build_table(tResampler * rs) {
    // Cutoff as a fraction of the INPUT Nyquist. Upsampling needs no extra band limiting - the
    // input is already band limited to its own Nyquist. Downsampling does: the output Nyquist is
    // lower, so everything above it must go before the rate changes, or it folds back as aliasing.
    double cutoff = RESAMPLER_GUARD * ((rs->nominalRatio > 1.0) ? (1.0 / rs->nominalRatio) : 1.0);
    double i0Beta = bessel_i0(KAISER_BETA);

    for (int p = 0; p <= RESAMPLER_PHASES; p++) {
        double phase = (double)p / (double)RESAMPLER_PHASES;
        double sum   = 0.0;
        float * row  = rs->coef + (size_t)p * RESAMPLER_TAPS;

        for (int k = 0; k < RESAMPLER_TAPS; k++) {
            // Distance, in input samples, from the point being reconstructed to this tap's sample.
            double d = (double)(k - HALF_TAPS + 1) - phase;
            double t = d / (double)HALF_TAPS;

            if (t < -1.0) {
                t = -1.0;
            } else if (t > 1.0) {
                t = 1.0;
            }

            double window = bessel_i0(KAISER_BETA * sqrt(1.0 - (t * t))) / i0Beta;
            double h      = cutoff * sinc(cutoff * d) * window;

            row[k] = (float)h;
            sum   += h;
        }

        // Normalise each phase to unity DC gain. Without this the interpolated signal gains a
        // small level ripple that tracks the fractional position - a periodic amplitude wobble
        // whose rate is exactly the drift rate, which is precisely the artefact being chased away.
        if (fabs(sum) > 1e-12) {
            for (int k = 0; k < RESAMPLER_TAPS; k++) {
                row[k] = (float)(row[k] / sum);
            }
        }
    }
}

bool resampler_init(tResampler * rs, uint32_t channels, double nominalRatio, uint32_t maxOutFrames) {
    memset(rs, 0, sizeof(*rs));

    rs->channels     = channels;
    rs->nominalRatio = nominalRatio;

    // Enough for the worst block, the filter's history either side, and slack for the ratio
    // running above nominal while the loop pulls a deep buffer back down.
    double worstRatio = (nominalRatio > 1.0 ? nominalRatio : 1.0) * 1.05;

    rs->capacity = (uint32_t)((double)maxOutFrames * worstRatio) + (4 * RESAMPLER_TAPS) + 256;

    rs->coef   = (float *)calloc((size_t)(RESAMPLER_PHASES + 1) * RESAMPLER_TAPS, sizeof(float));
    rs->buffer = (float *)calloc((size_t)rs->capacity * channels, sizeof(float));

    if ((rs->coef == NULL) || (rs->buffer == NULL)) {
        resampler_free(rs);
        return false;
    }

    build_table(rs);
    resampler_reset(rs);

    return true;
}

void resampler_free(tResampler * rs) {
    free(rs->coef);
    free(rs->buffer);
    rs->coef   = NULL;
    rs->buffer = NULL;
}

void resampler_reset(tResampler * rs) {
    memset(rs->buffer, 0, (size_t)rs->capacity * rs->channels * sizeof(float));

    // Start with a full filter's worth of silence behind the read point, so the first output
    // sample has real taps to read rather than whatever the buffer happened to contain.
    rs->count = RESAMPLER_TAPS;
    rs->pos   = (double)(HALF_TAPS - 1);
}

uint32_t resampler_needed(const tResampler * rs, uint32_t outFrames, double ratio) {
    if (outFrames == 0) {
        return 0;
    }

    // The last output sample reads taps up to HALF_TAPS ahead of its own position, so the buffer
    // has to extend that far past it.
    double   lastPos  = rs->pos + ((double)(outFrames - 1) * ratio);
    uint32_t required = (uint32_t)floor(lastPos) + HALF_TAPS + 1;

    return (required > rs->count) ? (required - rs->count) : 0;
}

void resampler_push(tResampler * rs, const float * in, uint32_t frames) {
    if (rs->count + frames > rs->capacity) {
        // Sizing bug rather than a clock problem: resampler_needed() asked for more than the
        // buffer can hold. Drop it and count it so it shows up in the telemetry instead of
        // corrupting memory.
        rs->overruns++;
        return;
    }

    memcpy(rs->buffer + ((size_t)rs->count * rs->channels), in,
           (size_t)frames * rs->channels * sizeof(float));

    rs->count += frames;
}

void resampler_process(tResampler * rs, float * out, uint32_t outFrames, double ratio) {
    uint32_t channels = rs->channels;
    float    coefs[RESAMPLER_TAPS];

    for (uint32_t j = 0; j < outFrames; j++) {
        double t    = rs->pos + ((double)j * ratio);
        int32_t i   = (int32_t)floor(t);
        double frac = t - (double)i;

        // Interpolate between the two nearest phases of the table. Doing this once per output
        // frame and then reusing the taps across channels keeps the per-channel inner loop to a
        // plain multiply-accumulate.
        double   phase = frac * (double)RESAMPLER_PHASES;
        int32_t  p0    = (int32_t)phase;
        float    pf    = (float)(phase - (double)p0);

        if (p0 >= RESAMPLER_PHASES) {
            p0 = RESAMPLER_PHASES - 1;
            pf = 1.0f;
        }

        const float * row0 = rs->coef + ((size_t)p0 * RESAMPLER_TAPS);
        const float * row1 = row0 + RESAMPLER_TAPS;

        for (int k = 0; k < RESAMPLER_TAPS; k++) {
            coefs[k] = row0[k] + (pf * (row1[k] - row0[k]));
        }

        int32_t base = i - HALF_TAPS + 1;

        for (uint32_t ch = 0; ch < channels; ch++) {
            float acc = 0.0f;

            for (int k = 0; k < RESAMPLER_TAPS; k++) {
                acc += rs->buffer[((size_t)(base + k) * channels) + ch] * coefs[k];
            }

            out[((size_t)j * channels) + ch] = acc;
        }
    }

    rs->pos += (double)outFrames * ratio;

    // Compact: discard input the filter can no longer reach, keeping HALF_TAPS - 1 frames of
    // history behind the read point. The memmove is a few hundred frames per callback, which is
    // nothing beside the filter itself, and it keeps the buffer a plain linear array - the taps
    // can then be indexed directly with no wrap test in the innermost loop.
    int32_t drop = (int32_t)floor(rs->pos) - (HALF_TAPS - 1);

    if (drop > 0) {
        if ((uint32_t)drop > rs->count) {
            drop = (int32_t)rs->count;
        }

        memmove(rs->buffer, rs->buffer + ((size_t)drop * channels),
                (size_t)(rs->count - (uint32_t)drop) * channels * sizeof(float));

        rs->count -= (uint32_t)drop;
        rs->pos   -= (double)drop;
    }
}
