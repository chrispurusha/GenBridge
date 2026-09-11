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

#ifndef GB_VIEW_H
#define GB_VIEW_H

#include "gbDraw.h"

#ifdef __cplusplus
extern "C" {
#endif

// A click the host must be told about, handed back to gbPlugin.c.
typedef void (*tGbEditCallback)(void * user, const tGbEditRequest * request);

// CALLED BEFORE EVERY FRAME AND EVERY CLICK, on the main thread, to put THIS editor's status slot and
// parameter values into the draw layer - which keeps both file-scope, so with two editors open
// whichever set them last would otherwise speak for both. The callback answers with
// gb_view_set_status_slot() and gb_view_set_values().
typedef void (*tGbSyncCallback)(void * user, void * view);

// Returns an NSView *, RETAINED, as a void * so C need not import AppKit - SynthLib's wrappers own it
// from there and release it after taking it out of the host's window.
void * gb_view_create(double width, double height, tGbEditCallback callback, tGbSyncCallback sync,
                      void * user, bool instrument);
void   gb_view_set_status_slot(void * view, int statusSlot);
void   gb_view_set_values(void * view, double device, double rate, double frames, double trim,
                          double mode, double firstChannel, double midiDest, double offset,
                          double midiChannel, double testNote, double source);

#ifdef __cplusplus
}
#endif

#endif // GB_VIEW_H
