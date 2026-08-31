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

#ifndef GB_DRAW_H
#define GB_DRAW_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "device.h"

// The editor's contents, drawn through SynthLib's renderer - the same render_rectangle(),
// render_text() and draw_button() the sibling applications use, so this cannot drift away from
// their look without the change being visible in all of them.

// The logical canvas. Every coordinate below is in these units and is scaled to whatever surface
// the host gives us, so the panel is the same shape at any window size.
#define GB_CANVAS_W    (520.0)
#define GB_CANVAS_H    (340.0)

// A click landed on a control that the HOST must be told about, because these are VST3 parameters
// and changing one behind the host's back would leave its automation and its saved state wrong.
typedef enum {
    eGbEditNone = 0,
    eGbEditDevice,
    eGbEditRate,
    eGbEditFrames,
    eGbEditTrim,
    eGbEditMode,
    eGbEditFirstChannel,
} tGbEdit;

typedef struct {
    tGbEdit which;
    double  normalized;    // the new value, already normalised for the parameter
} tGbEditRequest;

// HOW A NORMALISED DEVICE PARAMETER BECOMES A DEVICE, in one place.
//
// This existed three times - in the editor, in the controller's value-to-string, and in the
// processor - and two of them used a different scale. A VST3 stepped parameter has a FIXED step
// count, decided at registration and cached by the host, so it cannot follow how many devices the
// machine happens to have; the editor scaled across the real device count instead, which meant the
// same normalised value named one device in the panel and opened another. The symptom was hearing
// a Kronos while the panel said Analog Keys.
//
// So: the slot count is fixed, every caller uses these two functions, and the only thing that
// varies is how many of the slots currently point at a real device.
#define GB_DEVICE_SLOTS    (DEVICE_MAX)

int    gb_device_slot(double normalized);           // normalised -> slot index
double gb_device_normalized(int slot);              // slot index -> normalised
int    gb_input_device_count(void);                 // how many slots point at something real
void   gb_input_device_name(int slot, char * out, unsigned long len);
int    gb_slot_for_uid(const char * uid);           // -1 when the device is not present
void   gb_device_list_invalidate(void);             // after a change, so the next read is fresh

// The stepper lists, shared with the processor so the two cannot disagree about what index 2 means.
extern const double gGbRates[];
extern const int    gGbFrames[];
extern const int    gGbRateCount;
extern const int    gGbFrameCount;

void gb_draw_init(void);
void gb_draw_set_status_slot(int slot);
void gb_draw_frame(int pixelWidth, int pixelHeight);

// Hit test in LOGICAL units. Returns true when something was hit; request describes what the host
// should be told.
bool gb_draw_click(double x, double y, tGbEditRequest * request);

// Current parameter values, so the editor draws what the host believes rather than its own idea.
void gb_draw_set_values(double device, double rate, double frames, double trim,
                        double mode, double firstChannel);

#ifdef __cplusplus
}
#endif

#endif // GB_DRAW_H
