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
// Notes: Docs/code-notes/gbAlign.c.md - "// notes §k" refers there.

#ifndef GB_ALIGN_H
#define GB_ALIGN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// notes §1 - every instance in the host is a sender; one in the Align role is a receiver

typedef struct tGbAlignSender     tGbAlignSender;
typedef struct tGbAlignReceiver   tGbAlignReceiver;

// A receiver setting a sender's Extra Latency. Called from the align thread.
typedef void (*tGbAlignApply)(void * owner, double extraMs);

tGbAlignSender * gb_align_sender_create(void * owner, tGbAlignApply apply);
void gb_align_sender_destroy(tGbAlignSender * sender);

// Audio thread: what this instance just output, and the Extra Latency it is reporting.
void gb_align_sender_write(tGbAlignSender * sender, float *const * out, uint32_t channels, uint32_t frames, double rate, double extraMs, int statusSlot);

// The user set Extra Latency by hand, so the panel stops saying a receiver chose it.
void gb_align_sender_manual(tGbAlignSender * sender);
bool gb_align_sender_auto_set(const tGbAlignSender * sender);

tGbAlignReceiver * gb_align_receiver_create(void);
void gb_align_receiver_destroy(tGbAlignReceiver * receiver);

// Audio thread: what arrived on the recording track this block.
void gb_align_receiver_write(tGbAlignReceiver * receiver, const float *const * in, uint32_t channels, uint32_t frames, double rate, bool recording);

// The Re-align button: zero the sender's Extra Latency and measure afresh (notes §6).
void gb_align_receiver_realign(tGbAlignReceiver * receiver);

typedef enum {
    eGbAlignListening = 0,     // nothing measured yet
    eGbAlignNoSender,          // no GenBridge capturing anywhere in this host
    eGbAlignNoAudio,           // a sender is playing and nothing arrives here
    eGbAlignUnsure,            // audio arrives but matches no sender clearly
    eGbAlignAligned,
    eGbAlignAdjusting,         // a correction was sent, waiting for the host to apply it
    eGbAlignHostStuck,         // the correction was sent and the delay did not move
    eGbAlignHeldRecording,     // late, but not corrected mid-take
    eGbAlignAtLimit,           // would need more than GB_EXTRA_LATENCY_MAX_MS
    eGbAlignTwoSenders         // two GenBridges send the same audio, so either could be the one
} tGbAlignState;

typedef struct {
    tGbAlignState state;
    double        lateMs;        // the last confirmed route delay
    double        extraMs;       // the sender's Extra Latency
    int           senderSlot;    // its status slot, for its device name; -1 when none
} tGbAlignStatus;

// Main thread.
void gb_align_receiver_status(tGbAlignReceiver * receiver, tGbAlignStatus * out);

#ifdef __cplusplus
}
#endif

#endif // GB_ALIGN_H
