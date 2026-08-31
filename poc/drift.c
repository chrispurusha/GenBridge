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
#include <string.h>

#include "drift.h"

#define PPM    (1.0e-6)

tDriftConfig drift_default_config(void) {
    tDriftConfig config;

    config.bandwidthHz      = 0.02;   // ~50 s natural period; see the header for why it is this low
    config.damping          = 1.0;    // critically damped - overshoot in a buffer level is an xrun
    config.maxCorrectionPpm = 500.0;  // real crystals are within ~200 ppm; the rest is recovery room

    return config;
}

void drift_init(tDrift * drift, const tDriftConfig * config, double inRate,
                double setpointFrames, double updateIntervalSeconds) {
    memset(drift, 0, sizeof(*drift));

    drift->config         = *config;
    drift->setpointFrames = setpointFrames;

    double omega = 2.0 * M_PI * config->bandwidthHz;

    // Kp and Ki fall straight out of the second order form in the header. Ki is per update rather
    // than per second, so the loop behaves the same whatever block size the device hands us.
    drift->kp = (2.0 * config->damping * omega) / inRate;
    drift->ki = ((omega * omega) / inRate) * updateIntervalSeconds;

    drift_reset(drift);
}

void drift_reset(tDrift * drift) {
    drift->integrator = 0.0;
    drift->correction = 0.0;
    drift->primed     = false;
}

double drift_update(tDrift * drift, double fillFrames) {
    double error = fillFrames - drift->setpointFrames;
    double limit = drift->config.maxCorrectionPpm * PPM;

    drift->integrator += drift->ki * error;

    // Anti-windup. Without this a long start-up transient - or a device that was stopped and
    // restarted - leaves the integrator holding a value it then takes minutes to unwind, during
    // which the ratio is pinned at the clamp and the buffer walks off in the other direction.
    if (drift->integrator > limit) {
        drift->integrator = limit;
    } else if (drift->integrator < -limit) {
        drift->integrator = -limit;
    }

    double correction = (drift->kp * error) + drift->integrator;

    if (correction > limit) {
        correction = limit;
    } else if (correction < -limit) {
        correction = -limit;
    }

    drift->correction = correction;
    drift->primed     = true;

    return correction;
}

double drift_measured_ppm(const tDrift * drift) {
    return drift->integrator / PPM;
}

double drift_correction_ppm(const tDrift * drift) {
    return drift->correction / PPM;
}
