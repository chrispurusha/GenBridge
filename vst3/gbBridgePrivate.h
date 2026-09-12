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
// Notes: Docs/code-notes/gbBridgePrivate.h.md - "// notes §k" refers there.

#ifndef GB_BRIDGE_PRIVATE_H
#define GB_BRIDGE_PRIVATE_H

// notes §1

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

// notes §2
#define GB_TARGET_AUTO    (0.0)

// notes §3
#define GB_AUTO_MARGIN    (1.25)

// How long to watch the host's real block size before trusting it, and how much has to be on the
// table before disturbing the host's delay compensation to claim it.
#define GB_SETTLE_SECONDS    (2.0)

// notes §4
#define GB_DEVICE_SETTLE_MS  (250.0)

// notes §5
#define GB_OFFSET_SETTLE_MS  (400.0)
#define GB_RETUNE_MIN_GAIN   (64.0)      // frames

// How long to listen for the note before giving up, and how far above the noise floor counts as an
// onset.
#define GB_MEASURE_TIMEOUT_S  (1.5)
#define GB_MEASURE_FLOOR_S    (0.15)

// notes §6
#define GB_MEASURE_SETTLE_S   (0.35)

// The longest the settle will wait for the previous note to decay before giving up on quiet and
// taking the floor anyway. A synth that never goes quiet is a measurement that never happens.
#define GB_MEASURE_SETTLE_MAX_S    (2.5)

// One-pole coefficient for the reported pipeline delay, at roughly two seconds of time constant
// over 128-frame blocks. Slow enough to ignore the ring's sawtooth, quick enough to follow a
// device change without the panel looking stuck.
#define GB_PIPELINE_SMOOTH    (0.0013)

// notes §7
#define GB_LATENCY_DEADBAND   (64)

// A sanity ceiling on a callback size read back out of a project file. Nothing sane hands over a
// third of a second in one go, and a corrupt or hand-edited value must not be able to ask for a
// ring measured in seconds.
#define GB_MAX_BLOCK_FRAMES   (16384)

// notes §8
#define GB_MEASURE_MARGIN     (0.02f)    // absolute minimum rise, for a genuinely silent input
#define GB_MEASURE_RATIO      (8.0f)     // ...or this much above the noise, whichever is greater
#define GB_MEASURE_CEILING    (0.70f)    // never demand more than this; see the note in the code
#define GB_MEASURE_CONFIRM    (2)        // consecutive blocks required, so one glitch is not an onset

// notes §9
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

// notes §10
#define GB_HOST_INPUT_KEY     "(host input)"

#define GB_DEFAULT_FRAMES     (128)
#define GB_DEFAULT_RATE       (48000.0)
#define GB_MAX_REMEMBERED     (32)

// ── The tables the state blob carries ───────────────────────────────────────

// notes §11
typedef struct {
    char     audioUid[DEVICE_UID_LEN];
    char     midiDest[GB_MIDI_NAME_LEN];
    uint32_t hardwareSamples;    // the round trip MINUS whatever the plug-in was contributing
    double   offsetMs;           // seeded from the measurement, then adjusted by hand
} tMeasured;

// notes §12
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
    // notes §13
    tGbHostOps          hostOps;

    // Assigned when the host connects the two ends (its own thread) and read from the audio thread
    // by publish_status() on every block.
    _Atomic int            statusSlot;
    // Written by whichever thread last told the host, read by getLatencySamples() on any thread -
    // see its comment. Atomic because those are genuinely different threads, not for ordering.
    _Atomic uint32_t       reportedLatency;

    // notes §14
    _Atomic double         eventRate;

    // notes §15
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

    // notes §16
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

    // notes §17
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

    // notes §18
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

    // notes §19
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

    // WHERE THE AUDIO COMES FROM: GB_SOURCE_DEVICE or GB_SOURCE_HOST. Read by the audio thread on
    // every block and written by the parameter pass just before it, so atomic - and read by the
    // worker, which decides whether to open anything at all.
    _Atomic int            captureSource;

    // How long the host's input has been silent, in frames. AUDIO THREAD ONLY - it is written and
    // read on the same block, and only published as a flag.
    uint64_t               hostSilentFrames;

    // Which variant this is. The audio path is identical either way; what it decides is whether
    // there is a MIDI destination, a measurement and a correction at all.
    bool                   instrument;
};

// IS THERE AUDIO TO WORK WITH? Not "is a device running" - that was the same question until the
// host-input mode existed, and the difference is why Measure did nothing in it: gb_start_measurement()
// tested self->running, which only open_capture_locked() ever sets.
static inline bool gb_capturing(tGbBridge * self) {
    return (atomic_load(&self->captureSource) == GB_SOURCE_HOST) || atomic_load(&self->running);
}

// The audio half of the (audio, MIDI) pair, whichever mode is in force.
static inline const char * gb_audio_key(tGbBridge * self) {
    return (atomic_load(&self->captureSource) == GB_SOURCE_HOST) ? GB_HOST_INPUT_KEY
                                                                 : self->deviceSelector;
}

// notes §20

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
