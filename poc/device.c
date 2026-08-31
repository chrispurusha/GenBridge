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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include <CoreFoundation/CoreFoundation.h>
#pragma clang diagnostic pop

#include "device.h"

static AudioObjectPropertyAddress address_of(AudioObjectPropertySelector selector, bool isInput) {
    AudioObjectPropertyAddress address;

    address.mSelector = selector;
    address.mScope    = isInput ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput;
    address.mElement  = kAudioObjectPropertyElementMain;

    return address;
}

static bool string_property(AudioObjectID id, AudioObjectPropertySelector selector, char * out, uint32_t len) {
    AudioObjectPropertyAddress address = { selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef                value   = NULL;
    UInt32                     size    = sizeof(value);

    out[0] = '\0';

    if (AudioObjectGetPropertyData(id, &address, 0, NULL, &size, &value) != noErr) {
        return false;
    }

    if (value == NULL) {
        return false;
    }

    Boolean ok = CFStringGetCString(value, out, (CFIndex)len, kCFStringEncodingUTF8);

    CFRelease(value);

    return ok ? true : false;
}

static uint32_t channel_count(AudioObjectID id, bool isInput) {
    AudioObjectPropertyAddress address = address_of(kAudioDevicePropertyStreamConfiguration, isInput);
    UInt32                     size    = 0;

    if (AudioObjectGetPropertyDataSize(id, &address, 0, NULL, &size) != noErr) {
        return 0;
    }

    if (size == 0) {
        return 0;
    }

    AudioBufferList * list = (AudioBufferList *)malloc(size);

    if (list == NULL) {
        return 0;
    }

    uint32_t total = 0;

    if (AudioObjectGetPropertyData(id, &address, 0, NULL, &size, list) == noErr) {
        for (UInt32 i = 0; i < list->mNumberBuffers; i++) {
            total += list->mBuffers[i].mNumberChannels;
        }
    }

    free(list);

    return total;
}

double device_sample_rate(AudioObjectID id) {
    AudioObjectPropertyAddress address = { kAudioDevicePropertyNominalSampleRate,
                                           kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMain };
    Float64                    rate    = 0.0;
    UInt32                     size    = sizeof(rate);

    if (AudioObjectGetPropertyData(id, &address, 0, NULL, &size, &rate) != noErr) {
        return 0.0;
    }

    return (double)rate;
}

bool device_set_sample_rate(AudioObjectID id, double rate) {
    AudioObjectPropertyAddress address = { kAudioDevicePropertyNominalSampleRate,
                                           kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMain };
    Float64                    value   = (Float64)rate;

    return AudioObjectSetPropertyData(id, &address, 0, NULL, sizeof(value), &value) == noErr;
}

bool device_set_sample_rate_and_wait(AudioObjectID id, double rate) {
    if (device_sample_rate(id) == rate) {
        return true;
    }

    if (!device_set_sample_rate(id, rate)) {
        return false;
    }

    // Bounded, because this is called from setActive() - the HOST'S MAIN THREAD - where a plug-in
    // is expected to do its expensive set-up but not to stall the application. Two seconds per
    // instance made Ableton visibly slow to load a set with several of them. A device that has not
    // taken the rate in under a second is not going to.
    for (int i = 0; i < 40; i++) {
        usleep(20000);

        if (device_sample_rate(id) == rate) {
            return true;
        }
    }

    return false;
}

uint32_t device_buffer_frames(AudioObjectID id) {
    AudioObjectPropertyAddress address = { kAudioDevicePropertyBufferFrameSize,
                                           kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMain };
    UInt32                     frames  = 0;
    UInt32                     size    = sizeof(frames);

    if (AudioObjectGetPropertyData(id, &address, 0, NULL, &size, &frames) != noErr) {
        return 0;
    }

    return (uint32_t)frames;
}

bool device_set_buffer_frames(AudioObjectID id, uint32_t frames) {
    AudioObjectPropertyAddress address = { kAudioDevicePropertyBufferFrameSize,
                                           kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMain };
    UInt32                     value   = (UInt32)frames;

    return AudioObjectSetPropertyData(id, &address, 0, NULL, sizeof(value), &value) == noErr;
}

uint32_t device_latency_frames(AudioObjectID id, bool isInput) {
    uint32_t total = device_buffer_frames(id);
    UInt32   value = 0;
    UInt32   size  = sizeof(value);

    AudioObjectPropertyAddress address = address_of(kAudioDevicePropertyLatency, isInput);

    if (AudioObjectGetPropertyData(id, &address, 0, NULL, &size, &value) == noErr) {
        total += value;
    }

    address = address_of(kAudioDevicePropertySafetyOffset, isInput);
    size    = sizeof(value);

    if (AudioObjectGetPropertyData(id, &address, 0, NULL, &size, &value) == noErr) {
        total += value;
    }

    return total;
}

uint32_t device_enumerate(tDeviceInfo * list, uint32_t max) {
    AudioObjectPropertyAddress address = { kAudioHardwarePropertyDevices,
                                           kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMain };
    UInt32                     size    = 0;

    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, NULL, &size) != noErr) {
        return 0;
    }

    uint32_t        count = size / sizeof(AudioObjectID);
    AudioObjectID * ids   = (AudioObjectID *)malloc(size);

    if (ids == NULL) {
        return 0;
    }

    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, ids) != noErr) {
        free(ids);
        return 0;
    }

    uint32_t found = 0;

    for (uint32_t i = 0; (i < count) && (found < max); i++) {
        tDeviceInfo * info = &list[found];

        memset(info, 0, sizeof(*info));

        info->id = ids[i];

        string_property(ids[i], kAudioDevicePropertyDeviceNameCFString, info->name, DEVICE_NAME_LEN);
        string_property(ids[i], kAudioDevicePropertyDeviceUID, info->uid, DEVICE_UID_LEN);

        info->inputChannels  = channel_count(ids[i], true);
        info->outputChannels = channel_count(ids[i], false);
        info->sampleRate     = device_sample_rate(ids[i]);

        found++;
    }

    free(ids);

    return found;
}

bool device_find(const char * needle, bool needInput, tDeviceInfo * found) {
    tDeviceInfo list[DEVICE_MAX];
    uint32_t    count = device_enumerate(list, DEVICE_MAX);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t channels = needInput ? list[i].inputChannels : list[i].outputChannels;

        if (channels == 0) {
            continue;
        }

        if ((strcasestr(list[i].name, needle) != NULL) || (strcasestr(list[i].uid, needle) != NULL)) {
            *found = list[i];
            return true;
        }
    }

    return false;
}

// CoreAudio hands over an AudioBufferList whose layout varies by device: one buffer holding N
// interleaved channels, or N buffers of one channel each, or something in between. Rather than
// assume, walk the buffers and track a running channel index - which covers every layout with one
// piece of code, and is the reason this loop looks more general than it first appears it needs to.
static void gather(const AudioBufferList * list, float * out, uint32_t frames,
                   uint32_t firstChannel, uint32_t wanted) {
    uint32_t written = 0;
    uint32_t global  = 0;      // channel index across the whole device, not within one buffer

    memset(out, 0, (size_t)frames * wanted * sizeof(float));

    for (UInt32 b = 0; (b < list->mNumberBuffers) && (written < wanted); b++) {
        const AudioBuffer * buffer = &list->mBuffers[b];
        uint32_t            stride = buffer->mNumberChannels;
        const float *       src    = (const float *)buffer->mData;

        if (src == NULL) {
            global += stride;
            continue;
        }

        for (uint32_t c = 0; (c < stride) && (written < wanted); c++, global++) {
            // Skipping has to count across buffers, not within them: a device may present 32
            // channels as 32 single-channel buffers, one 32-channel buffer, or anything between,
            // and "channel 17" must mean the same thing in every case.
            if (global < firstChannel) {
                continue;
            }

            for (uint32_t f = 0; f < frames; f++) {
                out[((size_t)f * wanted) + written] = src[((size_t)f * stride) + c];
            }

            written++;
        }
    }
}

static void scatter(AudioBufferList * list, const float * in, uint32_t frames, uint32_t provided) {
    uint32_t taken = 0;

    for (UInt32 b = 0; b < list->mNumberBuffers; b++) {
        AudioBuffer * buffer = &list->mBuffers[b];
        uint32_t      stride = buffer->mNumberChannels;
        float *       dst    = (float *)buffer->mData;

        if (dst == NULL) {
            continue;
        }

        for (uint32_t c = 0; c < stride; c++) {
            if (taken < provided) {
                for (uint32_t f = 0; f < frames; f++) {
                    dst[((size_t)f * stride) + c] = in[((size_t)f * provided) + taken];
                }

                taken++;
            } else {
                // More device channels than the caller supplies - silence the rest, or they carry
                // whatever the previous cycle left behind.
                for (uint32_t f = 0; f < frames; f++) {
                    dst[((size_t)f * stride) + c] = 0.0f;
                }
            }
        }
    }
}

static OSStatus io_proc(AudioObjectID device, const AudioTimeStamp * now,
                        const AudioBufferList * inputData, const AudioTimeStamp * inputTime,
                        AudioBufferList * outputData, const AudioTimeStamp * outputTime,
                        void * client) {
    (void)device;
    (void)now;
    (void)inputTime;
    (void)outputTime;

    tDeviceStream * stream = (tDeviceStream *)client;

    if (stream->isInput) {
        if ((inputData == NULL) || (inputData->mNumberBuffers == 0)) {
            return noErr;
        }

        uint32_t frames = inputData->mBuffers[0].mDataByteSize
                          / (uint32_t)(sizeof(float) * inputData->mBuffers[0].mNumberChannels);

        if (frames > stream->scratchFrames) {
            frames = stream->scratchFrames;
        }

        gather(inputData, stream->scratch, frames, stream->firstChannel, stream->channels);
        stream->callback(stream->user, stream->scratch, NULL, frames);
    } else {
        if ((outputData == NULL) || (outputData->mNumberBuffers == 0)) {
            return noErr;
        }

        uint32_t frames = outputData->mBuffers[0].mDataByteSize
                          / (uint32_t)(sizeof(float) * outputData->mBuffers[0].mNumberChannels);

        if (frames > stream->scratchFrames) {
            frames = stream->scratchFrames;
        }

        stream->callback(stream->user, NULL, stream->scratch, frames);
        scatter(outputData, stream->scratch, frames, stream->channels);
    }

    return noErr;
}

bool device_open(tDeviceStream * stream, AudioObjectID id, bool isInput,
                 uint32_t firstChannel, uint32_t channels,
                 uint32_t maxFrames, tDeviceCallback callback, void * user) {
    memset(stream, 0, sizeof(*stream));

    stream->id             = id;
    stream->isInput        = isInput;
    stream->firstChannel   = firstChannel;
    stream->channels       = channels;
    stream->deviceChannels = channel_count(id, isInput);
    stream->callback       = callback;
    stream->user           = user;
    stream->scratchFrames  = maxFrames;
    stream->scratch        = (float *)calloc((size_t)maxFrames * channels, sizeof(float));

    if (stream->scratch == NULL) {
        return false;
    }

    if (stream->deviceChannels < (firstChannel + channels)) {
        fprintf(stderr, "device has %u %s channels; %u from channel %u requested\n",
                stream->deviceChannels, isInput ? "input" : "output", channels, firstChannel + 1);
        return false;
    }

    OSStatus status = AudioDeviceCreateIOProcID(id, io_proc, stream, &stream->procId);

    if (status != noErr) {
        fprintf(stderr, "AudioDeviceCreateIOProcID failed: %d\n", (int)status);
        return false;
    }

    return true;
}

bool device_start(tDeviceStream * stream) {
    OSStatus status = AudioDeviceStart(stream->id, stream->procId);

    if (status != noErr) {
        fprintf(stderr, "AudioDeviceStart failed: %d\n", (int)status);
        return false;
    }

    stream->running = true;

    return true;
}

void device_stop(tDeviceStream * stream) {
    if (stream->running) {
        AudioDeviceStop(stream->id, stream->procId);
        stream->running = false;
    }
}

void device_close(tDeviceStream * stream) {
    device_stop(stream);

    if (stream->procId != NULL) {
        AudioDeviceDestroyIOProcID(stream->id, stream->procId);
        stream->procId = NULL;
    }

    free(stream->scratch);
    stream->scratch = NULL;
}
