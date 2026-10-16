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
// Notes: Docs/code-notes/gmInsert.c.md - "// notes §k" refers there.

// notes §1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gmInsert.h"

// Helix Native's own registration (its Info.plist): a music effect
#define HELIX_TYPE            'aumf'
#define HELIX_SUBTYPE         '6HPA'
#define HELIX_MANUFACTURER    'LSIX'

#define GM_SETTINGS_DIR       "Library/Application Support/GenBridge Monitor"
#define GM_INSERT_STATE       "helix-native.plist"

static AudioUnit gUnit       = NULL;
static void      (*gClosing)(void) = NULL;

// The audio thread's, between AudioUnitRender() and the unit pulling its input
static float *   gInLeft     = NULL;
static float *   gInRight    = NULL;
static Float64   gSampleTime = 0.0;

static AudioComponent helix_component(void) {
    AudioComponentDescription desc = {
        .componentType         = HELIX_TYPE,
        .componentSubType      = HELIX_SUBTYPE,
        .componentManufacturer = HELIX_MANUFACTURER,
    };

    return AudioComponentFindNext(NULL, &desc);
}

bool gm_insert_available(void) {
    return helix_component() != NULL;
}

bool gm_insert_is_open(void) {
    return gUnit != NULL;
}

AudioUnit gm_insert_unit(void) {
    return gUnit;
}

void gm_insert_on_close(void ( *closing )(void)) {
    gClosing = closing;
}

static void state_path(char * out, size_t len) {
    const char * home = getenv("HOME");

    snprintf(out, len, "%s/%s/%s", (home != NULL) ? home : "", GM_SETTINGS_DIR, GM_INSERT_STATE);
}

// notes §3
static void save_state(void) {
    CFPropertyListRef plist = NULL;
    UInt32            size  = sizeof(plist);

    if (  (AudioUnitGetProperty(gUnit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &plist, &size) != noErr)
       || (plist == NULL)) {
        return;
    }
    CFDataRef         data  = CFPropertyListCreateData(NULL, plist, kCFPropertyListBinaryFormat_v1_0, 0, NULL);

    CFRelease(plist);

    if (data == NULL) {
        return;
    }
    char              path[1200];
    FILE *            f;

    state_path(path, sizeof(path));
    f = fopen(path, "wb");

    if (f != NULL) {
        fwrite(CFDataGetBytePtr(data), 1, (size_t)CFDataGetLength(data), f);
        fclose(f);
    }
    CFRelease(data);
}

static void restore_state(void) {
    char              path[1200];
    FILE *            f;

    state_path(path, sizeof(path));
    f = fopen(path, "rb");

    if (f == NULL) {
        return;
    }
    fseek(f, 0, SEEK_END);
    long              size  = ftell(f);

    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return;
    }
    UInt8 *           bytes = (UInt8 *)malloc((size_t)size);

    if ((bytes == NULL) || (fread(bytes, 1, (size_t)size, f) != (size_t)size)) {
        free(bytes);
        fclose(f);
        return;
    }
    fclose(f);

    CFDataRef         data  = CFDataCreate(NULL, bytes, size);
    CFPropertyListRef plist = (data != NULL) ? CFPropertyListCreateWithData(NULL, data, kCFPropertyListImmutable, NULL, NULL)
                              : NULL;

    if (plist != NULL) {
        AudioUnitSetProperty(gUnit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &plist, sizeof(plist));
        CFRelease(plist);
    }

    if (data != NULL) {
        CFRelease(data);
    }
    free(bytes);
}

// notes §2 - the unit pulls the monitor's audio from here
static OSStatus render_input(void * refCon, AudioUnitRenderActionFlags * flags, const AudioTimeStamp * when,
                             UInt32 bus, UInt32 frames, AudioBufferList * data) {
    (void)refCon;
    (void)flags;
    (void)when;
    (void)bus;

    float * source[2] = {gInLeft, gInRight};

    for (UInt32 b = 0; (b < data->mNumberBuffers) && (b < 2); b++) {
        if (source[b] == NULL) {
            memset(data->mBuffers[b].mData, 0, frames * sizeof(float));
        } else if (data->mBuffers[b].mData != source[b]) {   // an in-place unit hands back our own buffers
            memcpy(data->mBuffers[b].mData, source[b], frames * sizeof(float));
        }
    }

    return noErr;
}

bool gm_insert_open(double rate, uint32_t maxFrames) {
    AudioComponent              component = helix_component();

    gm_insert_close();

    if ((component == NULL) || (AudioComponentInstanceNew(component, &gUnit) != noErr)) {
        gUnit = NULL;
        return false;
    }
    // Stereo, 32-bit float, one buffer a channel - the AU canonical format, at the output's rate
    AudioStreamBasicDescription format    = {
        .mSampleRate       = rate,
        .mFormatID         = kAudioFormatLinearPCM,
        .mFormatFlags      = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved,
        .mBytesPerPacket   = sizeof(float),
        .mFramesPerPacket  =                                                                     1,
        .mBytesPerFrame    = sizeof(float),
        .mChannelsPerFrame =                                                                     2,
        .mBitsPerChannel   =                                                                    32,
    };
    UInt32                      max       = maxFrames;
    AURenderCallbackStruct      callback  = {.inputProc = render_input, .inputProcRefCon = NULL};

    if (  (AudioUnitSetProperty(gUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, sizeof(format)) != noErr)
       || (AudioUnitSetProperty(gUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &format, sizeof(format)) != noErr)
       || (AudioUnitSetProperty(gUnit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &max, sizeof(max)) != noErr)
       || (AudioUnitSetProperty(gUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback, sizeof(callback)) != noErr)
       || (AudioUnitInitialize(gUnit) != noErr)) {
        AudioComponentInstanceDispose(gUnit);
        gUnit = NULL;
        return false;
    }
    restore_state();
    gSampleTime = 0.0;
    return true;
}

void gm_insert_close(void) {
    if (gUnit == NULL) {
        return;
    }

    if (gClosing != NULL) {
        gClosing();
    }
    save_state();
    AudioUnitUninitialize(gUnit);
    AudioComponentInstanceDispose(gUnit);
    gUnit = NULL;
}

void gm_insert_process(float * left, float * right, uint32_t frames) {
    AudioUnit                  unit  = gUnit;

    if (unit == NULL) {
        return;
    }
    struct {
        AudioBufferList list;
        AudioBuffer     second;
    }                          buffers;
    AudioUnitRenderActionFlags flags = 0;
    AudioTimeStamp             when  = {.mSampleTime = gSampleTime, .mFlags = kAudioTimeStampSampleTimeValid};

    gInLeft                     = left;
    gInRight                    = right;
    buffers.list.mNumberBuffers = 2;
    buffers.list.mBuffers[0]    = (AudioBuffer){
        1, frames * sizeof(float), left
    };
    buffers.list.mBuffers[1]    = (AudioBuffer){
        1, frames * sizeof(float), right
    };

    // notes §2 - on failure the block passes dry rather than silent
    (void)AudioUnitRender(unit, &flags, &when, 0, frames, &buffers.list);
    gSampleTime                += frames;
}
