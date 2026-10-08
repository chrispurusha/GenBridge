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

#ifndef GM_DRAW_H
#define GM_DRAW_H

#include <stdbool.h>

#include "gmEngine.h"

#define GM_CANVAS_W    (520.0)
#define GM_CANVAS_H    (540.0)

typedef enum {
    eGmEditNone = 0,
    eGmEditConfig,       // a setting changed - save and restart
    eGmEditTrim,         // trim only - applied live
    eGmEditRun,          // Start/Stop pressed
    eGmEditDisplay,      // the display toggle - pause or resume all drawing
} tGmEdit;

void    gm_draw_init(void);
void    gm_draw_set_mouse(double x, double y);
bool    gm_draw_menu_active(void);
void    gm_draw_frame(const tGmConfig * config, bool running, bool displayPaused, int pixelWidth, int pixelHeight);
// Edits config in place and says what kind of change it was.
tGmEdit gm_draw_click(tGmConfig * config, bool running, double x, double y);

#endif // GM_DRAW_H
