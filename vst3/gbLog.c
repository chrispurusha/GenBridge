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

#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "gbLog.h"

void gb_log_line(const char * format, ...) {
    // THE GATE IS CACHED, because it is a syscall and this is called from threads that must not
    // spend them. access() on every call is cheap next to the fopen below when logging is ON, and
    // it is the entire cost when logging is OFF - which is almost always, and is exactly when it
    // must be free. Re-polled once a second so touching the file still enables logging mid-session
    // rather than needing a reload.
    static _Atomic double checkedAt = -1000.0;
    static _Atomic bool   enabled   = false;
    struct timespec       ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    double now = (double)ts.tv_sec + ((double)ts.tv_nsec * 1e-9);

    if ((now - atomic_load(&checkedAt)) >= 1.0) {
        atomic_store(&checkedAt, now);
        atomic_store(&enabled, access(GB_LOG_GATE_PATH, F_OK) == 0);
    }

    if (!atomic_load(&enabled)) {
        return;
    }

    FILE * file = fopen(GB_LOG_PATH, "a");

    if (file == NULL) {
        return;
    }

    static const char * name = NULL;

    if (name == NULL) {
        // The executable's own name, so a line from Live is distinguishable from one from the
        // checker at a glance rather than by pid alone.
        const char * path = getprogname();

        name = (path != NULL) ? path : "?";
    }

    fprintf(file, "[%s %d] ", name, (int)getpid());

    va_list args;

    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);

    fputc('\n', file);
    fclose(file);
}
