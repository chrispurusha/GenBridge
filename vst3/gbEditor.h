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

#ifndef GB_EDITOR_H
#define GB_EDITOR_H

#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

// Called from the editor view's destructor. THE HOST OWNS THE VIEW, not the controller: createView()
// hands over a reference the host releases whenever it closes the editor, and the object deletes
// itself at that point. Whoever kept the pointer has to be told, or it keeps a dangling one and
// writes through it the next time a status update or a parameter change comes round.
typedef void (*tGbEditorGone)(void * user);

// Called whenever the host resizes the editor. The size belongs to the CONTROLLER, not to the view:
// a view is built afresh every time the editor is opened, so anything it remembers itself is gone the
// moment the window closes. Both callbacks share one user pointer - the controller is the only thing
// either of them has ever wanted to talk to.
typedef void (*tGbEditorResized)(void * user, double width, double height);

Steinberg::IPlugView * gb_create_editor_view(Steinberg::Vst::IEditController * controller,
                                             Steinberg::Vst::IComponentHandler * handler,
                                             int statusSlot, bool instrument,
                                             double width, double height,
                                             tGbEditorGone gone, tGbEditorResized resized,
                                             void * user);

// The slot may arrive after the editor is open - a host connects the two ends whenever it likes.
void gb_editor_set_status_slot(Steinberg::IPlugView * view, int statusSlot);

// Re-read the controller's parameters into the panel. The editor otherwise pushes values only when
// it is attached and after its own clicks, so a value the PROCESSOR changed - a measurement seeding
// the offset, or a device change bringing that device's own correction with it - would sit unseen
// until the next click, and be stepped from as though it had never happened.
void gb_editor_refresh_values(Steinberg::IPlugView * view);

#endif // GB_EDITOR_H
