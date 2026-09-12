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
// Notes: Docs/code-notes/gbMeasure.c.md - "// notes §k" refers there.


// notes §1

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <CoreAudio/HostTime.h>

#include "gbBridgePrivate.h"
#include "synthlibLog.h"
#include "gbStatus.h"

// FILE-LOCAL, AND FORWARD-DECLARED. A class let its members call each other in
// any order; a C file does not, and these are the ones nothing outside this file
// needs to see.
static int gb_trip_result(tGbBridge * self);

// notes §2
void gb_start_measurement(tGbBridge * self) {
    // AUDIO, NOT A DEVICE. This tested self->running until 2026-09-09, which meant the same thing
    // until the host-input mode existed and then silently meant "a device is open" - so pressing
    // Measure in that mode returned here and did nothing at all, with no log line to say why.
    if (!self->instrument || !gb_capturing(self)) {
        return;
    }

    // notes §3
    atomic_store(&self->measurePanic, true);
    gb_wake_worker(self);

    self->measureUnderrunsAtStart = atomic_load(&self->ring.underflows);
    self->measureResyncsAtStart    = atomic_load(&self->resyncs);

    self->measureState       = eMeasureSettle;
    self->measureFrames      = 0;
    self->measurePeak        = 0.0f;
    self->measureFloor       = 0.0f;
    self->measureConfirm     = 0;
    self->measureOnsetFrames = 0;
    self->measureOnsetFill   = 0.0;
    self->measureOnsetOurs   = 0.0;
    self->measureNoteTime    = 0;
    self->measureTrip        = 0;

    for (int i = 0; i < GB_MEASURE_TRIPS; i++) {
        self->measureTrips[i] = 0;
    }
    atomic_store(&self->measureLatency, 0);
}

// notes §4
void gb_send_all_notes_off(tGbBridge * self) {
    int destination = atomic_load(&self->midiDestination);

    for (uint8_t channel = 0; channel < 16; channel++) {
        uint8_t note[3]  = { (uint8_t)(0x80 | channel), (uint8_t)atomic_load(&self->testNote), 0 };
        uint8_t panic[3] = { (uint8_t)(0xB0 | channel), 123, 0 };   // All Notes Off

        gb_midi_send(destination, note, 3);
        gb_midi_send(destination, panic, 3);
    }
}

void gb_run_measurement(tGbBridge * self, float ** out, int32_t frames, uint64_t blockHostTime, double fill) {
    if (self->measureState == eMeasureIdle) {
        return;
    }

    float peak = 0.0f;

    for (int32_t i = 0; i < frames; i++) {
        for (int c = 0; c < GB_CHANNELS; c++) {
            float magnitude = (out[c][i] < 0.0f) ? -out[c][i] : out[c][i];

            if (magnitude > peak) {
                peak = magnitude;
            }
        }
    }

    self->measureFrames += (uint32_t)frames;

    if (self->measureState == eMeasureSettle) {
        // notes §5
        bool quiet   = (peak < GB_MEASURE_MARGIN);
        bool settled = ((double)self->measureFrames >= (GB_MEASURE_SETTLE_S * self->hostRate)) && quiet;

        if (settled || ((double)self->measureFrames >= (GB_MEASURE_SETTLE_MAX_S * self->hostRate))) {
            self->measureState     = eMeasureFloor;
            self->measureFrames    = 0;
            self->measureFloorLate = false;
            self->measureFloor  = 0.0f;
        }

        return;
    }

    if (self->measureState == eMeasureFloor) {
        // notes §6
        if ((double)self->measureFrames >= (GB_MEASURE_FLOOR_S * self->hostRate * 0.667)) {
            if (!self->measureFloorLate) {
                self->measureFloorLate = true;
                self->measureFloor     = 0.0f;
            }
        }

        if (peak > self->measureFloor) {
            self->measureFloor = peak;
        }

        if ((double)self->measureFrames >= (GB_MEASURE_FLOOR_S * self->hostRate)) {
            uint8_t note[3] = { 0x90, (uint8_t)atomic_load(&self->testNote), 100 };

            // This trip's own clean-capture baseline. See where it is checked.
            self->measureTripUnderruns = atomic_load(&self->ring.underflows);
            self->measureTripResyncs   = atomic_load(&self->resyncs);

            // notes §7
            self->measureNoteTime = self->nextBlockHostTime;

            gb_midi_send_at(atomic_load(&self->midiDestination), note, 3, self->measureNoteTime);

            self->measureState  = eMeasureListening;
            self->measureFrames = 0;
        }

        return;
    }

    // Listening.
    float threshold = self->measureFloor * GB_MEASURE_RATIO;

    if (threshold < GB_MEASURE_MARGIN) {
        threshold = GB_MEASURE_MARGIN;
    }

    // notes §8
    if (threshold > GB_MEASURE_CEILING) {
        threshold = GB_MEASURE_CEILING;
    }

    if (peak > threshold) {
        self->measureConfirm++;

        // notes §9
        if (self->measureConfirm == 1) {
            int32_t cross = frames;

            for (int32_t i = 0; (i < frames) && (cross == frames); i++) {
                for (int c = 0; c < GB_CHANNELS; c++) {
                    float magnitude = (out[c][i] < 0.0f) ? -out[c][i] : out[c][i];

                    if (magnitude > threshold) {
                        cross = i;
                        break;
                    }
                }
            }

            double elapsed = gb_frames_between(self, self->measureNoteTime, self->blockActualHostTime)
                             + (double)cross;

            // notes §10
            self->measureOnsetFrames = (elapsed > 0.0) ? (uint32_t)elapsed : 0;
            self->measureOnsetFill   = fill;
            self->measureOnsetOurs   = gb_latency_frames_measured(self, fill, self->blockActualHostTime);
        }
    } else {
        self->measureConfirm = 0;
    }

    if (self->measureConfirm >= GB_MEASURE_CONFIRM) {
        uint8_t off[3] = { 0x80, (uint8_t)atomic_load(&self->testNote), 0 };

        gb_midi_send_at(atomic_load(&self->midiDestination), off, 3, blockHostTime);

        // Our own share is subtracted here, while it is unambiguously the share that was in
        // force during the measurement.
        uint32_t total = self->measureOnsetFrames;
        // notes §11
        double   lead    = gb_mean_callback_lead(self);

        // notes §12
        synthlib_log_line("measure: callback lead %.0f frames (host burst %u, this call %u)",
                    lead, self->observedMaxFrames, self->lastCallFrames);
        double   oursNow = ((self->measureOnsetOurs > 0.0) ? self->measureOnsetOurs
                           : gb_internal_latency_frames(self)) + lead;
        uint32_t ours    = (oursNow > 0.0) ? (uint32_t)oursNow : 0;

        // notes §13
        int      theirs = (total > ours) ? (int)(total - ours) : GB_MEASURE_TOO_EARLY;

        // notes §14
        bool clean = (atomic_load(&self->ring.underflows) == self->measureTripUnderruns)
                     && (atomic_load(&self->resyncs) == self->measureTripResyncs);

        self->measureTrips[self->measureTrip] = (clean && (theirs > 0)) ? theirs : GB_MEASURE_TOO_EARLY;
        self->measureTrip++;

        atomic_store(&self->measureOnset, (int)total);
        atomic_store(&self->measureOurs, (int)ours);
        atomic_store(&self->measureFillSeen, self->measureOnsetFill);
        atomic_store(&self->measureTriggerPeak, peak);
        atomic_store(&self->measureFloorSeen, self->measureFloor);

        if (self->measureTrip < GB_MEASURE_TRIPS) {
            // Round again. Back to settle, which is where the note just played gets time to
            // decay before the next floor is taken - a floor measured over a ringing note sets
            // a threshold the next one cannot cross.
            self->measureState     = eMeasureSettle;
            self->measureFrames    = 0;
            self->measureConfirm   = 0;
            self->measureFloor     = 0.0f;
            self->measureFloorLate = false;
            return;
        }

        atomic_store(&self->measureLatency, gb_trip_result(self));
        self->measureState = eMeasureIdle;
        atomic_store(&self->measureStore, true);
        gb_wake_worker(self);
        return;
    }

    if ((double)self->measureFrames >= (GB_MEASURE_TIMEOUT_S * self->hostRate)) {
        uint8_t off[3] = { 0x80, (uint8_t)atomic_load(&self->testNote), 0 };

        gb_midi_send_at(atomic_load(&self->midiDestination), off, 3, blockHostTime);

        // A TRIP THAT HEARD NOTHING ENDS THE RUN. Something is wrong with the destination, the
        // channel or the note, and four more silences take six more seconds to say so.
        atomic_store(&self->measureLatency, (self->measureTrip > 0) ? gb_trip_result(self) : GB_MEASURE_TIMED_OUT);
        self->measureState = eMeasureIdle;
        atomic_store(&self->measureStore, true);
        gb_wake_worker(self);
    }
}

// The trimmed mean of the trips that voted. Sorted in place - five elements, on the audio
// thread, and an insertion sort of five is nothing beside the block it sits in.
static int gb_trip_result(tGbBridge * self) {
    int valid[GB_MEASURE_TRIPS];
    int count = 0;

    for (int i = 0; i < self->measureTrip; i++) {
        if (self->measureTrips[i] > 0) {
            valid[count++] = self->measureTrips[i];
        }
    }

    if (count == 0) {
        return GB_MEASURE_TOO_EARLY;
    }

    for (int i = 1; i < count; i++) {
        int key = valid[i];
        int j   = i - 1;

        while ((j >= 0) && (valid[j] > key)) {
            valid[j + 1] = valid[j];
            j--;
        }
        valid[j + 1] = key;
    }

    // Trim only when there is enough left to be worth averaging. Three votes still give a
    // middle one; two would leave nothing after dropping both ends.
    int drop  = (count >= (2 * GB_MEASURE_DROP) + 1) ? GB_MEASURE_DROP : 0;
    int total = 0;
    int used  = 0;

    for (int i = drop; i < (count - drop); i++) {
        total += valid[i];
        used++;
    }

    atomic_store(&self->measureTripsUsed, used);
    atomic_store(&self->measureTripLow, valid[0]);
    atomic_store(&self->measureTripHigh, valid[count - 1]);
    atomic_store(&self->measureTripSpread, (count > 1) ? (valid[count - 1] - valid[0]) : 0);

    return (used > 0) ? (total / used) : GB_MEASURE_TOO_EARLY;
}

void gb_store_measurement(tGbBridge * self) {
    int result = atomic_load(&self->measureLatency);

    // notes §15
    unsigned underruns = (unsigned)(atomic_load(&self->ring.underflows) - self->measureUnderrunsAtStart);
    int      resynced  = atomic_load(&self->resyncs) - self->measureResyncsAtStart;

    // notes §16
    if ((result >= 0) && (atomic_load(&self->measureTripsUsed) <= 0)) {
        synthlib_log_line("measurement discarded: no trip completed over a clean capture "
                 "(%u underruns, %d resyncs during the run) - try again", underruns, resynced);

        tGbStatus * status = gb_status(self->statusSlot);

        if (status != NULL) {
            atomic_store(&status->measureFailed, 1);
        }

        return;
    }

    if (result < 0) {
        if (result == GB_MEASURE_TOO_EARLY) {
            synthlib_log_line("measurement: onset at %d frames but our own share is %u - the note cannot "
                     "have arrived before our buffering delivered it. Either the threshold was "
                     "crossed by something other than the test note, or the ring was not at its "
                     "setpoint. floor %.4f, triggered at %.4f",
                     atomic_load(&self->measureOnset), gb_internal_latency(self),
                     (double)atomic_load(&self->measureFloorSeen), (double)atomic_load(&self->measureTriggerPeak));
        } else {
            synthlib_log_line("measurement: nothing came back within %.1f s - is the synth on the channel "
                     "and audible?", GB_MEASURE_TIMEOUT_S);
        }

        tGbStatus * status = gb_status(self->statusSlot);

        if (status != NULL) {
            atomic_store(&status->measureFailed, (result == GB_MEASURE_TOO_EARLY) ? 0 : 1);
            atomic_store(&status->measureRanEmpty, (result == GB_MEASURE_TOO_EARLY) ? 1 : 0);
        }

        return;
    }

    char destination[GB_MIDI_NAME_LEN];

    gb_current_midi_name(self, destination, sizeof(destination));

    {
        tGbStatus * status = gb_status(self->statusSlot);

        if (status != NULL) {
            atomic_store(&status->measureRanEmpty, 0);
            atomic_store(&status->measureFailed, 0);
        }
    }

    // notes §17
    double measuredMs = (double)result / (self->hostRate / 1000.0);
    double seeded     = (measuredMs < GB_OFFSET_MIN_MS) ? GB_OFFSET_MIN_MS
                        : ((measuredMs > GB_OFFSET_MAX_MS) ? GB_OFFSET_MAX_MS : measuredMs);

    if (seeded != measuredMs) {
        synthlib_log_line("measured %.1f ms is outside the offset range %.0f..%.0f - clamped to %.1f",
                 measuredMs, GB_OFFSET_MIN_MS, GB_OFFSET_MAX_MS, seeded);
    }

    tMeasured * entry = gb_measured_for(self, gb_audio_key(self), destination, true);

    if (entry != NULL) {
        entry->hardwareSamples = (uint32_t)result;
        entry->offsetMs        = seeded;
    }

    atomic_store(&self->offsetMs, seeded);
    gb_remember_offset_pair(self, destination);

    // The panel reads the offset from the CONTROLLER, and the controller has no idea a
    // measurement just happened. Without this the readout stays at its old value until the next
    // click, and worse, the next +/- steps from the stale number and throws the reading away.
    gb_send_message(self, "gbOffset", (int)lround(seeded * 1000.0));

    pthread_mutex_lock(&self->configLock);
    self->hardwareSamples = (uint32_t)result;

    uint32_t nowLatency = gb_capturing(self) ? gb_report_latency(self) : 0;

    pthread_mutex_unlock(&self->configLock);

    // notes §18
    char trips[128];
    int  at = 0;

    trips[0] = '\0';

    for (int i = 0; (i < self->measureTrip) && (at < (int)sizeof(trips) - 12); i++) {
        at += snprintf(&trips[at], sizeof(trips) - (size_t)at, "%s%.1f",
                       (i == 0) ? "" : "/",
                       (self->measureTrips[i] > 0)
                       ? ((double)self->measureTrips[i] / (self->hostRate / 1000.0)) : -1.0);
    }

    synthlib_log_line("measured: %d of %d trips used [%s], range %.1f..%.1f ms, spread %d frames (%.1f ms). "
             "last onset %d, our "
             "share %d (ring %.0f of %.0f), hardware %d (%.1f ms). floor %.4f, triggered at "
             "%.4f, underruns %u resyncs %d during. '%s' -> '%s'",
             atomic_load(&self->measureTripsUsed), GB_MEASURE_TRIPS, trips,
             (double)atomic_load(&self->measureTripLow) / (self->hostRate / 1000.0),
             (double)atomic_load(&self->measureTripHigh) / (self->hostRate / 1000.0),
             atomic_load(&self->measureTripSpread),
             (double)atomic_load(&self->measureTripSpread) / (self->hostRate / 1000.0),
             atomic_load(&self->measureOnset), atomic_load(&self->measureOurs),
             atomic_load(&self->measureFillSeen), self->setpointFrames, result,
             (double)result / (self->hostRate / 1000.0),
             (double)atomic_load(&self->measureFloorSeen), (double)atomic_load(&self->measureTriggerPeak),
             (unsigned)(atomic_load(&self->ring.underflows) - self->measureUnderrunsAtStart),
             atomic_load(&self->resyncs) - self->measureResyncsAtStart,
             gb_audio_key(self), destination);

    gb_publish_measurement(self);

    if (nowLatency != self->reportedLatency) {
        self->reportedLatency = nowLatency;
        gb_send_latency_changed(self);
    }
}