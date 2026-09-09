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

#ifndef __GB_LOG_H__
#define __GB_LOG_H__

#ifdef __cplusplus
extern "C" {
#endif

// Diagnostics, gated on a FILE rather than an environment variable.
//
// The obvious gate would be getenv, and it does not work: a host launched from the Dock inherits no
// shell environment, so the variable is never seen in the one situation that matters. Testing for a
// file the user can touch works from anywhere, and is the same trick the sibling projects use for
// their backdoor channels.
//
//     touch /tmp/genbridge-log        # then reload the plug-in
//     cat /tmp/genbridge.log
//
// The gate and the output are deliberately similar and deliberately different, which has caught
// MidiSyncTool out once already - the GATE is the hyphen and the OUTPUT is the dot.
#define GB_LOG_GATE_PATH    "/tmp/genbridge-log"
#define GB_LOG_PATH         "/tmp/genbridge.log"

// EVERY LINE SAYS WHO WROTE IT. One log file is shared by every instance in every process on the
// machine - a DAW with two plug-ins in it, and a command line harness running alongside, all append
// here. Reading it without attribution means diagnosing one process's symptom from another's
// output, which is exactly what happened: a run of "measured: 0" lines was read as a harness fault
// when it came from a DAW that also had the device open.
void gb_log_line(const char * format, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif // __GB_LOG_H__
