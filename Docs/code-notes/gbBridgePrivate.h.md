# gbBridgePrivate.h notes

The longer comments from `gbBridgePrivate.h`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. file scope

THE BRIDGE'S OWN STATE, AND DELIBERATELY NOT IN gbBridge.h.

The wrapper holds a tGbBridge * and never looks inside it, which is what lets this file use
C11 _Atomic - a keyword C++ does not have. That is not a technicality to work around: the whole
point of the split is that everything below is plain C that a C compiler checks, and the only
code that has to speak C++ is the part that implements COM interfaces.

Included by gbBridge.c, gbMeasure.c and gbState.c - the three files that make up the bridge -
and by nothing else.

## 2. `GB_TARGET_AUTO`

THE SETPOINT IS DERIVED, NOT CHOSEN. A fixed default in milliseconds is the wrong shape for this
number: the floor below which the ring cannot go depends on the host's block size, the device's
block size and the rate ratio, so any constant is either needlessly large on one rig or unsafe on
another. A per-device targetMs of 0 means "work it out", which is the default; a non-zero value
is an explicit override and is still clamped up to the floor.

## 3. `GB_AUTO_MARGIN`

Headroom above the theoretical floor. The floor already covers the worst phase alignment between
the two callbacks; this covers scheduling jitter - a device callback that runs late - which is
not bounded by anything we control. 25% of a few hundred frames is a millisecond or two, which is
cheap next to an audible dropout.

## 4. `GB_DEVICE_SETTLE_MS`

HOW LONG A DEVICE/RATE/FRAMES CHANGE MUST STAND STILL BEFORE IT IS ACTED ON.

Every one of those settings is a stepper, so moving two places sends two values, and each value
used to mean a full teardown and rebuild - closing the device, opening whatever slot was passed
over, then closing it again on the next press. Stepping past a device OPENED it, which is the very
thing "nothing opens until explicitly chosen" exists to prevent; on this rig slot 0 is an iPhone
Continuity microphone, so stepping down from a mixer woke it in passing.

A quarter of a second is longer than a person's gap between arrow presses and far shorter than the
open it defers - an open can spend 800 ms in device_set_sample_rate_and_wait() alone - so a burst
collapses to one device change and a single deliberate change is not perceptibly slower.

This lives in the PROCESSOR rather than in the editor on purpose. A drop-down would stop the
panel's own arrows walking the list, but the host's generic panel and any automation lane can
still sweep the parameter, and they reach this code by the same path.

## 5. `GB_OFFSET_SETTLE_MS`

HOW LONG THE OFFSET MUST STAND STILL BEFORE THE HOST IS TOLD.

Telling the host its latency moved makes it redo delay compensation across the whole session, and
in Ableton that is a visible hitch - so it is the one thing that must not happen once per click.
The offset steps 0.1 ms at a time and is dialled in by ear, which means a dozen or more clicks in
quick succession: exactly the shape that turns a cheap control into a stuttering one.

The VALUE still moves immediately, so the readout follows the pointer and nothing feels laggy.
Only the notification waits. Slightly longer than the device settle because this is a control
someone nudges repeatedly while listening, rather than one they set once.

## 6. `GB_MEASURE_SETTLE_S`

Time to let a previous note decay before listening for silence. Without it a second measurement
starts while the first one's note is still sounding: the floor is taken from a decaying tail, or
the tail itself trips the threshold, and the answer comes back as zero. Measuring twice in a row
is the normal thing to do, so it has to survive it.

## 7. `GB_LATENCY_DEADBAND`

NOTHING USES THIS, AND THE CONVERSION TO C IS WHAT SAID SO (2026-09-09). It sized a deadband on
the reported latency, applied by a latency_worth_reporting() that no caller ever had: C++ does not
warn about an unused private member function, and C warns about an unused static one, so eleven
lines of dead code came out of the move. The function is gone; the number stays because it is the
answer to a question that can come back.

It became moot on 2026-09-08, when the reported figure went back to being the SETPOINT rather than
the smoothed measurement - see gb_snapshot_latency_frames(). A setpoint only moves when something
structural does, so every change is worth telling the host about and there is nothing to filter.
If the measured pipeline is ever reported again, this is the deadband it will need: 64 frames is
1.3 ms at 48 kHz, below what the offset control is ever dialled in to correct.

## 8. `GB_MEASURE_MARGIN`

The onset threshold is RELATIVE to whatever the input is already doing, with an absolute floor
under it. A synth with a hissy output, a hum, or a pad still decaying would sit above any fixed
level and trip the detector the instant the note went out. Measuring the quiet first and then
demanding a multiple of it is what makes the answer mean something.

## 9. `GB_MEASURE_TRIPS`

SEVERAL ROUND TRIPS, NOT ONE, because a single one is not a measurement of anything repeatable.
USB MIDI transit, the synth's own scheduler and its envelope all move the onset a little from one
note to the next, and a figure taken from one note carries all of it. Five is enough to throw
away the extremes and still average three.

## 10. `GB_HOST_INPUT_KEY`

THE AUDIO HALF OF A MEASURED PAIR WHEN THERE IS NO DEVICE. A round trip is a property of (audio
source, MIDI destination) together, and in host-input mode the audio source is not a device with a
UID - it is whatever the host has routed in. Naming it keeps a figure measured that way from being
confused with one measured through a device, which is a different path and a different number.

## 11. `tMeasured`

ONE ENTRY PER (AUDIO DEVICE, MIDI DESTINATION) PAIR, because that pair is what a round trip is a
property of. Two figures, and the difference matters:

```
  hardwareSamples  what the last measurement actually returned. A record, never edited.
  offsetMs         the correction IN FORCE, which is what report_latency() adds. A measurement
                   seeds it; the panel's +/- moves it from there.

```
Splitting them is what lets the panel show "measured 4.6, using 4.8" - and it means re-measuring
replaces the reading and the value together, while a nudge moves only the value.

## 12. `tDeviceSettings`

Per device settings, remembered across sessions and across device changes within a session.

Switching away from a device and back should not lose how it was set up - a 32 channel drum
module and a stereo synth want completely different buffer sizes and channel pairs, and having
to redial them every time is the sort of friction that makes a plug-in annoying rather than
broken. So the state carries a small table keyed by device UID, not just the active device.

## 13. file scope

WHERE THE WRAPPER IS REACHED, and the only route back to it. Everything the bridge needs to
tell the host - a device slot it resolved for itself, a latency that moved - goes out through
these, because only the controller holds an IComponentHandler and only the wrapper can make
an IMessage. See gbBridge.h.

## 14. file scope

---- THE PUBLISHED SNAPSHOT --------------------------------------------------------------

hostRate, nominalRatio, setpointFrames and deviceLatency are plain fields, written by the
worker under configLock while it swaps a device. Several readers need them and CANNOT take
that lock: process() must never block, and the panel publisher is called from paths that
already hold it. Reading the raw fields from there is a data race, and not a harmless one -
nominalRatio is a divisor, so observing the 0 it briefly holds mid-swap yields inf or NaN.

Making `running` atomic fixed the FLAG and not the state it gates, which is the more dangerous
shape: the obvious warning sign disappears while the composite read stays broken. So the
derived values get published as atomics, once, by the thread that changed them - and every
lock-free reader uses these and never the fields behind them.
THE HOST RATE, READABLE WITHOUT THE LOCK. snapHostRate is the config snapshot's copy and is 0
until a capture device opens, but events are forwarded whether one is open or not - the DAW
still plays the hardware. Written in setupProcessing() and read by forward_events(), which
runs before the trylock.

## 15. file scope

THE PIPELINE'S DELAY, SMOOTHED - and it is what the host is told, rather than the setpoint
the ring is aimed at.

The two are not the same thing and the difference is audible. The reported figure used to be
built from setpointFrames, on the reasonable ground that a latency moving every block would
have the host redo delay compensation continuously. But the ring only sits AT its setpoint on
average and, on a bursty host, sits below it: measured on this rig, told 2679 against an
actual 2221-2506, a gap of 4 to 9 ms. A host compensating by 9 ms for a 5 ms delay records
everything 4 ms early, which is exactly the report that led here - and no amount of measuring
the SYNTH can fix a figure that is wrong about US.

A two-second average, so it follows the ring without chasing its sawtooth, and a deadband
before the host is told (see latency_worth_reporting) so a figure that now moves does not
cost a compensation pass every block.

## 16. file scope

Frames handed over so far inside the current callback, and how many of them came BEFORE the
current call. The second is the one the latency arithmetic wants, and it has to be counted
rather than inferred from the model: the model legitimately runs ahead of the clock across
callbacks as well as within them, so model-minus-clock grows without bound and is not this.
AUDIO THREAD ONLY.

## 17. file scope

How many trips survived to be averaged, and what the extremes were. Published because an
average on its own hides the thing worth knowing: five readings within a millisecond mean the
number can be trusted, and the same average from readings 9 ms apart means it cannot. The
panel shows the range beside the figure for exactly that reason.

## 18. file scope

WHICH PAIR THE LIVE offsetMs IS FOR. Without this the correction could not survive its own
application: changing the reported latency makes the host reactivate the plug-in, which
reopens the device, and a reopen that re-seeded the offset from the table would undo every
nudge the moment it took effect. Re-seeding is now gated on the pair actually having changed,
so reopening the same device leaves the value exactly where the user put it.

## 19. file scope

THE UID OF WHAT IS OPEN. A char array rather than the std::string it was, and the change is
not only about the language: getState() and the worker both touch this, and a std::string
being reassigned under a concurrent reader is a pointer that may already have been freed -
which is what the lock around them exists to prevent. A fixed array cannot dangle.

## 20. `gb_lock_config_from_host()`

── What the three files that make up the bridge call in each other ─────────

Every one of these was a member function. They are declared here rather than left to the order
they happen to appear in, which is what a class gave for free and a C file does not.
