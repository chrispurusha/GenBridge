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

#include <string.h>
#include <time.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>
#pragma clang diagnostic pop

#include "gbMidi.h"
#include "synthlibMidi.h"

static MIDIClientRef   gClient    = 0;
static MIDIPortRef     gPort      = 0;
static bool            gReady     = false;

static MIDIEndpointRef gDest[GB_MIDI_MAX_DEST];
static char            gName[GB_MIDI_MAX_DEST][GB_MIDI_NAME_LEN];
static int             gCount     = 0;
static double          gCachedAt  = -1000.0;

static double monotonic_seconds(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (double)ts.tv_sec + ((double)ts.tv_nsec * 1e-9);
}

bool gb_midi_init(void) {
    if (gReady) {
        return true;
    }

    // A plug-in may be instantiated many times; one client and one port serve all of them, which is
    // also what keeps the host's MIDI panel from filling with duplicates.
    if (MIDIClientCreate(CFSTR("GenBridge"), NULL, NULL, &gClient) != noErr) {
        return false;
    }

    if (MIDIOutputPortCreate(gClient, CFSTR("GenBridge Out"), &gPort) != noErr) {
        return false;
    }

    synthlib_midi_set_out_port(gPort);

    gReady = true;

    return true;
}

void gb_midi_invalidate(void) {
    gCachedAt = -1000.0;
}

// Enumerating CoreMIDI takes locks in the framework, and the editor asks for names on every repaint.
// Cached for the same reason the audio device list is.
static void refresh(void) {
    double now = monotonic_seconds();

    if ((now - gCachedAt) < 1.0) {
        return;
    }

    gCachedAt = now;
    gCount    = 0;

    ItemCount total = MIDIGetNumberOfDestinations();

    for (ItemCount i = 0; (i < total) && (gCount < GB_MIDI_MAX_DEST); i++) {
        MIDIEndpointRef endpoint = MIDIGetDestination(i);

        if (endpoint == 0) {
            continue;
        }

        CFStringRef name = NULL;

        gName[gCount][0] = '\0';

        if ((MIDIObjectGetStringProperty(endpoint, kMIDIPropertyDisplayName, &name) == noErr)
            && (name != NULL)) {
            CFStringGetCString(name, gName[gCount], GB_MIDI_NAME_LEN, kCFStringEncodingUTF8);
            CFRelease(name);
        }

        if (gName[gCount][0] == '\0') {
            snprintf(gName[gCount], GB_MIDI_NAME_LEN, "destination %d", gCount + 1);
        }

        gDest[gCount] = endpoint;
        gCount++;
    }
}

int gb_midi_destination_count(void) {
    refresh();

    return gCount;
}

void gb_midi_destination_name(int index, char * out, unsigned long len) {
    refresh();

    if ((index < 0) || (index >= gCount)) {
        snprintf(out, len, "%s", "-");
        return;
    }

    snprintf(out, len, "%s", gName[index]);
}

int gb_midi_slot_for_name(const char * name) {
    refresh();

    if ((name == NULL) || (name[0] == '\0')) {
        return -1;
    }

    for (int i = 0; i < gCount; i++) {
        if (strcmp(gName[i], name) == 0) {
            return i;
        }
    }

    return -1;
}

bool gb_midi_send(int index, const uint8_t * data, uint32_t length) {
    if (!gReady || (index < 0) || (index >= gCount)) {
        return false;
    }

    return synthlib_midi_send_to(data, length, gDest[index]);
}
