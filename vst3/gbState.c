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


// THE SAVED STATE, AND THE TABLES IT CARRIES.
//
// THE FORMAT IS VERSIONED AND LINE BASED, and it is that way now rather than later because a state
// format becomes expensive to change the moment anyone saves a session against it. Text costs
// nothing at this size, survives being looked at in a hex editor, and lets an older build skip keys
// it does not recognise instead of rejecting the whole blob.
//
// The UID is written LAST on each line and read as "everything after the last comma of the numeric
// part", because real UIDs contain commas - "AppleUSBAudioEngine:CalDigit, Inc.:..." - and splitting
// on them would truncate it.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gbBridgePrivate.h"
#include "gbLog.h"
#include "gbMidi.h"
#include "gbStatus.h"

// FILE-LOCAL, AND FORWARD-DECLARED. A class let its members call each other in
// any order; a C file does not, and these are the ones nothing outside this file
// needs to see.
static void gb_capture_live_settings(tGbBridge * self);
static tDeviceSettings * gb_find_settings(tGbBridge * self, const char * uid);
static void gb_parse_device_line(tGbBridge * self, const char * body, size_t length, int version);
static void gb_parse_measured_line(tGbBridge * self, const char * body, size_t length);

// ---- per device settings -------------------------------------------------------------------

static tDeviceSettings * gb_find_settings(tGbBridge * self, const char * uid) {
    for (uint32_t i = 0; i < self->rememberedCount; i++) {
        if (strncmp(self->remembered[i].uid, uid, DEVICE_UID_LEN) == 0) {
            return &self->remembered[i];
        }
    }

    return NULL;
}

tDeviceSettings * gb_ensure_settings(tGbBridge * self, const char * uid) {
    tDeviceSettings * found = gb_find_settings(self, uid);

    if (found != NULL) {
        return found;
    }

    // Full table: overwrite the oldest entry rather than refusing. Losing the least recently
    // added device's settings is a far better failure than silently ignoring the new one.
    uint32_t slot = self->rememberedCount;

    if (self->rememberedCount < GB_MAX_REMEMBERED) {
        self->rememberedCount++;
    } else {
        slot = 0;
        memmove(&self->remembered[0], &self->remembered[1],
                sizeof(tDeviceSettings) * (GB_MAX_REMEMBERED - 1));
        slot = GB_MAX_REMEMBERED - 1;
    }

    tDeviceSettings * entry = &self->remembered[slot];

    memset(entry, 0, sizeof(*entry));
    strncpy(entry->uid, uid, DEVICE_UID_LEN - 1);
    // ZERO MEANS "LEAVE THE DEVICE ALONE", and that is the default for both.
    //
    // Setting a device's nominal rate or buffer size is a GLOBAL operation affecting every
    // client of that device - including the host itself, if it happens to be the same
    // interface. Doing it uninvited during load is how a plug-in wedges a DAW. The device's own
    // settings are now simply adopted, and these are written only when the user changes them.
    entry->frames       = 0;
    entry->rate         = 0.0;
    entry->targetMs     = GB_TARGET_AUTO;
    entry->firstChannel    = 0;
    entry->captureChannels = GB_CHANNELS;
    entry->trim            = 1.0f;

    return entry;
}

// Fold whatever the user has changed live back into the active device's entry, so that saving
// the project records what is actually on screen rather than what was last loaded.
//
// ONLY WHEN A DEVICE IS ACTUALLY RUNNING. If nothing is open, the live values are construction
// defaults rather than anything the user chose, and writing them back destroys the settings
// that were just loaded - a project saved without ever starting playback came back with every
// trim reset to 1.0.
static void gb_capture_live_settings(tGbBridge * self) {
    if ((self->deviceSelector[0] == '\0') || !self->running) {
        return;
    }

    tDeviceSettings * entry = gb_ensure_settings(self, self->deviceSelector);

    entry->trim = atomic_load(&self->trimGain);

    // DELIBERATELY NOT WRITING BACK targetMs WHEN IT IS AUTO. It used to record the computed
    // setpoint, which turned "work it out" into a fixed number the moment a project was saved -
    // so a session saved at one buffer size reopened with that size's setpoint baked in, and the
    // floor calculation was quietly bypassed for ever after. Only an explicit choice is stored.
    if (self->running && (entry->targetMs > 0.0)) {
        entry->targetMs = (self->setpointFrames / (self->nominalRatio * self->hostRate)) * 1000.0;
    }
}

// ── Reading a blob, without a string class ──────────────────────────────────
//
// The parsing below walks the bytes in place. It was written against std::string, whose substr()
// and find() made each step read nicely and allocated a copy for every line, every field and every
// tail; what is here does the same work with a pointer and a length. The one thing worth saying is
// that NOTHING IS ASSUMED TO BE NUL-TERMINATED - a host hands over a byte count, and a blob that
// has been truncated in a project file is exactly the case that must not run off the end.

typedef struct {
    const char * text;
    size_t       length;
} tGbLine;

// The next line, or false at the end. The newline is not part of what comes back, and a final line
// without one is still a line.
static bool next_line(const char ** at, const char * end, tGbLine * out) {
    if (*at >= end) {
        return false;
    }
    const char * newline = memchr(*at, '\n', (size_t)(end - *at));

    out->text   = *at;
    out->length = (newline != NULL) ? (size_t)(newline - *at) : (size_t)(end - *at);
    *at         = (newline != NULL) ? (newline + 1) : end;

    return true;
}

// Does the line start with this key, and if so where does its value begin? One call rather than a
// compare and a substr, because the two were always written together and could disagree.
static bool line_key(const tGbLine * line, const char * key, const char ** value, size_t * length) {
    size_t keyLength = strlen(key);

    if ((line->length < keyLength) || (memcmp(line->text, key, keyLength) != 0)) {
        return false;
    }
    *value  = line->text + keyLength;
    *length = line->length - keyLength;

    return true;
}

// A run of bytes into a fixed buffer, always terminated. Everything a blob names - a UID, a device
// name, a MIDI destination - has a fixed home in the tables, so this is the one way in.
static void copy_field(char * out, size_t max, const char * text, size_t length) {
    if (length >= max) {
        length = max - 1;
    }
    memcpy(out, text, length);
    out[length] = '\0';
}

// The next comma-separated field as a number. Advances past the comma; false when there is no
// comma left, which is what every caller treats as a malformed line.
static bool next_number(const char ** at, const char * end, double * out) {
    const char * comma = memchr(*at, ',', (size_t)(end - *at));

    if (comma == NULL) {
        return false;
    }
    char   text[64];
    size_t length = (size_t)(comma - *at);

    copy_field(text, sizeof(text), *at, length);
    *out = strtod(text, NULL);
    *at  = comma + 1;

    return true;
}

bool gb_parse_state(tGbBridge * self, const char * blob, size_t length) {
    // Version 2 added the per-device sample rate as a numeric field, which changes how a dev=
    // line is split - so it needed a version bump rather than a new key. Reading version 1 is
    // still supported: this is exactly the situation the format was versioned for, and refusing
    // to open a session saved yesterday would be a poor advertisement for it.
    int version = 0;

    if (length < 10) {
        return false;
    }

    if (memcmp(blob, "GENBRIDGE3", 10) == 0) {
        version = 3;
    } else if (memcmp(blob, "GENBRIDGE2", 10) == 0) {
        version = 2;
    } else if (memcmp(blob, "GENBRIDGE1", 10) == 0) {
        version = 1;
    } else {
        return false;
    }

    self->rememberedCount    = 0;
    self->measuredCount      = 0;
    self->deviceSelector[0]  = '\0';
    self->savedDeviceName[0] = '\0';

    // A RESTORE IS NOT A CHOICE. Everything after this point until the user actually picks
    // something is the project being reopened, and the saved UID - not the saved slot index -
    // is what says which device that was. See the device parameter in gb_bridge_parameter() and
    // gb_reconfigure().
    atomic_store(&self->savedDevicePending, false);
    atomic_store(&self->deviceParamSeen, false);

    // Defaults for a blob that predates these keys, so loading an older session zeroes the
    // correction rather than leaving whatever the previous project in this instance had. The
    // pair record is cleared with it, so the device open that follows treats this as a new pair
    // and seeds the offset from whatever the restored table holds for it.
    atomic_store(&self->offsetMs, 0.0);
    self->offsetUid[0]  = '\0';
    self->offsetDest[0] = '\0';

    const char * at   = blob;
    const char * end  = blob + length;
    tGbLine      line;

    while (next_line(&at, end, &line)) {
        const char * value     = NULL;
        size_t       valueLen  = 0;
        char         scratch[DEVICE_UID_LEN];

        if (line_key(&line, "midi=", &value, &valueLen)) {
            copy_field(scratch, sizeof(scratch), value, valueLen);

            int slot = gb_midi_slot_for_name(scratch);

            if (slot >= 0) {
                atomic_store(&self->midiDestination, slot);
            }
        } else if (line_key(&line, "midich=", &value, &valueLen)) {
            copy_field(scratch, sizeof(scratch), value, valueLen);
            atomic_store(&self->midiChannel, atoi(scratch));
        } else if (line_key(&line, "callback=", &value, &valueLen)) {
            // Into observedMaxFrames directly, so the first open sizes from it exactly as it
            // would from a live observation - see the note where it is written.
            copy_field(scratch, sizeof(scratch), value, valueLen);

            unsigned frames = (unsigned)strtoul(scratch, NULL, 10);

            if ((frames > 0) && (frames <= (GB_MAX_BLOCK_FRAMES))) {
                self->observedMaxFrames = frames;
            }
        } else if (line_key(&line, "testnote=", &value, &valueLen)) {
            copy_field(scratch, sizeof(scratch), value, valueLen);

            int note = atoi(scratch);

            atomic_store(&self->testNote, (note < 0) ? 0 : ((note > 127) ? 127 : note));
        } else if (line_key(&line, "activename=", &value, &valueLen)) {
            // Written purely so the panel can NAME what it is waiting for. The device is absent
            // by definition in that state, so its name cannot be looked up - a UID is all there
            // would otherwise be to show, and a CoreAudio UID is not something to hand a user.
            copy_field(self->savedDeviceName, sizeof(self->savedDeviceName), value, valueLen);
        } else if (line_key(&line, "active=", &value, &valueLen)) {
            copy_field(self->deviceSelector, sizeof(self->deviceSelector), value, valueLen);
        } else if (line_key(&line, "dev=", &value, &valueLen)) {
            gb_parse_device_line(self, value, valueLen, version);
        } else if (line_key(&line, "hw=", &value, &valueLen)) {
            gb_parse_measured_line(self, value, valueLen);
        }
        // Anything else is from a newer build; skipping it is the point of the format. That is
        // also why hw= arrives without a version bump: it is a new KEY, and only a change to how
        // a dev= line splits has ever needed the version.
        //
        // It also means the short-lived offset=/meas= pair from earlier is simply ignored
        // rather than misread. meas= carried SAMPLES where hw= carries milliseconds, so reusing
        // the name would have loaded a 221-sample reading as 221 ms.
    }

    // A device was named in the project, so it - and not a slot index recorded when the device
    // list had a different shape - decides what gets opened, until it either turns up or the
    // user chooses something else.
    atomic_store(&self->savedDevicePending, self->deviceSelector[0] != '\0');

    return true;
}

// hw=<samples>,<offset ms>,<destination name length>,<destination name><audio uid>
//
// THE LENGTH IS THERE BECAUSE BOTH TAILS ARE FREE TEXT. A dev= line gets away with putting its
// uid last and taking the rest of the line, but this one carries two names, and a MIDI
// destination is quite entitled to contain a comma - "Scarlett 2i2, Port 1" is an ordinary
// thing for a driver to call itself. Counting the first name off by length leaves nothing to
// guess at, where a third comma would have been a guess that fails on somebody's interface.
static void gb_parse_measured_line(tGbBridge * self, const char * body, size_t length) {
    const char * at  = body;
    const char * end = body + length;
    double       fields[3];

    for (int i = 0; i < 3; i++) {
        if (!next_number(&at, end, &fields[i])) {
            return;
        }
    }

    size_t destLen = (size_t)fields[2];
    size_t tailLen = (size_t)(end - at);

    if (destLen > tailLen) {
        return;
    }
    char dest[GB_MIDI_NAME_LEN];
    char uid[DEVICE_UID_LEN];

    copy_field(dest, sizeof(dest), at, destLen);
    copy_field(uid, sizeof(uid), at + destLen, tailLen - destLen);

    if (uid[0] == '\0') {
        return;
    }

    tMeasured * entry = gb_measured_for(self, uid, dest, true);

    if (entry != NULL) {
        entry->hardwareSamples = (uint32_t)fields[0];
        entry->offsetMs        = (fields[1] < GB_OFFSET_MIN_MS) ? GB_OFFSET_MIN_MS
                                 : ((fields[1] > GB_OFFSET_MAX_MS) ? GB_OFFSET_MAX_MS : fields[1]);
    }
}

// The canonical order of the numeric fields, and which of them each version actually wrote.
// A table rather than arithmetic: every time a field is added the shifting gets harder to do in
// the head, and one wrong offset silently loads a sample rate as a channel number.
enum { kSlotFrames = 0, kSlotRate, kSlotTarget, kSlotFirst, kSlotChannels, kSlotTrim, kSlotCount };

static void gb_parse_device_line(tGbBridge * self, const char * body, size_t length, int version) {
    static const int kV1[] = { kSlotFrames, kSlotTarget, kSlotFirst, kSlotTrim };
    static const int kV2[] = { kSlotFrames, kSlotRate, kSlotTarget, kSlotFirst, kSlotTrim };
    static const int kV3[] = { kSlotFrames, kSlotRate, kSlotTarget, kSlotFirst, kSlotChannels, kSlotTrim };

    const int * map     = (version >= 3) ? kV3 : ((version == 2) ? kV2 : kV1);
    int         numeric = (version >= 3) ? 6 : ((version == 2) ? 5 : 4);

    unsigned     frames = 0, firstChannel = 0, captureChannels = GB_CHANNELS;
    double       rate = 0.0, targetMs = GB_TARGET_AUTO, trim = 1.0;
    const char * at  = body;
    const char * end = body + length;

    for (int field = 0; field < numeric; field++) {
        double value = 0.0;

        if (!next_number(&at, end, &value)) {
            return;
        }

        switch (map[field]) {
            case kSlotFrames:   frames          = (unsigned)value; break;
            case kSlotRate:     rate            = value;           break;
            case kSlotTarget:   targetMs        = value;           break;
            case kSlotFirst:    firstChannel    = (unsigned)value; break;
            case kSlotChannels: captureChannels = (unsigned)value; break;
            default:            trim            = value;           break;
        }
    }
    char uid[DEVICE_UID_LEN];

    copy_field(uid, sizeof(uid), at, (size_t)(end - at));

    if (uid[0] == '\0') {
        return;
    }

    tDeviceSettings * entry = gb_ensure_settings(self, uid);

    // AND WHAT CAME BACK OUT OF IT. Paired with the "saving:" line, these two settle whether a
    // buffer size survived the round trip through the project file.
    gb_log_line("restoring: device '%s' frames %u rate %.0f", uid, frames, rate);

    entry->frames          = frames;
    entry->rate            = rate;
    entry->targetMs        = targetMs;    // 0 is legitimate: it means "auto"
    entry->firstChannel    = firstChannel;
    entry->captureChannels = ((captureChannels == 1) || (captureChannels == 2))
                             ? captureChannels : GB_CHANNELS;
    entry->trim            = (float)trim;
}


// ── Writing a blob ──────────────────────────────────────────────────────────
//
// A growable buffer, because the length depends on how many devices have been remembered and how
// many pairs measured - up to 32 of each. std::string did this by itself; three fields and one
// append do the same job with the allocation visible.
static void text_add(tGbBridge * self, const char * format, ...) {
    va_list args;
    char    piece[512 + DEVICE_UID_LEN + GB_MIDI_NAME_LEN];

    va_start(args, format);
    int wrote = vsnprintf(piece, sizeof(piece), format, args);
    va_end(args);

    if (wrote <= 0) {
        return;
    }
    size_t needed = self->stateLength + (size_t)wrote + 1;

    if (needed > self->stateCapacity) {
        size_t capacity = (self->stateCapacity > 0) ? self->stateCapacity : 1024;

        while (capacity < needed) {
            capacity *= 2;
        }
        char * grown = (char *)realloc(self->stateBlob, capacity);

        if (grown == NULL) {
            return;         // the blob is short, and a short blob simply restores less
        }
        self->stateBlob     = grown;
        self->stateCapacity = capacity;
    }

    memcpy(self->stateBlob + self->stateLength, piece, (size_t)wrote + 1);
    self->stateLength += (size_t)wrote;
}

const char * gb_bridge_state(tGbBridge * self, size_t * length) {
    // UNDER THE LOCK, and this is the one that could actually crash rather than merely report a
    // wrong number. deviceSelector is rewritten by gb_reconfigure() under configLock, and
    // gb_capture_live_settings() walks the remembered[] table the same worker appends to.
    //
    // Blocking is fine here in a way it never is in the render path: a host calls this on its own
    // thread when it saves, and the worst wait is one device swap.
    gb_lock_config_from_host(self, "getState");

    gb_capture_live_settings(self);

    self->stateLength = 0;

    text_add(self, "GENBRIDGE3\n");
    text_add(self, "active=%s\n", self->deviceSelector);

    // HOW MUCH THE HOST TAKES PER CALLBACK, which is a property of the HOST and its buffer setting
    // rather than of any device - so it is written once, outside the dev= lines.
    //
    // Saved because neither number available at the FIRST open is right: this host declares 256 and
    // hands over 512, and the observation needs an undisturbed callback that a project load does not
    // provide. Without it the ring comes up at 560, discovers the truth a second later and retunes
    // to 880 - which costs a device reopen per instance, 1.4 seconds each on these interfaces. With
    // it, the first open is already correct.
    //
    // A stale value is safe: the retune corrects in BOTH directions now, and
    // gb_bridge_setup_processing() discards it outright if the host comes back declaring a
    // different block size.
    if (self->observedMaxFrames > 0) {
        text_add(self, "callback=%u\n", self->observedMaxFrames);
    }

    // A NEW KEY, not a version bump: the format skips what it does not recognise, so an older build
    // reading this simply does not get a name to show.
    {
        tGbStatus * status = gb_status(atomic_load(&self->statusSlot));

        if ((status != NULL) && (status->deviceName[0] != '\0')) {
            snprintf(self->savedDeviceName, sizeof(self->savedDeviceName), "%s", status->deviceName);
        }

        if (self->savedDeviceName[0] != '\0') {
            text_add(self, "activename=%s\n", self->savedDeviceName);
        }
    }

    // BY NAME, not by index. The MIDI list shifts whenever a device is powered on or off, so an
    // index saved on Monday names something else on Tuesday - the same reasoning that keeps the
    // audio device stored as a UID. Ableton was not forgetting the destination; nothing was ever
    // writing it down.
    if (self->instrument) {
        char midiNameNow[GB_MIDI_NAME_LEN] = {0};

        gb_current_midi_name(self, midiNameNow, sizeof(midiNameNow));
        text_add(self, "midi=%s\n", midiNameNow);
        text_add(self, "midich=%d\n", atomic_load(&self->midiChannel));
        text_add(self, "testnote=%d\n", atomic_load(&self->testNote));

        // THE MANUAL TRIM, AND THE MEASUREMENTS IT TRIMS. Neither belongs on a dev= line: the offset
        // is one value for the whole plug-in, and a measurement is keyed by the audio device and the
        // MIDI destination TOGETHER - a pair no single dev= line names.
        //
        // Without both of these the feature came apart on reload, and quietly. gb_report_latency()
        // adds the measured hardware share and then the offset on top of it, so a session reopened
        // with the pair missing reported a latency short by the entire round trip, with the trim
        // someone had dialled in by ear silently back at zero. Restoring the offset alone would be
        // worse than neither: it would trim a base that was not there. The live value belongs to a
        // pair like every other, so fold it in before writing - otherwise a nudge made since the
        // last device change would not be in the table yet.
        gb_sync_offset_to_pair(self);

        for (uint32_t i = 0; i < self->measuredCount; i++) {
            const tMeasured * m = &self->measured[i];

            text_add(self, "hw=%u,%.3f,%u,%s%s\n", m->hardwareSamples, m->offsetMs,
                     (unsigned)strlen(m->midiDest), m->midiDest, m->audioUid);
        }
    }

    for (uint32_t i = 0; i < self->rememberedCount; i++) {
        const tDeviceSettings * d = &self->remembered[i];

        text_add(self, "dev=%u,%.1f,%.3f,%u,%u,%.4f,%s\n", d->frames, d->rate, d->targetMs,
                 d->firstChannel, d->captureChannels, (double)d->trim, d->uid);

        // NOT LOGGED FROM HERE, and that is not tidiness. This is called by a host far more often
        // than a save: Ableton takes an undo snapshot on ordinary UI actions, and the loop runs once
        // per REMEMBERED device - up to 32 of them. gb_log_line() opens and closes the file on every
        // call, so a line here is dozens of file operations on the HOST'S MAIN THREAD every time
        // someone moves a control. It was added to answer one question ("did the buffer size ever
        // reach the project file?"), it answered it, and it would have been a fresh cause of the
        // very beachball it was helping to chase.
        //
        // The "restoring:" line on the way back in survives, because a load runs once and is where
        // a value that failed to persist actually shows up as missing.
    }
    // Released before the caller writes: the bytes belong to the bridge now, and the wrapper's
    // write() calls back into the host - which must never happen with this lock held.
    pthread_mutex_unlock(&self->configLock);

    if (length != NULL) {
        *length = self->stateLength;
    }

    return (self->stateBlob != NULL) ? self->stateBlob : "";
}

bool gb_bridge_set_state(tGbBridge * self, const char * blob, size_t length) {
    // UNDER THE LOCK, the mirror of gb_bridge_state(). gb_parse_state() clears and rewrites almost
    // every piece of configuration the worker reads - the device selector and the saved name, the
    // remembered[] and measured[] tables, and the offset pair. A host may call this while the
    // plug-in is loaded and the worker is mid-reconfigure.
    //
    // The wrapper's read loop is deliberately OUTSIDE this: reading the stream calls back into the
    // host, which must never happen with this held.
    gb_lock_config_from_host(self, "setState");

    bool ok      = gb_parse_state(self, blob, length);
    bool wasOpen = atomic_load(&self->running);

    pthread_mutex_unlock(&self->configLock);

    // ASK FOR THE DEVICE AGAIN, because the settings may have arrived AFTER it was opened.
    //
    // Nothing here controls when a host calls this relative to everything else. If the device
    // parameter reaches the plug-in first - through the controller, or as a parameter change in the
    // first process() calls - the device is opened before this blob has been read, so it comes up
    // with no remembered entry and "leave the device alone" is the honest default. The saved buffer
    // size then lands in remembered[] with nothing to apply it.
    //
    // That is CT's "I still had to set 64 manually": the settings were restored correctly and simply
    // never reached the device. Rather than guess at a host's ordering, react to the late arrival -
    // gb_request_device() is debounced, so a host that DID call in the tidy order coalesces this
    // into the open it was going to do anyway.
    if (ok && wasOpen) {
        gb_request_device(self);
    }

    return ok;
}


// ── What the CONTROLLER needs from the same bytes ───────────────────────────
//
// A VST3 host saves the component's state and hands the same bytes to the controller through
// setComponentState, precisely so the two can agree on what was loaded - and a controller that
// ignores it comes up showing defaults. That is what made two tracks, saved with a Kronos and a
// Helix, both reopen as Analog Keys: the UID was in the file, but nothing told the panel about it.
//
// A SECOND READER OF ONE FORMAT, deliberately not a second parser of it: this reads far enough to
// recover what a panel has to show and nothing else, and it shares next_line(), line_key() and
// copy_field() with the parser above so the two cannot disagree about where a line ends.
void gb_state_parse_active(const char * blob, size_t length, tGbActive * out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->testNote = GB_MEASURE_NOTE;
    out->frames   = GB_DEFAULT_FRAMES;
    out->rate     = GB_DEFAULT_RATE;
    out->channels = GB_CHANNELS;
    out->trim     = 1.0f;

    int version = 0;

    if ((blob == NULL) || (length < 10)) {
        return;
    }

    if (memcmp(blob, "GENBRIDGE3", 10) == 0) {
        version = 3;
    } else if (memcmp(blob, "GENBRIDGE2", 10) == 0) {
        version = 2;
    } else if (memcmp(blob, "GENBRIDGE1", 10) == 0) {
        version = 1;
    } else {
        return;
    }

    // TWO PASSES, because a hw= line names its own pair and which pair is the ACTIVE one is not
    // known until active= and midi= have both been seen. getState() writes them first only by
    // convention, which is a thin thing to parse by.
    const char * at  = blob;
    const char * end = blob + length;
    tGbLine      line;

    while (next_line(&at, end, &line)) {
        const char * value    = NULL;
        size_t       valueLen = 0;
        char         scratch[DEVICE_UID_LEN];

        if (line_key(&line, "midi=", &value, &valueLen)) {
            copy_field(out->midiName, sizeof(out->midiName), value, valueLen);
        } else if (line_key(&line, "midich=", &value, &valueLen)) {
            copy_field(scratch, sizeof(scratch), value, valueLen);
            out->midiChannel = atoi(scratch);
        } else if (line_key(&line, "testnote=", &value, &valueLen)) {
            copy_field(scratch, sizeof(scratch), value, valueLen);
            out->testNote = atoi(scratch);
        } else if (line_key(&line, "active=", &value, &valueLen)) {
            copy_field(out->uid, sizeof(out->uid), value, valueLen);
            out->valid = (out->uid[0] != '\0');
        } else if (line_key(&line, "dev=", &value, &valueLen) && (out->uid[0] != '\0')) {
            // THE LINE FOR THE ACTIVE DEVICE AND NO OTHER, recognised by the uid it ends with.
            size_t uidLen = strlen(out->uid);

            if ((valueLen <= uidLen)
                || (memcmp(value + valueLen - uidLen, out->uid, uidLen) != 0)) {
                continue;
            }

            // Same slot mapping as the processor's own parser - v1 has no rate, v2 no channel
            // count. See gb_parse_device_line() for why it is a table rather than arithmetic.
            static const int kV1[] = { kSlotFrames, kSlotTarget, kSlotFirst, kSlotTrim };
            static const int kV2[] = { kSlotFrames, kSlotRate, kSlotTarget, kSlotFirst, kSlotTrim };
            static const int kV3[] = { kSlotFrames, kSlotRate, kSlotTarget, kSlotFirst,
                                       kSlotChannels, kSlotTrim };
            const int *  map     = (version >= 3) ? kV3 : ((version == 2) ? kV2 : kV1);
            int          numeric = (version >= 3) ? 6 : ((version == 2) ? 5 : 4);
            double       values[kSlotCount] = { GB_DEFAULT_FRAMES, GB_DEFAULT_RATE, 0.0, 0.0,
                                                GB_CHANNELS, 1.0 };
            const char * field   = value;
            const char * lineEnd = value + valueLen;

            for (int i = 0; i < numeric; i++) {
                double read = 0.0;

                if (!next_number(&field, lineEnd, &read)) {
                    break;
                }
                values[map[i]] = read;
            }

            out->frames       = (unsigned)values[kSlotFrames];
            out->rate         = values[kSlotRate];
            out->firstChannel = (unsigned)values[kSlotFirst];
            out->channels     = ((values[kSlotChannels] == 1.0) || (values[kSlotChannels] == 2.0))
                                ? (unsigned)values[kSlotChannels] : GB_CHANNELS;
            out->trim         = (float)values[kSlotTrim];
        }
    }

    // The correction for the pair this instance is actually on. Same length-prefixed layout the
    // processor writes - see gb_parse_measured_line() for why the destination is counted, not split.
    at = blob;

    while (next_line(&at, end, &line)) {
        const char * value    = NULL;
        size_t       valueLen = 0;

        if (!line_key(&line, "hw=", &value, &valueLen)) {
            continue;
        }
        const char * field   = value;
        const char * lineEnd = value + valueLen;
        double       fields[3];
        bool         ok = true;

        for (int i = 0; (i < 3) && ok; i++) {
            ok = next_number(&field, lineEnd, &fields[i]);
        }

        if (!ok) {
            continue;
        }
        size_t destLen = (size_t)fields[2];
        size_t tailLen = (size_t)(lineEnd - field);

        if (destLen > tailLen) {
            continue;
        }
        char dest[GB_MIDI_NAME_LEN];
        char uid[DEVICE_UID_LEN];

        copy_field(dest, sizeof(dest), field, destLen);
        copy_field(uid, sizeof(uid), field + destLen, tailLen - destLen);

        if ((strcmp(dest, out->midiName) == 0) && (strcmp(uid, out->uid) == 0)) {
            out->offsetMs = fields[1];
            break;
        }
    }
}
