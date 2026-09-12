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


// THE BRIDGE ITSELF: a CoreAudio device on one clock, a DAW on another, and a ring, a resampler and
// a drift loop between them. What used to be a 5,000 line C++ class is this file, gbMeasure.c and
// gbState.c - plain C, with the VST3 wrapper reduced to the COM plumbing it has to be.
//
// The substitution the whole plug-in rests on is smaller than it looks: process() consumes blocks on
// a clock that is not the capture device's, which is exactly what the proof of concept's output
// IOProc did. Everything underneath - SynthLib/audio/ring.c, poc/drift.c, poc/resampler.c - is the
// same code, unchanged and shared with the command line tool.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <CoreAudio/HostTime.h>

#include "gbBridgePrivate.h"
#include "gbDraw.h"
#include "synthlibLog.h"
#include "gbStatus.h"

// FILE-LOCAL, AND FORWARD-DECLARED. A class let its members call each other in
// any order; a C file does not, and these are the ones nothing outside this file
// needs to see.
static uint64_t gb_block_host_time(tGbBridge * self, int32_t frames);
static void gb_capture_callback(void * user, const float * input, float * output, uint32_t frames);
static uint8_t gb_channel_for(tGbBridge * self, int16_t incoming);
static void gb_close_capture(tGbBridge * self);
static void gb_close_capture_locked(tGbBridge * self);
static void gb_device_list_changed(void * user);
static uint64_t gb_host_time_for(tGbBridge * self, uint64_t blockHostTime, int32_t sampleOffset);
static double gb_latency_frames_for_fill(tGbBridge * self, double fillFrames);
static double gb_minimum_setpoint(tGbBridge * self, uint32_t deviceFrames);
static double gb_minimum_setpoint_for(tGbBridge * self, uint32_t hostFrames, uint32_t deviceFrames);
static double gb_minimum_setpoint_for_host(tGbBridge * self, uint32_t hostFrames);
static double gb_now_ms(void);
static void gb_observe_block(tGbBridge * self, int32_t frames);
static void gb_observe_burst(tGbBridge * self, int32_t frames);
static bool gb_open_capture_locked(tGbBridge * self, const tDeviceInfo * info);
static void gb_publish_config_snapshot(tGbBridge * self);
static void gb_publish_latency_breakdown(tGbBridge * self);
static void gb_publish_status(tGbBridge * self, float ** out, int32_t frames, double fill);
static void gb_publish_waiting(tGbBridge * self, bool waiting, const char * name);
static bool gb_resolve_slot(const tDeviceInfo * list, uint32_t count, int slot, tDeviceInfo * out);
static void gb_retune(tGbBridge * self);
static void gb_revert_retune(tGbBridge * self);
static void gb_send_controller(tGbBridge * self, uint32_t id, double value, uint64_t hostTime);
static void gb_send_slot(tGbBridge * self);
static void gb_silence(float ** out, int32_t frames);
static uint32_t gb_snapshot_latency(tGbBridge * self);
static double gb_snapshot_latency_frames(tGbBridge * self);
static void gb_start_worker(tGbBridge * self);
static void gb_stop_worker(tGbBridge * self);
static void gb_worker_loop(tGbBridge * self);

// The worker's own entry point. Declared here because start_worker() hands its address to
// pthread_create() before the function itself appears; a class had no such ordering to mind.
static void * gb_worker_entry(void * arg);

// From a CoreAudio thread: drop the cached list and wake the worker, nothing more.
static void gb_device_list_changed(void * user) {
    tGbBridge * self = (tGbBridge *)user;

    gb_device_list_invalidate();
    gb_request_device(self);
}

// HOW LONG THE HOST'S OWN THREAD WAITED FOR configLock, said out loud when it is long enough to
// see. getState(), setState() and setupProcessing() all take that lock, and the worker holds it
// across an entire device close and open - so a reconfigure can block whichever thread the host
// called on. In Ableton that thread is the main one, and it calls getState() for undo snapshots
// on ordinary UI actions: the beachball CT reports.
//
// This turns "occasional or regular spinning cursor" into a number with a cause beside it. If
// these lines are absent while the cursor spins, the cause is somewhere else entirely and this
// has ruled out the obvious suspect - which is worth as much.
void gb_lock_config_from_host(tGbBridge * self, const char * who) {
    double began = gb_now_ms();

    pthread_mutex_lock(&self->configLock);

    double waited = gb_now_ms() - began;

    if (waited >= 20.0) {
        synthlib_log_line("HOST THREAD BLOCKED: %s waited %.0f ms for configLock - a device open was in "
                 "flight. This is what a spinning cursor looks like from in here", who, waited);
    }
}

// WHAT THE PLUG-IN ITSELF ADDS, with no hardware correction in it. Split out of
// gb_report_latency(self) because the measurement has to subtract our share from the onset it sees,
// and subtracting the REPORTED figure meant subtracting the previous measurement along with it:
// every re-measure came back short by whatever correction was already in force, so the value
// walked towards zero the more times it was run. Only ever grows out of the ring and the
// converters, so it is the honest thing to net off.
// Call with configLock HELD, after any change to the four fields below. One place, so a new
// writer cannot forget half of them.
static void gb_publish_config_snapshot(tGbBridge * self) {
    atomic_store(&self->snapHostRate, self->hostRate);
    atomic_store(&self->snapRatio, self->nominalRatio);
    atomic_store(&self->snapSetpoint, self->setpointFrames);
    atomic_store(&self->snapDeviceLatency, self->deviceLatency);
}

// The snapshot's version of gb_internal_latency_frames(self). Safe from any thread; never touches a
// field the worker can be rewriting.
static double gb_snapshot_latency_frames(tGbBridge * self) {
    // THE SETPOINT, NOT THE MEASUREMENT - reverted 2026-09-08, and the reason is worth keeping.
    //
    // Reporting a smoothed measurement removed a 4-9 ms over-compensation and created something
    // far worse: the figure MOVES, every move past the deadband tells the host its latency
    // changed, and a host answers that by reactivating the plug-in. Reactivation closes and
    // reopens the device, which perturbs the ring, which moves the figure again. Ableton sat in
    // that loop - reconfigure every 1.5 seconds, each holding configLock for 1460 ms, the
    // device flapping between 64 and its restored 512 - which is both the beachball and the
    // "I set 64 and get 512" in one.
    //
    // A latency a host is told must be STABLE. pipelineAvg is still measured and still published
    // as "actual" beside this, so the gap that started this can be seen and dealt with by
    // shrinking it rather than by chasing it.
    double ratio = atomic_load(&self->snapRatio);

    if (ratio <= 0.0) {
        return 0.0;    // mid-swap, or nothing open: no latency to report rather than inf
    }

    // Before a block has run there is nothing measured yet, so the setpoint is the estimate.
    return (atomic_load(&self->snapSetpoint) + resampler_latency_frames() + (double)atomic_load(&self->snapDeviceLatency))
           / ratio;
}

static uint32_t gb_snapshot_latency(tGbBridge * self) {
    double total = gb_snapshot_latency_frames(self);

    if (self->instrument) {
        total += (atomic_load(&self->offsetMs) / 1000.0) * atomic_load(&self->snapHostRate);
    }

    return (total > 0.0) ? (uint32_t)total : 0;
}

// The pipeline's delay for a given ring occupancy. A sample read out of a ring holding `fill`
// frames entered it `fill` frames ago, so the occupancy IS the delay - the rest is what the
// resampler and the device's own converters add.
static double gb_latency_frames_for_fill(tGbBridge * self, double fillFrames) {
    double inputFrames = fillFrames + resampler_latency_frames() + (double)self->deviceLatency;

    if (self->nominalRatio <= 0.0) {
        return 0.0;
    }

    // Reported in the HOST's frames, and the ring is measured in the device's.
    return inputFrames / self->nominalRatio;
}

double gb_internal_latency_frames(tGbBridge * self) {
    return gb_latency_frames_for_fill(self, self->setpointFrames);
}

// THE PIPELINE'S ACTUAL DELAY AT THIS INSTANT, and it takes TWO numbers, not one.
//
// Occupancy on its own is not an age. The device writes a whole buffer at a time, so the fill
// jumps up by deviceFrames at each capture callback and falls as the host drains it - while the
// audio already in the ring is getting older at exactly the same rate. The two move in
// ANTI-PHASE and their sum is constant; take the fill alone and what is left is a sawtooth of a
// full device buffer. At 512 frames that is 10.7 ms, which is precisely the range a repeated
// measurement was wandering over.
//
// So: how long ago the newest frame arrived, plus how many sit in front of it. deviceLatency
// has the device's buffer inside it (device_latency_frames() sums bufferFrames + latency +
// safety offset + stream latency), and that buffer term is what "how long ago" now measures
// directly - counting it twice would put a whole buffer back on.
//
// The setpoint, NOT this, is what the host is told: the loop holds the AVERAGE fill there by
// design, and a latency that moved with every block would have the host redo its delay
// compensation continuously. This is for the measurement, which needs the instant it happened.
double gb_latency_frames_measured(tGbBridge * self, double fillFrames, uint64_t at) {
    if (self->nominalRatio <= 0.0) {
        return 0.0;
    }

    double buffered = (double)self->deviceLatency - (double)self->openDeviceFrames;

    if (buffered < 0.0) {
        buffered = 0.0;
    }

    double inRing = fillFrames + buffered + resampler_latency_frames();

    // NO CALLBACK-LEAD TERM HERE, and it was tried. The frames already handed over inside a
    // callback look like they belong - the fill at the fourth of four calls is 384 frames below
    // the fill at the first - but the ROUND TRIP this figure is subtracted from is invariant to
    // which call detects the onset, and adding the term destroys that.
    //
    // The reason is worth writing down. If the onset sits at position p in the ring, the call
    // that reads it finds it at cross = p - (frames already read), and that call's fill is
    // lower by exactly the same amount. The two cancel: whichever call detects it, the answer
    // is the same. Adding the lead uncancels them, and a measurement that had settled to 0.2 ms
    // across runs went back to a 7.3 ms spread - the very lottery it was meant to remove.
    //
    // The lead DOES belong in the delay from capture to where the host finally places the
    // audio, which is a different quantity - see the note on gb_internal_latency_frames(self) and
    // findings 2026-09-08 (5).
    // HOW LONG AGO THE LAST CAPTURE CALLBACK LANDED - CLAMPED, and it needs to be.
    //
    // Before the first callback the timestamp is 0, and the gap from 0 is the machine's entire
    // uptime; after the device stops delivering it grows without limit. Either way the figure
    // stops meaning "how far into the current device period we are" and starts poisoning the
    // reported latency - it put 151 ms on the panel and failed two checks that had passed for
    // weeks. In normal running this term never exceeds one device period, so anything past a
    // few of them is not a measurement, it is a device that has gone quiet.
    double sinceWrite = gb_frames_between(self, atomic_load(&self->lastWriteHostTime), at);
    double sanest     = ((double)self->openDeviceFrames * 4.0) / self->nominalRatio;

    if ((atomic_load(&self->lastWriteHostTime) == 0) || (sinceWrite > sanest)) {
        sinceWrite = 0.0;
    }

    return sinceWrite + (inRing / self->nominalRatio);
}

uint32_t gb_internal_latency(tGbBridge * self) {
    double total = gb_internal_latency_frames(self);

    return (total > 0.0) ? (uint32_t)total : 0;
}

uint32_t gb_report_latency(tGbBridge * self) {
    double total = gb_internal_latency_frames(self);

    // THE HARDWARE'S SHARE IS ADDED FOR THE INSTRUMENT, because that is what makes a recorded
    // part land on the beat. The host delays everything else to match, which is precisely the
    // job an External Instrument device does in Live with its Hardware Latency field.
    //
    // Not for the effect: nothing is being played through it, so there is no round trip to
    // compensate and inflating its latency would only push a live input further out of place.
    //
    // ONE TERM, NOT TWO. This used to add hardwareSamples and then offsetMs on top of it, which
    // made the panel incoherent: Measure wrote a figure you could not touch, beside a trim that
    // started at zero and existed only to correct it. The measurement now lands IN offsetMs, so
    // what is added is simply the correction in force - and adding hardwareSamples as well here
    // would count the round trip twice.
    if (self->instrument) {
        total += (atomic_load(&self->offsetMs) / 1000.0) * self->hostRate;
    }

    return (total > 0.0) ? (uint32_t)total : 0;
}

// WATCH THE BLOCK SIZE THE HOST ACTUALLY USES, which need not be the one it declared.
//
// The ring's floor is computed from maxSamplesPerBlock, because that is all a plug-in is told
// before it has to size anything. Ableton declares 512 and then calls with 128 - so the floor
// came out at 768 frames where 384 would do, and the plug-in reported 26 ms of latency for a
// job that needs 16.
//
// So: watch for a couple of seconds, take the largest block actually seen, and if that leaves a
// worthwhile amount on the table, ask the worker to retune. This runs on the audio thread and
// therefore only ever raises a flag - the setpoint change and the message to the host both
// allocate, and neither belongs here.
//
// ONCE ONLY, and never upward. A latency change makes the host redo its delay compensation, so
// doing it repeatedly would be worse than the latency it saves. If the gamble is wrong - a
// larger block arrives later and underruns - the safety net below puts the conservative floor
// back for the rest of the session and stops trying.
// THE DAW PLAYS THE HARDWARE. Note data arrives as VST3 events and leaves as MIDI bytes on a
// CoreMIDI destination; the audio comes back through the same capture path the effect uses.
// That round trip is what makes this an instrument rather than a recorder.
//
// Sent straight from process(), not queued. MIDISend is not strictly real-time safe, but the
// alternative - handing the bytes to another thread - adds exactly the jitter that makes a
// hardware synth feel loose, and every plug-in that drives external gear makes the same trade.
//
// Continuous controllers, pitch bend and aftertouch do NOT arrive here: VST3 delivers those as
// parameter changes via IMidiMapping, which is a separate piece of work and is why G2-Edit's
// controller implements it. Notes first.
// 0 keeps whatever the host sent; anything else forces the channel.
static uint8_t gb_channel_for(tGbBridge * self, int16_t incoming) {
    int forced = atomic_load(&self->midiChannel);

    return (forced <= 0) ? (uint8_t)(incoming & 0x0F) : (uint8_t)((forced - 1) & 0x0F);
}

// WHERE THIS BLOCK BEGINS IN WALL TIME - AND IT IS NOT "NOW".
//
// A host is entitled to hand over several blocks per audio callback, and the one this is built
// against does: Ableton declares 512 and calls with 128, which is FOUR process() calls computed
// back to back inside one 512-frame cycle, microseconds apart. Reading the clock at the top of
// each and calling it the block's start time gives all four nearly the same answer - so blocks
// two, three and four are stamped up to 384 frames early, and worse, their events INTERLEAVE
// with the previous block's: a note at offset 0 of the second call is truly 128 frames after a
// note at offset 127 of the first, but comes out stamped 127 frames BEFORE it. CoreMIDI
// delivers in timestamp order, so that is a note-off overtaking its note-on. It sounds exactly
// as bad as it reads.
//
// THE FRAMES THEMSELVES CARRY THE PACING. A block starts where the frames already handed over
// run out, so the model is one addition: carry the end of the last block forward and use it,
// unless real time has already passed it - which is what a new device cycle looks like, and
// what a late callback looks like too. Nothing has to classify cycles or measure a buffer.
//
// MidiSyncTool reached the same place from the other end (its findings, 2026-09-02: Live splits
// a buffer at a loop wrap into 500 frames and then 12). Its version has to identify the cycle
// because it needs the cycle's own length for its telemetry; this one does not.
//
// THE CAP IS FOR A HOST THAT RUNS FASTER THAN REALTIME. An offline bounce hands over blocks as
// fast as it can compute them, and an unbounded model would schedule MIDI further and further
// into the future. A quarter of a second is far longer than any single device cycle and far
// shorter than a bounce takes to run away.
static uint64_t gb_block_host_time(tGbBridge * self, int32_t frames) {
    uint64_t now   = AudioGetCurrentHostTime();

    // THE RAW CLOCK IS KEPT AS WELL AS THE MODEL, and the two are not interchangeable. Outgoing
    // MIDI is stamped on the MODEL, because that is where a note belongs musically. Anything
    // measured about the audio COMING BACK has to use the raw one, because the ring holds what
    // physically arrived by this instant - and for the second, third and fourth call of a
    // callback the model is up to a whole cycle ahead of it. See gb_run_measurement(self).
    self->blockActualHostTime = now;
    uint64_t start = now;
    double   rate  = atomic_load(&self->eventRate);

    // THE CYCLE BOUNDARY, decided here because both users need the same answer: this is where
    // the model is allowed to carry forward, and it is what sizes the ring in gb_observe_burst(self).
    //
    // Two calls belong to one callback if the second arrives before half of the first could
    // have been played. Back to back they are microseconds apart; a real boundary is a whole
    // block. Three orders of magnitude between the regimes, so the threshold is not delicate.
    double gap = gb_frames_between(self, self->lastCallHostTime, now);

    self->blockStartsCycle = (self->lastCallHostTime == 0) || (gap > ((double)self->lastCallFrames * 0.5));
    self->lastCallHostTime = now;
    self->lastCallFrames   = (uint32_t)frames;

    // MONOTONIC, AND THAT IS THE WHOLE POINT OF IT.
    //
    // The model may never hand back a time earlier than the end of the block before it. Letting
    // it re-anchor to the clock at every callback boundary looks right - the clock is the truth,
    // after all - and it inverts the event stream: audio callbacks jitter by a few hundred
    // microseconds, so a callback arriving EARLY gets a first stamp before the previous
    // callback's last one. CoreMIDI delivers in timestamp order, so a note-off can be handed
    // over ahead of the note-on it belongs to. Notes stick on, and the next ones are lost
    // behind them - which from the keyboard looks like the plug-in has stopped passing MIDI
    // through altogether, while a quantised clip, whose events are sparse and far apart, plays
    // perfectly.
    //
    // So: carry forward, always, and let the ceiling below be the only thing that pulls it back.
    // A block boundary that jitters early simply waits; the grid does not move.
    if (self->nextBlockHostTime > now) {
        start = self->nextBlockHostTime;
    }

    // A HARD CEILING ON TOP OF THE RULE, because the rule is a heuristic and this is not.
    //
    // Within a callback the model is legitimately ahead of the clock, and by definition never
    // by more than one callback's worth of frames. If it ever is, the boundary test has missed
    // one - a host rendering faster than realtime hands over call after call with no gap
    // between them, and every one of them reads as a continuation. The model then walks into
    // the future without limit, and MIDI stamped from it is not late, it is GONE: CoreMIDI
    // holds each packet until a time that may be minutes away. Nothing sounds, and the symptom
    // is "the plug-in stopped passing notes through" with everything else working perfectly.
    //
    // Two callbacks of slack, so ordinary jitter never touches it, and falling back to the
    // clock when it trips. The cost of the ceiling being wrong is one callback of MIDI timing;
    // the cost of no ceiling is silence.
    //
    // It is also what makes the monotonic rule above safe. Never going backwards on its own
    // would let one fast burst push the grid permanently into the future; never going forwards
    // too far on its own inverts the stream. Together they say: follow the frames, and if that
    // has drifted implausibly far from the clock, admit it and start again.
    // GENEROUS, DELIBERATELY. The ceiling is a last resort against a runaway, not a tuning
    // knob, and tripping it IS a backwards step - the one thing the rule above exists to
    // prevent. Set it at twice the measured callback it came out at 256 frames while the host
    // was handing over bursts of 512, so it tripped on every burst and inverted the stream it
    // was supposed to protect.
    //
    // A host taking more than eight of its own declared blocks in one callback is pathological,
    // and the measured burst is not always available - observedMaxFrames is only collected
    // while a device is running, and MIDI flows whether one is or not.
    double cycle = (double)self->hostMaxFrames * 8.0;

    if ((double)self->observedMaxFrames * 2.0 > cycle) {
        cycle = (double)self->observedMaxFrames * 2.0;
    }

    if (cycle < 4096.0) {
        cycle = 4096.0;
    }

    if (rate > 0.0) {
        uint64_t ceiling = now + AudioConvertNanosToHostTime(
                               (uint64_t)((cycle / rate) * 1.0e9));

        if (start > ceiling) {
            start = now;
        }
    }

    // AND IT HAS TO COME BACK, WITHOUT EVER GOING BACKWARDS.
    //
    // The model advances by the host's NOMINAL frame count while the clock advances at the
    // device's real rate, and no two crystals agree - so a model that only ever carries forward
    // creeps ahead by a few parts per million for as long as the session lasts. Left alone it
    // walks into the ceiling, and the ceiling resets to the clock, which is the one backwards
    // step this whole arrangement exists to avoid.
    //
    // So when it is further ahead than a callback can account for, each block advances by a
    // shade LESS than its own length. Two per cent of a block is 53 microseconds at 128 frames
    // - inaudible on any single block, and it closes a whole callback of excess inside half a
    // second. The advance stays positive, so the stream stays monotonic while it converges.
    double advance = (double)frames;

    if (rate > 0.0) {
        double ahead = gb_frames_between(self, now, start);
        double allow = (self->observedMaxFrames > 0) ? (double)self->observedMaxFrames : (double)self->hostMaxFrames;

        if (ahead > allow) {
            double shave = (ahead - allow);
            double most  = advance * 0.02;

            advance -= (shave > most) ? most : shave;
        }
    }

    self->nextBlockHostTime = (rate > 0.0)
                        ? start + AudioConvertNanosToHostTime(
                              (uint64_t)((advance / rate) * 1.0e9))
                        : 0;

    self->blockHostTimeNow = start;

    return start;
}

// HOW FAR INTO A CALLBACK AN AVERAGE BLOCK SITS. A host that hands over one block per callback
// has none of this; one that delivers 512 in four 128s has a block starting 0, 128, 256 or 384
// frames in, averaging 192. Audio thread only, and both figures it reads are audio-thread state.
double gb_mean_callback_lead(tGbBridge * self) {
    double burst = (double)self->observedMaxFrames;
    double call  = (double)self->lastCallFrames;

    return (burst > call) ? ((burst - call) / 2.0) : 0.0;
}

// How many of the host's frames separate two host-clock instants. 0 if they are the wrong way
// round, which is what a caller wants everywhere it is used here.
double gb_frames_between(tGbBridge * self, uint64_t from, uint64_t to) {
    double rate = atomic_load(&self->eventRate);

    if ((to <= from) || (rate <= 0.0)) {
        return 0.0;
    }

    return ((double)AudioConvertHostTimeToNanos(to - from) / 1.0e9) * rate;
}

// WHERE AN EVENT BELONGS IN WALL TIME, from where it belongs in the block. Offset 0 is the top
// of this block, which is now; anything later is that many frames into the future and CoreMIDI
// will hold it until then.
static uint64_t gb_host_time_for(tGbBridge * self, uint64_t blockHostTime, int32_t sampleOffset) {
    double rate = atomic_load(&self->eventRate);

    if ((sampleOffset <= 0) || (rate <= 0.0)) {
        return blockHostTime;
    }

    double nanos = ((double)sampleOffset / rate) * 1.0e9;

    return blockHostTime + AudioConvertNanosToHostTime((uint64_t)nanos);
}

// Back from a normalised parameter to the wire.
// ON THE SAME CLOCK AS THE NOTES, and that is not a nicety. Half a stream stamped into the
// future and half sent immediately is a stream that arrives out of ORDER - CoreMIDI delivers by
// timestamp, so an "immediate" controller overtakes every note still waiting for its offset.
// A bend that lands before the note it belongs to is heard as a glitch, not as a timing error.
static void gb_send_controller(tGbBridge * self, uint32_t id, double value, uint64_t hostTime) {
    uint32_t index      = (uint32_t)(id - (uint32_t)GB_CC_BASE);
    uint8_t  channel    = gb_channel_for(self, (int16_t)((index / GB_CC_PER_CHANNEL) & 0x0F));
    uint32_t controller = index % GB_CC_PER_CHANNEL;
    int      destination = atomic_load(&self->midiDestination);
    uint8_t  message[3];

    if (value < 0.0) {
        value = 0.0;
    } else if (value > 1.0) {
        value = 1.0;
    }

    if (controller == (uint32_t)GB_CC_AFTERTOUCH) {
        // Channel pressure is two bytes, not three.
        message[0] = (uint8_t)(0xD0 | channel);
        message[1] = (uint8_t)((value * 127.0) + 0.5);

        gb_midi_send_at(destination, message, 2, hostTime);
        return;
    }

    if (controller == (uint32_t)GB_CC_PITCHBEND) {
        // FOURTEEN BITS, centred at 8192 - the one controller that is not a 0..127 byte, and
        // the one a keyboard player notices immediately if it is wrong.
        int bend = (int)((value * 16383.0) + 0.5);

        message[0] = (uint8_t)(0xE0 | channel);
        message[1] = (uint8_t)(bend & 0x7F);
        message[2] = (uint8_t)((bend >> 7) & 0x7F);

        gb_midi_send_at(destination, message, 3, hostTime);
        return;
    }

    if (controller > 127) {
        return;
    }

    message[0] = (uint8_t)(0xB0 | channel);
    message[1] = (uint8_t)controller;
    message[2] = (uint8_t)((value * 127.0) + 0.5);

    gb_midi_send_at(destination, message, 3, hostTime);
}

// WHAT THE HOST TAKES PER CALLBACK, NOT PER CALL - and getting this wrong was a glitch you could
// hear.
//
// The ring has to cover the largest single demand made on it, and that demand is a whole audio
// callback. Ableton declares 512, calls with 128, and takes all four back to back. The retune
// read "the largest block actually seen" as 128 and shrank the setpoint to suit, so the ring
// held 400 frames against a callback that drained 512 of them. Every cycle ran it dry: 45
// underruns and 44 resyncs in two seconds on a KRONOS at a 64 frame device buffer, one audible
// click each. It survived a 512 frame device buffer only because the setpoint then came out at
// 960 by accident, which is more than one callback - which is why this never showed up until
// someone ran a small buffer.
//
// The DECLARED maximum was right all along. What is measured here is not a correction to it so
// much as a confirmation, and on a host that genuinely uses less than it declares - one block
// per callback, smaller than declared - it still reclaims the difference.
//
// CALLED BEFORE ANY EARLY RETURN, unlike gb_observe_block(self). Sitting inside that one, it saw
// nothing at all while the ring was resyncing - which is exactly when the numbers are needed,
// and is why the first attempt at this fix changed nothing.
//
// The boundary itself is decided in gb_block_host_time(self); this only adds up what fell inside one.
static void gb_observe_burst(tGbBridge * self, int32_t frames) {
    // CLAMPED TO ONE CALLBACK. burstFrames only resets when a cycle boundary is detected, so a
    // boundary that is missed accumulates without limit - and this figure feeds the reported
    // latency. Nothing can legitimately have handed over more than a callback's worth before
    // the current call, by definition of what a callback is.
    self->burstBefore = self->blockStartsCycle ? 0 : self->burstFrames;

    if ((self->observedMaxFrames > 0) && (self->burstBefore > self->observedMaxFrames)) {
        self->burstBefore = self->observedMaxFrames;
    }

    if (self->blockStartsCycle) {
        // A CEILING, because a detection that misfires would otherwise stick for the session.
        // Nothing legitimately takes more than a few of its declared blocks per callback, and
        // sizing a ring for a figure this side of that costs a few milliseconds where trusting
        // a runaway one costs seconds.
        uint32_t ceiling = (self->hostMaxFrames > 0) ? (self->hostMaxFrames * 4) : 8192;

        if ((self->burstFrames > self->observedMaxFrames) && (self->burstFrames <= ceiling)) {
            self->observedMaxFrames = self->burstFrames;
        }

        self->burstFrames = (uint32_t)frames;
    } else {
        self->burstFrames += (uint32_t)frames;
    }
}

static void gb_observe_block(tGbBridge * self, int32_t frames) {
    // A RING TOO SMALL IS FIXED AT ONCE, AND NOT ONLY DOWNWARDS.
    //
    // The retune below exists to RECLAIM frames once the host has been watched, and it is
    // deliberately slow and one-way about it. This is the opposite case and it cannot wait: a
    // ring sized for less than one callback is drained dry every cycle, which is an audible
    // click each time - and there was no path to it except gb_revert_retune(self), which only fires
    // AFTER an underrun has already been heard.
    //
    // It reaches here because neither number available at the first open is right. CT's Live
    // DECLARES 256 and hands over 512 in one callback; the declaration under-states, and the
    // observation is still partial during a project load - which is the least undisturbed
    // moment there is and the one where a busy CPU splits a burst in two. Both said 256, the
    // ring came up at 560, and only switching the buffer away and back settled it at 880.
    //
    // So: whenever the burst actually seen needs more ring than is in force, ask for it. Once
    // per increase, since observedMaxFrames only ever grows.
    if (self->running && (self->observedMaxFrames > 0) && (self->retuneState != eRetuneRequested)) {
        double needed = gb_minimum_setpoint_for_host(self, self->observedMaxFrames) * GB_AUTO_MARGIN;

        if (needed > (self->setpointFrames + GB_RETUNE_MIN_GAIN)) {
            self->retuneState = eRetuneRequested;
            gb_wake_worker(self);
            return;
        }
    }

    if (self->retuneState != eRetuneWatching) {
        return;
    }

    self->observedFrames += (uint64_t)frames;

    if ((double)self->observedFrames < (GB_SETTLE_SECONDS * self->hostRate)) {
        return;
    }

    double candidate = gb_minimum_setpoint_for_host(self, self->observedMaxFrames) * GB_AUTO_MARGIN;

    // Nothing to gain if the host is using what it declared.
    if (self->observedMaxFrames >= self->hostMaxFrames) {
        self->retuneState = eRetuneSettled;
        return;
    }

    if ((self->setpointFrames - candidate) >= GB_RETUNE_MIN_GAIN) {
        self->retuneState = eRetuneRequested;
        gb_wake_worker(self);
    } else {
        self->retuneState = eRetuneSettled;
    }
}

// Peak-hold with a slow decay, which is what makes a meter readable: a true instantaneous peak
// on a 30 Hz repaint shows almost nothing of a transient.
static void gb_publish_status(tGbBridge * self, float ** out, int32_t frames, double fill) {
    tGbStatus * status = gb_status(self->statusSlot);

    if (status == NULL) {
        return;
    }
    float       peak[GB_CHANNELS] = { 0.0f, 0.0f };

    for (int c = 0; c < GB_CHANNELS; c++) {
        for (int32_t i = 0; i < frames; i++) {
            float magnitude = (out[c][i] < 0.0f) ? -out[c][i] : out[c][i];

            if (magnitude > peak[c]) {
                peak[c] = magnitude;
            }
        }
    }

    float heldLeft  = atomic_load(&status->peakLeft) * 0.85f;
    float heldRight = atomic_load(&status->peakRight) * 0.85f;

    atomic_store(&status->peakLeft, (peak[0] > heldLeft) ? peak[0] : heldLeft);
    atomic_store(&status->peakRight, (peak[1] > heldRight) ? peak[1] : heldRight);

    atomic_store(&status->fillFrames, fill);
    atomic_store(&status->driftPpm, drift_measured_ppm(&self->drift));
    atomic_store(&status->underruns, (int)atomic_load(&self->ring.underflows));
    atomic_store(&status->resyncs, (int)atomic_load(&self->resyncs));
}

// TELLING THE HOST ITS LATENCY CHANGED, which it will not ask about on its own.
//
// A host reads getLatencySamples() once, shortly after activation, and caches it until told
// otherwise. Now that a fresh instance opens nothing, that reading is always zero - so
// selecting a device later left Ableton compensating for nothing at all, and every track fed by
// the plug-in sat late by the whole buffer.
//
// Only the CONTROLLER can say so: restartComponent lives on IComponentHandler, which the
// processor never sees. So it goes over the same connection the status slot does.
void gb_send_latency_changed(tGbBridge * self) {
    gb_send_message(self, "gbLatency", 0);
}

static void gb_send_slot(tGbBridge * self) {
    gb_send_message(self, "gbStatusSlot", self->statusSlot);
}

void gb_send_message(tGbBridge * self, const char * id, int value) {
    // OUT THROUGH THE WRAPPER, because making an IMessage needs the host application object and
    // delivering one needs the IConnectionPoint - two things a plug-in can only touch in C++. What
    // the bridge knows is that it has something to say and what to call it; see tGbHostOps.
    if (self->hostOps.send_message != NULL) {
        self->hostOps.send_message(self->hostOps.user, id, value);
    }
}

static void gb_silence(float ** out, int32_t frames) {
    for (int c = 0; c < GB_CHANNELS; c++) {
        memset(out[c], 0, (size_t)frames * sizeof(float));
    }
}

// MONO IS WIDENED HERE, on the way into the ring, rather than on the way out.
//
// The ring, the resampler and the drift loop are all stereo, and keeping them that way means
// one code path downstream instead of a channel count threaded through every one of them. The
// cost is resampling a duplicated channel, which is a few hundred thousand multiplies a second
// - nothing beside the clarity of not having a mono variant of the whole chain.
static void gb_capture_callback(void * user, const float * input, float * output, uint32_t frames) {
    (void)output;

    tGbBridge * self = (tGbBridge *)user;
    const float *     source = input;

    if (self->captureChannels == 1) {
        for (uint32_t i = 0; i < frames; i++) {
            self->widen[(i * 2) + 0] = input[i];
            self->widen[(i * 2) + 1] = input[i];
        }

        source = self->widen;
    }

    // WHEN THE NEWEST FRAME IN THE RING GOT HERE. Without it the ring's occupancy cannot be
    // turned into an age: see gb_latency_frames_measured(self).
    atomic_store(&self->lastWriteHostTime, AudioGetCurrentHostTime());

    if (!ring_write(&self->ring, source, frames)) {
        atomic_store(&self->needResync, true);
    }
}

// ---- device life cycle, worker thread only -------------------------------------------------
//
// The device is opened IN PROCESS for now. The feeder process this eventually wants - spawned
// with posix_spawn so it inherits the host's microphone consent - is a robustness and sharing
// move, not a correctness one, and splitting it out before there is a working plug-in to feed
// means debugging IPC and DAW hosting at the same time with no baseline.
//
// Everything here runs on the worker, never on the audio thread and never on the host's UI
// thread. Opening a Core Audio device allocates, talks to a driver and can block for tens of
// milliseconds; process() only ever asks for a change and carries on.

static void gb_start_worker(tGbBridge * self) {
    if (self->workerActive) {
        return;
    }

    atomic_store(&self->workerQuit, false);
    self->workerActive = (pthread_create(&self->worker, NULL, gb_worker_entry, self) == 0);
}

static void gb_stop_worker(tGbBridge * self) {
    if (!self->workerActive) {
        return;
    }

    atomic_store(&self->workerQuit, true);
    gb_wake_worker(self);
    pthread_join(self->worker, NULL);
    self->workerActive = false;
}

void gb_wake_worker(tGbBridge * self) {
    pthread_mutex_lock(&self->wakeMutex);
    self->wakeFlag = true;
    pthread_cond_signal(&self->wakeCond);
    pthread_mutex_unlock(&self->wakeMutex);
}

static double gb_now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((double)ts.tv_sec * 1000.0) + ((double)ts.tv_nsec / 1.0e6);
}

void gb_request_device(tGbBridge * self) {
    atomic_store(&self->lastDeviceRequestMs, gb_now_ms());
    atomic_store(&self->deviceDirty, true);
    gb_wake_worker(self);
}

// The panel reads this out of the shared block rather than being messaged, like every other live
// figure - see gbStatus.h.
static void gb_publish_waiting(tGbBridge * self, bool waiting, const char * name) {
    tGbStatus * status = gb_status(self->statusSlot);

    if (status == NULL) {
        return;
    }

    // NAME FIRST, FLAG SECOND. The panel only reads the name while the flag is up, so writing
    // them in this order means it can never show "waiting for" against a stale name.
    if (waiting && (name != NULL)) {
        snprintf(status->waitingName, sizeof(status->waitingName), "%s", name);
    }
    atomic_store(&status->waitingForDevice, waiting ? 1 : 0);
}

static void * gb_worker_entry(void * arg) {
    gb_worker_loop((tGbBridge *)arg);
    return NULL;
}

static void gb_worker_loop(tGbBridge * self) {
    while (!atomic_load(&self->workerQuit)) {
        pthread_mutex_lock(&self->wakeMutex);

        while (!self->wakeFlag && !atomic_load(&self->workerQuit)) {
            pthread_cond_wait(&self->wakeCond, &self->wakeMutex);
        }

        self->wakeFlag = false;
        pthread_mutex_unlock(&self->wakeMutex);

        if (atomic_load(&self->workerQuit)) {
            break;
        }

        // SETTLE BEFORE ACTING - see GB_DEVICE_SETTLE_MS. Waiting here rather than in the
        // parameter handler keeps every route to a device change on one path: the panel's
        // arrows, the host's generic control and an automation lane all arrive as
        // gb_request_device(self), and all of them get coalesced by the same clock.
        //
        // deviceDirty is not consumed until the wait is over, so a request arriving mid-wait
        // simply pushes the deadline out rather than being lost or acted on twice.
        while (atomic_load(&self->deviceDirty) && !atomic_load(&self->workerQuit)) {
            double waited = gb_now_ms() - atomic_load(&self->lastDeviceRequestMs);

            if (waited >= GB_DEVICE_SETTLE_MS) {
                break;
            }
            usleep((useconds_t)((GB_DEVICE_SETTLE_MS - waited) * 1000.0));
        }

        if (!atomic_load(&self->workerQuit) && atomic_exchange(&self->deviceDirty, false)) {
            gb_reconfigure(self);
        }

        if (atomic_load(&self->retuneState) == eRetuneRequested) {
            gb_retune(self);
        }

        if (atomic_exchange(&self->revertWanted, false)) {
            gb_revert_retune(self);
        }

        if (atomic_exchange(&self->measurePanic, false)) {
            synthlib_log_line("measure requested");
            gb_send_all_notes_off(self);
        }

        if (atomic_exchange(&self->measureStore, false)) {
            gb_store_measurement(self);
        }

        // SETTLE THE OFFSET BEFORE ACTING ON IT, the same shape as the device debounce above and
        // for the same reason: a burst of clicks should cost the host one delay-compensation
        // pass, not one per click. offsetDirty is not consumed until the wait is over, so a
        // click arriving mid-wait pushes the deadline out instead of being lost.
        while (atomic_load(&self->offsetDirty) && !atomic_load(&self->workerQuit)) {
            double waited = gb_now_ms() - atomic_load(&self->lastOffsetChangeMs);

            if (waited >= GB_OFFSET_SETTLE_MS) {
                break;
            }
            usleep((useconds_t)((GB_OFFSET_SETTLE_MS - waited) * 1000.0));
        }

        if (!atomic_load(&self->workerQuit) && atomic_exchange(&self->offsetDirty, false)) {
            uint32_t nowLatency = gb_snapshot_latency(self);

            if (nowLatency != self->reportedLatency) {
                self->reportedLatency = nowLatency;
                atomic_store(&self->latencyDirty, true);
            }
        }

        // THE MEASURED PIPELINE HAS MOVED FAR ENOUGH TO SAY SO. The reported figure follows a
        // live average now rather than a constant, so something has to notice when it has
        // drifted past the deadband - a device swap and a ring that has finally settled both
        // land here. The audio thread raises the flag; this is the only place that acts on it.
        if (atomic_load(&self->latencyDirty)) {
            // WHERE THE NUMBER COMES FROM, every time it changes. A total on its own starts an
            // argument that only the breakdown can settle - "43.5 ms" is a ring, a device
            // buffer, a resampler and a measured round trip, and which of them is the big one
            // decides what to do about it. Twice now a figure has been questioned and the
            // components were only on the panel, where they cannot be pasted into a message.
            double ratio    = (atomic_load(&self->snapRatio) > 0.0) ? atomic_load(&self->snapRatio) : 1.0;
            double perMs    = (atomic_load(&self->snapHostRate) > 0.0) ? (atomic_load(&self->snapHostRate) / 1000.0) : 48.0;
            double ringPart = atomic_load(&self->snapSetpoint) / ratio;
            double devPart  = (double)atomic_load(&self->snapDeviceLatency) / ratio;
            double filtPart = resampler_latency_frames() / ratio;
            double offPart  = (atomic_load(&self->offsetMs) / 1000.0) * atomic_load(&self->snapHostRate);

            synthlib_log_line("latency %u smp (%.1f ms) = pipeline %.0f (setpoint would say %.0f: ring "
                     "%.0f + device %.0f + filter %.0f) + measured %.0f (%.1f ms)",
                     atomic_load(&self->reportedLatency), (double)atomic_load(&self->reportedLatency) / perMs,
                     atomic_load(&self->pipelineAvg), ringPart + devPart + filtPart,
                     ringPart, devPart, filtPart, offPart, offPart / perMs);
        }

        if (atomic_exchange(&self->latencyDirty, false)) {
            // BEFORE gb_send_latency_changed(self), deliberately. That call is what makes the host
            // reactivate us and reopen the device, and gb_reconfigure(self) decides on reopen whether
            // to re-seed the offset. Writing the new value into the table first means the pair
            // is already up to date by the time anything looks at it.
            gb_sync_offset_to_pair(self);
            gb_send_latency_changed(self);
            gb_publish_measurement(self);
        }
    }
}

void gb_reconfigure(tGbBridge * self) {
    tDeviceInfo list[DEVICE_MAX];
    uint32_t    count = device_enumerate(list, DEVICE_MAX);
    tDeviceInfo chosen;
    bool        found = false;

    int index = atomic_load(&self->wantedDevice);

    // HOST INPUT OPENS NOTHING, and closes whatever was open. Holding a capture device while the
    // audio is coming from the host would keep hardware nobody is listening to - and on a device
    // that is also the host's own interface, it would keep imposing a buffer size on it.
    if (atomic_load(&self->captureSource) == GB_SOURCE_HOST) {
        pthread_mutex_lock(&self->configLock);
        gb_close_capture_locked(self);
        pthread_mutex_unlock(&self->configLock);

        gb_publish_waiting(self, false, NULL);

        tGbStatus * status = gb_status(atomic_load(&self->statusSlot));

        if (status != NULL) {
            snprintf(status->deviceName, sizeof(status->deviceName), "%s", "Host input");
            atomic_store(&status->active, true);
        }

        // WHAT THE HOST IS TOLD IS THE CORRECTION AND NOTHING ELSE. gb_report_latency() adds the
        // pipeline term to it, and with nothing open that term is already zero - nominalRatio is 0,
        // so gb_internal_latency_frames() returns 0 rather than dividing by it.
        uint32_t nowLatency = gb_report_latency(self);

        if (nowLatency != atomic_load(&self->reportedLatency)) {
            atomic_store(&self->reportedLatency, nowLatency);
            gb_send_latency_changed(self);
        }
        synthlib_log_line("capture source: HOST INPUT - no device opened, latency %u samples", nowLatency);
        return;
    }

    synthlib_log_line("reconfigure: slot %d, saved uid '%s'", index, self->deviceSelector);

    // THE PARAMETER IS THE ONLY SELECTOR. It used to be one of two, with a saved UID as the
    // other, and they disagreed: the panel drew the parameter while the processor had opened
    // whatever the UID named, so the header said it was capturing a QU-24 while the device row
    // said Analog Keys and the audio was a Kronos. Three answers, all sincerely held.
    //
    // The UID is still stored, and still keys the per-device settings - it is simply no longer
    // allowed to decide WHICH device. One selector, one answer, and the panel cannot be wrong
    // about it.
    // THE SAVED DEVICE FIRST while a restore is still pending, whatever slot the parameter
    // names. Resolving it also puts the parameter right, so the panel and the host stop
    // disagreeing with what is actually open.
    if (atomic_load(&self->savedDevicePending) && !(self->deviceSelector[0] == '\0')) {
        found = device_find(self->deviceSelector, true, &chosen);

        if (found) {
            atomic_store(&self->savedDevicePending, false);
            gb_publish_waiting(self, false, NULL);

            int slot = gb_slot_for_uid(chosen.uid);

            if (slot >= 0) {
                atomic_store(&self->wantedDevice, slot);
                index = slot;
                gb_send_message(self, "gbDeviceSlot", slot);
            }
            synthlib_log_line("saved device '%s' present - opening it", chosen.name);
        } else {
            // Device not present, don't modify deviceSelector - preserve the saved intent.
            // gb_publish_waiting(self, true, (savedDeviceName[0] == '\0') ? deviceSelector
            //                                               : savedDeviceName);
            synthlib_log_line("saved device '%s' not present - waiting (not modifying deviceSelector)",
                     (self->savedDeviceName[0] == '\0') ? self->deviceSelector : self->savedDeviceName);
        }
    } else if (index >= 0) {
        gb_publish_waiting(self, false, NULL);
        found = gb_resolve_slot(list, count, index, &chosen);
    }

    // Before any parameter has arrived, the saved UID is what a reopened project has to go on.
    // It is no longer a competing selector: the controller resolves the same UID to the same
    // slot and sets the parameter to match, so what arrives next agrees with what opened here.
    if (!found && (index < 0) && !(self->deviceSelector[0] == '\0')) {
        found = device_find(self->deviceSelector, true, &chosen);
        synthlib_log_line("no parameter yet - restoring saved uid: %s", found ? chosen.name : "not present");
    }

    // NOTHING IS OPENED UNTIL SOMETHING HAS ACTUALLY BEEN CHOSEN.
    //
    // Every previous version of this opened SOMETHING on a fresh instance - first a hard-coded
    // Kronos, then whatever sat at slot 0 - on the reasoning that a silent plug-in looks broken.
    // That reasoning was wrong, and expensively so. Slot 0 on this machine is an iPhone
    // Continuity microphone, so loading a set woke it once per instance, synchronously, on the
    // host's main thread during load. Ableton stopped starting.
    //
    // A plug-in has no business seizing capture hardware nobody asked it to. Idle until chosen
    // is both safer and more honest, and the panel says "no device selected" rather than naming
    // something the user never picked.
    if (!found && (index < 0)) {
        synthlib_log_line("nothing selected yet - staying idle");
    }

    if (!found) {
        synthlib_log_line("slot %d resolved to nothing - staying closed", index);
    } else {
        synthlib_log_line("slot %d -> '%s'", index, chosen.name);
    }

    // What gb_close_capture_locked(self) must NOT hand back, because we are about to reopen it. Zero
    // when the reconfigure is a genuine device change or a close, where the restore is right.
    self->keepSettingsFor = found ? chosen.id : 0;

    double heldFrom = gb_now_ms();
    pthread_mutex_lock(&self->configLock);
    gb_close_capture_locked(self);
    self->keepSettingsFor = 0;

    if (found) {
        // Recorded only on SUCCESS. Writing it before the attempt meant a device that failed to
        // open still became the thing the project saved and reopened with.
        if (gb_open_capture_locked(self, &chosen)) {
            snprintf(self->deviceSelector, sizeof(self->deviceSelector), "%s", chosen.uid);
            self->appliedDevice  = index;
        } else {
            self->running = false;
            synthlib_log_line("open failed - selection unchanged, staying closed");
        }
    }

    uint32_t nowLatency = self->running ? gb_report_latency(self) : 0;
    double held       = gb_now_ms() - heldFrom;

    pthread_mutex_unlock(&self->configLock);

    // THE OTHER HALF OF THE SPINNING CURSOR. This is how long the worker owned the lock that
    // every host-thread entry point waits for; gb_lock_config_from_host(self) reports the wait from
    // the other side. The two together say whether a beachball was this plug-in's doing.
    if (held >= 20.0) {
        // BROKEN DOWN, because "it held the lock for 1441 ms" says a refactor is needed and not
        // WHICH of the three things inside it to move first.
        synthlib_log_line("reconfigure held configLock for %.0f ms (close+probe %.0f, rate+buffer %.0f, "
                 "open %.0f) - anything the host asked of this plug-in on its own thread waited "
                 "behind it", held,
                 (self->gPhaseIdle > heldFrom) ? (self->gPhaseIdle - heldFrom) : 0.0,
                 (self->gPhaseOpen > self->gPhaseProps) ? (self->gPhaseOpen - self->gPhaseProps) : 0.0,
                 (self->gPhaseOpen > 0.0) ? (gb_now_ms() - self->gPhaseOpen) : 0.0);
    }

    // Told OUTSIDE the lock: restartComponent re-enters the plug-in, and a host is entitled to
    // call straight back into it - including into process(), which trylocks this same mutex.
    if (nowLatency != self->reportedLatency) {
        self->reportedLatency = nowLatency;
        gb_send_latency_changed(self);
        synthlib_log_line("latency now %u samples - asked the host to re-read it", nowLatency);
    }
}

// Slot to device, skipping anything with no inputs - the same filter, in the same order, that
// the editor and the controller use. One function so the three cannot drift apart again.
// SLOT 0 IS "NONE", and every real device sits one higher.
//
// Before this there was no way to express "nothing", which made two different states share one
// value: a fresh instance that had chosen nothing and an instance that had chosen the first
// device both read 0. The panel showed that as the first device's name while its own header said
// "no device selected" - the plug-in contradicting itself in two lines of the same window - and
// there was no way back to nothing once a device had been picked.
//
// Devices moved UP by one rather than "None" being bolted on at the end, because 0 is the value
// a VST3 parameter defaults to and the value a host restores when a project names no device. It
// costs nothing on load: setComponentState() resolves a saved project by its device UID and
// recomputes the parameter from that, so a saved set still reopens on the device it named. What
// does shift is a recorded AUTOMATION lane of the device parameter, which would now name the
// device one place earlier - accepted deliberately at v0.1.0 for a setup control nobody
// automates.
static bool gb_resolve_slot(const tDeviceInfo * list, uint32_t count, int slot, tDeviceInfo * out) {
    uint32_t seen = 1;      // slot 0 is None, so the first real device is slot 1

    if (slot <= 0) {
        return false;       // None: nothing to open, and gb_reconfigure(self) closes what is open
    }

    for (uint32_t i = 0; i < count; i++) {
        if (list[i].inputChannels == 0) {
            continue;
        }

        if ((int)seen == slot) {
            *out = list[i];
            return true;
        }

        seen++;
    }

    return false;
}

// Apply the smaller setpoint the observed block size allows, then tell the host its latency
// moved. Worker thread only.
static void gb_retune(tGbBridge * self) {
    pthread_mutex_lock(&self->configLock);

    if (!self->running) {
        pthread_mutex_unlock(&self->configLock);
        atomic_store(&self->retuneState, eRetuneSettled);
        return;
    }

    double before = self->setpointFrames;

    self->setpointFrames = gb_minimum_setpoint_for_host(self, self->observedMaxFrames) * GB_AUTO_MARGIN;

    drift_set_setpoint(&self->drift, self->setpointFrames);
    gb_publish_config_snapshot(self);
    gb_publish_latency_breakdown(self);

    // The ring holds more than the new setpoint wants, so snap it down rather than waiting for
    // the loop to drain it at a few hundred ppm - which would take minutes.
    atomic_store(&self->needResync, true);

    tGbStatus * status = gb_status(self->statusSlot);

    if (status != NULL) {
        atomic_store(&status->setpointFrames, self->setpointFrames);
    }

    uint32_t nowLatency = gb_report_latency(self);

    pthread_mutex_unlock(&self->configLock);

    atomic_store(&self->retuneState, eRetuneSettled);

    // Reads both ways now: reclaiming frames when the host uses less than it declared, and
    // CLAIMING them when it hands over more per callback than either number suggested.
    synthlib_log_line("retuned %s: host declares %u and takes %u per callback - setpoint %.0f -> %.0f, "
             "latency %u -> %u",
             (self->setpointFrames > before) ? "UP - the ring was smaller than one callback" : "down",
             self->hostMaxFrames, self->observedMaxFrames, before, self->setpointFrames, atomic_load(&self->reportedLatency),
             nowLatency);

    if (nowLatency != self->reportedLatency) {
        self->reportedLatency = nowLatency;

        if (status != NULL) {
            atomic_store(&status->latencySamples, (int)nowLatency);
        }

        gb_send_latency_changed(self);
    }
}

// The pair a measurement belongs to. Both halves matter: the same synth reached over USB and
// over DIN answers at different speeds, and two synths on one interface are not comparable.
tMeasured * gb_measured_for(tGbBridge * self, const char * audioUid, const char * midiDest, bool create) {
    for (uint32_t i = 0; i < self->measuredCount; i++) {
        if ((strncmp(self->measured[i].audioUid, audioUid, DEVICE_UID_LEN) == 0)
            && (strncmp(self->measured[i].midiDest, midiDest, GB_MIDI_NAME_LEN) == 0)) {
            return &self->measured[i];
        }
    }

    if (!create) {
        return NULL;
    }

    uint32_t slot = self->measuredCount;

    if (self->measuredCount < GB_MAX_MEASURED) {
        self->measuredCount++;
    } else {
        memmove(&self->measured[0], &self->measured[1], sizeof(tMeasured) * (GB_MAX_MEASURED - 1));
        slot = GB_MAX_MEASURED - 1;
    }

    memset(&self->measured[slot], 0, sizeof(self->measured[slot]));
    strncpy(self->measured[slot].audioUid, audioUid, DEVICE_UID_LEN - 1);
    strncpy(self->measured[slot].midiDest, midiDest, GB_MIDI_NAME_LEN - 1);

    return &self->measured[slot];
}

void gb_current_midi_name(tGbBridge * self, char * out, unsigned long len) {
    gb_midi_destination_name(atomic_load(&self->midiDestination), out, len);
}

void gb_remember_offset_pair(tGbBridge * self, const char * destination) {
    snprintf(self->offsetUid, sizeof(self->offsetUid), "%s", gb_audio_key(self));
    snprintf(self->offsetDest, sizeof(self->offsetDest), "%s", destination);
}

// WORKER THREAD ONLY. The +/- arrives on the audio thread, which stores the atomic and asks for
// a latency update; the table write happens here instead, because gb_measured_for(self) can allocate a
// slot and memmove the array and that has no business running under a process() call.
void gb_sync_offset_to_pair(tGbBridge * self) {
    // THE KEY IS THE AUDIO SOURCE, which in host-input mode is not a device UID - see gb_audio_key().
    // Testing deviceSelector here meant that in that mode there was no pair to fold the correction
    // into, so a measured or nudged offset was never written down.
    if (!self->instrument || (gb_audio_key(self)[0] == '\0')) {
        return;
    }

    char destination[GB_MIDI_NAME_LEN];

    gb_current_midi_name(self, destination, sizeof(destination));

    tMeasured * entry = gb_measured_for(self, gb_audio_key(self), destination, true);

    if (entry != NULL) {
        entry->offsetMs = atomic_load(&self->offsetMs);
    }

    gb_remember_offset_pair(self, destination);
}

// No configLock here, and it must stay that way - it is called from the worker's idle path and
// from gb_store_measurement(self) after that has released the lock. So it publishes only what the
// snapshot already holds; the snapshot itself is refreshed by whoever changed the config, under
// the lock they were already holding.
void gb_publish_measurement(tGbBridge * self) {
    gb_publish_latency_breakdown(self);
}

// Every component of the reported figure, so the panel can show where the time actually goes
// rather than one number nobody can argue with.
static void gb_publish_latency_breakdown(tGbBridge * self) {
    tGbStatus * status = gb_status(self->statusSlot);

    if (status == NULL) {
        return;
    }

    // FROM THE SNAPSHOT, not the fields. This is called both with configLock held (from the
    // open and from retune) and without it (from the worker's idle publish), so it cannot take
    // the lock and must not read anything the lock protects.
    double setpoint = atomic_load(&self->snapSetpoint);
    double ratio    = atomic_load(&self->snapRatio);

    if (ratio <= 0.0) {
        ratio = 1.0;
    }

    atomic_store(&status->ringSamples, (int)(setpoint / ratio));
    atomic_store(&status->deviceSamples, (int)((double)atomic_load(&self->snapDeviceLatency) / ratio));
    atomic_store(&status->filterSamples, (int)(resampler_latency_frames() / ratio));
    atomic_store(&status->measuredSamples, (int)self->hardwareSamples);
    atomic_store(&status->measuredLow, atomic_load(&self->measureTripLow));
    atomic_store(&status->measuredHigh, atomic_load(&self->measureTripHigh));
    atomic_store(&status->measuredTrips, atomic_load(&self->measureTripsUsed));
    atomic_store(&status->offsetSamples, (int)((atomic_load(&self->offsetMs) / 1000.0) * atomic_load(&self->snapHostRate)));
    atomic_store(&status->latencySamples, (int)gb_snapshot_latency(self));
    atomic_store(&status->recommendedFrames, self->recommendedSetpoint);
    atomic_store(&status->eventsIn, (int)atomic_load(&self->eventsIn));
    atomic_store(&status->eventsOut, (int)atomic_load(&self->eventsOut));
}

// Put the conservative floor back after a retune turned out to be too tight.
static void gb_revert_retune(tGbBridge * self) {
    pthread_mutex_lock(&self->configLock);

    if (!self->running) {
        pthread_mutex_unlock(&self->configLock);
        return;
    }

    double before = self->setpointFrames;

    self->setpointFrames = gb_minimum_setpoint(self, self->openDeviceFrames) * GB_AUTO_MARGIN;

    drift_set_setpoint(&self->drift, self->setpointFrames);
    atomic_store(&self->needResync, true);

    tGbStatus * status = gb_status(self->statusSlot);

    if (status != NULL) {
        atomic_store(&status->setpointFrames, self->setpointFrames);
    }

    uint32_t nowLatency = gb_report_latency(self);

    pthread_mutex_unlock(&self->configLock);

    synthlib_log_line("retune reverted after an underrun: setpoint %.0f -> %.0f", before, self->setpointFrames);

    if (nowLatency != self->reportedLatency) {
        self->reportedLatency = nowLatency;

        if (status != NULL) {
            atomic_store(&status->latencySamples, (int)nowLatency);
        }

        gb_send_latency_changed(self);
    }
}

static bool gb_open_capture_locked(tGbBridge * self, const tDeviceInfo * info) {
    tDeviceSettings * settings = gb_ensure_settings(self, info->uid);

    // A rate or buffer size chosen since the last open takes precedence over what was stored,
    // and is then stored itself - so the choice survives the next reopen of the project.
    if (atomic_load(&self->wantedRate) > 0.0) {
        settings->rate = atomic_exchange(&self->wantedRate, 0.0);
    }

    if (atomic_load(&self->wantedFrames) > 0) {
        settings->frames = (uint32_t)atomic_exchange(&self->wantedFrames, 0);
    }

    if (atomic_load(&self->wantedChannels) > 0) {
        settings->captureChannels = (uint32_t)atomic_exchange(&self->wantedChannels, 0);
    }

    if (atomic_load(&self->wantedFirstChannel) >= 0) {
        settings->firstChannel = (uint32_t)atomic_exchange(&self->wantedFirstChannel, -1);
    }

    // CLAMP THE CHANNEL REQUEST TO WHAT THE DEVICE ACTUALLY HAS, rather than letting the open
    // fail.
    //
    // The channel settings persist per device, but the PARAMETER is global to the instance - so
    // selecting a 32 input desk, choosing channels 17/18, then switching to a two-channel synth
    // asks for channels that do not exist. device_open() refused, nothing opened, and the plug-in
    // appeared stuck on the last device big enough to satisfy the request. Silently refusing to
    // change device is a far worse answer than capturing the nearest thing that exists and
    // showing what happened.
    uint32_t available = info->inputChannels;
    uint32_t first     = settings->firstChannel;
    uint32_t wantCount = settings->captureChannels;

    if (available == 0) {
        synthlib_log_line("device '%s' reports no input channels", info->name);
        return false;
    }

    if (first >= available) {
        synthlib_log_line("first channel %u past the device's %u - using channel 1", first + 1, available);
        first = 0;
    }

    if ((first + wantCount) > available) {
        wantCount = available - first;      // at least 1, since first < available
        synthlib_log_line("only %u channel(s) available from %u - capturing %u",
                 available - first, first + 1, wantCount);
    }

    // SAY SO IF THE REQUEST WAS TRIMMED. The clamp above is the right behaviour - capturing the
    // nearest thing that exists beats refusing to open - but on its own it left the panel and
    // the host showing a channel the device does not have while a different one was being
    // captured. The editor's arrows cannot reach an impossible value any more; a DEVICE CHANGE
    // still can, because the parameter is global to the instance while the channel setting is
    // per device. Same message the device slot already uses on the same kind of mismatch.
    if (first != settings->firstChannel) {
        settings->firstChannel = first;
        gb_send_message(self, "gbFirstChannel", (int)first);
    }

    // Same again for the width. A one-channel device cannot give a stereo pair, and the capture
    // callback widens the single channel rather than failing - but the panel must not go on
    // saying "Stereo" over a mono capture.
    if (wantCount != settings->captureChannels) {
        settings->captureChannels = wantCount;
        gb_send_message(self, "gbMode", (int)((wantCount > 1) ? 1 : 0));
    }
    self->captureChannels = wantCount;

    // LEAVE A DEVICE SOMEBODY ELSE IS DRIVING ALONE.
    //
    // Rate and buffer size are global to the device, so setting either reaches into every other
    // client of it - the host included. The default has always been to touch neither, but that
    // protection ended the moment a size was picked in the panel, and the case where it matters
    // most is the easiest to walk into: a mixer serving as the host's OWN output and as this
    // plug-in's capture source. There the device's buffer frame size IS the host's block size,
    // so imposing one is the plug-in setting its own process() call rate on hardware it does not
    // own - and the smaller the size, the harder every subsequent device change becomes.
    //
    // Probed BEFORE device_open() below, so what it reports is other clients and never our own
    // stream. Reported rather than silently obeyed: the panel setting is still whatever the user
    // chose, and the log says why the device did not take it.
    // OUR OWN GHOST FIRST. CoreAudio tears an IOProc down asynchronously, so a device we closed
    // moments ago can still report that it is running - and the probe below reads that as
    // "another client has it" and declines to touch the buffer.
    //
    // That is the whole of CT's "I'm attempting to set 64 and getting 512" on project load. The
    // sequence is: the device opens before the saved buffer size is known, so it comes up at
    // whatever it was - 512; the restored state then arrives and asks for 64; gb_reconfigure(self)
    // closes the stream and immediately probes; the probe sees the stream it just closed; the
    // set is skipped; and 512 stands for the session. Setting 64 by hand later works because by
    // then the ghost is long gone - which is exactly why retrying "fixed" it, and why the
    // device is plainly not really shared.
    //
    // Bounded, so a device that is genuinely being driven by someone else still reaches the
    // shared path below after a fifth of a second rather than being waited on for ever.
    // 120 ms, not longer, and the reason is the lock this runs under - see the todo about
    // gb_reconfigure(self)'s scope. Every millisecond spent here is a millisecond the host's main
    // thread can be blocked in getState().
    device_wait_until_idle(info->id, 120);

    self->gPhaseIdle = gb_now_ms();

    bool deviceIsShared = device_is_running_somewhere(info->id);

    if (deviceIsShared && ((settings->rate > 0.0) || (settings->frames > 0))) {
        synthlib_log_line("device '%s' is already running for another client - leaving its rate and"
                 " buffer size alone (asked for %.0f Hz, %u frames). Its buffer is whatever that"
                 " client set, and the ring and reported latency follow it",
                 info->name, settings->rate, settings->frames);
    }

    {
        tGbStatus * status = gb_status(self->statusSlot);

        if (status != NULL) {
            atomic_store(&status->deviceShared, deviceIsShared ? 1 : 0);
        }
    }

    self->gPhaseProps = gb_now_ms();

    // Rate before buffer size: changing the nominal rate can reset the buffer size on some
    // devices, so doing it the other way round silently loses the buffer setting.
    if (!deviceIsShared && (settings->rate > 0.0)) {
        // WHAT IT WAS, so gb_close_capture_locked(self) can hand the device back as it found it.
        // Recorded only when we are actually about to change it, and only for the device we
        // changed - restoring one we never touched would be its own kind of interference.
        if (device_sample_rate(info->id) != settings->rate) {
            self->restoreDevice = info->id;
            self->restoreRate   = device_sample_rate(info->id);
        }
        device_set_sample_rate_and_wait(info->id, settings->rate);
    }

    if (!deviceIsShared && (settings->frames > 0)) {
        uint32_t lowest  = 0;
        uint32_t highest = 0;
        uint32_t wanted  = settings->frames;

        // CLAMPED TO WHAT THE DEVICE ALLOWS. Asking a USB interface for 16 frames is simply
        // refused, and a refusal looks exactly like a device that changed its mind on its own -
        // so ask what it can do and say what happened.
        if (device_buffer_frame_range(info->id, &lowest, &highest)) {
            if (wanted < lowest) {
                synthlib_log_line("device '%s' will not go below %u frames; %u requested",
                         info->name, lowest, settings->frames);
                wanted = lowest;
            } else if ((highest > 0) && (wanted > highest)) {
                wanted = highest;
            }
        }

        if (device_buffer_frames(info->id) != wanted) {
            self->restoreDevice = info->id;
            self->restoreFrames = device_buffer_frames(info->id);
        }
        // The result MATTERS now that it is confirmed rather than assumed - see the note on
        // device_set_buffer_frames(). A false here is a device that had the size asked of it,
        // took the call and never changed, which is what "something else already has it open"
        // looks like from this side.
        if (!device_set_buffer_frames(info->id, wanted) && (self->restoreDevice == info->id)) {
            // Nothing was changed, so there is nothing to hand back on close.
            self->restoreDevice = 0;
            self->restoreFrames = 0;
        }
    }

    double deviceRate = device_sample_rate(info->id);

    if ((deviceRate <= 0.0) || (self->hostRate <= 0.0)) {
        return false;
    }

    uint32_t deviceFrames = device_buffer_frames(info->id);

    // SAID OUT LOUD WHEN IT IS NOT WHAT WAS ASKED FOR. Everything downstream uses the real
    // size - the ring's floor and the device latency both - so the arithmetic was never wrong;
    // what was missing was anyone being told, and a device silently running eight times the
    // requested buffer is most of a plug-in's reported latency.
    if ((settings->frames > 0) && (deviceFrames != settings->frames)) {
        uint32_t canDo   = 0;
        uint32_t canDoTo = 0;
        bool     ranged  = device_buffer_frame_range(info->id, &canDo, &canDoTo);

        // TWO QUITE DIFFERENT CAUSES, and the range tells them apart. If the size asked for is
        // inside what the device says it supports, the driver took the call and ignored it -
        // which is what a device ALREADY OPEN by something else does, because the buffer belongs
        // to whoever opened it first. If it is outside, the driver simply cannot do it.
        bool inRange = ranged && (settings->frames >= canDo)
                       && ((canDoTo == 0) || (settings->frames <= canDoTo));

        // AND IF THE OTHER HOLDER IS ONE OF US, SAY SO BY NAME. A host loads every plug-in
        // into one process, so the status block this instance publishes into is the same array
        // every other GenBridge in the session publishes into - which makes "something else has
        // it open" answerable rather than a shrug. Two instances pointed at one device is an
        // ordinary thing to end up with, and the second one silently inherits the first one's
        // buffer.
        const char * culprit = NULL;

        for (uint32_t slot = 0; slot < GB_STATUS_SLOTS; slot++) {
            tGbStatus * other = gb_status(slot);

            if ((other == NULL) || (slot == (uint32_t)self->statusSlot)
                || !atomic_load(&other->active)) {
                continue;
            }

            if (strcmp(other->deviceName, info->name) == 0) {
                culprit = other->deviceName;
                break;
            }
        }

        synthlib_log_line("asked %s for %u frames and it gave %u - %s. Its range is %u..%u. The ring and "
                 "the reported latency follow the %u, which is why they look large against the "
                 "setting on the panel",
                 info->name, settings->frames, deviceFrames,
                 (culprit != NULL)
                 ? "ANOTHER GenBridge IN THIS HOST ALREADY HAS THIS DEVICE OPEN, and the buffer "
                   "belongs to whoever opened it first - set them both the same, or point them "
                   "at different devices"
                 : (inRange ? "the size is one it says it supports, so something outside this "
                    "host already has the device open and the buffer belongs to whoever opened "
                    "it first"
                    : "outside what its driver will do"),
                 ranged ? canDo : 0, ranged ? canDoTo : 0, deviceFrames);
    }

    self->nominalRatio   = deviceRate / self->hostRate;
    self->deviceLatency  = device_latency_frames(info->id, true);
    self->manualSetpoint = (settings->targetMs > 0.0);
    self->setpointFrames = self->manualSetpoint
                     ? ((settings->targetMs / 1000.0) * deviceRate)
                     : 0.0;
    atomic_store(&self->trimGain, settings->trim);

    // THE DECLARATION IS THE FLOOR, AND THE OBSERVATION ONLY EVER RAISES IT HERE.
    //
    // This used to trust the observation outright, which is wrong at the FIRST open: the burst
    // measurement needs a whole undisturbed callback to be right, and a project load is the
    // least undisturbed moment there is. A busy load separates back-to-back calls by more than
    // half a block, the boundary test reads that as two cycles, and the burst comes out half
    // what it really is.
    //
    // Measured by CT: an Analog Rytm at a 64 frame device buffer came up with setpoint 560,
    // which is 1.25 * (256 + 64 + 128) - a 256 frame callback. Switching the buffer to 128 and
    // back settled it at 880, which is the same arithmetic on the true 512. Only the first
    // value was ever wrong, and it was wrong in the direction that underruns.
    //
    // maxSamplesPerBlock is a contract and is safe to size for; the retune still reclaims the
    // difference once it has watched for a couple of seconds and can be believed.
    // ONCE THE OBSERVATION HAS SETTLED, it is the better number and is used as it stands - that
    // is the whole point of the retune, and a reopen must not throw the reclaimed frames away.
    // While it is still WATCHING, it is a partial count and only ever raises the declaration.
    bool trustObserved = (atomic_load(&self->retuneState) != eRetuneWatching) && (self->observedMaxFrames > 0);

    uint32_t effectiveHostFrames = trustObserved
                                   ? self->observedMaxFrames
                                   : ((self->observedMaxFrames > self->hostMaxFrames) ? self->observedMaxFrames
                                      : self->hostMaxFrames);
    double   minimum              = gb_minimum_setpoint_for(self, effectiveHostFrames, deviceFrames);

    // THE FLOOR IS ADVICE, NOT A LIMIT — for a setting the user typed. Auto still takes it, and
    // still adds the margin; an explicit target is now honoured as given, however low.
    //
    // It is advice because the floor is deliberately conservative and cannot be otherwise: it
    // covers the WORST phase alignment between two unrelated clocks and the LARGEST block the
    // host says it may ever ask for, neither of which is what a given session actually does. A
    // number that pessimistic is worth showing and wrong to impose — clamping silently replaced
    // what the user asked for with a figure they could not see, which reads as the control not
    // working.
    //
    // Safe to allow because the failure is bounded and visible. ring_read() hands the device
    // silence on an underrun and does NOT advance the read cursor, so the loop still sees the
    // true depth and pulls to refill; nothing is corrupted and nothing runs away. The panel
    // already shows fill/setpoint, underruns and resyncs, so too tight a setting reports itself
    // in the one place the user is looking while they choose it.
    self->recommendedSetpoint = minimum * GB_AUTO_MARGIN;

    if (self->setpointFrames <= 0.0) {
        self->setpointFrames = self->recommendedSetpoint;           // auto
    } else if (self->setpointFrames < minimum) {
        synthlib_log_line("setpoint %.0f is below the %.0f frame floor (recommended %.0f) — honouring it;"
                 " watch the underrun count", self->setpointFrames, minimum, self->recommendedSetpoint);
    }

    synthlib_log_line("open %s rate %.0f frames %u ratio %.6f floor %.0f setpoint %.0f devlat %u"
             " -> reported latency %u",
             info->name, deviceRate, deviceFrames, self->nominalRatio, minimum, self->setpointFrames,
             self->deviceLatency,
             (unsigned)((self->setpointFrames + resampler_latency_frames() + (double)self->deviceLatency)
                        / self->nominalRatio));

    uint32_t ringFrames = (uint32_t)(self->setpointFrames * 8.0)
                          + (4 * (deviceFrames + self->hostMaxFrames));

    if (!ring_init(&self->ring, ringFrames, GB_CHANNELS)) {
        return false;
    }

    if (!resampler_init(&self->resampler, GB_CHANNELS, self->nominalRatio, self->hostMaxFrames)) {
        return false;
    }

    self->pullCapacity = (uint32_t)((double)self->hostMaxFrames * self->nominalRatio * 1.5) + (4 * RESAMPLER_TAPS);
    self->pullBuffer   = (float *)calloc((size_t)self->pullCapacity * GB_CHANNELS, sizeof(float));
    self->interleaved  = (float *)calloc((size_t)self->hostMaxFrames * GB_CHANNELS, sizeof(float));
    self->widen        = (float *)calloc((size_t)deviceFrames * 4 * GB_CHANNELS, sizeof(float));

    if ((self->pullBuffer == NULL) || (self->interleaved == NULL) || (self->widen == NULL)) {
        return false;
    }

    tDriftConfig config = drift_default_config();

    drift_init(&self->drift, &config, deviceRate, self->setpointFrames);

    self->gPhaseOpen = gb_now_ms();

    if (!device_open(&self->capture, info->id, true, first, self->captureChannels,
                     deviceFrames * 4, gb_capture_callback, self)) {
        synthlib_log_line("device_open failed for '%s' (%u ch from %u)", info->name, atomic_load(&self->captureChannels),
                 first + 1);
        return false;
    }

    if (!device_start(&self->capture)) {
        return false;
    }

    // COUNTERS BELONG TO THIS OPEN, NOT TO THE INSTANCE. A buffer or rate change tears the
    // device down and rebuilds it, and the rebuild resyncs by design - so carrying the old
    // totals over reports a fault that was actually a setting being changed. Worse, it makes a
    // latency measurement refuse itself, because it checks those same counters to decide
    // whether the capture was clean.
    atomic_store(&self->ring.underflows, 0);
    atomic_store(&self->ring.overflows, 0);
    atomic_store(&self->resyncs, 0);
    self->primed = false;

    atomic_store(&self->needResync, true);
    self->running = true;

    // What is now genuinely in force. The parameter handlers compare against these, so a
    // re-sent value that changes nothing costs nothing.
    self->appliedRate         = deviceRate;
    self->appliedFrames       = deviceFrames;
    self->appliedChannels     = self->captureChannels;
    self->appliedFirstChannel = settings->firstChannel;
    self->openDeviceFrames    = deviceFrames;

    // Whatever was measured for this device and destination before.
    {
        char destination[GB_MIDI_NAME_LEN];

        gb_current_midi_name(self, destination, sizeof(destination));

        const tMeasured * previous = gb_measured_for(self, info->uid, destination, false);

        self->hardwareSamples = (previous != NULL) ? previous->hardwareSamples : 0;

        // ONLY WHEN THE PAIR HAS ACTUALLY CHANGED - see offsetUid/offsetDest for why. A reopen
        // of the same device must leave the live correction alone, or applying it would undo
        // it. A genuinely different device gets that device's own stored value, or zero if it
        // has never been measured: a correction belongs to the rig it was taken from, and
        // carrying it to another one is a whole round trip of error, not a small trim.
        if ((strncmp(self->offsetUid, info->uid, DEVICE_UID_LEN) != 0)
            || (strncmp(self->offsetDest, destination, GB_MIDI_NAME_LEN) != 0)) {
            atomic_store(&self->offsetMs, (previous != NULL) ? previous->offsetMs : 0.0);
            gb_remember_offset_pair(self, destination);
            gb_send_message(self, "gbOffset", (int)lround(atomic_load(&self->offsetMs) * 1000.0));
        }
    }

    // THE OBSERVATION SURVIVES A REOPEN, and this is what stops the retune eating itself.
    //
    // Telling the host its latency changed makes it deactivate and reactivate the plug-in, which
    // reopens the device. Resetting the observation there meant: open wide, watch two seconds,
    // retune, tell the host, get reactivated, open wide again - for ever, with the device torn
    // down and rebuilt every few seconds and the audio in pieces throughout.
    //
    // The real block size is a property of the HOST, not of the device, so once known it stays
    // known and the correct setpoint is used from the first frame. Nothing then changes after
    // activation, so nothing asks the host to restart anything.
    self->observedFrames = 0;

    // NOT ARMED FOR A MANUAL SETPOINT. Retuning exists to claw back latency the host's declared
    // block size overstates, and it does that by REPLACING the setpoint. Against a figure the
    // user typed that is not a saving, it is the plug-in overruling them a couple of seconds
    // after they set it — the same silent overrule the clamp above used to do, arriving late.
    atomic_store(&self->retuneState, ((self->observedMaxFrames > 0) || self->manualSetpoint)
                      ? eRetuneSettled : eRetuneWatching);

    tGbStatus * status = gb_status(self->statusSlot);

    snprintf(status->deviceName, sizeof(status->deviceName), "%s", info->name);
    atomic_store(&status->deviceRate, (int)deviceRate);
    atomic_store(&status->deviceFrames, (int)deviceFrames);
    atomic_store(&status->setpointFrames, self->setpointFrames);
    atomic_store(&status->active, true);

    gb_publish_config_snapshot(self);
    gb_publish_latency_breakdown(self);

    return true;
}

static void gb_close_capture_locked(tGbBridge * self) {
    tGbStatus * status = gb_status(self->statusSlot);

    if (status != NULL) {
        // Cleared, not just marked inactive: a stale device name outlives the device and the
        // panel goes on naming something it is no longer connected to.
        atomic_store(&status->active, false);
        atomic_store(&status->deviceRate, 0);
        atomic_store(&status->latencySamples, 0);
        status->deviceName[0] = '\0';
    }

    if (self->running) {
        device_close(&self->capture);
        ring_free(&self->ring);
        resampler_free(&self->resampler);
        self->running = false;
    }

    // HAND THE DEVICE BACK AS WE FOUND IT.
    //
    // Rate and buffer size are global to the device and nothing put them back, so a device this
    // plug-in had merely PASSED THROUGH was left reconfigured for everything else on the machine
    // - including, when it is the host's own device, the host. Switching away from it therefore
    // did not release it in any meaningful sense.
    //
    // AFTER device_close(), so our own stream is already gone and the device is not being
    // reconfigured underneath a running IOProc of ours. The rate is set without waiting for
    // confirmation: we are on our way out, nothing here depends on it having landed, and the
    // wait costs up to 800 ms of the caller's time - which on this path is a device change the
    // user is waiting on.
    // The snapshot describes a device that is now gone. Cleared here so nothing reports the
    // latency of a bridge that is no longer running.
    self->nominalRatio   = 0.0;
    self->setpointFrames = 0.0;
    self->deviceLatency  = 0;
    gb_publish_config_snapshot(self);

    // NOT WHEN WE ARE ABOUT TO REOPEN THE SAME DEVICE. gb_reconfigure(self) closes and reopens, and
    // handing the buffer back to what it was in between means every reconfigure drives the
    // device 64 -> 512 -> 64 for no one's benefit. On a USB interface each of those costs over
    // a second inside CoreAudio, and between the two the device really IS at 512 - which is what
    // gets read off the panel and reported as "I set 64 and I keep getting 512".
    //
    // The RECORD is kept, so the eventual real close still hands the device back as it was
    // found. Only the pointless middle of a reopen is skipped.
    if ((self->restoreDevice != 0) && (self->restoreDevice == self->keepSettingsFor)) {
        synthlib_log_line("reopening the same device - leaving its buffer alone rather than restoring "
                 "%u frames and setting it straight back", self->restoreFrames);
    } else if (self->restoreDevice != 0) {
        if (self->restoreFrames > 0) {
            device_set_buffer_frames(self->restoreDevice, self->restoreFrames);
        }

        if (self->restoreRate > 0.0) {
            device_set_sample_rate(self->restoreDevice, self->restoreRate);
        }
        synthlib_log_line("restored device settings: %u frames, %.0f Hz", self->restoreFrames, self->restoreRate);

        self->restoreDevice = 0;
        self->restoreFrames = 0;
        self->restoreRate   = 0.0;
    }

    // observedMaxFrames DELIBERATELY SURVIVES A CLOSE. The largest block the host has actually
    // asked for is a property of the HOST, not of the device being closed - so a device change,
    // or the reactivation that telling the host about a latency change itself provokes, must not
    // throw the observation away. Resetting it here is what made a reopen go straight back to
    // the conservative setpoint and re-learn from scratch, which vst3check catches as "a reopen
    // keeps the tuned setpoint". It is cleared in setupProcessing() instead, and only when the
    // host declares a different maximum.
    self->observedFrames = 0;
    atomic_store(&self->retuneState, (self->observedMaxFrames > 0) ? eRetuneSettled : eRetuneWatching);

    free(self->pullBuffer);
    free(self->interleaved);
    free(self->widen);
    self->pullBuffer  = NULL;
    self->interleaved = NULL;
    self->widen       = NULL;
}

// THE RING CANNOT GO TO ZERO, and it is worth being precise about why rather than treating it
// as a tuning knob that happens to bottom out.
//
// The device's callback and the host's process() are driven by different clocks and fire at
// unrelated moments. In the worst phase alignment, process() is called immediately BEFORE the
// device callback that would have supplied its samples - so the ring must already hold a whole
// host block's worth of input, or that call underruns. It must also hold a device block, since
// input arrives in whole blocks and nothing can be consumed from a block that is still being
// filled. The filter needs its taps either side on top.
//
// That floor is a property of block-based audio, not of this design: passing samples straight
// through would mean a host block landing in a gap between device callbacks and getting
// silence. It is also why hostMaxFrames is the host's MAXIMUM block size rather than its usual
// one - the floor has to cover the largest block the host may ever ask for, even if it
// normally asks for far less.
static double gb_minimum_setpoint(tGbBridge * self, uint32_t deviceFrames) {
    return gb_minimum_setpoint_for(self, self->hostMaxFrames, deviceFrames);
}

// TWO NAMES WHERE C++ HAD ONE OVERLOADED, and the shorter one says which frames it means: the
// device's own, as they stand. An overload set is a thing a reader has to resolve; two names are
// not.
static double gb_minimum_setpoint_for_host(tGbBridge * self, uint32_t hostFrames) {
    return gb_minimum_setpoint_for(self, hostFrames, self->openDeviceFrames);
}

static double gb_minimum_setpoint_for(tGbBridge * self, uint32_t hostFrames, uint32_t deviceFrames) {
    return ((double)hostFrames * self->nominalRatio)
           + (double)deviceFrames
           + (2.0 * RESAMPLER_TAPS);
}

static void gb_close_capture(tGbBridge * self) {
    pthread_mutex_lock(&self->configLock);
    gb_close_capture_locked(self);
    pthread_mutex_unlock(&self->configLock);
}
// ════════════════════════════════════════════════════════════════════════════
// THE API THE WRAPPER CALLS
//
// Everything above is the bridge talking to itself. What follows is the whole of its outside edge -
// the calls gbPlugin.c makes, in the order a host makes them - and it is deliberately small. A block
// becomes four calls, a saved state a block of bytes, and nothing in this file has ever heard of
// either plug-in format.
// ════════════════════════════════════════════════════════════════════════════

tGbBridge * gb_bridge_create(bool instrument) {
    tGbBridge * self = (tGbBridge *)calloc(1, sizeof(tGbBridge));

    if (self == NULL) {
        return NULL;
    }

    // WHAT IS NOT ZERO. calloc has done the rest, which is what a C++ member initialiser list was
    // doing one field at a time - and the few below are the ones where zero would be a lie: no
    // status slot yet, a ratio that is a DIVISOR, and the "nothing chosen" that -1 means.
    self->instrument = instrument;
    atomic_store(&self->statusSlot, gb_status_claim());
    atomic_store(&self->snapRatio, 1.0);
    atomic_store(&self->retuneState, eRetuneSettled);
    atomic_store(&self->testNote, GB_MEASURE_NOTE);
    atomic_store(&self->needResync, true);
    atomic_store(&self->trimGain, 1.0f);
    atomic_store(&self->wantedDevice, -1);
    atomic_store(&self->wantedFirstChannel, -1);
    atomic_store(&self->captureChannels, GB_CHANNELS);
    self->blockStartsCycle = true;
    self->measureState     = eMeasureIdle;
    self->appliedDevice    = -1;
    self->hostMaxFrames    = 1024;
    self->nominalRatio     = 1.0;

    if (instrument) {
        gb_midi_init();
    }

    pthread_mutex_init(&self->configLock, NULL);
    pthread_mutex_init(&self->wakeMutex, NULL);
    pthread_cond_init(&self->wakeCond, NULL);

    return self;
}

void gb_bridge_destroy(tGbBridge * self) {
    if (self == NULL) {
        return;
    }
    gb_stop_worker(self);
    gb_close_capture(self);
    gb_status_release(atomic_load(&self->statusSlot));
    free(self->stateBlob);       // the blob the host last asked for; see gb_bridge_state()

    pthread_cond_destroy(&self->wakeCond);
    pthread_mutex_destroy(&self->wakeMutex);
    pthread_mutex_destroy(&self->configLock);

    free(self);
}

void gb_bridge_connect(tGbBridge * self, const tGbHostOps * ops) {
    self->hostOps = *ops;

    // The status slot, announced once. The panel reads the meters and the drift figures straight
    // out of that slot rather than being sent a message per frame.
    gb_send_slot(self);
    synthlib_log_line("connected to controller, published status slot %d", atomic_load(&self->statusSlot));
}

void gb_bridge_disconnect(tGbBridge * self) {
    tGbHostOps none = { NULL, NULL };

    self->hostOps = none;
}

void gb_bridge_watch_devices(tGbBridge * self) {
    // Hot-plug. Nothing noticed a device appearing before this, so a plug-in waiting for a saved
    // interface would have waited until the user touched a control - see device_watch_list().
    device_watch_list(gb_device_list_changed, self);
}

void gb_bridge_unwatch_devices(tGbBridge * self) {
    device_unwatch_list(self);
    gb_close_capture(self);
}

// ACTIVATION OPENS THE DEVICE SYNCHRONOUSLY, and that is the whole reason the host sees a sensible
// latency figure.
//
// A host asks getLatencySamples() shortly after activating a plug-in and then caches the answer; it
// only asks again if told to, via IComponentHandler::restartComponent. Opening the device on the
// worker meant latency was still 0 when Ableton asked, and it reported zero latency for ever after -
// while a test harness that polls until it settles saw the real 2228 and looked perfectly healthy.
// Both were right, which is what made it worth writing down.
//
// setActive is not the audio thread, and it is where a plug-in is expected to do its expensive
// set-up, so a blocking device open belongs here. The worker stays for CHANGES made while running,
// which is where doing it asynchronously actually matters.
void gb_bridge_set_active(tGbBridge * self, bool active) {
    if (active) {
        gb_start_worker(self);
        gb_reconfigure(self);       // synchronous: latency must be known before the host asks
    } else {
        gb_stop_worker(self);
        gb_close_capture(self);
    }
}

// WHETHER THE HOST INTENDS TO RUN US FASTER THAN REALTIME, which it tells us here and nowhere else.
// There is no reciprocal call - a plug-in cannot demand realtime, it can only declare OnlyRT in its
// class subcategories and find out here whether that was honoured. Logged for exactly that reason:
// it is the only evidence of what a host decided.
//
// A bounce in offline mode cannot work. The ring is filled by a device running at one second per
// second, so a host consuming it faster simply drains it, and the render comes out silent or in
// pieces. Nothing in here can fix that; the flag exists so the panel can say so afterwards rather
// than leaving a silent bounce to be puzzled over.
void gb_bridge_setup_processing(tGbBridge * self, double sampleRate, int32_t maxBlockFrames,
                                bool offline) {
    // UNDER THE LOCK. hostRate, hostMaxFrames, observedMaxFrames and observedFrames are all read by
    // the worker while it holds this - gb_minimum_setpoint_for() is built on the first two and
    // gb_retune() on the second two - and the worker is running by the time a host calls this. VST3
    // guarantees the AUDIO thread is stopped here, which is why the same fields are safe to touch
    // from gb_observe_block(); it guarantees nothing about a thread of the plug-in's own.
    gb_lock_config_from_host(self, "setupProcessing");

    self->hostRate = sampleRate;
    atomic_store(&self->eventRate, sampleRate);

    // A DIFFERENT DECLARED MAXIMUM INVALIDATES THE OBSERVATION, and nothing else does. What was
    // learned about one host block size says nothing about another, so this is the one place that
    // forgets it - see gb_close_capture_locked(), which used to.
    // NOT ON THE FIRST CALL, which would throw away a callback size just restored from the project -
    // setState() and setupProcessing() arrive in whichever order the host likes, and this used to
    // clear a good value simply for being the first to see a block size at all. A LATER change of
    // declared size is a genuine reconfiguration and does discard it.
    if ((self->hostMaxFrames != 0) && ((uint32_t)maxBlockFrames != self->hostMaxFrames)) {
        self->observedMaxFrames = 0;
        self->observedFrames    = 0;
        atomic_store(&self->retuneState, eRetuneWatching);
    }
    self->hostMaxFrames = (uint32_t)maxBlockFrames;

    pthread_mutex_unlock(&self->configLock);

    if (offline != atomic_load(&self->offlineRender)) {
        synthlib_log_line("host set process mode %s%s", offline ? "OFFLINE" : "realtime",
                    offline ? " - a bounce in this mode captures nothing, the device runs in real"
                              " time" : "");
    }

    atomic_store(&self->offlineRender, offline);

    tGbStatus * status = gb_status(atomic_load(&self->statusSlot));

    if (status != NULL) {
        atomic_store(&status->offlineRender, offline ? 1 : 0);
    }
}

void gb_bridge_processing_started(tGbBridge * self) {
    // The block timeline starts again with the transport. Carrying it across a stop would only
    // matter for the first block after one - the model is behind real time by then and re-anchors
    // on its own - but starting clean says what is meant.
    self->nextBlockHostTime = 0;
}

uint32_t gb_bridge_latency(tGbBridge * self) {
    // THE PUBLISHED FIGURE, not a fresh computation. A host may call this at any time on any
    // thread; recomputing meant reading setpointFrames, deviceLatency and nominalRatio - all plain
    // fields the worker rewrites under configLock during a device swap - so the answer could be
    // assembled from a half-updated set, and nominalRatio is a DIVISOR: observed as 0 mid-swap it
    // yields inf or NaN, handed straight to the host as a latency.
    return atomic_load(&self->reportedLatency);
}

uint64_t gb_bridge_block_begin(tGbBridge * self, int32_t frames) {
    // BEFORE ANYTHING THAT SENDS, and once. Every MIDI byte this block produces - notes,
    // controllers, the measurement's own note and its panic - is stamped from this one instant, so
    // the whole stream stays in order however the host chops its blocks up.
    //
    // It is deliberately NOT the moment the block is heard - the host's output latency sits in
    // between - but that term cancels here and does not belong in the compensation: the audio comes
    // back through a ring read at the top of the same call, so both ends of the round trip are
    // anchored to the same clock and the difference between them is free of it.
    uint64_t blockHostTime = gb_block_host_time(self, frames);

    // ONLY WHILE THERE IS AUDIO. Blocks handed over before anything opens say nothing about what the
    // ring will have to cover, and a host - or a checker - that drives a burst of them unpaced while
    // nothing is open would leave a fictitious cycle length behind that the retune then treats as
    // settled for the rest of the session.
    //
    // BUT "AUDIO" IS NOT "A DEVICE", and testing self->running here cost 2.0 ms in the host-input
    // mode (CT, 2026-09-09: measured 19.8, correct at 17.8). What the burst observation measures is
    // how the HOST delivers blocks - its own property, as observedMaxFrames' note says - and
    // gb_mean_callback_lead() is built on it. With no device open the observation never ran, the
    // lead came out 0, and the constant bias it exists to remove went straight into every
    // measurement. The comment on that subtraction in gbMeasure.c describes the same symptom on the
    // same synth: "both a KRONOS and an Analog Rytm recorded 4 ms early on exactly that".
    if (gb_capturing(self)) {
        gb_observe_burst(self, frames);
    }

    return blockHostTime;
}

// ONE PARAMETER, ALREADY REDUCED TO ITS LAST POINT. Walking the host's queues is the wrapper's job
// because IParamValueQueue is a C++ interface; deciding what a value MEANS is this one's, and every
// route to a change - the panel, the host's generic control, an automation lane - arrives here.
void gb_bridge_parameter(tGbBridge * self, uint32_t id, double value, int32_t sampleOffset,
                         uint64_t blockHostTime) {
    if ((id >= GB_CC_BASE) && (id < (GB_CC_BASE + GB_CC_COUNT))) {
        if (self->instrument) {
            // The point's own offset, exactly as a note gets its own - a controller move
            // inside a block belongs where the host put it.
            gb_send_controller(self, id, value, gb_host_time_for(self, blockHostTime, sampleOffset));
        }

        return;
    }

    if (id == kParamTrim) {
        atomic_store(&self->trimGain, (float)(value * 2.0));
    } else if (id == kParamDevice) {
        int wanted = gb_device_slot(value);

        if (wanted != atomic_load(&self->wantedDevice)) {
            // THE FIRST ONE AFTER A RESTORE IS THE HOST'S, NOT THE USER'S, and that
            // distinction is the whole fix. A host saves this parameter as a SLOT INDEX, and
            // an index is a position in a list that changes shape the moment a device is
            // unplugged - so the value restored with the project names whatever has moved
            // into that position, which on this machine is a Continuity microphone. Honour
            // the saved UID for that first value and let every later change through: a
            // later change can only have come from the user or from automation, and both
            // are deliberate.
            if (atomic_exchange(&self->deviceParamSeen, true) == true) {
                atomic_store(&self->savedDevicePending, false);
            }
            atomic_store(&self->wantedDevice, wanted);
            gb_request_device(self);       // signals the worker; opens nothing on this thread
        }
    } else if (id == kParamMidiDest) {
        int wanted = (int)(value * (double)(GB_MIDI_SLOTS - 1) + 0.5);

        atomic_store(&self->midiDestination, wanted);
        // NO SHARED BUFFER. This used to snprintf the destination's name into a member
        // char array - on the AUDIO THREAD - which getState() then read from the host's
        // thread and gb_parse_state(self) wrote from it. The name is derivable from the atomic
        // above wherever it is actually wanted (gb_current_midi_name(self)), so the buffer that
        // was being raced over simply does not need to exist.
    } else if (id == kParamMidiChannel) {
        atomic_store(&self->midiChannel, (int)(value * (double)(GB_CHANNEL_SLOTS - 1) + 0.5));
    } else if (id == kParamTestNote) {
        int note = (int)((value * 127.0) + 0.5);

        atomic_store(&self->testNote, (note < 0) ? 0 : ((note > 127) ? 127 : note));
        return;
    } else if (id == kParamOffsetMs) {
        atomic_store(&self->offsetMs, GB_OFFSET_MIN_MS + (value * (GB_OFFSET_MAX_MS - GB_OFFSET_MIN_MS)));

        // The correction is part of the reported figure, so the host has to be told - but
        // NOT on this click. See GB_OFFSET_SETTLE_MS: the worker waits for the value to stop
        // moving and then tells it once, because each telling costs the host a full delay
        // compensation pass and this control is dialled in a dozen clicks at a time.
        //
        // Nothing is computed here. What the latency becomes depends on state the worker
        // owns, and this runs on the audio thread before process() has even taken its
        // trylock, so the whole decision belongs on the other side of the queue.
        atomic_store(&self->lastOffsetChangeMs, gb_now_ms());
        atomic_store(&self->offsetDirty, true);
        gb_wake_worker(self);
    } else if (id == kParamSource) {
        int wanted = (value < 0.5) ? GB_SOURCE_DEVICE : GB_SOURCE_HOST;

        if (wanted != atomic_load(&self->captureSource)) {
            atomic_store(&self->captureSource, wanted);

            // THE WORKER DOES THE REST. Going to Host input has to CLOSE whatever device is open -
            // leaving it running would hold hardware nobody is listening to - and coming back has to
            // open one again. Both are gb_reconfigure()'s job, and it is debounced like every other
            // route to it.
            gb_request_device(self);
        }
    } else if (id == kParamRate) {
        int index = (int)(value * (double)(gGbRateCount - 1) + 0.5);

        if (gGbRates[index] != atomic_load(&self->wantedRate)) {
            atomic_store(&self->wantedRate, gGbRates[index]);
            gb_request_device(self);
        }
    } else if (id == kParamMode) {
        int wanted = (value < 0.5) ? 1 : 2;

        if ((uint32_t)wanted != self->captureChannels) {
            atomic_store(&self->wantedChannels, wanted);
            gb_request_device(self);
        }
    } else if (id == kParamFirstChannel) {
        int wanted = (int)(value * (double)(GB_MAX_FIRST_CHANNEL - 1) + 0.5);

        atomic_store(&self->wantedFirstChannel, wanted);
        gb_request_device(self);
    } else if (id == kParamFrames) {
        int index = (int)(value * (double)(gGbFrameCount - 1) + 0.5);

        if (gGbFrames[index] != atomic_load(&self->wantedFrames)) {
            atomic_store(&self->wantedFrames, gGbFrames[index]);
            gb_request_device(self);
        }
    }
}

// THE MEASURE BUTTON, which needs both halves of what the host delivered - see gbBridge.h.
void gb_bridge_measure_trigger(tGbBridge * self, bool sawPress, bool held) {
    // TRIGGER FROM ANY POINT, BUT ARM FROM THE LAST ONE. Both halves matter and they are not the
    // same question: the press has to be found wherever it lands in the block, while whether the
    // button is still DOWN is whatever it settled at. Arming from "was there a press" latched the
    // flag true - the editor's own release arrives in the same block - so the button worked exactly
    // once and then never again.
    if (sawPress && !self->measureArmed && (self->measureState == eMeasureIdle)) {
        // NOT LOGGED FROM HERE. synthlib_log_line() opens and closes the file on every call, and this is
        // the audio thread - three syscalls in the middle of a 2.7 ms block, at the exact moment a
        // measurement is about to start. It showed up as a resync during the run and the run then
        // discarded itself for not being clean: a measurement failing because of the act of
        // measuring. The worker logs it.
        gb_start_measurement(self);
    }

    self->measureArmed = held;
}

// THE DAW PLAYS THE HARDWARE. Note data arrives as VST3 events and leaves as MIDI bytes on a
// CoreMIDI destination; the audio comes back through the same capture path the effect uses. That
// round trip is what makes this an instrument rather than a recorder.
//
// Sent straight from the audio thread, not queued. MIDISend is not strictly real-time safe, but the
// alternative - handing the bytes to another thread - adds exactly the jitter that makes a hardware
// synth feel loose, and every plug-in that drives external gear makes the same trade.
void gb_bridge_note(tGbBridge * self, tGbNoteKind kind, int16_t channel, int16_t pitch,
                    float value, int32_t sampleOffset, uint64_t blockHostTime) {
    int     destination = atomic_load(&self->midiDestination);
    uint8_t message[3];

    if (kind == eGbNoteOn) {
        int velocity = (int)((value * 127.0f) + 0.5f);

        // A note-on at zero velocity is a note-off on the wire, and some devices treat the two
        // differently. Sending a real note-off is the safer of the two.
        if (velocity <= 0) {
            message[0] = (uint8_t)(0x80 | gb_channel_for(self, channel));
            message[1] = (uint8_t)(pitch & 0x7F);
            message[2] = 64;
        } else {
            message[0] = (uint8_t)(0x90 | gb_channel_for(self, channel));
            message[1] = (uint8_t)(pitch & 0x7F);
            message[2] = (uint8_t)((velocity > 127) ? 127 : velocity);
        }
    } else if (kind == eGbNoteOff) {
        int velocity = (int)((value * 127.0f) + 0.5f);

        message[0] = (uint8_t)(0x80 | gb_channel_for(self, channel));
        message[1] = (uint8_t)(pitch & 0x7F);
        message[2] = (uint8_t)((velocity < 0) ? 0 : ((velocity > 127) ? 127 : velocity));
    } else {
        message[0] = (uint8_t)(0xA0 | gb_channel_for(self, channel));
        message[1] = (uint8_t)(pitch & 0x7F);
        message[2] = (uint8_t)((value * 127.0f) + 0.5f);
    }

    // STAMPED WITH ITS OWN OFFSET, not sent on arrival. See gb_midi_send_at(): a block's worth of
    // notes fired at the block boundary is a bias of half a buffer, always early, and it grows with
    // every increase in the host's buffer size.
    //
    // COUNTED, BOTH SIDES. "Notes are not getting through" has three quite different causes - the
    // host is not delivering them, the plug-in is dropping them, or the send is failing - and from
    // outside they look identical. The panel shows in/out, so one glance says which half to look at.
    atomic_fetch_add(&self->eventsIn, 1);

    if (gb_midi_send_at(destination, message, 3,
                        gb_host_time_for(self, blockHostTime, sampleOffset))) {
        atomic_fetch_add(&self->eventsOut, 1);
    }
}

// ── Capturing from the HOST rather than from a device ───────────────────────
//
// The External-Instrument mode: Live's own interface input arrives on the plug-in's side-chain, and
// all this has to do is hand it back. No ring, no resampler, no drift loop - the host's input and
// its output are the same clock, and reconciling two clocks is the only reason any of that exists.
//
// SILENCE IS THE HONEST ANSWER when the host has routed nothing. A user who has chosen Host input
// and connected nothing should hear nothing, not whatever a device left in the ring.
static void gb_render_from_host(tGbBridge * self, float ** out, const float * const * in,
                                int32_t inChannels, int32_t frames, uint64_t blockHostTime) {
    float trim = atomic_load(&self->trimGain);

    for (int c = 0; c < GB_CHANNELS; c++) {
        // A MONO SOURCE FEEDS BOTH SIDES, the same rule the device path applies on the way into the
        // ring - see the widen buffer in gb_capture_callback().
        const float * source = ((in != NULL) && (inChannels > 0))
                               ? in[(c < inChannels) ? c : (inChannels - 1)] : NULL;

        if (source == NULL) {
            memset(out[c], 0, (size_t)frames * sizeof(float));
            continue;
        }

        for (int32_t i = 0; i < frames; i++) {
            out[c][i] = source[i] * trim;
        }
    }

    // WAS THERE ANYTHING IN IT? A cheap test with an early exit, because the answer is the one
    // thing the panel cannot work out for itself and the one thing a user needs told - see
    // hostInputPresent in gbStatus.h.
    bool audible = false;

    for (int c = 0; (c < GB_CHANNELS) && !audible; c++) {
        for (int32_t i = 0; i < frames; i++) {
            if ((out[c][i] > 1.0e-6f) || (out[c][i] < -1.0e-6f)) {
                audible = true;
                break;
            }
        }
    }

    if (audible) {
        self->hostSilentFrames = 0;
    } else {
        self->hostSilentFrames += (uint64_t)frames;
    }

    // THE MEASUREMENT IS UNCHANGED, AND THAT IS THE POINT. It reads the OUTPUT buffer, which now
    // holds the host's input, so it times exactly the same round trip - note out, synth, interface,
    // host, here - without knowing the capture path underneath it has gone.
    gb_run_measurement(self, out, frames, blockHostTime, 0.0);

    tGbStatus * status = gb_status(atomic_load(&self->statusSlot));

    if (status != NULL) {
        // NO RING, SO NO RING FIGURES. A published zero would read as a starved buffer rather than
        // an absent one; the panel greys these, and these are the values it greys.
        atomic_store(&status->fillFrames, 0.0);
        atomic_store(&status->actualSamples,
                     (int)((atomic_load(&self->offsetMs) / 1000.0) * self->hostRate));
        atomic_store(&status->measureTripNow, self->measureTrip);
        atomic_store(&status->measurePhase, (int)self->measureState);

        // TWO SECONDS, so a genuine gap between notes does not read as a broken routing while a
        // routing that never carries anything says so quickly enough to be useful.
        atomic_store(&status->hostInputPresent,
                     ((double)self->hostSilentFrames < (self->hostRate * 2.0)) ? 1 : 0);
    }
    gb_publish_status(self, out, frames, 0.0);
}

void gb_bridge_render(tGbBridge * self, float ** out, const float * const * in, int32_t inChannels,
                      int32_t frames, uint64_t blockHostTime) {
    // THE HOST-INPUT MODE TAKES NO LOCK AND NEEDS NONE. It touches no ring, no resampler and no
    // device - nothing the worker can be rebuilding underneath it - so the trylock below, and the
    // block of silence a failed trylock produces, would be cost for nothing.
    if (atomic_load(&self->captureSource) == GB_SOURCE_HOST) {
        gb_render_from_host(self, out, in, inChannels, frames, blockHostTime);
        return;
    }

    // TRYLOCK, NEVER LOCK. The worker holds this while it tears down and rebuilds the ring,
    // the resampler and the device - during which none of them may be touched. Blocking here
    // would stall the host's audio thread on a CoreAudio device open, which is exactly the
    // kind of thing that makes a DAW drop out. Failing to acquire it means a device change is
    // in flight, and a block of silence is the right answer.
    if (pthread_mutex_trylock(&self->configLock) != 0) {
        gb_silence(out, frames);
        return;
    }

    if (!self->running) {
        gb_silence(out, frames);
        pthread_mutex_unlock(&self->configLock);
        return;
    }

    // Same start-up and recovery rule as the command line bridge: hold silence until there is
    // a setpoint's worth to snap to, then resync so the loop opens with zero error.
    if (atomic_load(&self->needResync)) {
        if (ring_fill(&self->ring) < (uint64_t)self->setpointFrames) {
            gb_silence(out, frames);
            pthread_mutex_unlock(&self->configLock);
            return;
        }

        ring_resync(&self->ring, (uint32_t)self->setpointFrames);
        resampler_reset(&self->resampler);
        drift_reset(&self->drift);
        atomic_store(&self->needResync, false);

        // The first one is the prime, not a fault. Anything after it means the loop lost the
        // buffer and had to be rescued, which is exactly what should never happen.
        if (self->primed) {
            atomic_fetch_add(&self->resyncs, 1);
        }

        self->primed = true;
    }

    gb_observe_block(self, frames);

    double fill       = (double)ring_fill(&self->ring);
    double interval   = (double)frames / self->hostRate;
    double correction = drift_update(&self->drift, fill, interval);
    double ratio      = self->nominalRatio * (1.0 + correction);

    uint32_t needed = resampler_needed(&self->resampler, (uint32_t)frames, ratio);

    if (needed > self->pullCapacity) {
        needed = self->pullCapacity;
    }

    if (needed > 0) {
        if (!ring_read(&self->ring, self->pullBuffer, needed)) {
            atomic_store(&self->needResync, true);
        }

        resampler_push(&self->resampler, self->pullBuffer, needed);
    }

    resampler_process(&self->resampler, self->interleaved, (uint32_t)frames, ratio);

    float trim = atomic_load(&self->trimGain);

    for (int32_t i = 0; i < frames; i++) {
        for (int c = 0; c < GB_CHANNELS; c++) {
            out[c][i] = self->interleaved[(i * GB_CHANNELS) + c] * trim;
        }
    }

    gb_run_measurement(self, out, frames, blockHostTime, fill);

    {
        tGbStatus * status = gb_status(self->statusSlot);

        if (status != NULL) {
            // WITH the callback lead, unlike the measurement's own subtraction. This figure
            // answers "how far behind the audio is where the host finally puts it", and the
            // frames already handed over inside this callback are part of that distance. The
            // measurement subtracts a round trip that is invariant to them - see the note on
            // gb_latency_frames_measured(self).
            double actual = gb_latency_frames_measured(self, fill, self->blockActualHostTime)
                            + (double)self->burstBefore;

            if (self->instrument) {
                actual += (atomic_load(&self->offsetMs) / 1000.0) * self->hostRate;
            }

            atomic_store(&status->actualSamples, (int)actual);

            // THE SAME NUMBER THE HOST WILL BE TOLD, averaged. Seeded rather than eased in from
            // zero: a fresh device would otherwise report a latency climbing out of nothing for
            // its first second, and every step of that is a compensation pass.
            double instant = actual - ((atomic_load(&self->offsetMs) / 1000.0) * self->hostRate);
            double prior   = atomic_load(&self->pipelineAvg);

            // BOUNDED BY THE ESTIMATE IT REPLACED, and this is not belt and braces.
            //
            // A measured figure can be wrong in ways a constant cannot. The ring's occupancy is
            // read live, the frames-so-far-this-callback depend on a cycle boundary being
            // detected correctly, and either can be disturbed - a mis-detected boundary
            // accumulates a "callback" of arbitrary length, and a ring that has just been
            // primed or snapped reads full. The result reached a host as 41 ms of plug-in
            // latency where the setpoint says 20, which is far worse than the 4-9 ms error this
            // whole exercise set out to remove.
            //
            // The pipeline genuinely cannot sit more than about one callback either side of
            // what the setpoint implies - the drift loop holds it there and the burst explains
            // the rest - so anything outside that band is a fault, not a measurement.
            double estimate = gb_latency_frames_for_fill(self, self->setpointFrames);
            double allow    = (double)((self->observedMaxFrames > 0) ? self->observedMaxFrames : self->hostMaxFrames)
                              + ((double)self->openDeviceFrames / ((self->nominalRatio > 0.0) ? self->nominalRatio : 1.0));

            if (instant > (estimate + allow)) {
                instant = estimate + allow;
            } else if (instant < (estimate - allow)) {
                instant = (estimate > allow) ? (estimate - allow) : estimate;
            }

            atomic_store(&self->pipelineAvg, (prior > 0.0)
                              ? (prior + ((instant - prior) * GB_PIPELINE_SMOOTH))
                              : instant);

            // NOTHING IS TOLD TO THE HOST FROM HERE ANY MORE. This used to raise a flag when
            // the measured pipeline drifted past the deadband, which is what drove the
            // reconfigure loop above: a latency change is an instruction to the host to
            // reactivate us. The measurement is for the panel and the log now, and the figure
            // the host is given changes only when something structural does - a device, a rate,
            // a buffer, a retune, the trim.
            atomic_store(&status->measureTripNow, self->measureTrip);
            atomic_store(&status->measurePhase, (int)self->measureState);
        }
    }
    gb_publish_status(self, out, frames, fill);

    pthread_mutex_unlock(&self->configLock);

    return;
}
