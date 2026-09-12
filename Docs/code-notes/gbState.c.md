# gbState.c notes

The longer comments from `gbState.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. file scope

THE SAVED STATE, AND THE TABLES IT CARRIES.

THE FORMAT IS VERSIONED AND LINE BASED, and it is that way now rather than later because a state
format becomes expensive to change the moment anyone saves a session against it. Text costs
nothing at this size, survives being looked at in a hex editor, and lets an older build skip keys
it does not recognise instead of rejecting the whole blob.

The UID is written LAST on each line and read as "everything after the last comma of the numeric
part", because real UIDs contain commas - "AppleUSBAudioEngine:CalDigit, Inc.:..." - and splitting
on them would truncate it.

## 2. in `gb_ensure_settings()`

ZERO MEANS "LEAVE THE DEVICE ALONE", and that is the default for both.

Setting a device's nominal rate or buffer size is a GLOBAL operation affecting every
client of that device - including the host itself, if it happens to be the same
interface. Doing it uninvited during load is how a plug-in wedges a DAW. The device's own
settings are now simply adopted, and these are written only when the user changes them.

## 3. `gb_capture_live_settings()`

Fold whatever the user has changed live back into the active device's entry, so that saving
the project records what is actually on screen rather than what was last loaded.

ONLY WHEN A DEVICE IS ACTUALLY RUNNING. If nothing is open, the live values are construction
defaults rather than anything the user chose, and writing them back destroys the settings
that were just loaded - a project saved without ever starting playback came back with every
trim reset to 1.0.

## 4. in `gb_capture_live_settings()`

DELIBERATELY NOT WRITING BACK targetMs WHEN IT IS AUTO. It used to record the computed
setpoint, which turned "work it out" into a fixed number the moment a project was saved -
so a session saved at one buffer size reopened with that size's setpoint baked in, and the
floor calculation was quietly bypassed for ever after. Only an explicit choice is stored.

## 5. `tGbLine`

── Reading a blob, without a string class ──────────────────────────────────

The parsing below walks the bytes in place. It was written against std::string, whose substr()
and find() made each step read nicely and allocated a copy for every line, every field and every
tail; what is here does the same work with a pointer and a length. The one thing worth saying is
that NOTHING IS ASSUMED TO BE NUL-TERMINATED - a host hands over a byte count, and a blob that
has been truncated in a project file is exactly the case that must not run off the end.

## 6. in `gb_parse_state()`

Version 2 added the per-device sample rate as a numeric field, which changes how a dev=
line is split - so it needed a version bump rather than a new key. Reading version 1 is
still supported: this is exactly the situation the format was versioned for, and refusing
to open a session saved yesterday would be a poor advertisement for it.

## 7. in `gb_parse_state()`

A RESTORE IS NOT A CHOICE. Everything after this point until the user actually picks
something is the project being reopened, and the saved UID - not the saved slot index -
is what says which device that was. See the device parameter in gb_bridge_parameter() and
gb_reconfigure().

## 8. in `gb_parse_state()`

Defaults for a blob that predates these keys, so loading an older session zeroes the
correction rather than leaving whatever the previous project in this instance had. The
pair record is cleared with it, so the device open that follows treats this as a new pair
and seeds the offset from whatever the restored table holds for it.

## 9. in `gb_parse_state()`

Anything else is from a newer build; skipping it is the point of the format. That is
also why hw= arrives without a version bump: it is a new KEY, and only a change to how
a dev= line splits has ever needed the version.

It also means the short-lived offset=/meas= pair from earlier is simply ignored
rather than misread. meas= carried SAMPLES where hw= carries milliseconds, so reusing
the name would have loaded a 221-sample reading as 221 ms.

## 10. in `gb_parse_state()`

AND THE CONTROLLER HAS TO BE TOLD IF THIS CHANGED THE MODE, for the same reason gbDeviceSlot
exists: the parameter is the host's copy, and a state restore reaches this side only. Live
calls setState repeatedly, so a blob that disagreed with the panel used to leave the two
permanently out of step - the panel saying Host input while the processor had gone back to
opening a device, and no further parameter change coming because nothing the HOST knew about
had changed.

## 11. `gb_parse_measured_line()`

hw=<samples>,<offset ms>,<destination name length>,<destination name><audio uid>

THE LENGTH IS THERE BECAUSE BOTH TAILS ARE FREE TEXT. A dev= line gets away with putting its
uid last and taking the rest of the line, but this one carries two names, and a MIDI
destination is quite entitled to contain a comma - "Scarlett 2i2, Port 1" is an ordinary
thing for a driver to call itself. Counting the first name off by length leaves nothing to
guess at, where a third comma would have been a guess that fails on somebody's interface.

## 12. `text_add()`

── Writing a blob ──────────────────────────────────────────────────────────

A growable buffer, because the length depends on how many devices have been remembered and how
many pairs measured - up to 32 of each. std::string did this by itself; three fields and one
append do the same job with the allocation visible.

## 13. in `gb_bridge_state()`

UNDER THE LOCK, and this is the one that could actually crash rather than merely report a
wrong number. deviceSelector is rewritten by gb_reconfigure() under configLock, and
gb_capture_live_settings() walks the remembered[] table the same worker appends to.

Blocking is fine here in a way it never is in the render path: a host calls this on its own
thread when it saves, and the worst wait is one device swap.

## 14. in `gb_bridge_state()`

HOW MUCH THE HOST TAKES PER CALLBACK, which is a property of the HOST and its buffer setting
rather than of any device - so it is written once, outside the dev= lines.

Saved because neither number available at the FIRST open is right: this host declares 256 and
hands over 512, and the observation needs an undisturbed callback that a project load does not
provide. Without it the ring comes up at 560, discovers the truth a second later and retunes
to 880 - which costs a device reopen per instance, 1.4 seconds each on these interfaces. With
it, the first open is already correct.

A stale value is safe: the retune corrects in BOTH directions now, and
gb_bridge_setup_processing() discards it outright if the host comes back declaring a
different block size.

## 15. in `gb_bridge_state()`

BY NAME, not by index. The MIDI list shifts whenever a device is powered on or off, so an
index saved on Monday names something else on Tuesday - the same reasoning that keeps the
audio device stored as a UID. Ableton was not forgetting the destination; nothing was ever
writing it down.

## 16. in `gb_bridge_state()`

ALWAYS WRITTEN, both values. A key that only appears in one mode says nothing in the
other, and a host that re-applies a state - Live does it repeatedly, for undo snapshots
among other things - would then read "no key" as "device" and quietly switch the plug-in
out of the mode the user chose. An older build ignores the key it does not know.

## 17. in `gb_bridge_state()`

THE MANUAL TRIM, AND THE MEASUREMENTS IT TRIMS. Neither belongs on a dev= line: the offset
is one value for the whole plug-in, and a measurement is keyed by the audio device and the
MIDI destination TOGETHER - a pair no single dev= line names.

Without both of these the feature came apart on reload, and quietly. gb_report_latency()
adds the measured hardware share and then the offset on top of it, so a session reopened
with the pair missing reported a latency short by the entire round trip, with the trim
someone had dialled in by ear silently back at zero. Restoring the offset alone would be
worse than neither: it would trim a base that was not there. The live value belongs to a
pair like every other, so fold it in before writing - otherwise a nudge made since the
last device change would not be in the table yet.

## 18. in `gb_bridge_state()`

NOT LOGGED FROM HERE, and that is not tidiness. This is called by a host far more often
than a save: Ableton takes an undo snapshot on ordinary UI actions, and the loop runs once
per REMEMBERED device - up to 32 of them. synthlib_log_line() opens and closes the file on every
call, so a line here is dozens of file operations on the HOST'S MAIN THREAD every time
someone moves a control. It was added to answer one question ("did the buffer size ever
reach the project file?"), it answered it, and it would have been a fresh cause of the
very beachball it was helping to chase.

The "restoring:" line on the way back in survives, because a load runs once and is where
a value that failed to persist actually shows up as missing.

## 19. in `gb_bridge_set_state()`

UNDER THE LOCK, the mirror of gb_bridge_state(). gb_parse_state() clears and rewrites almost
every piece of configuration the worker reads - the device selector and the saved name, the
remembered[] and measured[] tables, and the offset pair. A host may call this while the
plug-in is loaded and the worker is mid-reconfigure.

The wrapper's read loop is deliberately OUTSIDE this: reading the stream calls back into the
host, which must never happen with this held.

## 20. in `gb_bridge_set_state()`

ASK FOR THE DEVICE AGAIN, because the settings may have arrived AFTER it was opened.

Nothing here controls when a host calls this relative to everything else. If the device
parameter reaches the plug-in first - through the controller, or as a parameter change in the
first process() calls - the device is opened before this blob has been read, so it comes up
with no remembered entry and "leave the device alone" is the honest default. The saved buffer
size then lands in remembered[] with nothing to apply it.

That is CT's "I still had to set 64 manually": the settings were restored correctly and simply
never reached the device. Rather than guess at a host's ordering, react to the late arrival -
gb_request_device() is debounced, so a host that DID call in the tidy order coalesces this
into the open it was going to do anyway.

## 21. `gb_state_parse_active()`

── What the CONTROLLER needs from the same bytes ───────────────────────────

A VST3 host saves the component's state and hands the same bytes to the controller through
setComponentState, precisely so the two can agree on what was loaded - and a controller that
ignores it comes up showing defaults. That is what made two tracks, saved with a Kronos and a
Helix, both reopen as Analog Keys: the UID was in the file, but nothing told the panel about it.

A SECOND READER OF ONE FORMAT, deliberately not a second parser of it: this reads far enough to
recover what a panel has to show and nothing else, and it shares next_line(), line_key() and
copy_field() with the parser above so the two cannot disagree about where a line ends.
