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

#ifndef DEVICE_H
#define DEVICE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include <CoreAudio/CoreAudio.h>
#pragma clang diagnostic pop

// The HAL directly - AudioDeviceCreateIOProcID - rather than a HAL output AudioUnit as
// G2-Edit's audioOutput.c uses. An AudioUnit is the right choice when something must be rendered
// INTO a device and the unit's own pull model is convenient. Here two devices are being run
// against each other and the interesting quantity is when each one's callback fires relative to
// the other, so the extra layer only gets in the way. It is also what AudioMovers' feeder does.

#define DEVICE_NAME_LEN    (128)
#define DEVICE_UID_LEN     (256)
#define DEVICE_MAX         (64)

typedef struct {
    AudioObjectID id;
    char          name[DEVICE_NAME_LEN];
    char          uid[DEVICE_UID_LEN];
    uint32_t      inputChannels;
    uint32_t      outputChannels;
    double        sampleRate;
} tDeviceInfo;

// Called from the device's real-time thread. Exactly one of input/output is non-NULL, according
// to how the stream was opened. Buffers are interleaved float, 'channels' wide as opened.
typedef void (*tDeviceCallback)(void * user, const float * input, float * output, uint32_t frames);

typedef struct {
    AudioObjectID       id;
    AudioDeviceIOProcID procId;
    bool                isInput;
    bool                running;
    uint32_t            firstChannel;  // first device channel to take
    uint32_t            channels;      // channels the caller asked for
    uint32_t            deviceChannels;// channels the device actually presents on that scope
    float *             scratch;       // interleaved staging buffer
    uint32_t            scratchFrames;
    tDeviceCallback     callback;
    void *              user;
} tDeviceStream;

uint32_t device_enumerate(tDeviceInfo * list, uint32_t max);

// Case-insensitive substring match on name or UID. needInput selects which scope must be present.
bool     device_find(const char * needle, bool needInput, tDeviceInfo * found);

double   device_sample_rate(AudioObjectID id);
bool     device_set_sample_rate(AudioObjectID id, double rate);

// Setting a nominal rate is ASYNCHRONOUS - AudioObjectSetPropertyData returns before the device has
// changed, and reading it straight back returns the old value. This polls until it takes. Never
// call it from an audio callback; it can block for a second or more.
bool     device_set_sample_rate_and_wait(AudioObjectID id, double rate);
uint32_t device_buffer_frames(AudioObjectID id);
bool     device_set_buffer_frames(AudioObjectID id, uint32_t frames);

// What the device will actually accept. Asking for less than the minimum is simply refused, and a
// refusal is indistinguishable from a device that changed its mind - so ask first.
bool     device_buffer_frame_range(AudioObjectID id, uint32_t * minFrames, uint32_t * maxFrames);

// deviceLatency + safetyOffset + bufferFrames + streamLatency. The first three are what AudioMovers'
// feeder logs separately; the fourth is declared on the STREAM rather than the device and is zero on
// every USB and Thunderbolt interface here - but 2399 frames on the built-in microphone, which is
// how it stayed missing. Together they are what a host must be told about.
uint32_t device_latency_frames(AudioObjectID id, bool isInput);

// firstChannel is the device channel the first returned channel comes from, so a stereo pair can
// be taken from anywhere on a 32 input interface rather than always from 1/2.
bool     device_open(tDeviceStream * stream, AudioObjectID id, bool isInput,
                     uint32_t firstChannel, uint32_t channels,
                     uint32_t maxFrames, tDeviceCallback callback, void * user);
// Told when a device is plugged in, unplugged, or otherwise appears or vanishes.
//
// NOTHING NOTICED HOT-PLUG BEFORE THIS. The device list is enumerated when something asks for it and
// the plug-in only asks when a parameter changes, so a USB interface switched on after a project was
// opened stayed invisible until the user touched a control - which is precisely the case the
// "waiting for a saved device" state exists to serve, and it would have waited for ever.
//
// The callback comes from a CoreAudio thread, so it must do no more than set a flag and wake
// somebody. Registering the same `user` twice replaces the first entry rather than adding a second.
typedef void (*tDeviceListChanged)(void * user);

bool     device_watch_list(tDeviceListChanged callback, void * user);
void     device_unwatch_list(void * user);

bool     device_start(tDeviceStream * stream);
void     device_stop(tDeviceStream * stream);
void     device_close(tDeviceStream * stream);

#ifdef __cplusplus
}
#endif

#endif // DEVICE_H
