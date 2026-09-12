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
// Notes: Docs/code-notes/gbDraw.c.md - "// notes §k" refers there.

// notes §1

#include <math.h>      // round, to snap the offset to the fine grid after a coarse step
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "gbDraw.h"
#include "gbMidi.h"
#include "gbStatus.h"

#include "device.h"
#include "synthlibDefs.h"
#include "synthlibGlobals.h"
#include "geometry.h"
#include "synthlibHost.h"
#include "synthlibTypes.h"
#include "utilsGraphics.h"
#include "contextMenu.h"
#include "synthlibTypes.h"

#define FONT_PATH             "/System/Library/Fonts/Supplemental/Arial.ttf"
#define FONT_PRELOAD_SIZE     (16.0)

#define ROW_H                 (26.0)
#define LABEL_W               (74.0)
#define ARROW_W               (22.0)
#define TEXT_H                (13.0)

// The lists the stepper parameters index into. These must agree with the processor's own mapping,
// which is why both read them from here rather than each keeping a copy.
// The stepper lists are defined in gbParams.c, beside the parameter table that scales them.

static bool   gFontReady  = false;

// notes §2
#define GB_MENU_BG    ((tRgb){ 0.22, 0.22, 0.24 })

static tCoord gMouse      = { 0.0, 0.0 };

static void gb_mouse_coord(tCoord * coord) {
    if (coord != NULL) {
        *coord = gMouse;
    }
}

void gb_draw_set_mouse(double x, double y) {
    gMouse.x = x;
    gMouse.y = y;
}

bool gb_draw_menu_active(void) {
    return gContextMenu.active;
}

// What a menu selection produced, picked up by gb_draw_click() on the click that made it. The action
// callback carries only an index, so the menu that is open has to be recorded alongside it - the
// same "keep it in your own app-local struct" arrangement contextMenu.h describes for G2-Edit.
static tGbEdit gMenuFor      = eGbEditNone;
static int     gMenuChoice   = -1;

// Labels must outlive the click that opens the menu: tMenuItem holds a const char *, it does not
// copy. One buffer per slot, filled when the menu is built.
#define GB_MENU_MAX      (GB_DEVICE_SLOTS + 1)
#define GB_MENU_LABEL    (64)

static char       gMenuLabels[GB_MENU_MAX][GB_MENU_LABEL];
static tMenuItem  gMenuItems[GB_MENU_MAX];

static void gb_menu_action(int index) {
    gMenuChoice = index;
}

// A menu wide enough for the longest name it holds would swallow the panel, so the cell width is
// capped and long names are simply truncated by the renderer's own clipping. Two columns is what
// fits GB_CANVAS_W at that width; beyond what two columns hold, the arrows remain the way through.
static void gb_open_menu(tGbEdit which, int count, tRectangle anchor) {
    int columns = 1;

    if (count > GB_MENU_MAX - 1) {
        count = GB_MENU_MAX - 1;
    }

    for (int i = 0; i < count; i++) {
        gMenuItems[i].label           = gMenuLabels[i];
        gMenuItems[i].colour          = GB_MENU_BG;
        gMenuItems[i].action          = gb_menu_action;
        gMenuItems[i].param           = (uint32_t)i;
        gMenuItems[i].subMenu         = NULL;
        gMenuItems[i].subMenuColumns  = 0;
        gMenuItems[i].subMenuCellWidth = 0.0;
    }
    gMenuItems[count].label  = NULL;
    gMenuItems[count].action = NULL;

    // 22 px a row (STANDARD_TEXT_HEIGHT + 5*2). Two columns once one would not fit the canvas.
    if ((count * 22) > (int)(GB_CANVAS_H - 40.0)) {
        columns = 2;
    }
    gMenuFor    = which;
    gMenuChoice = -1;
    open_context_menu(below_rect(anchor), gMenuItems, (uint32_t)columns, 210.0);
}

// Set by the view before every frame rather than once at creation: like the status slot, one editor
// must not answer for another's. With an effect and an instrument both open, whichever drew last
// would otherwise decide the layout for both.
static bool   gInstrument = false;

void gb_draw_set_instrument(bool instrument) {
    gInstrument = instrument;
}
static double gDevice    = 0.0;
static double gRate      = 0.0;
static double gFrames    = 0.0;
static double gTrim      = 0.5;
static double gMode      = 1.0;
static double gFirst     = 0.0;
static double gMidiDest  = 0.0;
static double gOffset    = 0.5;
static double gTestNote  = 60.0 / 127.0;
static double gMidiChan  = 0.0;
static double gSource    = 0.0;    // 0 = an audio device, 1 = the host's own input


// Which processor's figures this panel shows. -1 until the host has connected the two ends, which
// it may do before or after the editor opens - so the panel simply shows no live figures until it
// knows, rather than showing another instance's.
static int    gStatusSlot = -1;

void gb_draw_set_status_slot(int slot) {
    gStatusSlot = slot;
}

// notes §3
#define ROW_TOP          (56.0)
#define ROW_STEP         (36.0)

// notes §4
typedef enum {
    eRowSource = 0,
    eRowDevice,
    eRowRate,
    eRowFrames,
    eRowMode,
    eRowFirstChannel,
    eRowMidiDest,
    eRowMidiChannel,
    eRowCount
} tGbRow;

// Where a row actually sits. On the effect there is no Source row, so everything below it moves up.
static int row_of(tGbRow which) {
    return gInstrument ? (int)which : ((int)which - 1);
}

static int row_count(void) {
    // The effect drops three: the source, and the two MIDI rows.
    return gInstrument ? (int)eRowCount : ((int)eRowCount - 3);
}

static double row_y(int row) {
    return ROW_TOP + (ROW_STEP * (double)row);
}

// Where the block of steppers ends, and everything below it begins.
static double rows_bottom(void) {
    return row_y(row_count() - 1) + ROW_STEP;
}

static double trim_y(void)      { return rows_bottom() + 8.0; }
static double level_y(void)     { return trim_y() + 30.0; }
static double measure_y(void)   { return level_y() + 40.0; }
static double offset_y(void)    { return measure_y() + 34.0; }
static double telemetry_y(void) { return (gInstrument ? offset_y() : level_y()) + 42.0; }


// notes §5
static bool row_is_menu(int row) {
    (void)row;
    return true;    // every stepper row is a drop-down; the offset below them keeps its arrows
}

// notes §6
static bool row_live(tGbRow which) {
    if (gSource < 0.5) {
        return true;
    }

    switch (which) {
        case eRowDevice:
        case eRowRate:
        case eRowFrames:
        case eRowMode:
        case eRowFirstChannel:
            return false;

        default:
            return true;
    }
}

static tRectangle row_prev(int row) {
    return (tRectangle){ { LABEL_W, row_y(row) }, { ARROW_W, ROW_H - 6.0 } };
}

static tRectangle row_next(int row) {
    return (tRectangle){ { GB_CANVAS_W - 30.0 - ARROW_W, row_y(row) }, { ARROW_W, ROW_H - 6.0 } };
}

static tRectangle row_value(int row) {
    double x     = row_is_menu(row) ? LABEL_W : (LABEL_W + ARROW_W + 6.0);
    double right = row_is_menu(row) ? (GB_CANVAS_W - 30.0) : (GB_CANVAS_W - 36.0 - ARROW_W);

    return (tRectangle){ { x, row_y(row) }, { right - x, ROW_H - 6.0 } };
}

#define RIGHT_GUTTER    (74.0)     // room for the trim readout, which sits outside the track


// notes §7
#define GB_OFFSET_STEP_FINE_MS      (0.1)
#define GB_OFFSET_STEP_COARSE_MS    (1.0)

#define OFFSET_VALUE_W              (56.0)     // "-100.0 ms" with room around it

// notes §8
#define OFFSET_GAP                  (10.0)

#define MEASURE_LABEL     "Measure"
#define BUTTON_H          (18.0)

// draw_button() renders its label at the RECTANGLE'S OWN HEIGHT, not at some smaller text size, and
// then draws the box a margin larger than what it was passed. Sizing the width from TEXT_H got both
// of those wrong at once and the word ran out of its box.
static tRectangle measure_button(void) {
    return (tRectangle){ { LABEL_W, measure_y() },
                         { get_text_width(MEASURE_LABEL, BUTTON_H, eCache), BUTTON_H } };
}

// What draw_button actually paints, and therefore what a click has to land in.
static tRectangle measure_bounds(void) {
    return draw_button_bounds(measure_button());
}

// notes §9
#define NOTE_ARROW_W    (20.0)
#define NOTE_VALUE_W    (34.0)
#define NOTE_GAP        (6.0)
#define NOTE_X          (356.0)

static tRectangle note_arrow(int slot) {
    double x = NOTE_X + ((double)slot * (NOTE_ARROW_W + NOTE_GAP + 4.0));

    if (slot >= 2) {
        x += NOTE_VALUE_W + NOTE_GAP;
    }

    return (tRectangle){ { x, measure_y() }, { NOTE_ARROW_W, BUTTON_H } };
}

static tRectangle note_value_box(void) {
    return (tRectangle){ { NOTE_X + (2.0 * (NOTE_ARROW_W + NOTE_GAP + 4.0)), measure_y() },
                         { NOTE_VALUE_W, BUTTON_H } };
}

// C-2 IS NOTE 0 - middle C as C3, which is what the hardware this is pointed at prints on its own
// screen. Naming note 0 "C-1" instead would put a user an octave out on the one device that made
// this control necessary.
static void note_name(int note, char * out, unsigned long len) {
    static const char * const kName[12] = { "C",  "C#", "D",  "D#", "E",  "F",
                                            "F#", "G",  "G#", "A",  "A#", "B" };

    note = (note < 0) ? 0 : ((note > 127) ? 127 : note);
    snprintf(out, len, "%s%d", kName[note % 12], (note / 12) - 2);
}

// notes §10
static tRectangle offset_arrow(int slot) {
    double x = LABEL_W + ((double)slot * (ARROW_W + OFFSET_GAP));

    if (slot >= 2) {
        x += OFFSET_VALUE_W + OFFSET_GAP;
    }

    return (tRectangle){ { x, offset_y() }, { ARROW_W, 20.0 } };
}

static tRectangle offset_value_box(void) {
    return (tRectangle){ { LABEL_W + (2.0 * (ARROW_W + OFFSET_GAP)), offset_y() },
                         { OFFSET_VALUE_W, 20.0 } };
}

static tRectangle trim_track(void) {
    return (tRectangle){ { LABEL_W, trim_y() }, { GB_CANVAS_W - LABEL_W - RIGHT_GUTTER, 14.0 } };
}

static bool hit(tRectangle r, double x, double y) {
    return (x >= r.coord.x) && (x <= (r.coord.x + r.size.w))
           && (y >= r.coord.y) && (y <= (r.coord.y + r.size.h));
}

// notes §11
static tDeviceInfo gCachedList[DEVICE_MAX];
static uint32_t    gCachedCount = 0;
static double      gCachedAt    = -1000.0;
static bool        gCacheValid  = false;

static double monotonic_seconds(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (double)ts.tv_sec + ((double)ts.tv_nsec * 1e-9);
}

// notes §12
static const tDeviceInfo * device_list(uint32_t * count) {
    double now = monotonic_seconds();

    if (!gCacheValid || ((now - gCachedAt) > 30.0)) {
        gCachedCount = device_enumerate(gCachedList, DEVICE_MAX);
        gCachedAt    = now;
        gCacheValid  = true;
    }

    *count = gCachedCount;

    return gCachedList;
}

void gb_device_list_invalidate(void) {
    gCacheValid = false;
}

int gb_slot_for_uid(const char * uid) {
    uint32_t            count = 0;
    const tDeviceInfo * list  = device_list(&count);
    int                 seen  = 1;      // slot 0 is None - see gb_resolve_slot() in gbBridge.c

    if ((uid == NULL) || (uid[0] == '\0')) {
        return -1;
    }

    for (uint32_t i = 0; i < count; i++) {
        if (list[i].inputChannels == 0) {
            continue;
        }

        if (strcmp(list[i].uid, uid) == 0) {
            return seen;
        }

        seen++;
    }

    return -1;
}

int gb_device_slot(double normalized) {
    int slot = (int)((normalized * (double)(GB_DEVICE_SLOTS - 1)) + 0.5);

    if (slot < 0) {
        slot = 0;
    } else if (slot > (GB_DEVICE_SLOTS - 1)) {
        slot = GB_DEVICE_SLOTS - 1;
    }

    return slot;
}

double gb_device_normalized(int slot) {
    return (double)slot / (double)(GB_DEVICE_SLOTS - 1);
}

// How many input-capable devices there are. The device parameter is normalised across a FIXED slot
// count, so this is only used to stop the arrows walking past the end of the real list.
int gb_input_device_count(void) {
    uint32_t            count = 0;
    const tDeviceInfo * list  = device_list(&count);
    int                 found = 1;      // None is always selectable, so it always counts

    for (uint32_t i = 0; i < count; i++) {
        if (list[i].inputChannels > 0) {
            found++;
        }
    }

    return found;
}

// notes §13
int gb_input_device_channels(int index) {
    uint32_t            count = 0;
    const tDeviceInfo * list  = device_list(&count);
    int                 seen  = 1;      // slot 0 is None - see gb_resolve_slot() in gbBridge.c

    if (index <= 0) {
        return 0;       // None has no channels, and 0 means "unknown" to the callers, which is right
    }

    for (uint32_t i = 0; i < count; i++) {
        if (list[i].inputChannels == 0) {
            continue;
        }

        if (seen == index) {
            return (int)list[i].inputChannels;
        }
        seen++;
    }

    return 0;
}

void gb_input_device_name(int index, char * out, unsigned long len) {
    uint32_t            count = 0;
    const tDeviceInfo * list  = device_list(&count);
    int                 seen  = 1;      // slot 0 is None - see gb_resolve_slot() in gbBridge.c

    snprintf(out, len, "%s", "None");

    if (index <= 0) {
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        if (list[i].inputChannels == 0) {
            continue;
        }

        if (seen == index) {
            snprintf(out, len, "%s", list[i].name);
            return;
        }

        seen++;
    }
}

// Kept for the rows that genuinely do step across a short fixed list.

void gb_draw_set_values(double device, double rate, double frames, double trim,
                        double mode, double firstChannel, double midiDest, double offset,
                        double midiChannel, double testNote, double source) {
    gSource   = source;
    gMidiDest = midiDest;
    gOffset   = offset;
    gMidiChan = midiChannel;
    gTestNote = testNote;
    gDevice = device;
    gRate   = rate;
    gFrames = frames;
    gTrim   = trim;
    gMode   = mode;
    gFirst  = firstChannel;
}

void gb_draw_init(void) {
    render_backend_init();

    // The renderer is TOLD what the colours mean rather than including an application's defs.h.
    // Values come from SynthLib's own synthlibDefs.h so the plug-in cannot drift from the siblings.
    configure_synthlib_theme((tSynthLibTheme){
        .topBarHeight   = 0.0,                       // no menu bar in this panel
        .orange1        = (tRgb)RGB_ORANGE_1,
        .orange2        = (tRgb)RGB_ORANGE_2,
        .greenOn        = (tRgb)RGB_GREEN_ON,
        .backgroundGrey = (tRgb)RGB_BACKGROUND_GREY,
    });

    // contextMenu.c reaches back for the pointer position through this rather than declaring its own
    // extern - see synthlibHost.h. The redraw half is already covered by SynthLib/plugin/pluginStubs.c. No
    // pointerCaptured predicate: this panel never hides the pointer for a drag.
    synthlib_host_init((tSynthLibHost){ .mouseCoord = gb_mouse_coord, .pointerCaptured = NULL });

    gFontReady = preload_glyph_textures(FONT_PATH, FONT_PRELOAD_SIZE);
}

static void label(double x, double y, const char * text) {
    set_rgb_colour((tRgb){ 0.72, 0.72, 0.74 });
    render_text(mainArea, (tRectangle){ { x, y }, { 0.0, TEXT_H } }, text);
}

static void value_box(int row, const char * text, bool live) {
    tRectangle box = row_value(row);

    set_rgb_colour(live ? (tRgb){ 0.16, 0.16, 0.18 } : (tRgb){ 0.22, 0.22, 0.24 });
    render_rectangle(mainArea, box);

    // A DIM VALUE IS STILL A VALUE. What a greyed row shows is what the plug-in would go back to,
    // which is worth reading - so it drops a tier rather than blanking, the same treatment the
    // sibling projects' panels give a figure that is held rather than live.
    set_rgb_colour(live ? (tRgb){ 0.92, 0.92, 0.94 } : (tRgb){ 0.48, 0.48, 0.50 });
    render_text(mainArea, (tRectangle){ { box.coord.x + 8.0, box.coord.y + 5.0 }, { 0.0, TEXT_H } }, text);
}

static void stepper_row(tGbRow which, const char * labelText, const char * valueText) {
    int  row  = row_of(which);
    bool live = row_live(which);

    if (live) {
        label(20.0, row_y(row) + 5.0, labelText);
    } else {
        set_rgb_colour((tRgb){ 0.46, 0.46, 0.48 });
        render_text(mainArea, (tRectangle){ { 20.0, row_y(row) + 5.0 }, { 0.0, TEXT_H } }, labelText);
    }

    if (!row_is_menu(row)) {
        draw_button(mainArea, row_prev(row), "<", (tRgb){ 0.30, 0.30, 0.33 });
        draw_button(mainArea, row_next(row), ">", (tRgb){ 0.30, 0.30, 0.33 });
    }

    value_box(row, valueText, live);
}

// notes §14
#define STAT_VALUE_DX    (52.0)

// notes §15
#define GB_CAPTION_GREY    {0.60, 0.60, 0.63}

static void stat(double x, double y, const char * name, const char * value) {
    set_rgb_colour((tRgb)GB_CAPTION_GREY);
    render_text(mainArea, (tRectangle){ { x, y }, { 0.0, 11.0 } }, name);

    set_rgb_colour((tRgb){ 0.70, 0.70, 0.73 });
    render_text(mainArea, (tRectangle){ { x + STAT_VALUE_DX, y }, { 0.0, 11.0 } }, value);
}

static void meter(double x, double y, double w, float peak) {
    tRectangle back = { { x, y }, { w, 10.0 } };

    set_rgb_colour((tRgb){ 0.14, 0.14, 0.16 });
    render_rectangle(mainArea, back);

    double level = (peak > 1.0f) ? 1.0 : (double)peak;

    if (level > 0.0) {
        tRgb colour = (level > 0.98) ? (tRgb){ 0.90, 0.25, 0.20 }
                      : (level > 0.7) ? (tRgb){ 0.90, 0.72, 0.20 }
                      : (tRgb){ 0.35, 0.78, 0.42 };

        set_rgb_colour(colour);
        render_rectangle(mainArea, (tRectangle){ { x, y }, { w * level, 10.0 } });
    }
}

void gb_draw_frame(int pixelWidth, int pixelHeight) {
    tGbStatus * status = gb_status(gStatusSlot);
    char        buffer[192];

    if ((pixelWidth <= 0) || (pixelHeight <= 0)) {
        return;
    }

    render_backend_set_surface(pixelWidth, pixelHeight);
    set_render_width(pixelWidth);
    set_render_height(pixelHeight);

    // The logical canvas is fixed, so the panel is the same shape whatever size the host makes the
    // window - the whole layout simply scales. G2-Edit maps onto a 1280 unit canvas because that is
    // the width its patch area was designed at; this panel has its own, much smaller, one.
    gGlobalGuiScale = (double)pixelWidth / GB_CANVAS_W;

    render_backend_clear((tRgb)RGB_BACKGROUND_GREY);

    if (!gFontReady) {
        render_backend_flush();
        return;
    }

    // ---- header ----
    set_rgb_colour((tRgb){ 0.95, 0.95, 0.97 });
    render_text(mainArea, (tRectangle){ { 20.0, 18.0 }, { 0.0, 20.0 } }, "GenBridge");

    if (gSource >= 0.5) {
        // notes §16
        if (status == NULL) {
            // NOT AN ANSWER, so it does not claim one. Without a status slot the panel knows nothing
            // about what the processor is receiving, and saying "audio from the host" here would be
            // a guess dressed as a report.
            set_rgb_colour((tRgb){ 0.72, 0.72, 0.74 });
            snprintf(buffer, sizeof(buffer), "%s", "external instrument - waiting for the processor");
        } else if (atomic_load(&status->hostInputPresent) == 0) {
            set_rgb_colour((tRgb){ 0.85, 0.60, 0.25 });
            snprintf(buffer, sizeof(buffer), "%s",
                     "external instrument - NO AUDIO from the host: check the side-chain routing");
        } else {
            set_rgb_colour((tRgb){ 0.45, 0.75, 0.50 });
            snprintf(buffer, sizeof(buffer), "%s",
                     "external instrument - audio from the host, latency measured");
        }
    } else if ((status != NULL) && atomic_load(&status->offlineRender)) {
        // AHEAD OF "capturing", because during an offline render it is still nominally capturing
        // and that is precisely the misleading thing to show. A bounce faster than realtime drains
        // the ring - the device cannot be hurried - so the render is silent whatever else is true.
        set_rgb_colour((tRgb){ 0.85, 0.60, 0.25 });
        snprintf(buffer, sizeof(buffer), "%s", "host is rendering offline - bounce in real time");
    } else if ((status != NULL) && atomic_load(&status->active)) {
        set_rgb_colour((tRgb){ 0.45, 0.75, 0.50 });
        snprintf(buffer, sizeof(buffer), "capturing %s", status->deviceName);
    } else if ((status != NULL) && (atomic_load(&status->waitingForDevice) != 0)) {
        // NAMED, not just "no device". The whole point of the wait is that the plug-in knows exactly
        // what it is waiting for, and a user who sees the name knows what to plug in.
        set_rgb_colour((tRgb){ 0.85, 0.60, 0.25 });
        snprintf(buffer, sizeof(buffer), "waiting for %s", status->waitingName);
    } else {
        // Amber rather than grey: a plug-in that has failed to open a device looks exactly like one
        // whose device happens to be silent, and the two want very different responses.
        set_rgb_colour((tRgb){ 0.85, 0.60, 0.25 });
        snprintf(buffer, sizeof(buffer), "%s",
                 (status == NULL) ? "no device selected"
                 : ((atomic_load(&status->deviceRate) > 0)
                    ? "not capturing - device unavailable"
                    : "no device selected"));
    }

    render_text(mainArea, (tRectangle){ { 20.0, 38.0 }, { 0.0, 11.0 } }, buffer);

    // notes §17
    if (gInstrument) {
        stepper_row(eRowSource, "Source",
                    (gSource < 0.5) ? "Audio device" : "Host input (external instrument)");
    }

    if ((status != NULL) && (atomic_load(&status->waitingForDevice) != 0)) {
        snprintf(buffer, sizeof(buffer), "%s", status->waitingName);
    } else {
        gb_input_device_name(gb_device_slot(gDevice), buffer, sizeof(buffer));
    }
    stepper_row(eRowDevice, "Device", buffer);

    snprintf(buffer, sizeof(buffer), "%.0f Hz",
             gGbRates[(int)(gRate * (double)(gGbRateCount - 1) + 0.5)]);
    stepper_row(eRowRate, "Rate", buffer);

    // notes §18
    {
        int asked  = gGbFrames[(int)(gFrames * (double)(gGbFrameCount - 1) + 0.5)];
        int actual = (status != NULL) ? atomic_load(&status->deviceFrames) : 0;

        int shared = (status != NULL) ? atomic_load(&status->deviceShared) : 0;

        if ((actual > 0) && (actual != asked) && shared) {
            // notes §19
            snprintf(buffer, sizeof(buffer), "%d samples  (shared - device at %d)", asked, actual);
        } else if ((actual > 0) && (actual != asked)) {
            snprintf(buffer, sizeof(buffer), "%d samples  (device gave %d)", asked, actual);
        } else {
            snprintf(buffer, sizeof(buffer), "%d samples", asked);
        }
    }
    stepper_row(eRowFrames, "Buffer", buffer);

    stepper_row(eRowMode, "Mode", (gMode < 0.5) ? "Mono" : "Stereo");

    // Shown as the channel numbers a person would read off the back of the interface, so 1-based -
    // and as a pair when in stereo, because "channel 3" meaning "3 and 4" is exactly the sort of
    // thing that gets a take recorded off the wrong output.
    int first = (int)(gFirst * (double)(GB_MAX_FIRST_CHANNEL - 1) + 0.5);

    if (gMode < 0.5) {
        snprintf(buffer, sizeof(buffer), "%d", first + 1);
    } else {
        snprintf(buffer, sizeof(buffer), "%d - %d", first + 1, first + 2);
    }

    stepper_row(eRowFirstChannel, "Input", buffer);

    // The instrument's own row, and the measurement it enables.
    if (gInstrument) {
        gb_midi_destination_name((int)(gMidiDest * (double)(GB_MIDI_MAX_DEST - 1) + 0.5),
                                 buffer, sizeof(buffer));
        stepper_row(eRowMidiDest, "MIDI Out", buffer);

        int channel = (int)(gMidiChan * (double)(GB_CHANNEL_SLOTS - 1) + 0.5);

        if (channel <= 0) {
            snprintf(buffer, sizeof(buffer), "%s", "Source (as the DAW sends)");
        } else {
            snprintf(buffer, sizeof(buffer), "%d", channel);
        }

        stepper_row(eRowMidiChannel, "Channel", buffer);
    }

    // ---- trim ----
    if (gInstrument) {
        label(20.0, measure_y() + 4.0, "Latency");

        draw_button(mainArea, measure_button(), MEASURE_LABEL, (tRgb){ 0.30, 0.42, 0.55 });

        {
            static const char * const kNoteArrow[4] = { "<<", "<", ">", ">>" };

            char       note[8];
            int        value = (int)((gTestNote * 127.0) + 0.5);
            tRectangle valueBox = note_value_box();

            for (int slot = 0; slot < 4; slot++) {
                draw_button(mainArea, note_arrow(slot), kNoteArrow[slot], (tRgb){ 0.30, 0.30, 0.33 });
            }

            note_name(value, note, sizeof(note));
            set_rgb_colour((tRgb){ 0.92, 0.92, 0.94 });

            // eNoCache: the width of a formatted buffer, and the cache is keyed on the pointer.
            double noteW = get_text_width(note, 11.0, eNoCache);

            render_text(mainArea,
                        (tRectangle){ { valueBox.coord.x + ((valueBox.size.w - noteW) / 2.0),
                                        measure_y() + 5.0 }, { 0.0, 11.0 } }, note);
        }

        int measured = (status != NULL) ? atomic_load(&status->measuredSamples) : 0;
        int rate     = (status != NULL) ? atomic_load(&status->deviceRate) : 48000;

        if (rate <= 0) {
            rate = 48000;
        }

        int failed   = (status != NULL) ? atomic_load(&status->measureFailed) : 0;
        int ranEmpty = (status != NULL) ? atomic_load(&status->measureRanEmpty) : 0;

        if (failed) {
            snprintf(buffer, sizeof(buffer), "%s", "measurement failed - try again");
            set_rgb_colour((tRgb){ 0.85, 0.60, 0.25 });
        } else if (measured > 0) {
            // notes §20
            int low   = (status != NULL) ? atomic_load(&status->measuredLow) : 0;
            int high  = (status != NULL) ? atomic_load(&status->measuredHigh) : 0;
            int trips = (status != NULL) ? atomic_load(&status->measuredTrips) : 0;
            double perMs = (double)rate / 1000.0;

            if ((trips > 0) && (high > low)) {
                snprintf(buffer, sizeof(buffer), "%.1f ms  (%d trips, %.1f - %.1f)",
                         (double)measured / perMs, trips, (double)low / perMs, (double)high / perMs);
            } else {
                snprintf(buffer, sizeof(buffer), "%d smp (%.1f ms) measured", measured,
                         (double)measured / perMs);
            }
            set_rgb_colour((tRgb){ 0.72, 0.72, 0.74 });
        } else if (ranEmpty) {
            // RAN, AND CAME BACK WITH NOTHING. Indistinguishable from "never measured" until now,
            // which is exactly the wrong thing to show: one is a starting state and the other is a
            // result that needs acting on.
            snprintf(buffer, sizeof(buffer), "%s", "onset beat our own latency - see log");
            set_rgb_colour((tRgb){ 0.85, 0.60, 0.25 });
        } else {
            snprintf(buffer, sizeof(buffer), "%s", "not measured");
            set_rgb_colour((tRgb){ 0.72, 0.72, 0.74 });
        }

        tRectangle bounds = measure_bounds();

        render_text(mainArea,
                    (tRectangle){ { bounds.coord.x + bounds.size.w + 14.0, measure_y() + 5.0 },
                                  { 0.0, 11.0 } }, buffer);

        // notes §21
        label(20.0, offset_y() + 4.0, "In use");

        static const char * const kArrow[4] = { "<<", "<", ">", ">>" };

        for (int slot = 0; slot < 4; slot++) {
            draw_button(mainArea, offset_arrow(slot), kArrow[slot], (tRgb){ 0.30, 0.30, 0.33 });
        }

        double     offsetMs = GB_OFFSET_MIN_MS + (gOffset * (GB_OFFSET_MAX_MS - GB_OFFSET_MIN_MS));
        tRectangle valueBox = offset_value_box();

        snprintf(buffer, sizeof(buffer), "%+.1f ms", offsetMs);
        set_rgb_colour((tRgb){ 0.92, 0.92, 0.94 });

        // CENTRED, and measured WITHOUT the cache. get_text_width's cache is keyed on the string's
        // POINTER, and this is a reused stack buffer - a cached width would be whatever was last
        // formatted into it. eNoCache is the only correct answer for a formatted string.
        double textW = get_text_width(buffer, 11.0, eNoCache);

        render_text(mainArea,
                    (tRectangle){ { valueBox.coord.x + ((valueBox.size.w - textW) / 2.0),
                                    offset_y() + 6.0 }, { 0.0, 11.0 } }, buffer);

        // WHICH WAY IT MOVES THE RECORDING, spelled out. "+8 ms" tells nobody whether their part
        // will end up earlier or later, and getting it backwards doubles the error instead of
        // removing it.
        set_rgb_colour((tRgb)GB_CAPTION_GREY);

        if (offsetMs > 0.05) {
            snprintf(buffer, sizeof(buffer), "%s", "recording pulled earlier");
        } else if (offsetMs < -0.05) {
            snprintf(buffer, sizeof(buffer), "%s", "recording pushed later");
        } else {
            // Zero here now means NOTHING is being corrected, which after a measurement would be a
            // fault rather than a default - so it points at the measurement instead of offering
            // the old "nudge me if takes sound late" advice.
            snprintf(buffer, sizeof(buffer), "%s", "no correction - measure, or set by ear");
        }

        render_text(mainArea, (tRectangle){ { 270.0, offset_y() + 6.0 }, { 0.0, 11.0 } }, buffer);
    }

    label(20.0, trim_y() + 2.0, "Trim");

    tRectangle track = trim_track();

    set_rgb_colour((tRgb){ 0.16, 0.16, 0.18 });
    render_rectangle(mainArea, track);

    set_rgb_colour((tRgb){ 0.35, 0.62, 0.85 });
    render_rectangle(mainArea, (tRectangle){ track.coord, { track.size.w * gTrim, track.size.h } });

    snprintf(buffer, sizeof(buffer), "%.2fx", gTrim * 2.0);
    set_rgb_colour((tRgb){ 0.72, 0.72, 0.74 });
    render_text(mainArea, (tRectangle){ { track.coord.x + track.size.w + 6.0, trim_y() + 2.0 }, { 0.0, 11.0 } }, buffer);

    // ---- meters ----
    label(20.0, level_y() + 2.0, "Level");
    meter(LABEL_W, level_y(), GB_CANVAS_W - LABEL_W - RIGHT_GUTTER,
          (status != NULL) ? atomic_load(&status->peakLeft) : 0.0f);
    meter(LABEL_W, level_y() + 14.0, GB_CANVAS_W - LABEL_W - RIGHT_GUTTER,
          (status != NULL) ? atomic_load(&status->peakRight) : 0.0f);

    // notes §22
    int rate = (status != NULL) ? atomic_load(&status->deviceRate) : 0;

    if (rate <= 0) {
        rate = 48000;
    }

    double perMs = (double)rate / 1000.0;

    int ring   = (status != NULL) ? atomic_load(&status->ringSamples) : 0;
    int device = (status != NULL) ? atomic_load(&status->deviceSamples) : 0;
    int filter = (status != NULL) ? atomic_load(&status->filterSamples) : 0;
    int hw     = (status != NULL) ? atomic_load(&status->measuredSamples) : 0;
    int off    = (status != NULL) ? atomic_load(&status->offsetSamples) : 0;
    int total  = (status != NULL) ? atomic_load(&status->latencySamples) : 0;

    // Four columns across the panel, so a figure that grows a digit does not shove its neighbours.
    const double kCol[4] = { 20.0, 145.0, 270.0, 395.0 };

    double y = telemetry_y();

    snprintf(buffer, sizeof(buffer), "%d", ring);
    stat(kCol[0], y, "ring", buffer);

    snprintf(buffer, sizeof(buffer), "%d", device);
    stat(kCol[1], y, "device", buffer);

    snprintf(buffer, sizeof(buffer), "%d", filter);
    stat(kCol[2], y, "filter", buffer);

    if (gInstrument) {
        y += 16.0;

        // notes §23
        snprintf(buffer, sizeof(buffer), "%d (%.1f ms)", hw, (double)hw / perMs);
        stat(kCol[0], y, "measured", buffer);

        snprintf(buffer, sizeof(buffer), "%+d (%+.1f ms)", off, (double)off / perMs);
        stat(kCol[2], y, "in use", buffer);
    }

    y += 20.0;

    // Hand-rolled rather than stat(), because its value spans two columns - but it is the same
    // caption, so it takes the same colour.
    set_rgb_colour((tRgb)GB_CAPTION_GREY);
    render_text(mainArea, (tRectangle){ { kCol[0], y }, { 0.0, 12.0 } }, "reported");

    set_rgb_colour((tRgb){ 0.88, 0.88, 0.91 });
    snprintf(buffer, sizeof(buffer), "%d smp", total);
    render_text(mainArea, (tRectangle){ { kCol[0] + STAT_VALUE_DX, y }, { 0.0, 12.0 } }, buffer);

    snprintf(buffer, sizeof(buffer), "%.1f ms", (double)total / perMs);
    render_text(mainArea, (tRectangle){ { kCol[1] + STAT_VALUE_DX, y }, { 0.0, 12.0 } }, buffer);

    y += 20.0;

    // notes §24
    {
        double setpoint    = (status != NULL) ? atomic_load(&status->setpointFrames) : 0.0;
        double recommended = (status != NULL) ? atomic_load(&status->recommendedFrames) : 0.0;

        if ((recommended > 0.0) && ((setpoint + 1.0) < recommended)) {
            snprintf(buffer, sizeof(buffer), "%.0f/%.0f (rec %.0f)",
                     (status != NULL) ? atomic_load(&status->fillFrames) : 0.0, setpoint, recommended);
        } else {
            snprintf(buffer, sizeof(buffer), "%.0f/%.0f",
                     (status != NULL) ? atomic_load(&status->fillFrames) : 0.0, setpoint);
        }
    }
    stat(kCol[0], y, "fill", buffer);

    snprintf(buffer, sizeof(buffer), "%+.2f", (status != NULL) ? atomic_load(&status->driftPpm) : 0.0);
    stat(kCol[1], y, "drift", buffer);

    snprintf(buffer, sizeof(buffer), "%d", (status != NULL) ? atomic_load(&status->underruns) : 0);
    stat(kCol[2], y, "under", buffer);

    snprintf(buffer, sizeof(buffer), "%d", (status != NULL) ? atomic_load(&status->resyncs) : 0);
    stat(kCol[3], y, "resync", buffer);

    // notes §25
    if (gInstrument) {
        y += 20.0;

        snprintf(buffer, sizeof(buffer), "%d / %d",
                 (status != NULL) ? atomic_load(&status->eventsIn) : 0,
                 (status != NULL) ? atomic_load(&status->eventsOut) : 0);
        stat(kCol[0], y, "notes", buffer);
    }

    // LAST, so it draws over everything - and the hover update goes here rather than in the click
    // path because the highlight has to follow the pointer while no button is down.
    update_context_menu_hover();
    render_context_menu();

    render_backend_flush();
}

// Stepping a normalised list parameter. Clamped rather than wrapped: an arrow that jumps from the
// last device back to the first looks like a glitch when the list is long.
static double step(double normalized, int count, int delta) {
    if (count <= 1) {
        return 0.0;
    }

    int index = (int)(normalized * (double)(count - 1) + 0.5) + delta;

    if (index < 0) {
        index = 0;
    } else if (index > (count - 1)) {
        index = count - 1;
    }

    return (double)index / (double)(count - 1);
}

bool gb_draw_click(double x, double y, tGbEditRequest * request) {
    struct { tGbEdit which; double * value; int count; } rows[] = {
        { eGbEditDevice,       &gDevice, 0                     },   // handled separately, see below
        { eGbEditRate,         &gRate,   gGbRateCount          },
        { eGbEditFrames,       &gFrames, gGbFrameCount         },
        { eGbEditMode,         &gMode,   2                     },
        { eGbEditFirstChannel, &gFirst,  GB_MAX_FIRST_CHANNEL  },
        { eGbEditMidiDest,     &gMidiDest, GB_MIDI_MAX_DEST     },
        { eGbEditMidiChannel,  &gMidiChan, GB_CHANNEL_SLOTS     },
    };

    request->which = eGbEditNone;

    // THE MENU GETS FIRST REFUSAL, and a click anywhere while it is open belongs to it - either
    // choosing an item or dismissing it. Letting the click fall through to the rows underneath would
    // mean dismissing the menu and working a control in the same gesture.
    if (gContextMenu.active) {
        gMenuChoice = -1;
        handle_context_menu_click((tCoord){ x, y });

        if ((gMenuChoice >= 0) && (gMenuFor != eGbEditNone)) {
            int    choice = gMenuChoice;
            tGbEdit which = gMenuFor;

            gMenuChoice    = -1;
            gMenuFor       = eGbEditNone;
            request->which = which;

            switch (which) {
                case eGbEditDevice:
                    request->normalized = gb_device_normalized(choice);
                    break;

                case eGbEditRate:
                    request->normalized = (double)choice / (double)(gGbRateCount - 1);
                    break;

                case eGbEditFrames:
                    request->normalized = (double)choice / (double)(gGbFrameCount - 1);
                    break;

                case eGbEditMode:
                    request->normalized = (double)choice;       // 0 mono, 1 stereo
                    break;

                case eGbEditFirstChannel:
                    request->normalized = (double)choice / (double)(GB_MAX_FIRST_CHANNEL - 1);
                    break;

                case eGbEditMidiDest:
                    request->normalized = (double)choice / (double)(GB_MIDI_MAX_DEST - 1);
                    break;

                case eGbEditMidiChannel:
                    request->normalized = (double)choice / (double)(GB_CHANNEL_SLOTS - 1);
                    break;

                case eGbEditSource:
                    request->normalized = (double)choice;       // 0 device, 1 host input
                    break;

                default:
                    request->which = eGbEditNone;
                    return false;
            }
            return true;
        }
        return false;
    }

    // The VALUE between the arrows opens a menu; the arrows themselves still step. Both reach the
    // same parameter, so nothing is lost either way - see the note on gMouse above for why both.
    {
        struct { tGbEdit which; tGbRow row; } menus[] = {
            { eGbEditSource,       eRowSource       },
            { eGbEditDevice,       eRowDevice       },
            { eGbEditRate,         eRowRate         },
            { eGbEditFrames,       eRowFrames       },
            { eGbEditMode,         eRowMode         },
            { eGbEditFirstChannel, eRowFirstChannel },
            { eGbEditMidiDest,     eRowMidiDest     },
            { eGbEditMidiChannel,  eRowMidiChannel  },
        };
        // notes §26
        int channels = gb_input_device_channels(gb_device_slot(gDevice));

        for (unsigned m = 0; m < (sizeof(menus) / sizeof(menus[0])); m++) {
            // A ROW THE HOST-INPUT MODE HAS GREYED DOES NOT ANSWER A CLICK. Drawing it dim and then
            // opening its menu anyway would be the worst of both: it says the control is inert and
            // then behaves as though it is not.
            if (!row_live(menus[m].row) || (row_of(menus[m].row) >= row_count())
                || (row_of(menus[m].row) < 0)
                || !hit(row_value(row_of(menus[m].row)), x, y)) {
                continue;
            }
            int count = 0;

            switch (menus[m].which) {
                case eGbEditSource:
                    count = 2;
                    snprintf(gMenuLabels[0], sizeof(gMenuLabels[0]), "%s", "Audio device");
                    snprintf(gMenuLabels[1], sizeof(gMenuLabels[1]), "%s",
                             "Host input (external instrument)");
                    break;

                case eGbEditDevice:
                    count = gb_input_device_count();

                    for (int i = 0; (i < count) && (i < GB_MENU_MAX - 1); i++) {
                        gb_input_device_name(i, gMenuLabels[i], sizeof(gMenuLabels[i]));
                    }
                    break;

                case eGbEditRate:
                    count = gGbRateCount;

                    for (int i = 0; i < count; i++) {
                        snprintf(gMenuLabels[i], sizeof(gMenuLabels[i]), "%.0f Hz", gGbRates[i]);
                    }
                    break;

                case eGbEditFrames:
                    count = gGbFrameCount;

                    for (int i = 0; i < count; i++) {
                        snprintf(gMenuLabels[i], sizeof(gMenuLabels[i]), "%d samples", gGbFrames[i]);
                    }
                    break;

                case eGbEditMode:
                    // Stereo is not offered by a device that has one input to give.
                    count = (channels == 1) ? 1 : 2;
                    snprintf(gMenuLabels[0], sizeof(gMenuLabels[0]), "%s", "Mono");

                    if (count > 1) {
                        snprintf(gMenuLabels[1], sizeof(gMenuLabels[1]), "%s", "Stereo");
                    }
                    break;

                case eGbEditFirstChannel: {
                    int width = (gMode < 0.5) ? 1 : 2;

                    // notes §27
                    count = ((channels > 0) ? channels : GB_MAX_FIRST_CHANNEL) - width + 1;

                    if (count < 1) {
                        count = 1;
                    }

                    if (count > GB_MAX_FIRST_CHANNEL) {
                        count = GB_MAX_FIRST_CHANNEL;
                    }

                    for (int i = 0; i < count; i++) {
                        if (width == 1) {
                            snprintf(gMenuLabels[i], sizeof(gMenuLabels[i]), "%d", i + 1);
                        } else {
                            snprintf(gMenuLabels[i], sizeof(gMenuLabels[i]), "%d - %d", i + 1, i + 2);
                        }
                    }
                    break;
                }

                case eGbEditMidiDest:
                    count = GB_MIDI_MAX_DEST;

                    if (count > GB_MENU_MAX - 1) {
                        count = GB_MENU_MAX - 1;
                    }

                    for (int i = 0; i < count; i++) {
                        gb_midi_destination_name(i, gMenuLabels[i], sizeof(gMenuLabels[i]));
                    }
                    break;

                case eGbEditMidiChannel:
                    count = GB_CHANNEL_SLOTS;
                    snprintf(gMenuLabels[0], sizeof(gMenuLabels[0]), "%s", "Source (as the DAW sends)");

                    for (int i = 1; i < count; i++) {
                        snprintf(gMenuLabels[i], sizeof(gMenuLabels[i]), "%d", i);
                    }
                    break;

                default:
                    break;
            }

            if (count > 0) {
                gb_open_menu(menus[m].which, count, row_value(row_of(menus[m].row)));
            }
            return false;
        }
    }

    for (int row = 0; row < row_count(); row++) {
        int delta = 0;

        if (row_is_menu(row)) {
            continue;   // its whole width is the menu, handled above
        }

        if (hit(row_prev(row), x, y)) {
            delta = -1;
        } else if (hit(row_next(row), x, y)) {
            delta = 1;
        }

        if (delta == 0) {
            continue;
        }

        request->which = rows[row].which;

        if (rows[row].which == eGbEditDevice) {
            // Whole SLOTS, not a fraction of the device count - the scale has to match what the
            // processor will do with the value. Clamped to the devices that actually exist so the
            // arrows cannot walk into empty slots, which open nothing and sound like a fault.
            int slot  = gb_device_slot(gDevice) + delta;
            int limit = gb_input_device_count() - 1;

            if (slot < 0) {
                slot = 0;
            } else if (slot > limit) {
                slot = limit;
            }

            request->normalized = gb_device_normalized(slot);
        } else if (rows[row].which == eGbEditMode) {
            // notes §28
            int channels = gb_input_device_channels(gb_device_slot(gDevice));
            int wanted   = (int)((gMode < 0.5) ? 0 : 1) + delta;

            if (wanted < 0) {
                wanted = 0;
            } else if (wanted > 1) {
                wanted = 1;
            }

            if ((channels == 1) && (wanted > 0)) {
                wanted = 0;
            }
            request->normalized = (double)wanted;
        } else if (rows[row].which == eGbEditFirstChannel) {
            // Limited to what the SELECTED device has, and to what the current mode will take from
            // it: a stereo pair needs two channels, so the last usable start is one lower.
            int channels = gb_input_device_channels(gb_device_slot(gDevice));
            int width    = (gMode < 0.5) ? 1 : 2;
            int first    = (int)((gFirst * (double)(GB_MAX_FIRST_CHANNEL - 1)) + 0.5) + delta;
            int limit    = GB_MAX_FIRST_CHANNEL - 1;

            // A channel count of 0 means the cache could not say, so the fixed range stands rather
            // than the control seizing at channel 1 on a device it simply has not seen yet.
            if (channels > 0) {
                limit = channels - width;
            }

            if (limit < 0) {
                limit = 0;      // a mono-only device in stereo mode: channel 1 is all there is
            }

            if (first < 0) {
                first = 0;
            } else if (first > limit) {
                first = limit;
            }
            request->normalized = (double)first / (double)(GB_MAX_FIRST_CHANNEL - 1);
        } else {
            request->normalized = step(*rows[row].value, rows[row].count, delta);
        }

        return true;
    }

    if (gInstrument) {
        if (hit(measure_bounds(), x, y)) {
            request->which      = eGbEditMeasure;
            request->normalized = 1.0;          // a trigger; the plug-in acts on the rising edge
            return true;
        }

        for (int slot = 0; slot < 4; slot++) {
            if (!hit(draw_button_bounds(note_arrow(slot)), x, y)) {
                continue;
            }

            static const int kNoteStep[4] = { -12, -1, 1, 12 };

            int note = (int)((gTestNote * 127.0) + 0.5) + kNoteStep[slot];

            // Clamped rather than wrapped, for the reason every other stepper here is: an arrow
            // that jumps from the top of a range to the bottom reads as a fault. An octave step
            // that would fall off the end lands ON the end, so C-2 is always one click away.
            note = (note < 0) ? 0 : ((note > 127) ? 127 : note);

            request->which      = eGbEditTestNote;
            request->normalized = (double)note / 127.0;
            return true;
        }

        // notes §29
        for (int slot = 0; slot < 4; slot++) {
            if (!hit(offset_arrow(slot), x, y)) {
                continue;
            }

            static const double kStep[4] = { -GB_OFFSET_STEP_COARSE_MS, -GB_OFFSET_STEP_FINE_MS,
                                             GB_OFFSET_STEP_FINE_MS, GB_OFFSET_STEP_COARSE_MS };

            double ms = GB_OFFSET_MIN_MS + (gOffset * (GB_OFFSET_MAX_MS - GB_OFFSET_MIN_MS));

            ms += kStep[slot];

            // SNAPPED TO THE FINE GRID, which is what makes a coarse step usable on a measured
            // figure: Measure dials in 11.83 ms, and without this every coarse click would carry
            // that .03 along for ever while the reading claimed a round number.
            ms = round(ms / GB_OFFSET_STEP_FINE_MS) * GB_OFFSET_STEP_FINE_MS;

            if (ms < GB_OFFSET_MIN_MS) {
                ms = GB_OFFSET_MIN_MS;
            } else if (ms > GB_OFFSET_MAX_MS) {
                ms = GB_OFFSET_MAX_MS;
            }

            request->which      = eGbEditOffset;
            request->normalized = (ms - GB_OFFSET_MIN_MS) / (GB_OFFSET_MAX_MS - GB_OFFSET_MIN_MS);
            return true;
        }
    }

    tRectangle track = trim_track();

    if (hit(track, x, y)) {
        double v = (x - track.coord.x) / track.size.w;

        request->which      = eGbEditTrim;
        request->normalized = (v < 0.0) ? 0.0 : ((v > 1.0) ? 1.0 : v);
        return true;
    }

    return false;
}
