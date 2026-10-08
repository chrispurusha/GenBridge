/*
 * GenBridge Monitor - route an input pair to an output pair with as little delay as two clocks allow.
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
// Notes: Docs/code-notes/gmDraw.c.md - "// notes §k" refers there.

// notes §1

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gmDraw.h"

#include "contextMenu.h"
#include "device.h"
#include "geometry.h"
#include "synthlibDefs.h"
#include "synthlibGlobals.h"
#include "synthlibHost.h"
#include "synthlibTypes.h"
#include "utilsGraphics.h"

#define FONT_PATH           "/System/Library/Fonts/Supplemental/Arial.ttf"
#define FONT_PRELOAD_SIZE   (16.0)

#define ROW_TOP             (62.0)
#define ROW_STEP            (34.0)
#define ROW_H               (26.0)
#define LABEL_X             (20.0)
#define VALUE_X             (110.0)
#define VALUE_RIGHT         (GM_CANVAS_W - 20.0)
#define TEXT_H              (13.0)
#define MENU_BG             ((tRgb){ 0.22, 0.22, 0.24 })
#define CAPTION_GREY        ((tRgb){ 0.60, 0.60, 0.63 })
#define VALUE_GREY          ((tRgb){ 0.92, 0.92, 0.94 })
#define AMBER               ((tRgb){ 0.85, 0.60, 0.25 })
#define GREEN               ((tRgb){ 0.45, 0.75, 0.50 })

typedef enum {
    eRowInDevice = 0,
    eRowInChannels,
    eRowInFormat,
    eRowOutDevice,
    eRowOutChannels,
    eRowOutFormat,
    eRowBuffer,
    eRowSafety,
    eRowCount
} tRow;

static const uint32_t kFrames[]   = { 16, 32, 64, 128, 256, 512, 1024 };
static const double   kSafetyMs[] = { 0.0, 1.0, 2.0, 3.0, 5.0, 8.0, 12.0, 20.0 };

#define FRAMES_COUNT    ((int)(sizeof(kFrames) / sizeof(kFrames[0])))
#define SAFETY_COUNT    ((int)(sizeof(kSafetyMs) / sizeof(kSafetyMs[0])))

static bool   gFontReady = false;
static tCoord gMouse     = { 0.0, 0.0 };

// ---- devices --------------------------------------------------------------------------------

static tDeviceInfo gDevices[DEVICE_MAX];
static uint32_t    gDeviceCount = 0;

// notes §2 - re-read whenever a menu opens, so a device plugged in since shows up
static void refresh_devices(void) {
    gDeviceCount = device_enumerate(gDevices, DEVICE_MAX);
}

static const tDeviceInfo * device_by_uid(const char * uid) {
    for (uint32_t i = 0; i < gDeviceCount; i++) {
        if (strcmp(gDevices[i].uid, uid) == 0) {
            return &gDevices[i];
        }
    }

    return NULL;
}

// ---- menus ----------------------------------------------------------------------------------

#define MENU_MAX      (DEVICE_MAX + 1)
#define MENU_LABEL    (96)

static char      gMenuLabels[MENU_MAX][MENU_LABEL];
static tMenuItem gMenuItems[MENU_MAX];
static int       gMenuValue[MENU_MAX];   // what each item stands for: a device index, a channel, a slot
static tRow      gMenuFor    = eRowCount;
static int       gMenuChoice = -1;

static void menu_action(int index) {
    gMenuChoice = index;
}

static void mouse_coord(tCoord * coord) {
    if (coord != NULL) {
        *coord = gMouse;
    }
}

void gm_draw_set_mouse(double x, double y) {
    gMouse.x = x;
    gMouse.y = y;
}

bool gm_draw_menu_active(void) {
    return gContextMenu.active;
}

static tRectangle row_value(int row) {
    return (tRectangle){ { VALUE_X, ROW_TOP + (ROW_STEP * row) }, { VALUE_RIGHT - VALUE_X, ROW_H - 6.0 } };
}

static void open_menu(tRow row, int count) {
    int columns = ((count * 22) > (int)(GM_CANVAS_H - 40.0)) ? 2 : 1;

    for (int i = 0; i < count; i++) {
        gMenuItems[i] = (tMenuItem){ .label = gMenuLabels[i], .colour = MENU_BG, .action = menu_action,
                                     .param = (uint32_t)i, .subMenu = NULL, .subMenuColumns = 0,
                                     .subMenuCellWidth = 0.0 };
    }
    gMenuItems[count] = (tMenuItem){ .label = NULL, .action = NULL };
    gMenuFor          = row;
    gMenuChoice       = -1;
    open_context_menu(below_rect(row_value(row)), gMenuItems, (uint32_t)columns, 240.0);
}

// notes §3 - channel choices: every start position the device has room for, named the way its own manual counts
static int channel_choices(const tDeviceInfo * device, bool input, uint32_t width) {
    uint32_t total = (device == NULL) ? 0 : (input ? device->inputChannels : device->outputChannels);
    int      n     = 0;

    for (uint32_t first = 0; ((first + width) <= total) && (n < MENU_MAX - 1); first++) {
        if (width == 1) {
            snprintf(gMenuLabels[n], MENU_LABEL, "%u", first + 1);
        } else {
            snprintf(gMenuLabels[n], MENU_LABEL, "%u - %u", first + 1, first + 2);
        }
        gMenuValue[n++] = (int)first;
    }

    return n;
}

static int device_choices(bool input) {
    int n = 0;

    refresh_devices();

    for (uint32_t i = 0; (i < gDeviceCount) && (n < MENU_MAX - 1); i++) {
        uint32_t channels = input ? gDevices[i].inputChannels : gDevices[i].outputChannels;

        if (channels == 0) {
            continue;
        }
        snprintf(gMenuLabels[n], MENU_LABEL, "%s", gDevices[i].name);
        gMenuValue[n++] = (int)i;
    }

    return n;
}

// ---- layout ---------------------------------------------------------------------------------

static double trim_y(void)  { return ROW_TOP + (ROW_STEP * eRowCount) + 6.0; }
static double meter_y(void) { return trim_y() + 30.0; }
static double stats_y(void) { return meter_y() + 66.0; }

static tRectangle trim_track(void) {
    return (tRectangle){ { VALUE_X, trim_y() }, { VALUE_RIGHT - VALUE_X - 54.0, 14.0 } };
}

static tRectangle run_button(void) {
    return (tRectangle){ { GM_CANVAS_W - 96.0, 16.0 }, { 64.0, 18.0 } };
}

// notes §7
static tRectangle display_button(void) {
    return (tRectangle){ { GM_CANVAS_W - 200.0, 16.0 }, { 86.0, 18.0 } };
}

static bool hit(tRectangle r, double x, double y) {
    return (x >= r.coord.x) && (x <= (r.coord.x + r.size.w)) && (y >= r.coord.y) && (y <= (r.coord.y + r.size.h));
}

void gm_draw_init(void) {
    render_backend_init();
    configure_synthlib_theme((tSynthLibTheme){
        .topBarHeight   = 0.0,
        .orange1        = (tRgb)RGB_ORANGE_1,
        .orange2        = (tRgb)RGB_ORANGE_2,
        .greenOn        = (tRgb)RGB_GREEN_ON,
        .backgroundGrey = (tRgb)RGB_BACKGROUND_GREY,
    });
    synthlib_host_init((tSynthLibHost){ .mouseCoord = mouse_coord, .pointerCaptured = NULL });
    gFontReady = preload_glyph_textures(FONT_PATH, FONT_PRELOAD_SIZE);
    refresh_devices();
}

// ---- drawing --------------------------------------------------------------------------------

static void text(double x, double y, double h, tRgb colour, const char * s) {
    set_rgb_colour(colour);
    render_text(mainArea, (tRectangle){ { x, y }, { 0.0, h } }, s);
}

static void row(tRow which, const char * label, const char * value) {
    tRectangle box = row_value(which);

    text(LABEL_X, box.coord.y + 5.0, TEXT_H, (tRgb){ 0.72, 0.72, 0.74 }, label);
    set_rgb_colour((tRgb){ 0.16, 0.16, 0.18 });
    render_rectangle(mainArea, box);
    text(box.coord.x + 8.0, box.coord.y + 5.0, TEXT_H, VALUE_GREY, value);
}

static void meter(double y, float peak) {
    double     w     = VALUE_RIGHT - VALUE_X;
    double     level = (peak > 1.0f) ? 1.0 : (double)peak;
    // notes §4 - drawn in dB, -60 to 0, so a guitar at a sensible level moves the bar
    double     db    = (level > 0.0) ? (20.0 * log10(level)) : -100.0;
    double     frac  = (db <= -60.0) ? 0.0 : ((db + 60.0) / 60.0);

    set_rgb_colour((tRgb){ 0.14, 0.14, 0.16 });
    render_rectangle(mainArea, (tRectangle){ { VALUE_X, y }, { w, 9.0 } });

    if (frac > 0.0) {
        set_rgb_colour((db > -0.5) ? (tRgb){ 0.90, 0.25, 0.20 } : ((db > -6.0) ? (tRgb){ 0.90, 0.72, 0.20 }
                                                                    : (tRgb){ 0.35, 0.78, 0.42 }));
        render_rectangle(mainArea, (tRectangle){ { VALUE_X, y }, { w * frac, 9.0 } });
    }
}

static void stat(double x, double y, const char * name, const char * value) {
    text(x, y, 11.0, CAPTION_GREY, name);
    text(x + 62.0, y, 11.0, (tRgb){ 0.78, 0.78, 0.80 }, value);
}

void gm_draw_frame(const tGmConfig * config, bool running, bool displayPaused, int pixelWidth, int pixelHeight) {
    tGmStatus status;
    char      buffer[192];

    if ((pixelWidth <= 0) || (pixelHeight <= 0)) {
        return;
    }
    render_backend_set_surface(pixelWidth, pixelHeight);
    set_render_width(pixelWidth);
    set_render_height(pixelHeight);
    gGlobalGuiScale = (double)pixelWidth / GM_CANVAS_W;
    render_backend_clear((tRgb)RGB_BACKGROUND_GREY);

    if (!gFontReady) {
        render_backend_flush();
        return;
    }
    gm_engine_status(&status);

    // notes §7 - a paused panel draws its last frame without figures: stale numbers would read as live
    if (displayPaused) {
        memset(status.inPeak, 0, sizeof(status.inPeak));
        memset(status.outPeak, 0, sizeof(status.outPeak));
    }

    // ---- header ----
    text(LABEL_X, 16.0, 20.0, (tRgb){ 0.95, 0.95, 0.97 }, "GenBridge Monitor");
    draw_button(mainArea, run_button(), running ? "Stop" : "Start",
                running ? (tRgb){ 0.55, 0.30, 0.28 } : (tRgb){ 0.30, 0.50, 0.36 });
    draw_button(mainArea, display_button(), displayPaused ? "Display" : "Display",
                displayPaused ? (tRgb){ 0.26, 0.26, 0.29 } : (tRgb){ 0.30, 0.42, 0.55 });

    tRgb statusColour = (status.state == eGmRunning) ? GREEN
                        : (((status.state == eGmWaiting) || (status.state == eGmFailed)) ? AMBER
                           : (tRgb){ 0.72, 0.72, 0.74 });

    text(LABEL_X, 40.0, 11.0, statusColour, status.message);

    // ---- rows ----
    const tDeviceInfo * in  = device_by_uid(config->inUid);
    const tDeviceInfo * out = device_by_uid(config->outUid);

    row(eRowInDevice, "Input", (in != NULL) ? in->name : ((config->inUid[0] != '\0') ? "(not connected)" : "None"));

    if (config->inChannels > 1) {
        snprintf(buffer, sizeof(buffer), "%u - %u", config->inFirst + 1, config->inFirst + 2);
    } else {
        snprintf(buffer, sizeof(buffer), "%u", config->inFirst + 1);
    }
    row(eRowInChannels, "Channels", buffer);
    row(eRowInFormat, "Format", (config->inChannels > 1) ? "Stereo" : "Mono");

    row(eRowOutDevice, "Output", (out != NULL) ? out->name : ((config->outUid[0] != '\0') ? "(not connected)" : "None"));

    if (config->outChannels > 1) {
        snprintf(buffer, sizeof(buffer), "%u - %u", config->outFirst + 1, config->outFirst + 2);
    } else {
        snprintf(buffer, sizeof(buffer), "%u", config->outFirst + 1);
    }
    row(eRowOutChannels, "Channels", buffer);
    row(eRowOutFormat, "Format", (config->outChannels > 1) ? "Stereo" : "Mono");

    snprintf(buffer, sizeof(buffer), "%u samples", config->frames);
    row(eRowBuffer, "Buffer", buffer);
    snprintf(buffer, sizeof(buffer), "%.0f ms", config->safetyMs);
    row(eRowSafety, "Safety", buffer);

    // ---- trim ----
    tRectangle track = trim_track();

    text(LABEL_X, trim_y() + 1.0, TEXT_H, (tRgb){ 0.72, 0.72, 0.74 }, "Trim");
    set_rgb_colour((tRgb){ 0.16, 0.16, 0.18 });
    render_rectangle(mainArea, track);
    set_rgb_colour((tRgb){ 0.35, 0.62, 0.85 });
    render_rectangle(mainArea, (tRectangle){ track.coord, { track.size.w * (config->trim / 2.0), track.size.h } });
    if (config->trim < 0.001) {
        snprintf(buffer, sizeof(buffer), "%s", "off");
    } else {
        snprintf(buffer, sizeof(buffer), "%+.1f dB", 20.0 * log10(config->trim));
    }
    text(track.coord.x + track.size.w + 8.0, trim_y() + 1.0, 11.0, (tRgb){ 0.72, 0.72, 0.74 }, buffer);

    // ---- meters ----
    text(LABEL_X, meter_y(), TEXT_H, (tRgb){ 0.72, 0.72, 0.74 }, "In");
    meter(meter_y(), status.inPeak[0]);
    meter(meter_y() + 12.0, status.inPeak[1]);
    text(LABEL_X, meter_y() + 30.0, TEXT_H, (tRgb){ 0.72, 0.72, 0.74 }, "Out");
    meter(meter_y() + 30.0, status.outPeak[0]);
    meter(meter_y() + 42.0, status.outPeak[1]);

    // ---- figures ----
    double y = stats_y();

    if (displayPaused) {
        text(LABEL_X, y, 11.0, AMBER, "display paused - routing carries on; click Display to resume");
    } else if (status.state == eGmRunning) {
        snprintf(buffer, sizeof(buffer), "%.1f ms (devices %.1f, bridge %.1f)", status.latencyMs,
                 status.latencyMs - status.bridgeMs, status.bridgeMs);
        stat(LABEL_X, y, "latency", buffer);
        y += 18.0;
        snprintf(buffer, sizeof(buffer), "%u in, %u out", status.inFrames, status.outFrames);
        stat(LABEL_X, y, "buffers", buffer);
        snprintf(buffer, sizeof(buffer), "%.0f / %.0f", status.fillFrames, status.setpointFrames);
        stat(270.0, y, "fill", buffer);
        y += 18.0;
        snprintf(buffer, sizeof(buffer), "%u / %u", status.underruns, status.resyncs);
        stat(LABEL_X, y, "dropouts", buffer);
        snprintf(buffer, sizeof(buffer), "%+.1f ppm", status.driftPpm);
        stat(270.0, y, "drift", buffer);
        y += 18.0;
        snprintf(buffer, sizeof(buffer), "%.0f / %.0f Hz", status.inRate, status.outRate);
        stat(LABEL_X, y, "rates", buffer);

        // notes §5
        if (status.inShared || status.outShared) {
            y += 22.0;
            snprintf(buffer, sizeof(buffer), "%s in use elsewhere - its buffer was left as it is",
                     (status.inShared && status.outShared) ? "both devices" : (status.inShared ? "input" : "output"));
            text(LABEL_X, y, 11.0, AMBER, buffer);
        }
    }

    update_context_menu_hover();
    render_context_menu();
    render_backend_flush();
}

// ---- clicks ---------------------------------------------------------------------------------

static void clamp_channels(tGmConfig * config) {
    refresh_devices();
    const tDeviceInfo * in  = device_by_uid(config->inUid);
    const tDeviceInfo * out = device_by_uid(config->outUid);

    if ((in != NULL) && ((config->inFirst + config->inChannels) > in->inputChannels)) {
        config->inFirst = (in->inputChannels >= config->inChannels) ? (in->inputChannels - config->inChannels) : 0;
    }

    if ((out != NULL) && ((config->outFirst + config->outChannels) > out->outputChannels)) {
        config->outFirst = (out->outputChannels >= config->outChannels) ? (out->outputChannels - config->outChannels) : 0;
    }
}

static tGmEdit apply_choice(tGmConfig * config, tRow which, int choice) {
    int value = gMenuValue[choice];

    switch (which) {
        case eRowInDevice:
            snprintf(config->inUid, sizeof(config->inUid), "%s", gDevices[value].uid);
            config->inFirst = 0;
            break;

        case eRowOutDevice:
            snprintf(config->outUid, sizeof(config->outUid), "%s", gDevices[value].uid);
            config->outFirst = 0;
            break;

        case eRowInChannels:
            config->inFirst = (uint32_t)value;
            break;

        case eRowOutChannels:
            config->outFirst = (uint32_t)value;
            break;

        case eRowInFormat:
            config->inChannels = (uint32_t)value;
            break;

        case eRowOutFormat:
            config->outChannels = (uint32_t)value;
            break;

        case eRowBuffer:
            config->frames = kFrames[value];
            break;

        case eRowSafety:
            config->safetyMs = kSafetyMs[value];
            break;

        default:
            return eGmEditNone;
    }
    clamp_channels(config);

    return eGmEditConfig;
}

tGmEdit gm_draw_click(tGmConfig * config, bool running, double x, double y) {
    (void)running;

    // the open menu gets first refusal - a click while it is open either picks or dismisses
    if (gContextMenu.active) {
        gMenuChoice = -1;
        handle_context_menu_click((tCoord){ x, y });

        if ((gMenuChoice >= 0) && (gMenuFor != eRowCount)) {
            tRow which = gMenuFor;
            int  choice = gMenuChoice;

            gMenuFor    = eRowCount;
            gMenuChoice = -1;

            return apply_choice(config, which, choice);
        }

        return eGmEditNone;
    }

    if (hit(draw_button_bounds(run_button()), x, y)) {
        return eGmEditRun;
    }

    if (hit(draw_button_bounds(display_button()), x, y)) {
        return eGmEditDisplay;
    }

    tRectangle track = trim_track();

    if (hit((tRectangle){ { track.coord.x, track.coord.y - 4.0 }, { track.size.w, track.size.h + 8.0 } }, x, y)) {
        double frac = (x - track.coord.x) / track.size.w;

        frac         = (frac < 0.0) ? 0.0 : ((frac > 1.0) ? 1.0 : frac);
        // notes §6 - snaps to unity near the middle, so 0 dB is easy to get back to
        config->trim = (fabs(frac - 0.5) < 0.02) ? 1.0 : (2.0 * frac);

        return eGmEditTrim;
    }

    for (int r = 0; r < eRowCount; r++) {
        if (!hit(row_value(r), x, y)) {
            continue;
        }
        int n = 0;

        refresh_devices();

        switch ((tRow)r) {
            case eRowInDevice:
                n = device_choices(true);
                break;

            case eRowOutDevice:
                n = device_choices(false);
                break;

            case eRowInChannels:
                n = channel_choices(device_by_uid(config->inUid), true, config->inChannels);
                break;

            case eRowOutChannels:
                n = channel_choices(device_by_uid(config->outUid), false, config->outChannels);
                break;

            case eRowInFormat:
            case eRowOutFormat:
                snprintf(gMenuLabels[0], MENU_LABEL, "%s", "Mono");
                snprintf(gMenuLabels[1], MENU_LABEL, "%s", "Stereo");
                gMenuValue[0] = 1;
                gMenuValue[1] = 2;
                n             = 2;
                break;

            case eRowBuffer:
                for (n = 0; n < FRAMES_COUNT; n++) {
                    snprintf(gMenuLabels[n], MENU_LABEL, "%u samples", kFrames[n]);
                    gMenuValue[n] = n;
                }
                break;

            case eRowSafety:
                for (n = 0; n < SAFETY_COUNT; n++) {
                    snprintf(gMenuLabels[n], MENU_LABEL, "%.0f ms", kSafetyMs[n]);
                    gMenuValue[n] = n;
                }
                break;

            default:
                break;
        }

        if (n > 0) {
            open_menu((tRow)r, n);
        }

        return eGmEditNone;
    }

    return eGmEditNone;
}
