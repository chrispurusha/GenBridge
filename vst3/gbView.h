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

// A click that the host must be told about, handed back to gbEditor.mm which owns the controller.
typedef void (*tGbEditCallback)(void * user, const tGbEditRequest * request);

// Returns an NSView *, as a void * so the C++ side need not import AppKit.
void * gb_view_create(double width, double height, tGbEditCallback callback, void * user,
                      int statusSlot);
void   gb_view_set_status_slot(void * view, int statusSlot);
void   gb_view_destroy(void * view);
void   gb_view_set_values(void * view, double device, double rate, double frames, double trim,
                          double mode, double firstChannel);

#ifdef __cplusplus
}
#endif

#endif // GB_VIEW_H
