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

// Offline measurement of the resampler, with no hardware and no listening involved.
//
// The bridge's telemetry proves the buffer arithmetic and the control loop; it says nothing at all
// about what happens to a signal. A run with zero xruns and a rock-steady fill can still be
// destroying the audio. This puts a number on that, deterministically, so a change to the filter
// can be shown to be an improvement rather than assumed to be one.
//
// Method: a pure sine through the resampler at a given ratio, then a windowed FFT of the output.
// Everything that is not the fundamental is, by definition, something the resampler invented -
// imaging, aliasing, interpolation error or quantisation - so the ratio of that to the fundamental
// is the figure of merit. Reported as THD+N in dB; more negative is better.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "resampler.h"
#include "selftest.h"

#define FFT_SIZE      (65536)
#define BLOCK         (256)
#define SKIP_BLOCKS   (8)       // discard the filter's start-up transient before analysing

// A seven term Blackman-Harris window. The point of going this far is that its side lobes are
// below -150 dB: a lesser window's leakage would sit on top of whatever the resampler is doing and
// we would end up measuring the window instead of the filter.
static const double gWindow[7] = {
    0.27105140069342, 0.43329793923448, 0.21812299954311, 0.06592544638803,
    0.01081174209837, 0.00077658482522, 0.00001388721735
};

static void fft(double * re, double * im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;

        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }

        j ^= bit;

        if (i < j) {
            double tr = re[i]; re[i] = re[j]; re[j] = tr;
            double ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }

    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * M_PI / (double)len;

        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < (len / 2); k++) {
                double wr = cos(ang * k);
                double wi = sin(ang * k);
                double xr = re[i + k + (len / 2)];
                double xi = im[i + k + (len / 2)];
                double vr = (xr * wr) - (xi * wi);
                double vi = (xr * wi) + (xi * wr);
                double ur = re[i + k];
                double ui = im[i + k];

                re[i + k]             = ur + vr;
                im[i + k]             = ui + vi;
                re[i + k + (len / 2)] = ur - vr;
                im[i + k + (len / 2)] = ui - vi;
            }
        }
    }
}

// Run a sine of 'freqFraction' of the input rate through the resampler at 'ratio', and return
// THD+N in dB.
static double measure(double ratio, double freqFraction) {
    tResampler rs;

    if (!resampler_init(&rs, 1, ratio, BLOCK)) {
        return 0.0;
    }

    double * out = (double *)calloc(FFT_SIZE, sizeof(double));
    double * re  = (double *)calloc(FFT_SIZE, sizeof(double));
    double * im  = (double *)calloc(FFT_SIZE, sizeof(double));
    float    inBlock[BLOCK];
    float    outBlock[BLOCK];

    double phase    = 0.0;
    double step     = 2.0 * M_PI * freqFraction;
    int    produced = 0;
    int    blocks   = 0;

    while (produced < FFT_SIZE) {
        uint32_t needed = resampler_needed(&rs, BLOCK, ratio);

        while (needed > 0) {
            uint32_t take = (needed > BLOCK) ? BLOCK : needed;

            for (uint32_t i = 0; i < take; i++) {
                inBlock[i] = (float)(0.5 * sin(phase));
                phase     += step;

                if (phase > (2.0 * M_PI)) {
                    phase -= 2.0 * M_PI;
                }
            }

            resampler_push(&rs, inBlock, take);
            needed -= take;
        }

        resampler_process(&rs, outBlock, BLOCK, ratio);
        blocks++;

        if (blocks > SKIP_BLOCKS) {
            for (int i = 0; (i < BLOCK) && (produced < FFT_SIZE); i++) {
                out[produced++] = (double)outBlock[i];
            }
        }
    }

    for (int i = 0; i < FFT_SIZE; i++) {
        double w = 0.0;

        for (int k = 0; k < 7; k++) {
            w += ((k & 1) ? -1.0 : 1.0) * gWindow[k]
                 * cos((2.0 * M_PI * k * i) / (double)FFT_SIZE);
        }

        re[i] = out[i] * w;
        im[i] = 0.0;
    }

    fft(re, im, FFT_SIZE);

    int    half = FFT_SIZE / 2;
    int    peak = 16;
    double best = 0.0;

    for (int k = 16; k < half; k++) {
        double p = (re[k] * re[k]) + (im[k] * im[k]);

        if (p > best) {
            best = p;
            peak = k;
        }
    }

    // The window's main lobe spans several bins, so the fundamental has to be excised as a band
    // rather than a single bin, or its own skirts count as distortion.
    int    guard  = 14;
    double signal = 0.0;
    double noise  = 0.0;

    for (int k = 16; k < half; k++) {
        double p = (re[k] * re[k]) + (im[k] * im[k]);

        if (abs(k - peak) <= guard) {
            signal += p;
        } else {
            noise += p;
        }
    }

    free(out);
    free(re);
    free(im);
    resampler_free(&rs);

    if ((signal <= 0.0) || (noise <= 0.0)) {
        return -999.0;
    }

    return 10.0 * log10(noise / signal);
}

int self_test(void) {
    struct { const char * name; double ratio; } cases[] = {
        { "1:1        (48k -> 48k)",    1.0                   },
        { "up   0.919 (44.1k -> 48k)",  44100.0 / 48000.0     },
        { "down 1.088 (48k -> 44.1k)",  48000.0 / 44100.0     },
        { "up   0.5   (24k -> 48k)",    0.5                   },
        { "down 2.0   (96k -> 48k)",    2.0                   },
    };

    double freqs[] = { 0.01, 0.1, 0.25, 0.40, 0.45 };

    printf("\n  Resampler THD+N (dB), %d-point FFT. More negative is better.\n\n", FFT_SIZE);
    printf("  %-28s", "ratio");

    for (size_t f = 0; f < (sizeof(freqs) / sizeof(freqs[0])); f++) {
        printf(" %9.2ffs", freqs[f]);
    }

    printf("\n");

    for (size_t c = 0; c < (sizeof(cases) / sizeof(cases[0])); c++) {
        printf("  %-28s", cases[c].name);

        for (size_t f = 0; f < (sizeof(freqs) / sizeof(freqs[0])); f++) {
            // Above the OUTPUT Nyquist the signal is meant to be removed entirely, so "distortion
            // relative to the fundamental" is not a meaningful quantity - there should be no
            // fundamental. Reporting a number there measures alias rejection while looking like
            // THD, which is worse than reporting nothing.
            if (freqs[f] >= (0.5 / cases[c].ratio)) {
                printf(" %11s", "-");
            } else {
                printf(" %11.1f", measure(cases[c].ratio, freqs[f]));
            }
        }

        printf("\n");
    }

    printf("\n  fs = fraction of the INPUT sample rate. 0.45fs is near Nyquist, where a resampler\n");
    printf("  with no transition band images most audibly. '-' means the input is above the\n");
    printf("  output Nyquist, where the signal is meant to be removed and THD+N has no meaning.\n");
    printf("  taps %d, phases %d, guard %.2f\n\n", RESAMPLER_TAPS, RESAMPLER_PHASES, (double)RESAMPLER_GUARD);

    return 0;
}
