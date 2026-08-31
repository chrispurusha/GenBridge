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

#ifndef DRIFT_H
#define DRIFT_H

#include <stdbool.h>
#include <stdint.h>

// The clock-drift compensator: a PI controller holding the ring's fill depth at a setpoint by
// trimming the resampler ratio.
//
// WHY THIS EXISTS AT ALL. Two audio devices have two crystals, and no crystal is exact. A device
// nominally at 48 kHz may really run at 48000.5 Hz. Bridge two of them and the ring fills or
// empties at the difference - about one frame every two seconds at 10 ppm - until it overflows or
// starves. That is not a bug to be fixed elsewhere; it is a property of having two clocks.
//
// CoreAudio's aggregate device does not fix it, and neither does JUCE's software equivalent.
// JUCE's AudioIODeviceCombiner detects the collision and calls xrun(): it invalidates both device
// sample times, drops the block and resynchronises. Correct, but the glitch returns on a fixed
// schedule forever. There is no drift compensation anywhere in JUCE.
//
// The fix is to stop treating the rate ratio as a constant. Resample by a ratio that is nudged by
// a few parts per million, continuously, so the fill depth stays put. The integrator term then
// converges on the true ratio between the two crystals - so this both cancels the drift and
// measures it, which is what makes the result checkable against an independent estimate.
//
// TUNING. Let e be the fill error in frames and c the fractional ratio correction. Raising the
// ratio consumes input faster, so:
//
//     de/dt = -inRate * c,    c = Kp*e + I,    dI/dt = Ki*e
//
// which is the standard second order system e'' + (inRate*Kp) e' + (inRate*Ki) e = 0, giving
//
//     Kp = 2*zeta*omega / inRate        Ki = omega^2 / inRate      (omega = 2*pi*bandwidth)
//
// Bandwidth is set low - a fraction of a hertz - for an audible reason. The correction is a pitch
// shift. A loop that chases the error quickly turns buffer jitter into wow and flutter, so it must
// be slow enough that its own output is inaudible: at 0.02 Hz the ratio takes tens of seconds to
// move, and a real crystal offset does not change faster than that anyway. Critically damped
// (zeta = 1) because overshoot in a buffer level is an xrun.

typedef struct {
    double bandwidthHz;      // loop bandwidth; lower is smoother and slower to settle
    double damping;          // 1.0 = critically damped
    double maxCorrectionPpm; // clamp on the total correction, and on the integrator (anti-windup)
} tDriftConfig;

typedef struct {
    tDriftConfig config;
    double       kp;             // per frame of error
    double       ki;             // per frame of error, per update
    double       integrator;     // converges on the true fractional clock offset
    double       correction;     // last total correction applied, fractional
    double       setpointFrames;
    bool         primed;
} tDrift;

tDriftConfig drift_default_config(void);

// updateIntervalSeconds is the period at which drift_update() will be called - one output block.
void         drift_init(tDrift * drift, const tDriftConfig * config, double inRate,
                        double setpointFrames, double updateIntervalSeconds);

void         drift_reset(tDrift * drift);

// Feed the measured fill depth; returns the fractional ratio correction to apply. Multiply the
// nominal ratio by (1.0 + correction).
double       drift_update(tDrift * drift, double fillFrames);

// The converged integrator, in ppm: this is the measured offset between the two clocks.
double       drift_measured_ppm(const tDrift * drift);

// The correction actually applied on the last update, in ppm.
double       drift_correction_ppm(const tDrift * drift);

#endif // DRIFT_H
