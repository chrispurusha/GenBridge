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

#include "gbStatus.h"

#include <string.h>

static tGbStatus  gStatus[GB_STATUS_SLOTS];
static atomic_bool gTaken[GB_STATUS_SLOTS];

int gb_status_claim(void) {
    for (int i = 0; i < GB_STATUS_SLOTS; i++) {
        bool expected = false;

        if (atomic_compare_exchange_strong(&gTaken[i], &expected, true)) {
            memset(&gStatus[i], 0, sizeof(gStatus[i]));
            return i;
        }
    }

    return -1;
}

void gb_status_release(int slot) {
    if ((slot < 0) || (slot >= GB_STATUS_SLOTS)) {
        return;
    }

    memset(&gStatus[slot], 0, sizeof(gStatus[slot]));
    atomic_store(&gTaken[slot], false);
}

tGbStatus * gb_status(int slot) {
    if ((slot < 0) || (slot >= GB_STATUS_SLOTS)) {
        return NULL;
    }

    return &gStatus[slot];
}
