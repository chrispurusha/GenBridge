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

#ifdef __cplusplus
extern "C" {
#endif

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
// THE FILL MEASUREMENT MUST BE FILTERED BEFORE THE CONTROLLER SEES IT, and this is not optional
// once the two rates differ. Input arrives in whole device blocks while output consumes a
// fractional number of them, on a different callback rate: at 44.1 kHz in and 48 kHz out, blocks
// of 256 arrive at 172.3 Hz and roughly 235 frames leave at 187.5 Hz. The instantaneous depth
// therefore sawtooths by up to a whole input block no matter how perfect the clocks are. That is
// quantisation, not drift, and it is large - at 128 frames the proportional term alone demands
// 730 ppm, which pins the correction to its clamp on nearly every update and makes the loop
// thrash. At a ratio of exactly 1.0 the callbacks happen to interleave evenly and the effect is
// invisible, which is how it survives a same-rate test.
//
// A one-pole low-pass fixes it for nothing: the noise sits at block rate, hundreds of hertz, while
// the loop bandwidth is hundredths of a hertz, so a time constant anywhere between the two removes
// the noise without touching the loop's own response.
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

// Feed the measured fill depth and how long this block covers; returns the fractional ratio
// correction to apply. Multiply the nominal ratio by (1.0 + correction).
//
// THE INTERVAL IS PASSED PER CALL, not fixed at init, because a DAW is free to hand the plug-in a
// different block size on any call - and it does, at the start of playback and around loop points.
// The integrator gain and the measurement filter are both per unit TIME; deriving them once from an
// assumed block size means the loop silently retunes itself whenever the host changes its mind.
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
