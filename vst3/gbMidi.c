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

#include <stdatomic.h>
#include <pthread.h>
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

// PUBLISHED LAST, AND ATOMIC, because this table is process-global and every GenBridge in the host
// shares it - the audio threads of instances that are not the one asking for names read it while
// this one is rebuilding it.
//
// refresh() used to set gCount = 0 and then spend milliseconds in CoreMIDI. For that entire window
// every send in the process saw "index >= gCount" and returned false: notes silently dropped, on
// every instance, once a second for as long as any panel was open. Two instances in one project
// made it twice as likely and looked exactly like the two interfering with each other.
//
// The list is built into a shadow and copied in, and the COUNT goes last with a release. A reader
// indexing below the count it loaded therefore sees entries that were written before it. A slot can
// still name a different device after a rebuild, which is the reason destinations are saved and
// restored by NAME rather than by index.
static _Atomic int     gCount     = 0;
static double          gCachedAt  = -1000.0;
static _Atomic bool    gCacheValid = false;

static void midi_notify(const MIDINotification * message, void * refCon);

// Serialises rebuilds against each other - two editors repainting is two threads in here. Never
// taken by a send, which is the whole point of the atomic above.
static pthread_mutex_t gRefreshLock = PTHREAD_MUTEX_INITIALIZER;

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
    // A NOTIFY PROC, so the destination list does not have to be polled. Passing NULL here is what
    // left refresh() with a one-second timer as its only way of noticing a synth being switched on -
    // and that timer ran on the host's main thread, inside CoreMIDI, for as long as a panel was
    // open. CoreMIDI will now tell us instead.
    if (MIDIClientCreate(CFSTR("GenBridge"), midi_notify, NULL, &gClient) != noErr) {
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
    atomic_store(&gCacheValid, false);
}

// From CoreMIDI's own thread. Anything that changes the setup - a device appearing, a name changing,
// a port being added - means the cached list is stale, and that is all this needs to say.
static void midi_notify(const MIDINotification * message, void * refCon) {
    (void)refCon;

    if ((message != NULL) && (message->messageID == kMIDIMsgSetupChanged)) {
        atomic_store(&gCacheValid, false);
    }
}

// Enumerating CoreMIDI takes locks in the framework, and the editor asks for names on every repaint.
// Cached for the same reason the audio device list is.
static void refresh(void) {
    static MIDIEndpointRef next[GB_MIDI_MAX_DEST];
    static char            nextName[GB_MIDI_MAX_DEST][GB_MIDI_NAME_LEN];

    // EVENT-DRIVEN, with a long re-read as belt and braces - see midi_notify(). The timer that used
    // to be the only mechanism here put a CoreMIDI enumeration on the host's main thread once a
    // second for as long as a panel was open.
    if (atomic_load(&gCacheValid) && ((monotonic_seconds() - gCachedAt) < 30.0)) {
        return;
    }
    pthread_mutex_lock(&gRefreshLock);

    // Re-checked under the lock: two panels can both have passed the test above.
    if (atomic_load(&gCacheValid) && ((monotonic_seconds() - gCachedAt) < 30.0)) {
        pthread_mutex_unlock(&gRefreshLock);
        return;
    }

    int       found = 0;
    ItemCount total = MIDIGetNumberOfDestinations();

    for (ItemCount i = 0; (i < total) && (found < GB_MIDI_MAX_DEST); i++) {
        MIDIEndpointRef endpoint = MIDIGetDestination(i);

        if (endpoint == 0) {
            continue;
        }

        CFStringRef name = NULL;

        nextName[found][0] = '\0';

        if ((MIDIObjectGetStringProperty(endpoint, kMIDIPropertyDisplayName, &name) == noErr)
            && (name != NULL)) {
            CFStringGetCString(name, nextName[found], GB_MIDI_NAME_LEN, kCFStringEncodingUTF8);
            CFRelease(name);
        }

        if (nextName[found][0] == '\0') {
            snprintf(nextName[found], GB_MIDI_NAME_LEN, "destination %d", found + 1);
        }

        next[found] = endpoint;
        found++;
    }

    // ENTRIES FIRST, COUNT LAST. Everything above happened in a shadow, so no send has been able to
    // see a half-built list; this is the only moment anything changes for a reader.
    for (int i = 0; i < found; i++) {
        gDest[i] = next[i];
        memcpy(gName[i], nextName[i], GB_MIDI_NAME_LEN);
    }

    atomic_store_explicit(&gCount, found, memory_order_release);

    gCachedAt = monotonic_seconds();
    atomic_store(&gCacheValid, true);
    pthread_mutex_unlock(&gRefreshLock);
}

int gb_midi_destination_count(void) {
    refresh();

    return atomic_load_explicit(&gCount, memory_order_acquire);
}

void gb_midi_destination_name(int index, char * out, unsigned long len) {
    refresh();

    if ((index < 0) || (index >= atomic_load_explicit(&gCount, memory_order_acquire))) {
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

    int count = atomic_load_explicit(&gCount, memory_order_acquire);

    for (int i = 0; i < count; i++) {
        if (strcmp(gName[i], name) == 0) {
            return i;
        }
    }

    return -1;
}

bool gb_midi_send(int index, const uint8_t * data, uint32_t length) {
    return gb_midi_send_at(index, data, length, 0);
}

// ONE PATH, AND IT IS LOCAL RATHER THAN SynthLib'S. synthlib_midi_send_to() stamps every packet 0,
// which is the one thing this cannot do; and it builds the packet in a shared static buffer behind
// a mutex, so routing notes through it would take a lock per event on the audio thread. The packet
// list here is on the stack and the send takes nothing.
bool gb_midi_send_at(int index, const uint8_t * data, uint32_t length, uint64_t hostTime) {
    // ACQUIRE, to pair with the release in refresh(): entries written before the count was
    // published are guaranteed visible to a reader that has seen it.
    if (!gReady || (index < 0)
        || (index >= atomic_load_explicit(&gCount, memory_order_acquire))
        || (data == NULL) || (length == 0)) {
        return false;
    }

    // Everything this plug-in sends is a two or three byte channel message, so one stack packet
    // list holds it with room to spare. MIDIPacketListAdd() is the real guard - it returns NULL
    // rather than overrunning - and this only says what the size was chosen for.
    MIDIPacketList list;
    MIDIPacket *   packet = MIDIPacketListInit(&list);

    packet = MIDIPacketListAdd(&list, sizeof(list), packet, (MIDITimeStamp)hostTime, length, data);

    if (packet == NULL) {
        return false;
    }

    return MIDISend(gPort, gDest[index], &list) == noErr;
}
