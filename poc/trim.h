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
// Notes: Docs/code-notes/trim.h.md - "// notes §k" refers there.

#ifndef TRIM_H
#define TRIM_H

#include <math.h>
#include <stdio.h>

// notes §1 - the output trim, in decibels across the whole control; its bottom is silence
#define TRIM_MIN_DB    (-48.0)
#define TRIM_MAX_DB    (12.0)

// Where 0 dB sits on the control
static inline double trim_unity(void) {
    return -TRIM_MIN_DB / (TRIM_MAX_DB - TRIM_MIN_DB);
}

static inline float trim_gain(double normalized) {
    double n = (normalized < 0.0) ? 0.0 : ((normalized > 1.0) ? 1.0 : normalized);

    if (n <= 0.0) {
        return 0.0f;
    }
    return (float)pow(10.0, (TRIM_MIN_DB + (n * (TRIM_MAX_DB - TRIM_MIN_DB))) / 20.0);
}

static inline double trim_normalized(double gain) {
    if (!(gain > 0.0)) {
        return 0.0;
    }
    double n = ((20.0 * log10(gain)) - TRIM_MIN_DB) / (TRIM_MAX_DB - TRIM_MIN_DB);

    return (n < 0.0) ? 0.0 : ((n > 1.0) ? 1.0 : n);
}

static inline void trim_text(double normalized, char * out, unsigned long len) {
    if (normalized <= 0.0) {
        snprintf(out, len, "-inf dB");
    } else {
        snprintf(out, len, "%+.1f dB", TRIM_MIN_DB + (normalized * (TRIM_MAX_DB - TRIM_MIN_DB)));
    }
}

#endif // TRIM_H
