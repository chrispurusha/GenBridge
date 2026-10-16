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
// Notes: Docs/code-notes/gmInsert.c.md - "// notes §k" refers there.

#ifndef GM_INSERT_H
#define GM_INSERT_H

#include <stdbool.h>
#include <stdint.h>

#include <AudioToolbox/AudioToolbox.h>

// notes §1 - Helix Native, as an Audio Unit in the monitor's path. All main thread but gm_insert_process().
bool gm_insert_available(void);                            // installed
bool gm_insert_open(double rate, uint32_t maxFrames);      // the monitor's output rate and largest block
void gm_insert_close(void);                                // saves its state first
bool gm_insert_is_open(void);
AudioUnit gm_insert_unit(void);                            // for its editor; NULL while closed

// What the app does just before the unit goes: its editor must not outlive it
void gm_insert_on_close(void ( *closing )(void));

// notes §2 - audio thread: stereo in place, frames no more than maxFrames
void gm_insert_process(float * left, float * right, uint32_t frames);

#endif // GM_INSERT_H
