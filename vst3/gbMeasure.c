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


// MEASURING THE ROUND TRIP: play a note, time how long until anything comes back, and remember it.
//
// The whole point is that a DAW cannot compensate for a delay it does not know about. Without this,
// a part played through the instrument records roughly ninety milliseconds behind the beat on the
// rig this was built against, and no amount of buffer tuning touches it because most of it is the
// hardware.
//
// WHAT IS STORED IS THE HARDWARE'S SHARE, NOT THE TOTAL, and that distinction is what makes it
// correct under a host. The measured onset includes the plug-in's own path, which the host is
// ALREADY compensating for because getLatencySamples() reported it. Storing the total and then
// reporting it would count our part twice - and worse, the stored figure would silently go wrong the
// moment the ring was retuned. Subtracting our contribution at the moment of measurement leaves a
// number that is purely the synth and the wire, which stays true whatever the buffer does afterwards.
//
// IT CANNOT SEPARATE THE SYNTH FROM ITS PATCH. A slow pad crosses the threshold later than a piano,
// and nothing measuring from outside can tell the difference. Hence the manual offset: the
// measurement gets you within a few milliseconds and a person settles the rest.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <CoreAudio/HostTime.h>

#include "gbBridgePrivate.h"
#include "gbLog.h"
#include "gbStatus.h"

// FILE-LOCAL, AND FORWARD-DECLARED. A class let its members call each other in
// any order; a C file does not, and these are the ones nothing outside this file
// needs to see.
static int gb_trip_result(tGbBridge * self);

// ---- latency measurement -------------------------------------------------------------------
//
// Play a note, time how long until anything comes back, and remember it. The whole point is that
// a DAW cannot compensate for a delay it does not know about: without this, a part played
// through the instrument records roughly ninety milliseconds behind the beat on the rig this
// was built against, and no amount of buffer tuning touches it because most of it is the
// hardware.
//
// WHAT IS STORED IS THE HARDWARE'S SHARE, NOT THE TOTAL, and that distinction is what makes it
// correct under a host. The measured onset includes the plug-in's own path, which the host is
// ALREADY compensating for because getLatencySamples() reported it. Storing the total and then
// reporting it would count our part twice - and worse, the stored figure would silently go
// wrong the moment the ring was retuned. Subtracting our contribution at the moment of
// measurement leaves a number that is purely the synth and the wire, which stays true whatever
// the buffer does afterwards.
//
// IT CANNOT SEPARATE THE SYNTH FROM ITS PATCH. A slow pad crosses the threshold later than a
// piano, and nothing measuring from outside can tell the difference. Hence the manual offset:
// the measurement gets you within a few milliseconds and a person settles the rest.
void gb_start_measurement(tGbBridge * self) {
    if (!self->instrument || !self->running) {
        return;
    }

    // THE ALL-NOTES-OFF IS THE WORKER'S JOB, NOT THIS THREAD'S. It is 32 MIDISend calls - one
    // note-off and one All Notes Off on each of 16 channels - and every one of them is a mach
    // message to the MIDI server. Half a millisecond of a 2.7 ms block, spent on the audio
    // thread, at the one moment the ring must not be starved: it underran, the ring resynced,
    // and gb_store_measurement(self) then threw the run away for happening over a resync.
    //
    // The settle phase exists precisely to let things go quiet, and it is 350 ms long - orders
    // of magnitude more than the worker needs to get to this.
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

// WORKER THREAD. Everything hanging on every channel we might have used, silenced: our own
// test note is released explicitly and anything left by a previous attempt - or by playing -
// goes with it.
//
// Sent immediately rather than stamped, because this thread has no block timeline to stamp
// against. A played note scheduled up to one block ahead could in principle be overtaken by it,
// which is a stuck note; a panic pressed in the middle of playing is not a case worth carrying
// machinery for, and the note that follows would clear it.
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
        // QUIET, NOT MERELY ELAPSED - and it is what made four trips out of five disappear.
        //
        // Between trips this phase is waiting for the note just played to DECAY, and 0.35 s is
        // nowhere near enough for a piano or a pad. The floor was then taken over a still
        // ringing note, the threshold came out eight times too high, the next note could not
        // cross it, and the trip timed out - which ends the run. Every measurement rested on
        // one reading and the averaging was decoration.
        //
        // So: the settle ends when the input has actually gone quiet, with the old duration as
        // a MINIMUM and a hard ceiling so a noisy input cannot hang the run.
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
        // Establish what silence looks like on this input before deciding what a note looks
        // like. A noisy preamp would otherwise register an onset immediately.
        //
        // THE TRAILING PEAK, NOT THE PEAK OVER THE WHOLE WINDOW. A preset with reverb on it is
        // still decaying through this phase, so a peak held from the start of the window is a
        // measurement of the tail rather than of the floor - and the threshold is eight times
        // whatever this says. Too high a threshold does not fail loudly; it fires LATE into the
        // attack, which reads as a slower synth, which the host then over-compensates for, and
        // the take records early. A Kronos on a reverbed preset came out 5 ms adrift of an
        // Analog Rytm on a dry kick, both reporting the same figure.
        //
        // Restarting the hold two thirds of the way through leaves the last third - the part
        // nearest the note, and the quietest part of any decay - as the answer.
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

            // AT THE START OF THE NEXT BLOCK, which is the one instant here that is certain to
            // be in the FUTURE - nextBlockHostTime is this block's start plus its frames, and
            // the model never hands back a start earlier than the clock. CoreMIDI therefore
            // holds the packet and releases it exactly then, so the moment the note left is a
            // number this code knows rather than one it hopes for.
            //
            // Stamping it at the block start instead put it in the past by however long the
            // block had taken to compute - gb_run_measurement(self) runs after the audio is rendered -
            // so it went out at once, at an instant nothing recorded. Tens of microseconds on
            // its own, but it was the reference EVERY later arithmetic was measured from.
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

    // A very loud input would otherwise set a threshold no note could reach, and the
    // measurement would time out reporting "nothing came back" when the truth is "this input is
    // too noisy to measure". The ceiling turns that into a detection that at least tries, and
    // the logged floor tells the story afterwards.
    if (threshold > GB_MEASURE_CEILING) {
        threshold = GB_MEASURE_CEILING;
    }

    if (peak > threshold) {
        self->measureConfirm++;

        // The FIRST block that crossed is the onset; the confirmation only decides whether to
        // believe it. Counting from the confirming block instead would add its duration to
        // every measurement.
        //
        // TIMED ON THE CLOCK AT BOTH ENDS, not counted in blocks - and this is what stopped
        // the figure jumping about from run to run.
        //
        // Counting elapsed FRAMES assumes blocks arrive evenly spaced, and under a host that
        // hands over four of them per audio callback they do not: they arrive in a burst and
        // then nothing for the rest of the cycle. So the answer depended on WHICH call of the
        // callback the note happened to be detected in - 0, 128, 256 or 384 frames of pure
        // artefact at a 512-frame cycle, which is up to 8 ms of jump between two runs measuring
        // the same unchanged synth.
        //
        // Two real instants remove it: the note left at measureNoteTime, and this block's ring
        // read happened at blockActualHostTime. The RAW clock, deliberately - the model is
        // ahead of it inside a burst, and the ring holds only what physically arrived.
        //
        // THE SAMPLE, NOT THE BLOCK, for the last part of it: which sample of this block first
        // crossed says how far into it the sound began - 0 to a full buffer, half of one on
        // average, and always SHORT if discarded. One extra pass over the block that crossed,
        // and only that one.
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

            // OUR SHARE IS WORKED OUT HERE, AT THE ONSET, not two blocks later when the
            // confirmation arrives. It is built from three things read at one instant - the
            // fill, the clock, and when the last capture callback landed - and by the time the
            // confirmation comes in, the device has usually written again: lastWriteHostTime is
            // then AFTER the onset block, the "how long ago" term collapses to zero, and the
            // share comes out a whole device buffer short. It read as an 18 ms synth.
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
        // gb_internal_latency(self), NOT gb_report_latency(self) - see the comment on that pair. Netting off
        // the reported figure would subtract the correction already in force as well as our own
        // buffering, so each run would return less than the last.
        //
        // BUT FROM THE OCCUPANCY THE ONSET ACTUALLY CAME THROUGH, not from the setpoint. The
        // ring only sits AT its setpoint on average: a host that hands over four blocks per
        // audio callback drains it in a burst, so the fill at the fourth is a whole cycle below
        // the fill at the first, and which of them a note happened to land in moved the answer
        // by that much. That is where the run-to-run scatter came from. The setpoint remains
        // what the HOST is told, because that is the long-run figure; the measurement nets off
        // the real one, and the difference between the two is exactly the momentary deviation
        // it should not be reporting as hardware.
        // THE MEAN CALLBACK LEAD, ADDED AS A CONSTANT - which is the difference between the two
        // failed attempts at this and the right answer.
        //
        // The round trip is invariant to WHICH call of a callback detects the onset, because
        // the position in the ring and that call's fill move together and cancel. That is why
        // adding the per-block burstBefore made it worse: it uncancelled them and put the
        // block's own 0..384 frames of variance straight into the answer.
        //
        // But the AVERAGE of that term is not zero, and it is not in the subtraction at all -
        // so every measurement carried it as a constant bias. At a 512 frame callback delivered
        // in 128s that is (512 - 128) / 2 = 192 frames, 4 ms, and both a KRONOS and an Analog
        // Rytm recorded 4 ms early on exactly that. A constant has no variance, so this is the
        // one form of the term that removes the bias without reintroducing the scatter.
        double   lead    = gb_mean_callback_lead(self);
        double   oursNow = ((self->measureOnsetOurs > 0.0) ? self->measureOnsetOurs
                           : gb_internal_latency_frames(self)) + lead;
        uint32_t ours    = (oursNow > 0.0) ? (uint32_t)oursNow : 0;

        // A ROUND TRIP CANNOT BE FASTER THAN OUR OWN PIPELINE. If it looks like it was, the
        // onset is not the note - a threshold crossing on something else, or a ring that was
        // not at its setpoint when the arithmetic assumed it was. Reported rather than clamped
        // to zero: a silent 0 looked exactly like a device that had never been measured.
        int      theirs = (total > ours) ? (int)(total - ours) : GB_MEASURE_TOO_EARLY;

        // THE TRIP IS ONLY KEPT IF THE CAPTURE WAS CLEAN THROUGH IT. This used to be judged
        // over the whole run and thrown away wholesale - one resync anywhere and every note was
        // discarded, which on a rig that resyncs at all means no measurement is ever possible.
        // Per trip, a disturbed one simply does not vote.
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

    // A RESYNC OR AN UNDERRUN DURING THE MEASUREMENT INVALIDATES IT. Either one means the ring
    // was snapped or starved while we were counting, so the onset moved by however long the
    // disturbance lasted - and the resulting figure is a measurement of the glitch, not of the
    // hardware. Better to say so and let it be repeated than to store a number that looks
    // authoritative and is not.
    unsigned underruns = (unsigned)(atomic_load(&self->ring.underflows) - self->measureUnderrunsAtStart);
    int      resynced  = atomic_load(&self->resyncs) - self->measureResyncsAtStart;

    // JUDGED PER TRIP NOW, not over the whole run. A resync anywhere used to throw away every
    // note in the run, so on a rig that resyncs at all no measurement was ever possible - and
    // the resync was often caused by the act of measuring. A disturbed trip simply does not
    // vote; the run is only refused when nothing clean survived.
    if ((result >= 0) && (atomic_load(&self->measureTripsUsed) <= 0)) {
        gb_log_line("measurement discarded: no trip completed over a clean capture "
                 "(%u underruns, %d resyncs during the run) - try again", underruns, resynced);

        tGbStatus * status = gb_status(self->statusSlot);

        if (status != NULL) {
            atomic_store(&status->measureFailed, 1);
        }

        return;
    }

    if (result < 0) {
        if (result == GB_MEASURE_TOO_EARLY) {
            gb_log_line("measurement: onset at %d frames but our own share is %u - the note cannot "
                     "have arrived before our buffering delivered it. Either the threshold was "
                     "crossed by something other than the test note, or the ring was not at its "
                     "setpoint. floor %.4f, triggered at %.4f",
                     atomic_load(&self->measureOnset), gb_internal_latency(self),
                     (double)atomic_load(&self->measureFloorSeen), (double)atomic_load(&self->measureTriggerPeak));
        } else {
            gb_log_line("measurement: nothing came back within %.1f s - is the synth on the channel "
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

    // THE MEASUREMENT LANDS IN THE CORRECTION, which is the whole point of the arrangement: the
    // figure Measure produces is the one the panel then lets you nudge, rather than a number
    // sitting next to a separate trim that starts at zero.
    //
    // Clamped, because offsetMs is a normalised VST3 parameter with a fixed range and a reading
    // outside it cannot be represented. Logged when that bites - a silently clamped round trip
    // would put every take in the wrong place with nothing on screen to say why.
    double measuredMs = (double)result / (self->hostRate / 1000.0);
    double seeded     = (measuredMs < GB_OFFSET_MIN_MS) ? GB_OFFSET_MIN_MS
                        : ((measuredMs > GB_OFFSET_MAX_MS) ? GB_OFFSET_MAX_MS : measuredMs);

    if (seeded != measuredMs) {
        gb_log_line("measured %.1f ms is outside the offset range %.0f..%.0f - clamped to %.1f",
                 measuredMs, GB_OFFSET_MIN_MS, GB_OFFSET_MAX_MS, seeded);
    }

    tMeasured * entry = gb_measured_for(self, self->deviceSelector, destination, true);

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

    uint32_t nowLatency = self->running ? gb_report_latency(self) : 0;

    pthread_mutex_unlock(&self->configLock);

    // Underruns during the measurement matter: a gap in the capture delays the onset by
    // however long the gap was, so a figure taken while the ring was starving is not a
    // measurement of the hardware at all.
    // EVERY TRIP, NOT JUST THE SUMMARY. An average and a range still hide the shape: five
    // readings clustered with one wild outlier, and five spread evenly, produce the same two
    // numbers and mean quite different things. Safe to read here - the run is idle by the time
    // the worker gets to this, so the array is not being written.
    char trips[128];
    int  at = 0;

    trips[0] = '\0';

    for (int i = 0; (i < self->measureTrip) && (at < (int)sizeof(trips) - 12); i++) {
        at += snprintf(&trips[at], sizeof(trips) - (size_t)at, "%s%.1f",
                       (i == 0) ? "" : "/",
                       (self->measureTrips[i] > 0)
                       ? ((double)self->measureTrips[i] / (self->hostRate / 1000.0)) : -1.0);
    }

    gb_log_line("measured: %d of %d trips used [%s], range %.1f..%.1f ms, spread %d frames (%.1f ms). "
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
             self->deviceSelector, destination);

    gb_publish_measurement(self);

    if (nowLatency != self->reportedLatency) {
        self->reportedLatency = nowLatency;
        gb_send_latency_changed(self);
    }
}