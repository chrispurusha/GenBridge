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
// Notes: Docs/code-notes/gmSettings.c.md - "// notes §k" refers there.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gmSettings.h"

#define GM_SETTINGS_DIR     "Library/Application Support/GenBridge Monitor"
#define GM_SETTINGS_FILE    "settings.txt"

static void settings_path(char * out, size_t len, bool makeDir) {
    const char * home = getenv("HOME");
    char         dir[1024];

    snprintf(dir, sizeof(dir), "%s/%s", (home != NULL) ? home : ".", GM_SETTINGS_DIR);

    if (makeDir) {
        mkdir(dir, 0755);
    }
    snprintf(out, len, "%s/%s", dir, GM_SETTINGS_FILE);
}

void gm_settings_defaults(tGmConfig * config, bool * running) {
    memset(config, 0, sizeof(*config));
    config->inChannels  = 2;
    config->outChannels = 2;
    config->frames      = 64;
    config->safetyMs    = 0.0;
    config->trim        = 1.0;
    *running            = false;
}

// notes §1 - unknown keys are skipped, so an older build reads a newer file
void gm_settings_load(tGmConfig * config, bool * running) {
    char   path[1200];
    char   line[600];
    FILE * f;

    gm_settings_defaults(config, running);
    settings_path(path, sizeof(path), false);
    f = fopen(path, "r");

    if (f == NULL) {
        return;
    }

    while (fgets(line, sizeof(line), f) != NULL) {
        char * eq = strchr(line, '=');

        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        char * value = eq + 1;

        value[strcspn(value, "\r\n")] = '\0';

        if (strcmp(line, "in") == 0) {
            snprintf(config->inUid, sizeof(config->inUid), "%s", value);
        } else if (strcmp(line, "out") == 0) {
            snprintf(config->outUid, sizeof(config->outUid), "%s", value);
        } else if (strcmp(line, "infirst") == 0) {
            config->inFirst = (uint32_t)atoi(value);
        } else if (strcmp(line, "inch") == 0) {
            config->inChannels = (atoi(value) > 1) ? 2u : 1u;
        } else if (strcmp(line, "outfirst") == 0) {
            config->outFirst = (uint32_t)atoi(value);
        } else if (strcmp(line, "outch") == 0) {
            config->outChannels = (atoi(value) > 1) ? 2u : 1u;
        } else if (strcmp(line, "frames") == 0) {
            config->frames = (uint32_t)atoi(value);
        } else if (strcmp(line, "safetyms") == 0) {
            config->safetyMs = atof(value);
        } else if (strcmp(line, "trim") == 0) {
            config->trim = atof(value);
        } else if (strcmp(line, "running") == 0) {
            *running = (atoi(value) != 0);
        }
    }
    fclose(f);

    if ((config->trim < 0.0) || (config->trim > 2.0)) {
        config->trim = 1.0;
    }
}

void gm_settings_save(const tGmConfig * config, bool running) {
    char   path[1200];
    FILE * f;

    settings_path(path, sizeof(path), true);
    f = fopen(path, "w");

    if (f == NULL) {
        return;
    }
    fprintf(f, "in=%s\nout=%s\ninfirst=%u\ninch=%u\noutfirst=%u\noutch=%u\nframes=%u\nsafetyms=%.1f\ntrim=%.3f\nrunning=%d\n",
            config->inUid, config->outUid, config->inFirst, config->inChannels, config->outFirst,
            config->outChannels, config->frames, config->safetyMs, config->trim, running ? 1 : 0);
    fclose(f);
}
