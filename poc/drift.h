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
// Notes: Docs/code-notes/drift.h.md - "// notes §k" refers there.

#ifndef DRIFT_H
#define DRIFT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

// notes §1

typedef struct {
    double bandwidthHz;      // loop bandwidth; lower is smoother and slower to settle
    double damping;          // 1.0 = critically damped
    double maxCorrectionPpm; // clamp on the total correction, and on the integrator (anti-windup)
    double filterSeconds;    // time constant of the low-pass on the fill measurement
} tDriftConfig;

typedef struct {
    tDriftConfig config;
    double       kp;             // per frame of error
    double       kiPerSecond;    // per frame of error, per second - scaled by each block's duration
    double       integrator;     // converges on the true fractional clock offset
    double       correction;     // last total correction applied, fractional
    double       setpointFrames;
    double       fillFiltered;   // low-passed fill; negative means "not yet seeded"
    bool         primed;
} tDrift;

tDriftConfig drift_default_config(void);

void         drift_init(tDrift * drift, const tDriftConfig * config, double inRate,
                        double setpointFrames);

void         drift_reset(tDrift * drift);

// Move the setpoint without disturbing the integrator's estimate of the clock offset. The offset is
// a property of the two crystals and does not change because the buffer target did, so throwing it
// away would mean re-converging over tens of seconds for no reason.
void         drift_set_setpoint(tDrift * drift, double setpointFrames);

// notes §2
double       drift_update(tDrift * drift, double fillFrames, double intervalSeconds);

// The low-passed fill the controller actually acted on, as opposed to the raw instantaneous depth.
double       drift_filtered_fill(const tDrift * drift);

// The converged integrator, in ppm: this is the measured offset between the two clocks.
double       drift_measured_ppm(const tDrift * drift);

// The correction actually applied on the last update, in ppm.
double       drift_correction_ppm(const tDrift * drift);

#ifdef __cplusplus
}
#endif

#endif // DRIFT_H
