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

#ifndef GM_SETTINGS_H
#define GM_SETTINGS_H

#include <stdbool.h>

#include "gmEngine.h"

void gm_settings_defaults(tGmConfig * config, bool * running);
void gm_settings_load(tGmConfig * config, bool * running);
void gm_settings_save(const tGmConfig * config, bool running);
// The display toggle, kept in the same file.
bool gm_settings_load_display(void);
void gm_settings_save_display(bool paused);

#endif // GM_SETTINGS_H
