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

// THIS DRAWS THROUGH SYNTHLIB'S RENDERER, not through AppKit. Everything below is render_text(),
// render_rectangle() and draw_button() - the same calls the three sibling applications draw with -
// so the panel cannot drift away from their look without the change showing up in all of them.
//
// STEPPERS RATHER THAN DROP-DOWNS, deliberately. SynthLib has a context-menu system and it works in
// a plug-in, but pulling it in means linking the popup, menu-bar and click-region machinery for
// three settings. A pair of arrows either side of a value needs none of it, and the editor's whole
// job at this stage is to make the plug-in usable without the host's generic panel.
//
// EVERY CONTROL IS A VST3 PARAMETER, and a click returns a request rather than acting. The host has
// to be told through beginEdit/performEdit/endEdit or its automation and its saved state end up
// disagreeing with what the plug-in is actually doing - see gbEditor.mm.

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

#define FONT_PATH             "/System/Library/Fonts/Supplemental/Arial.ttf"
#define FONT_PRELOAD_SIZE     (16.0)

#define ROW_H                 (26.0)
#define LABEL_W               (74.0)
#define ARROW_W               (22.0)
#define TEXT_H                (13.0)

// The lists the stepper parameters index into. These must agree with the processor's own mapping,
// which is why both read them from here rather than each keeping a copy.
const double gGbRates[]    = { 44100.0, 48000.0, 88200.0, 96000.0 };
// Down to 16, which no USB device will accept but built-in and Thunderbolt hardware sometimes will.
// A device that cannot go that low says so through kAudioDevicePropertyBufferFrameSizeRange, and
// the request is clamped rather than failing - see the clamp in the plug-in's open path.
const int    gGbFrames[]   = { 16, 32, 64, 128, 256, 512, 1024 };
const int    gGbRateCount  = (int)(sizeof(gGbRates) / sizeof(gGbRates[0]));
const int    gGbFrameCount = (int)(sizeof(gGbFrames) / sizeof(gGbFrames[0]));

static bool   gFontReady  = false;

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
static double gMidiChan  = 0.0;

#define GB_CHANNEL_SLOTS    (17)

// Which processor's figures this panel shows. -1 until the host has connected the two ends, which
// it may do before or after the editor opens - so the panel simply shows no live figures until it
// knows, rather than showing another instance's.
static int    gStatusSlot = -1;

void gb_draw_set_status_slot(int slot) {
    gStatusSlot = slot;
}

// One table, used by BOTH the drawing and the hit test. Two copies of these numbers is how a
// control ends up looking like it is somewhere it cannot be clicked.
// EVERY POSITION IS COMPUTED FROM ONE PLACE, because the instrument has a row the effect does not
// and hard-coded coordinates simply drew the extra row on top of everything below it. The hit tests
// call the same functions the drawing does, so a control cannot appear somewhere it cannot be
// clicked.
#define ROW_TOP          (56.0)
#define ROW_STEP         (36.0)
#define ROW_COUNT        (7)          // the last two are MIDI destination and channel, instrument only

static int row_count(void) {
    return gInstrument ? ROW_COUNT : (ROW_COUNT - 2);
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

#define GB_MAX_FIRST_CHANNEL    (32)

static tRectangle row_prev(int row) {
    return (tRectangle){ { LABEL_W, row_y(row) }, { ARROW_W, ROW_H - 6.0 } };
}

static tRectangle row_next(int row) {
    return (tRectangle){ { GB_CANVAS_W - 30.0 - ARROW_W, row_y(row) }, { ARROW_W, ROW_H - 6.0 } };
}

static tRectangle row_value(int row) {
    double x = LABEL_W + ARROW_W + 6.0;

    return (tRectangle){ { x, row_y(row) }, { (GB_CANVAS_W - 36.0 - ARROW_W) - x, ROW_H - 6.0 } };
}

#define RIGHT_GUTTER    (74.0)     // room for the trim readout, which sits outside the track

#define GB_OFFSET_MIN_MS    (-100.0)
#define GB_OFFSET_MAX_MS    (100.0)
#define GB_OFFSET_STEP_MS   (0.1)

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

static tRectangle offset_down(void) {
    return (tRectangle){ { LABEL_W, offset_y() }, { ARROW_W, 20.0 } };
}

static tRectangle offset_up(void) {
    return (tRectangle){ { LABEL_W + ARROW_W + 6.0, offset_y() }, { ARROW_W, 20.0 } };
}

static tRectangle trim_track(void) {
    return (tRectangle){ { LABEL_W, trim_y() }, { GB_CANVAS_W - LABEL_W - RIGHT_GUTTER, 14.0 } };
}

static bool hit(tRectangle r, double x, double y) {
    return (x >= r.coord.x) && (x <= (r.coord.x + r.size.w))
           && (y >= r.coord.y) && (y <= (r.coord.y + r.size.h));
}

// THE DEVICE LIST IS CACHED, and that is not an optimisation - it is a correctness fix.
//
// device_enumerate() is a Core Audio HAL call that takes HAL locks. The panel repaints at 30 Hz and
// was calling it on every frame, so the editor was hammering those locks continuously - contending
// with the very open and close operations it was meant to be driving. Ableton became slow to load
// the plug-in and had to be force quit rather than shutting down. Enumerating once a second is
// still far more often than a device list actually changes.
static tDeviceInfo gCachedList[DEVICE_MAX];
static uint32_t    gCachedCount = 0;
static double      gCachedAt    = -1000.0;

static double monotonic_seconds(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (double)ts.tv_sec + ((double)ts.tv_nsec * 1e-9);
}

static const tDeviceInfo * device_list(uint32_t * count) {
    double now = monotonic_seconds();

    if ((now - gCachedAt) > 1.0) {
        gCachedCount = device_enumerate(gCachedList, DEVICE_MAX);
        gCachedAt    = now;
    }

    *count = gCachedCount;

    return gCachedList;
}

void gb_device_list_invalidate(void) {
    gCachedAt = -1000.0;
}

int gb_slot_for_uid(const char * uid) {
    uint32_t            count = 0;
    const tDeviceInfo * list  = device_list(&count);
    int                 seen  = 0;

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
    int                 found = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (list[i].inputChannels > 0) {
            found++;
        }
    }

    return (found > 0) ? found : 1;
}

void gb_input_device_name(int index, char * out, unsigned long len) {
    uint32_t            count = 0;
    const tDeviceInfo * list  = device_list(&count);
    int                 seen  = 0;

    snprintf(out, len, "%s", "-");

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
                        double midiChannel) {
    gMidiDest = midiDest;
    gOffset   = offset;
    gMidiChan = midiChannel;
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

    gFontReady = preload_glyph_textures(FONT_PATH, FONT_PRELOAD_SIZE);
}

static void label(double x, double y, const char * text) {
    set_rgb_colour((tRgb){ 0.72, 0.72, 0.74 });
    render_text(mainArea, (tRectangle){ { x, y }, { 0.0, TEXT_H } }, text);
}

static void value_box(int row, const char * text) {
    tRectangle box = row_value(row);

    set_rgb_colour((tRgb){ 0.16, 0.16, 0.18 });
    render_rectangle(mainArea, box);

    set_rgb_colour((tRgb){ 0.92, 0.92, 0.94 });
    render_text(mainArea, (tRectangle){ { box.coord.x + 8.0, box.coord.y + 5.0 }, { 0.0, TEXT_H } }, text);
}

static void stepper(int row, const char * labelText, const char * valueText) {
    label(20.0, row_y(row) + 5.0, labelText);

    draw_button(mainArea, row_prev(row), "<", (tRgb){ 0.30, 0.30, 0.33 });
    draw_button(mainArea, row_next(row), ">", (tRgb){ 0.30, 0.30, 0.33 });

    value_box(row, valueText);
}

// A LABELLED FIGURE AT A FIXED COLUMN. Everything below used to be built with snprintf into one
// string, so a fill of 480 and a fill of 1920 pushed everything to their right along by a character
// - the drift figure never sat still long enough to read, which rather defeats the point of showing
// it. Label and value each get their own x, so only the digits change.
#define STAT_VALUE_DX    (52.0)

// THE PANEL'S THIRD TEXT TIER - captions and hints, everything that supports a figure rather than
// being one. It sits on RGB_BACKGROUND_GREY, which is 0.30 in every build that is not G2_EDIT, so
// the old values were barely there: the stat captions ran at 0.45 (~1.8:1 against their own
// background) and the offset hint at 0.50 (~2.1:1). 0.60 lifts both to ~3.3:1 while staying under
// the 0.70 of the figure beside it, so the tier still reads as secondary instead of flattening
// into it. Named because three places share it and a fourth would otherwise be a fourth literal.
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

    if ((status != NULL) && atomic_load(&status->offlineRender)) {
        // AHEAD OF "capturing", because during an offline render it is still nominally capturing
        // and that is precisely the misleading thing to show. A bounce faster than realtime drains
        // the ring - the device cannot be hurried - so the render is silent whatever else is true.
        set_rgb_colour((tRgb){ 0.85, 0.60, 0.25 });
        snprintf(buffer, sizeof(buffer), "%s", "host is rendering offline - bounce in real time");
    } else if ((status != NULL) && atomic_load(&status->active)) {
        set_rgb_colour((tRgb){ 0.45, 0.75, 0.50 });
        snprintf(buffer, sizeof(buffer), "capturing %s", status->deviceName);
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

    // ---- the three steppers ----
    gb_input_device_name(gb_device_slot(gDevice), buffer, sizeof(buffer));
    stepper(0, "Device", buffer);

    snprintf(buffer, sizeof(buffer), "%.0f Hz",
             gGbRates[(int)(gRate * (double)(gGbRateCount - 1) + 0.5)]);
    stepper(1, "Rate", buffer);

    snprintf(buffer, sizeof(buffer), "%d samples",
             gGbFrames[(int)(gFrames * (double)(gGbFrameCount - 1) + 0.5)]);
    stepper(2, "Buffer", buffer);

    stepper(3, "Mode", (gMode < 0.5) ? "Mono" : "Stereo");

    // Shown as the channel numbers a person would read off the back of the interface, so 1-based -
    // and as a pair when in stereo, because "channel 3" meaning "3 and 4" is exactly the sort of
    // thing that gets a take recorded off the wrong output.
    int first = (int)(gFirst * (double)(GB_MAX_FIRST_CHANNEL - 1) + 0.5);

    if (gMode < 0.5) {
        snprintf(buffer, sizeof(buffer), "%d", first + 1);
    } else {
        snprintf(buffer, sizeof(buffer), "%d - %d", first + 1, first + 2);
    }

    stepper(4, "Input", buffer);

    // The instrument's own row, and the measurement it enables.
    if (gInstrument) {
        gb_midi_destination_name((int)(gMidiDest * (double)(GB_MIDI_MAX_DEST - 1) + 0.5),
                                 buffer, sizeof(buffer));
        stepper(5, "MIDI Out", buffer);

        int channel = (int)(gMidiChan * (double)(GB_CHANNEL_SLOTS - 1) + 0.5);

        if (channel <= 0) {
            snprintf(buffer, sizeof(buffer), "%s", "Source (as the DAW sends)");
        } else {
            snprintf(buffer, sizeof(buffer), "%d", channel);
        }

        stepper(6, "Channel", buffer);
    }

    // ---- trim ----
    if (gInstrument) {
        label(20.0, measure_y() + 4.0, "Latency");

        draw_button(mainArea, measure_button(), MEASURE_LABEL, (tRgb){ 0.30, 0.42, 0.55 });

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
            snprintf(buffer, sizeof(buffer), "%d smp (%.1f ms) measured", measured,
                     (double)measured / ((double)rate / 1000.0));
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

        // THE CORRECTION IN FORCE - what report_latency() actually adds, not a trim on top of it.
        // Measure seeds this row; the +/- moves it from there, because a measurement includes the
        // patch's own attack and cannot know it, so the last fraction of a millisecond stays a
        // judgement rather than a reading. The row above shows what was measured, so the two can be
        // compared and you can see how far you have moved from it.
        // "In use", not "Correction": labels are drawn at x=20 and the arrows start at LABEL_W (74),
        // so a label has 54 px. "Correction" ran straight under the < >. The longest label this
        // panel already proves fits is "MIDI Out" at eight characters - stay inside that.
        label(20.0, offset_y() + 4.0, "In use");

        draw_button(mainArea, offset_down(), "<", (tRgb){ 0.30, 0.30, 0.33 });
        draw_button(mainArea, offset_up(), ">", (tRgb){ 0.30, 0.30, 0.33 });

        double offsetMs = GB_OFFSET_MIN_MS + (gOffset * (GB_OFFSET_MAX_MS - GB_OFFSET_MIN_MS));

        snprintf(buffer, sizeof(buffer), "%+.1f ms", offsetMs);
        set_rgb_colour((tRgb){ 0.92, 0.92, 0.94 });
        render_text(mainArea, (tRectangle){ { 190.0, offset_y() + 6.0 }, { 0.0, 11.0 } }, buffer);

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

    // ---- telemetry ----
    //
    // The same figures the command line bridge prints. They are here because they are the only way
    // to see whether the drift loop is holding without attaching a debugger, and because a buffer
    // that is quietly resyncing every twenty seconds is otherwise indistinguishable from one that
    // is not.
    // WHERE THE LATENCY ACTUALLY GOES. One total tells you nothing actionable: on this rig two
    // thirds of it is usually the device's own buffer, which is fixed by a control three rows up,
    // and the ring is the only part the plug-in chooses. Showing the parts makes it obvious which
    // number to argue with.
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

        // MILLISECONDS IN BRACKETS, as the Latency row above prints them - samples alone mean
        // nothing without the rate in your head, and this pair is the one people actually read.
        //
        // "in use" moves to kCol[2], NOT kCol[1]. A stat's value is inset STAT_VALUE_DX (52) into a
        // column 125 wide, so it has 73 px before the next column's caption - enough for "221" and
        // not for "221 (4.6 ms)", which would have run straight under the caption beside it, the
        // same way "Correction" ran under its arrows. Columns 1 and 3 are unused on this row, so
        // spreading across the gap costs nothing and gives each value 198 px.
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

    snprintf(buffer, sizeof(buffer), "%.0f/%.0f",
             (status != NULL) ? atomic_load(&status->fillFrames) : 0.0,
             (status != NULL) ? atomic_load(&status->setpointFrames) : 0.0);
    stat(kCol[0], y, "fill", buffer);

    snprintf(buffer, sizeof(buffer), "%+.2f", (status != NULL) ? atomic_load(&status->driftPpm) : 0.0);
    stat(kCol[1], y, "drift", buffer);

    snprintf(buffer, sizeof(buffer), "%d", (status != NULL) ? atomic_load(&status->underruns) : 0);
    stat(kCol[2], y, "under", buffer);

    snprintf(buffer, sizeof(buffer), "%d", (status != NULL) ? atomic_load(&status->resyncs) : 0);
    stat(kCol[3], y, "resync", buffer);

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

    for (int row = 0; row < row_count(); row++) {
        int delta = 0;

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

        if (hit(offset_down(), x, y) || hit(offset_up(), x, y)) {
            // A TENTH OF A MILLISECOND A CLICK. This was 1 ms on the argument that a millisecond is
            // the resolution a person can judge - true of a latency heard on its own, wrong for this
            // control. The measurement already puts the hardware's share into report_latency(), so
            // what is left here is the residual a take still sounds early or late by, and a whole
            // millisecond steps straight over it. 0.1 ms is 4.8 samples at 48k, so it is a real
            // move rather than a rounding, and the readout was already %+.1f.
            //
            // The range stays +/-100 ms deliberately: it is a normalised VST3 parameter, so
            // narrowing it would silently re-scale the offset saved in every existing host project.
            double step = GB_OFFSET_STEP_MS / (GB_OFFSET_MAX_MS - GB_OFFSET_MIN_MS);
            double next = gOffset + (hit(offset_up(), x, y) ? step : -step);

            request->which      = eGbEditOffset;
            request->normalized = (next < 0.0) ? 0.0 : ((next > 1.0) ? 1.0 : next);
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
