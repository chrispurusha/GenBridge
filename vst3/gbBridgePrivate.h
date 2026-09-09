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

#ifndef GB_BRIDGE_PRIVATE_H
#define GB_BRIDGE_PRIVATE_H

// THE BRIDGE'S OWN STATE, AND DELIBERATELY NOT IN gbBridge.h.
//
// The wrapper holds a tGbBridge * and never looks inside it, which is what lets this file use
// C11 _Atomic - a keyword C++ does not have. That is not a technicality to work around: the whole
// point of the split is that everything below is plain C that a C compiler checks, and the only
// code that has to speak C++ is the part that implements COM interfaces.
//
// Included by gbBridge.c, gbMeasure.c and gbState.c - the three files that make up the bridge -
// and by nothing else.

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <CoreAudio/CoreAudio.h>

#include "device.h"
#include "drift.h"
#include "gbBridge.h"
#include "gbMidi.h"
#include "gbParams.h"
#include "resampler.h"
#include "ring.h"

// ── The tunings ─────────────────────────────────────────────────────────────

// THE SETPOINT IS DERIVED, NOT CHOSEN. A fixed default in milliseconds is the wrong shape for this
// number: the floor below which the ring cannot go depends on the host's block size, the device's
// block size and the rate ratio, so any constant is either needlessly large on one rig or unsafe on
// another. A per-device targetMs of 0 means "work it out", which is the default; a non-zero value
// is an explicit override and is still clamped up to the floor.
#define GB_TARGET_AUTO    (0.0)

// Headroom above the theoretical floor. The floor already covers the worst phase alignment between
// the two callbacks; this covers scheduling jitter - a device callback that runs late - which is
// not bounded by anything we control. 25% of a few hundred frames is a millisecond or two, which is
// cheap next to an audible dropout.
#define GB_AUTO_MARGIN    (1.25)

// How long to watch the host's real block size before trusting it, and how much has to be on the
// table before disturbing the host's delay compensation to claim it.
#define GB_SETTLE_SECONDS    (2.0)

// HOW LONG A DEVICE/RATE/FRAMES CHANGE MUST STAND STILL BEFORE IT IS ACTED ON.
//
// Every one of those settings is a stepper, so moving two places sends two values, and each value
// used to mean a full teardown and rebuild - closing the device, opening whatever slot was passed
// over, then closing it again on the next press. Stepping past a device OPENED it, which is the very
// thing "nothing opens until explicitly chosen" exists to prevent; on this rig slot 0 is an iPhone
// Continuity microphone, so stepping down from a mixer woke it in passing.
//
// A quarter of a second is longer than a person's gap between arrow presses and far shorter than the
// open it defers - an open can spend 800 ms in device_set_sample_rate_and_wait() alone - so a burst
// collapses to one device change and a single deliberate change is not perceptibly slower.
//
// This lives in the PROCESSOR rather than in the editor on purpose. A drop-down would stop the
// panel's own arrows walking the list, but the host's generic panel and any automation lane can
// still sweep the parameter, and they reach this code by the same path.
#define GB_DEVICE_SETTLE_MS  (250.0)

// HOW LONG THE OFFSET MUST STAND STILL BEFORE THE HOST IS TOLD.
//
// Telling the host its latency moved makes it redo delay compensation across the whole session, and
// in Ableton that is a visible hitch - so it is the one thing that must not happen once per click.
// The offset steps 0.1 ms at a time and is dialled in by ear, which means a dozen or more clicks in
// quick succession: exactly the shape that turns a cheap control into a stuttering one.
//
// The VALUE still moves immediately, so the readout follows the pointer and nothing feels laggy.
// Only the notification waits. Slightly longer than the device settle because this is a control
// someone nudges repeatedly while listening, rather than one they set once.
#define GB_OFFSET_SETTLE_MS  (400.0)
#define GB_RETUNE_MIN_GAIN   (64.0)      // frames

// How long to listen for the note before giving up, and how far above the noise floor counts as an
// onset.
#define GB_MEASURE_TIMEOUT_S  (1.5)
#define GB_MEASURE_FLOOR_S    (0.15)

// Time to let a previous note decay before listening for silence. Without it a second measurement
// starts while the first one's note is still sounding: the floor is taken from a decaying tail, or
// the tail itself trips the threshold, and the answer comes back as zero. Measuring twice in a row
// is the normal thing to do, so it has to survive it.
#define GB_MEASURE_SETTLE_S   (0.35)

// The longest the settle will wait for the previous note to decay before giving up on quiet and
// taking the floor anyway. A synth that never goes quiet is a measurement that never happens.
#define GB_MEASURE_SETTLE_MAX_S    (2.5)

// One-pole coefficient for the reported pipeline delay, at roughly two seconds of time constant
// over 128-frame blocks. Slow enough to ignore the ring's sawtooth, quick enough to follow a
// device change without the panel looking stuck.
#define GB_PIPELINE_SMOOTH    (0.0013)

// NOTHING USES THIS, AND THE CONVERSION TO C IS WHAT SAID SO (2026-09-09). It sized a deadband on
// the reported latency, applied by a latency_worth_reporting() that no caller ever had: C++ does not
// warn about an unused private member function, and C warns about an unused static one, so eleven
// lines of dead code came out of the move. The function is gone; the number stays because it is the
// answer to a question that can come back.
//
// It became moot on 2026-09-08, when the reported figure went back to being the SETPOINT rather than
// the smoothed measurement - see gb_snapshot_latency_frames(). A setpoint only moves when something
// structural does, so every change is worth telling the host about and there is nothing to filter.
// If the measured pipeline is ever reported again, this is the deadband it will need: 64 frames is
// 1.3 ms at 48 kHz, below what the offset control is ever dialled in to correct.
#define GB_LATENCY_DEADBAND   (64)

// A sanity ceiling on a callback size read back out of a project file. Nothing sane hands over a
// third of a second in one go, and a corrupt or hand-edited value must not be able to ask for a
// ring measured in seconds.
#define GB_MAX_BLOCK_FRAMES   (16384)

// The onset threshold is RELATIVE to whatever the input is already doing, with an absolute floor
// under it. A synth with a hissy output, a hum, or a pad still decaying would sit above any fixed
// level and trip the detector the instant the note went out. Measuring the quiet first and then
// demanding a multiple of it is what makes the answer mean something.
#define GB_MEASURE_MARGIN     (0.02f)    // absolute minimum rise, for a genuinely silent input
#define GB_MEASURE_RATIO      (8.0f)     // ...or this much above the noise, whichever is greater
#define GB_MEASURE_CEILING    (0.70f)    // never demand more than this; see the note in the code
#define GB_MEASURE_CONFIRM    (2)        // consecutive blocks required, so one glitch is not an onset

// SEVERAL ROUND TRIPS, NOT ONE, because a single one is not a measurement of anything repeatable.
// USB MIDI transit, the synth's own scheduler and its envelope all move the onset a little from one
// note to the next, and a figure taken from one note carries all of it. Five is enough to throw
// away the extremes and still average three.
#define GB_MEASURE_TRIPS      (5)

// TRIMMED, NOT AVERAGED FLAT. One trip landing on a resync, or on a note the synth happened to
// voice-steal, would drag a plain mean by its whole error; dropping the highest and lowest first
// costs nothing when they are honest and removes the outlier when they are not.
#define GB_MEASURE_DROP       (1)

// Remembered per audio device AND per MIDI destination. The same synth answers differently over USB
// than over DIN, and two different synths on one interface are not comparable at all - so the pair
// is the key, not either half of it.
#define GB_MAX_MEASURED       (32)

// measureLatency sentinels. Negative means no usable figure; they are told apart so the panel and
// the log can say WHICH kind of nothing came back, which is the difference between "the synth is on
// the wrong channel" and "the onset arrived before our own buffering could have delivered it".
#define GB_MEASURE_TIMED_OUT  (-1)
#define GB_MEASURE_TOO_EARLY  (-2)

#define GB_DEFAULT_FRAMES     (128)
#define GB_DEFAULT_RATE       (48000.0)
#define GB_MAX_REMEMBERED     (32)

// ── The tables the state blob carries ───────────────────────────────────────

// ONE ENTRY PER (AUDIO DEVICE, MIDI DESTINATION) PAIR, because that pair is what a round trip is a
// property of. Two figures, and the difference matters:
//
//   hardwareSamples  what the last measurement actually returned. A record, never edited.
//   offsetMs         the correction IN FORCE, which is what report_latency() adds. A measurement
//                    seeds it; the panel's +/- moves it from there.
//
// Splitting them is what lets the panel show "measured 4.6, using 4.8" - and it means re-measuring
// replaces the reading and the value together, while a nudge moves only the value.
typedef struct {
    char     audioUid[DEVICE_UID_LEN];
    char     midiDest[GB_MIDI_NAME_LEN];
    uint32_t hardwareSamples;    // the round trip MINUS whatever the plug-in was contributing
    double   offsetMs;           // seeded from the measurement, then adjusted by hand
} tMeasured;

// Per device settings, remembered across sessions and across device changes within a session.
//
// Switching away from a device and back should not lose how it was set up - a 32 channel drum
// module and a stereo synth want completely different buffer sizes and channel pairs, and having
// to redial them every time is the sort of friction that makes a plug-in annoying rather than
// broken. So the state carries a small table keyed by device UID, not just the active device.
typedef struct {
    char     uid[DEVICE_UID_LEN];
    uint32_t frames;          // device buffer frames; 0 means "leave the device as it is"
    double   rate;            // nominal sample rate to request; 0 means "leave the device as it is"
    double   targetMs;        // ring setpoint
    uint32_t firstChannel;    // first device channel to take
    uint32_t captureChannels; // 1 for mono, 2 for a stereo pair
    float    trim;
} tDeviceSettings;

// The block-size observation, and where it has got to.
typedef enum { eRetuneWatching = 0, eRetuneRequested, eRetuneSettled, eRetuneBlocked } tRetuneState;

typedef enum { eMeasureIdle = 0, eMeasureSettle, eMeasureFloor, eMeasureListening } tMeasureState;

struct tGbBridge {
    // WHERE THE WRAPPER IS REACHED, and the only route back to it. Everything the bridge needs to
    // tell the host - a device slot it resolved for itself, a latency that moved - goes out through
    // these, because only the controller holds an IComponentHandler and only the wrapper can make
    // an IMessage. See gbBridge.h.
    tGbHostOps          hostOps;

    // Assigned when the host connects the two ends (its own thread) and read from the audio thread
    // by publish_status() on every block.
    _Atomic int            statusSlot;
    // Written by whichever thread last told the host, read by getLatencySamples() on any thread -
    // see its comment. Atomic because those are genuinely different threads, not for ordering.
    _Atomic uint32_t       reportedLatency;

    // ---- THE PUBLISHED SNAPSHOT --------------------------------------------------------------
    //
    // hostRate, nominalRatio, setpointFrames and deviceLatency are plain fields, written by the
    // worker under configLock while it swaps a device. Several readers need them and CANNOT take
    // that lock: process() must never block, and the panel publisher is called from paths that
    // already hold it. Reading the raw fields from there is a data race, and not a harmless one -
    // nominalRatio is a divisor, so observing the 0 it briefly holds mid-swap yields inf or NaN.
    //
    // Making `running` atomic fixed the FLAG and not the state it gates, which is the more dangerous
    // shape: the obvious warning sign disappears while the composite read stays broken. So the
    // derived values get published as atomics, once, by the thread that changed them - and every
    // lock-free reader uses these and never the fields behind them.
    // THE HOST RATE, READABLE WITHOUT THE LOCK. snapHostRate is the config snapshot's copy and is 0
    // until a capture device opens, but events are forwarded whether one is open or not - the DAW
    // still plays the hardware. Written in setupProcessing() and read by forward_events(), which
    // runs before the trylock.
    _Atomic double         eventRate;

    // THE PIPELINE'S DELAY, SMOOTHED - and it is what the host is told, rather than the setpoint
    // the ring is aimed at.
    //
    // The two are not the same thing and the difference is audible. The reported figure used to be
    // built from setpointFrames, on the reasonable ground that a latency moving every block would
    // have the host redo delay compensation continuously. But the ring only sits AT its setpoint on
    // average and, on a bursty host, sits below it: measured on this rig, told 2679 against an
    // actual 2221-2506, a gap of 4 to 9 ms. A host compensating by 9 ms for a 5 ms delay records
    // everything 4 ms early, which is exactly the report that led here - and no amount of measuring
    // the SYNTH can fix a figure that is wrong about US.
    //
    // A two-second average, so it follows the ring without chasing its sawtooth, and a deadband
    // before the host is told (see latency_worth_reporting) so a figure that now moves does not
    // cost a compensation pass every block.
    _Atomic double         pipelineAvg;

    // Note events taken from the host, and note events that reached a MIDI destination. See the
    // note where they are counted.
    _Atomic uint32_t       eventsIn;
    _Atomic uint32_t       eventsOut;

    // AUDIO THREAD ONLY, and it is the whole state of the block timeline: the wall time at which
    // the frames handed over so far run out. See block_host_time().
    uint64_t               nextBlockHostTime;

    // The unmodelled clock, read at the top of the current block, and the model's answer for the
    // same block. Their difference is the frames already handed over inside this callback. AUDIO
    // THREAD ONLY.
    uint64_t               blockActualHostTime;
    uint64_t               blockHostTimeNow;

    // Whether the current call opened a new audio callback, and what the previous call was, which
    // is how that is decided. AUDIO THREAD ONLY.
    bool                   blockStartsCycle;
    uint64_t               lastCallHostTime;
    uint32_t               lastCallFrames;

    // Frames handed over so far inside the current callback, and how many of them came BEFORE the
    // current call. The second is the one the latency arithmetic wants, and it has to be counted
    // rather than inferred from the model: the model legitimately runs ahead of the clock across
    // callbacks as well as within them, so model-minus-clock grows without bound and is not this.
    // AUDIO THREAD ONLY.
    uint32_t               burstFrames;
    uint32_t               burstBefore;

    _Atomic double         snapHostRate;
    _Atomic double         snapRatio;
    _Atomic double         snapSetpoint;
    _Atomic uint32_t       snapDeviceLatency;

    // The block-size observation, and where it has got to.
    _Atomic tRetuneState   retuneState;
    _Atomic bool           revertWanted;

    // What the conservative floor would have picked, and whether the setpoint in force came from
    // the user rather than from it. Both are written under configLock on the open path.
    double                 recommendedSetpoint;
    bool                   manualSetpoint;
    _Atomic int            midiDestination;
    _Atomic bool           offlineRender;
    _Atomic double         offsetMs;
    _Atomic int            midiChannel; // 0 = whatever the note arrived on

    tMeasureState          measureState;
    uint32_t               measureFrames;

    // Whether the floor phase has reached its final third, where the hold restarts. AUDIO THREAD.
    bool                   measureFloorLate;
    uint32_t               measureOnsetFrames;

    // The ring occupancy at the block the onset was found in, in DEVICE frames - what the sound
    // actually came through, as against the setpoint it is supposed to sit at. See run_measurement().
    double                 measureOnsetFill;

    // The host-clock instant the test note was SCHEDULED for, and therefore the instant it left.
    // AUDIO THREAD ONLY.
    uint64_t               measureNoteTime;

    // Our own share of the round trip, as it stood at the onset block. AUDIO THREAD ONLY.
    double                 measureOnsetOurs;

    // The round trips of one Measure press, and where the current one is up to. AUDIO THREAD ONLY.
    int                    measureTrips[GB_MEASURE_TRIPS];
    int                    measureTrip;
    uint32_t               measureTripUnderruns;
    int                    measureTripResyncs;

    // How many trips survived to be averaged, and what the extremes were. Published because an
    // average on its own hides the thing worth knowing: five readings within a millisecond mean the
    // number can be trusted, and the same average from readings 9 ms apart means it cannot. The
    // panel shows the range beside the figure for exactly that reason.
    _Atomic int            measureTripsUsed;
    _Atomic int            measureTripLow;
    _Atomic int            measureTripHigh;
    _Atomic int            measureTripSpread;

    // Which note the measurement plays. A drum machine may have nothing on middle C at all - an
    // Analog Rytm wants the lowest note there is - so this is settable and saved.
    _Atomic int            testNote;
    int                    measureConfirm;
    float                  measurePeak;
    float                  measureFloor;
    bool                   measureArmed;
    _Atomic int            measureLatency;
    _Atomic bool           measureStore;

    // Raised by the audio thread when Measure is pressed; the worker sends the All Notes Off and
    // writes the log line, neither of which belongs in a process() call.
    _Atomic bool           measurePanic;
    _Atomic bool           latencyDirty;

    // Raised by the audio thread when the measured pipeline has drifted past the deadband; the
    // worker decides what to do about it.
    _Atomic bool           latencyStale;

    // Phase marks inside one reconfigure, for the lock-hold breakdown. Worker thread only.
    double                 gPhaseIdle;
    double                 gPhaseProps;
    double                 gPhaseOpen;

    // The offset moved and the host has not been told yet, and when it last moved - see
    // GB_OFFSET_SETTLE_MS.
    _Atomic bool           offsetDirty;
    _Atomic double         lastOffsetChangeMs;
    _Atomic int            measureOnset;
    _Atomic int            measureOurs;

    // The ring occupancy the onset actually came through, published so the log can show it beside
    // the setpoint it is meant to be sitting at. A run that disagrees with the others usually
    // disagrees here first.
    _Atomic double         measureFillSeen;
    _Atomic float          measureTriggerPeak;
    _Atomic float          measureFloorSeen;
    // Written by start_measurement() on the AUDIO thread (it runs from the parameter pass, before
    // process() takes its trylock) and read by store_measurement() on the worker, which subtracts
    // them to decide whether a measurement was clean. Two threads, so not plain ints.
    _Atomic uint32_t       measureUnderrunsAtStart;
    _Atomic int            measureResyncsAtStart;

    tMeasured              measured[GB_MAX_MEASURED];
    uint32_t               measuredCount;

    // WHICH PAIR THE LIVE offsetMs IS FOR. Without this the correction could not survive its own
    // application: changing the reported latency makes the host reactivate the plug-in, which
    // reopens the device, and a reopen that re-seeded the offset from the table would undo every
    // nudge the moment it took effect. Re-seeding is now gated on the pair actually having changed,
    // so reopening the same device leaves the value exactly where the user put it.
    char                   offsetUid[DEVICE_UID_LEN];
    char                   offsetDest[GB_MIDI_NAME_LEN];
    // In force for the current device/destination pair. Written by the worker (store_measurement and
    // the open), read by publish_latency_breakdown() - which is called both with configLock held and
    // without it, so it can take neither and this has to carry its own guarantee.
    _Atomic uint32_t       hardwareSamples;
    uint32_t               observedMaxFrames;
    uint64_t               observedFrames;
    uint32_t               openDeviceFrames;

    // The host-clock instant of the most recent capture callback. Written by the device thread,
    // read by the audio thread; see latency_frames_measured() for why occupancy alone is not enough.
    _Atomic uint64_t       lastWriteHostTime;
    _Atomic bool           needResync;
    _Atomic float          trimGain;
    _Atomic bool           deviceDirty;

    // When the last device/rate/frames request arrived, so a burst of them can settle into one
    // device change - see GB_DEVICE_SETTLE_MS.
    _Atomic double         lastDeviceRequestMs;

    // What the currently open device's rate and buffer size were before this plug-in changed them,
    // and which device that was. Zero means "changed nothing, restore nothing". Worker thread only,
    // written under configLock alongside the open and close they belong to.
    AudioObjectID          restoreDevice;
    uint32_t               restoreFrames;
    double                 restoreRate;
    _Atomic int            resyncs; // how often the ring had to be snapped back; 0 is healthy
    _Atomic bool           workerQuit;
    _Atomic int            wantedDevice;
    _Atomic bool           savedDevicePending; // a project named a device; honour it, not a slot
    _Atomic bool           deviceParamSeen; // the host's restored value has been and gone
    char                   savedDeviceName[DEVICE_NAME_LEN];  // for the panel: an absent device has no name
    _Atomic double         wantedRate;
    _Atomic int            wantedFrames;
    _Atomic int            wantedChannels;
    _Atomic int            wantedFirstChannel;
    // Written by the worker under configLock, read by the CoreAudio IO thread (capture_callback,
    // which takes no lock and must not) and by the audio thread's pre-trylock parameter pass. Three
    // threads, so it cannot be a plain int whatever the practical consequence of a torn read is.
    _Atomic uint32_t       captureChannels;

    // Set by the last successful open; read by the parameter handlers. Plain members rather than
    // atomics: only the worker writes them, and only under configLock, which process() holds
    // whenever it reads them.
    int                    appliedDevice;
    double                 appliedRate;
    uint32_t               appliedFrames;
    uint32_t               appliedChannels;
    uint32_t               appliedFirstChannel;
    float *                widen;

    // Set by reconfigure() around its close/open pair; see close_capture_locked(). Worker only.
    AudioObjectID          keepSettingsFor;

    pthread_mutex_t        configLock; // held by the worker while swapping devices; trylocked in process()
    pthread_mutex_t        wakeMutex;
    pthread_cond_t         wakeCond;
    pthread_t              worker;
    bool                   wakeFlag;
    bool                   workerActive;

    tDeviceSettings        remembered[GB_MAX_REMEMBERED];
    uint32_t               rememberedCount;

    tRing                  ring;
    tResampler             resampler;
    tDrift                 drift;
    tDeviceStream          capture;

    // THE UID OF WHAT IS OPEN. A char array rather than the std::string it was, and the change is
    // not only about the language: getState() and the worker both touch this, and a std::string
    // being reassigned under a concurrent reader is a pointer that may already have been freed -
    // which is what the lock around them exists to prevent. A fixed array cannot dangle.
    char                   deviceSelector[DEVICE_UID_LEN];
    double                 hostRate;
    uint32_t               hostMaxFrames;
    double                 nominalRatio;
    double                 setpointFrames;
    uint32_t               deviceLatency;
    uint32_t               pullCapacity;
    float *                pullBuffer;
    float *                interleaved;
    _Atomic bool           running;
    bool                   primed;

    // THE BLOB THE HOST LAST ASKED FOR, kept so the wrapper has something to point at. A
    // std::string returned by value did this for free; a C caller needs the bytes to outlive the
    // call, and the bridge owning one buffer is simpler than every caller sizing its own.
    char *                 stateBlob;
    size_t                 stateLength;
    size_t                 stateCapacity;

    // Which variant this is. The audio path is identical either way; what it decides is whether
    // there is a MIDI destination, a measurement and a correction at all.
    bool                   instrument;
};

// ── What the three files that make up the bridge call in each other ─────────
//
// Every one of these was a member function. They are declared here rather than left to the order
// they happen to appear in, which is what a class gave for free and a C file does not.

void gb_lock_config_from_host(tGbBridge * self, const char * who);
double gb_internal_latency_frames(tGbBridge * self);
double gb_latency_frames_measured(tGbBridge * self, double fillFrames, uint64_t at);
uint32_t gb_internal_latency(tGbBridge * self);
uint32_t gb_report_latency(tGbBridge * self);
double gb_mean_callback_lead(tGbBridge * self);
double gb_frames_between(tGbBridge * self, uint64_t from, uint64_t to);
void gb_send_latency_changed(tGbBridge * self);
void gb_send_message(tGbBridge * self, const char * id, int value);
void gb_wake_worker(tGbBridge * self);
void gb_request_device(tGbBridge * self);
void gb_reconfigure(tGbBridge * self);
tMeasured * gb_measured_for(tGbBridge * self, const char * audioUid, const char * midiDest, bool create);
void gb_current_midi_name(tGbBridge * self, char * out, unsigned long len);
void gb_remember_offset_pair(tGbBridge * self, const char * destination);
void gb_sync_offset_to_pair(tGbBridge * self);
void gb_publish_measurement(tGbBridge * self);
void gb_start_measurement(tGbBridge * self);
void gb_send_all_notes_off(tGbBridge * self);
void gb_run_measurement(tGbBridge * self, float ** out, int32_t frames, uint64_t blockHostTime, double fill);
void gb_store_measurement(tGbBridge * self);
tDeviceSettings * gb_ensure_settings(tGbBridge * self, const char * uid);
bool gb_parse_state(tGbBridge * self, const char * blob, size_t length);

#endif // GB_BRIDGE_PRIVATE_H
