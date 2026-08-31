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

// The handful of functions SynthLib calls that an APPLICATION would provide and a plug-in has to.
//
// It is deliberately short. G2-Edit's equivalent is four hundred lines because it links that
// application's own renderer, which reaches for undo, a message queue and a module database; none
// of that is here, so what remains is only what SynthLib itself reaches for.
//
// synthlibGlobals.c is NOT linked, for the same reason G2-Edit does not link it: its
// synthlib_request_redraw() calls glfwPostEmptyEvent(), which would drag GLFW into a plug-in that
// deliberately has none. The two functions actually reached from the renderer are provided here.

#include <stdbool.h>
#include <stdint.h>

#include "synthlibTypes.h"

// NOT a stub - the same job, done differently. An application posts an empty event to wake a
// blocked GLFW loop; here the panel repaints on a timer regardless, so a request needs no action.
// It is defined rather than omitted because the renderer calls it from several places, and a
// missing symbol at link time is a poorer answer than a deliberate no-op.
void synthlib_request_redraw(void) {
}

// The application's wake-the-render-loop wrapper. One line there too.
void wake_glfw(void) {
    synthlib_request_redraw();
}

// The panel has no dials, so the mode never comes up. Rotary is the value the sibling projects
// default to, and returning anything else would only matter if a dial appeared here.
tDialMode synthlib_dial_mode(void) {
    return eDialModeRotary;
}

void synthlib_set_dial_mode(tDialMode mode) {
    (void)mode;
}

void synthlib_save_dial_mode(tDialMode mode) {
    (void)mode;
}

// No dial drags, so nothing is ever mid-drag and the cursor is never hidden.
bool is_cursor_hidden_dragging(void) {
    return false;
}

void finish_param_drag(void) {
}
