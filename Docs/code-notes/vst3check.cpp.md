# vst3check.cpp notes

The longer comments from `vst3check.cpp`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. file scope

── Headless conformance and state checks for the plug-in ───────────────────────────────────────

The companion to tools/vst3host, and deliberately not the same program: vst3host opens a window
and shows the editor, which is the only way to check that drawing works and the only thing that
needs a run loop. This one has no window and answers different questions - does the factory look
the way a host expects, does the saved state survive a round trip, does activation actually open
a device - so it can run anywhere and be diffed.

It exists in the repository, rather than in a scratchpad, because the scratchpad version of it
was written twice in a single afternoon and thrown away twice. vst3host.mm carries the same note
for the same reason.

WHAT IT PROVES, AND WHAT IT DOES NOT. Passing every check here does not mean a DAW will accept
the plug-in. An earlier version of vst3host asked only for IPluginFactory and so never noticed
that IPluginFactory2 was missing - which is precisely what Ableton rejected G2-Edit for.
~/Library/Preferences/Ableton/Live */Log.txt remains the last word on a rejection.

```
Build: ./do-vst3host    (it builds this too)
```

## 2. file scope

THE PLUG-IN'S OWN LIVE FIGURES, read straight out of it. vst3check dlopen()s the bundle into its
OWN process, so the status block the panel reads thirty times a second is in this address space
too - gb_status() is exported and needs only a dlsym. Everything about the ring that matters is
in there: fill against setpoint, the drift correction in force, underruns and resyncs. Without it
a bad measurement is just a bad number.

## 3. `wait_ms()`

WAIT THE WAY A HOST WAITS: with the main run loop turning. Since GenBridge moved onto SynthLib's
wrappers (2026-09-11), what it tells the host - a latency change, a parameter it corrected itself -
is posted to the main thread, which is where both plug-in formats require it to be said. A host's
main thread is always running its loop; a checker that usleep()s instead never delivers any of it,
and would report a plug-in that spoke up as one that said nothing.

FOR THE WHOLE INTERVAL, not until the loop finds nothing to do: with no source to service,
CFRunLoopRunInMode() returns at once, and the waits below exist to give a device time to open.

## 4. in `wait_ms()`

---- a minimal host, enough to exercise the processor/controller connection ----

Messages between the two ends are created BY THE HOST - IHostApplication::createInstance is the
only way to get an IMessage - so a checker that passes a null context can never see them. That is
why the latency notification could not be tested here before, and why it reached a DAW broken.

## 5. in `wait_ms()`

One note, delivered the way a host delivers them.
TWO NOTES IN ONE BLOCK, at chosen sample offsets. The point of the test it serves is that they
must NOT come out together: a host block is not an instant, and an event's offset is where inside
it the note belongs.

## 6. in `wait_ms()`

THE UID OF A DEVICE THAT IS ACTUALLY ON THIS MACHINE, looked up by the display name the plug-in
itself reports for a slot.

The saved state used to name a hardcoded Line 6 Helix. That is a real device the author had
plugged in, and the check passed for exactly as long as it stayed plugged in - after which the
harness reported a product failure every run for an absent cable. Worse, it reported it for the
one thing it was built to catch: with the device gone the plug-in correctly waits rather than
resolving, so the test could no longer distinguish correct waiting from the silent default it
exists to detect.

Matching on NAME rather than replicating the plug-in's slot ordering is deliberate: the ordering
is the plug-in's business and duplicating it here would be a second copy to keep in step, which
is the bug class the plug-in's own notes already record twice.

## 7. in `wait_ms()`

INPUT-CAPABLE ONLY, because that is the list the plug-in shows (gbDraw.c's skips a device
with no input channels). Two devices can share a display name and differ only in
direction - this rig has an output-only and an input-only "CalDigit Thunderbolt 3 Audio" -
so matching on name alone picked the one the plug-in does not list, produced a UID it
could not resolve, and again looked like the silent-default bug rather than a harness that
had named the wrong device.

## 8. `gBurst`

HOW MANY BLOCKS THE HOST COMPUTES PER AUDIO CALLBACK. One is the easy case and the only one this
harness used to drive: a block, a pause, a block. Ableton declares 512 and calls with 128, which
is four blocks computed back to back and then nothing for the rest of the cycle - a completely
different shape, and the one every ordering and timing defect has hidden in. --burst K drives it.

## 9. `gDeadline`

PACED AGAINST AN ABSOLUTE DEADLINE, NEVER usleep(). usleep() sleeps AT LEAST what it is asked and
the overshoot accumulates, so a loop of usleep(2666) hands over blocks slower than 48 kHz - the
capture device fills the ring faster than the harness drains it, and the plug-in resyncs. That
resync is the harness's, and the log records it as the plug-in's: a rig that manufactures the
fault it is looking for. MidiSyncTool paid for this one already (its findings, 2026-09-02: 0.92 ms
of "jitter" that was entirely the usleep harness).

mach_wait_until() against a running deadline has no such drift - an early or late wake is absorbed
by the next one rather than added to it.

## 10. in `run_blocks()`

BUFFER FIRST, THEN DEVICE, and ONLY IF ONE WAS ASKED FOR. Sending it unasked meant
sending 0.0 - a real slot, not "leave it alone" - so every run_blocks() call changed the
device buffer, reopened the device, and destroyed whatever measurement was in flight. No
measurement completed at all and the plug-in looked broken.
DEVICE FIRST, THEN THE BUFFER, LATE - which is the order a HOST restores in, since it
walks parameters by id and the device is 0 while the buffer is 3. Sending the buffer
first, as this used to, opens the device already knowing the size it wants and never
exercises the path that was failing: open at whatever the device was, then be asked to
change it. That second reconfigure is where a just-closed stream looked like another
client and the request was skipped.

## 11. `run_one_now()`

ONE CALL, WITH NO PAUSE AFTER IT. run_blocks() paces itself at one 128-frame block per 2.666 ms,
which is a host handing over exactly one block per audio callback. That is the easy case. This is
the other one: several process() calls computed back to back inside a single callback, which is
what Ableton does when it declares 512 and calls with 128, and what Live does at a loop wrap.

## 12. `gSinkBytes`

── Does host MIDI actually come out the other side? ────────────────────────────────────────────

The bus checks above prove the instrument DECLARES an event input. They do not prove a note the
host hands it ever reaches a MIDI cable, which is the plug-in's entire claim as an instrument and
the thing that cannot be seen from the outside - a silent hardware synth looks the same whether
the plug-in dropped the note or the synth is switched off.

The --play round trip settles it, but only with a real synth on a real port. This does the same
job with neither: the test process creates its OWN virtual MIDI destination, so it appears in the
system's destination list like any interface, the plug-in can be pointed at it by name, and
whatever the plug-in sends arrives back here to be counted.

THE SINK MUST EXIST BEFORE THE PLUG-IN IS INSTANTIATED. The destination list is enumerated when
the plug-in builds its parameter, so a sink created afterwards is not there to be chosen.

## 13. in `midi_out_test()`

── AN EVENT'S OFFSET INSIDE THE BLOCK ──────────────────────────────────────────────────

The check above proves a note gets out. This proves it gets out at the right MOMENT, and
it is the regression test for the bias that made recorded parts creep earlier the larger
the host's buffer got: every note used to be fired the instant process() was entered, so
a note at offset 127 of a 128-frame block reached the synth 2.6 ms early - and at 2048
frames, 42 ms early. See findings 2026-09-08.

Both notes go in ONE block, so nothing about the pace of the test can affect the answer:
what is compared is the two packets' own timestamps, which is exactly what the plug-in
decided. 127 frames at 48 kHz is 2.6458 ms.

## 14. in `midi_out_test()`

TWO PACKETS, and the count is itself the first half of the answer. Stamped identically -
which is what firing both at the block boundary does - CoreMIDI coalesces them into ONE
packet carrying all six bytes, so a failure here means the offsets were discarded rather
than that a note went missing.

## 15. in `midi_out_test()`

── BLOCKS COMPUTED BACK TO BACK IN ONE CALLBACK ────────────────────────────────────────

The offset test above proves an event keeps its place inside a block. This proves the
BLOCKS keep their place relative to each other, which is a different question the moment
a host hands over more than one per audio callback - and the host this is built against
does, four at a time. Reading the wall clock per call and calling it the block's start
gives all four the same answer: the second call's note at offset 0 comes out stamped
BEFORE the first call's note at offset 127, though it is truly 128 frames later, and a
note-off overtaking its note-on is what that sounds like.

Two calls with no pause between them, a note at offset 0 in each. They belong exactly one
block apart - 128 frames, 2.667 ms - however close together they were computed.

## 16. in `midi_out_test()`

── THE STREAM MUST NEVER GO BACKWARDS ──────────────────────────────────────────────────

The two checks above prove events keep their place inside a block and blocks keep their
place inside a callback. This proves the CALLBACKS keep their place relative to each
other, which is the one that took a working plug-in and made it look like it had stopped
passing MIDI through.

Audio callbacks jitter. A callback that arrives a few hundred microseconds EARLY, on a
clock that re-anchors to the wall at every boundary, produces a first stamp before the
previous callback's last one - and CoreMIDI delivers by timestamp, so a note-off can
overtake its note-on. Notes stick on, later ones queue behind them, and a quantised clip
whose events are far apart still plays perfectly, which is what makes it so confusing.

Twenty notes, one per call, with the gap between calls deliberately alternating short and
long. The pitches must come back in the order they were played and their timestamps must
be strictly increasing.

## 17. in `midi_out_test()`

FOUR-BLOCK CALLBACKS WITH THE BOUNDARY JITTERED, which is the shape that inverts. A
burst leaves the block model up to a whole callback ahead of the clock; if the NEXT
callback then arrives early, re-anchoring to the clock steps the stamps backwards over
the burst that just went out.

The gaps alternate 8 ms and 13.3 ms around a true 10.67 ms cycle, so the pacing is right
ON AVERAGE - a test whose mean is wrong would just walk the model into its ceiling and
fail for a different reason.

## 18. in `midi_out_test()`

6 ms and 15.3 ms around a true 10.67 ms cycle. The early one has to be early by more
than a BLOCK, not merely early: a callback that re-anchors to the clock lands on the
stamp the previous burst's last block already used, and equal stamps still come out
in the order they were handed over. 8 ms sat exactly on that boundary and the test
passed either way, which is worse than no test.

## 19. in `midi_out_test()`

HONEST ABOUT ITS REACH. This sink is a virtual destination in the same process, and
CoreMIDI hands those to the read proc as they are sent rather than holding them to their
timestamps - so what is proved here is that nothing is DROPPED or reordered on the way
out, not that a hardware port would release them in this order. The stamps themselves are
checked by the two tests above.

## 20. in `midi_out_test()`

The weaker of the two, and worth knowing why: CoreMIDI has already sorted the packets by
timestamp before this reads them, so an inversion shows up as the PITCHES arriving out of
order rather than as a stamp going backwards. This catches only the case where the sink
received them in a batch the sort could not fix.

## 21. in `main()`

BOTH processors register under kVstAudioEffectClass - that is the VST3 category for
anything that makes audio, and the SUBcategory is what separates an effect from an
instrument. Matching on the category alone picked whichever came last, so every check
below silently ran against the instrument.

## 22. in `main()`

THE SINK IS MADE FIRST, before the plug-in is loaded at all. gbMidi.c caches the destination
list for a second, and that cache is per-dylib rather than per-instance - creating the sink
later meant the plug-in was still answering from a list taken before it existed, and the test
failed while the plug-in was behaving perfectly.

## 23. in `main()`

THE TAIL IS A PROMISE ABOUT SILENCE, and this plug-in must not make the usual one. kNoTail
says "nothing comes out once my input goes quiet" - true of a reverb, false of a bridge
whose audio arrives from hardware and never came from the input bus at all. The effect is
used on a track with nothing feeding it, which is precisely when a host may stop
processing a chain that has promised to be silent.

## 24. in `main()`

RESIZE MUST NOT OSCILLATE. A host negotiates a resize by proposing a rect and applying
what comes back, over and over through a drag, so checkSizeConstraint() has to be a
FUNCTION of the rect it is given and of nothing else.

HONEST LIMIT: these guard the property, they do NOT reproduce the reported fault. CT saw
the whole window jump and snap back mid-drag; the two history-dependent versions of
checkSizeConstraint() that preceded this one BOTH PASS these checks, because reproducing
them needs the host's real sequence of calls - how many times it asks per pointer move,
which rect it proposes, and when onSize() lands between them - and that was guessed at
twice, wrongly. gb_editor_log() in the old gbEditor.mm recorded the real sequence; the
shared SynthLib view that replaced it (2026-09-11) logs nothing, so add a log there first.
Until a log from
a host that shows the fault exists, do not trust a test here to have caught it.

## 25. in `main()`

THE WIDTH IS RESTORED AND THE HEIGHT IS DERIVED, which is what this pair now checks.

748 against 700 is the aspect of the canvas BEFORE the Capture Source row was added -
520x556 - and restoring it is precisely the bug CT hit on 2026-09-09: the window came
back at the old shape, the panel is scaled by WIDTH alone, and the rows past the
bottom edge were simply not there until a resize put the aspect right. So the saved
height is now ignored and recomputed from the current canvas, which makes any future
change of GB_CANVAS_H self-correcting for sessions saved before it.

## 26. in `main()`

THE MICROPHONE BUG. A project naming a device that is not plugged in must open NOTHING.
The host restores its saved DEVICE PARAMETER too, and that is a slot INDEX recorded when
the device list had a different shape - so slot 0 arrives here exactly as a host delivers
it, and slot 0 on a Mac is generally a built-in or Continuity microphone.

ASSERTED ON THE PLUG-IN'S OWN LOG, which is not where a check would rather look. The
obvious signals do not work: getLatencySamples() only moves when a device actually OPENS,
and on a developer machine every device is already claimed by something, so it reads zero
whether the plug-in tried the microphone or refused to. active= only changes on a
successful open, for the same reason. The log is the one place the plug-in records the
decision rather than the outcome, and the decision is what is on trial.

ONE parameter value, repeated. A host restoring a project sends one, and it is precisely
the FIRST one after a state restore that must not be believed; later changes are the user
choosing, and are meant to be honoured.

## 27. in `main()`

THE GATE IS RE-POLLED ONCE A SECOND (SynthLib's plugin/synthlibLog.c), so a line logged within a second of
an earlier one still finds it absent. This check passed only while nothing had
logged in the second before it; since 2026-09-11 every instance logs as it is
created, and the instance made just above for the resize checks had.

## 28. in `main()`

---- two instances must not share status ----

The status figures used to live in one process-wide struct, so with two plug-ins in a set the
editors read whichever processor wrote last: a panel showing a microphone cheerfully reported
that it was capturing a Kronos. Each processor now claims its own slot and tells its own
controller which one, over IConnectionPoint.
---- the instrument variant ----

Same code, registered a second time with its own identity, category and bus layout. An
instrument has NO audio input, which a host only tolerates because IPluginFactory2 declares
the subcategory - with the bare "Audio Module Class" a host assumes effect, looks for the
input an effect must have, and refuses to load. That is the trap G2-Edit fell into.

## 29. in `main()`

ONE AUDIO INPUT, AND IT MUST BE A SIDE-CHAIN. This said "no audio input bus" until
2026-09-09, which was the right assertion while there was none: a MAIN input on an
instrument makes a host go looking for a source and refuse the plug-in when there is
none, which is the trap G2-Edit fell into.

The instrument now declares one for the External-Instrument mode - the host's own
interface input, passed through and timed. What protects against the old trap is no
longer the ABSENCE of the bus but its SHAPE, so that is what is checked here: kAux,
and NOT default-active, so a host with no use for it leaves it alone.

## 30. in `main()`

---- the round trip, on real hardware ----

The instrument's entire claim is that the DAW can play the hardware and record it back. That
is testable without a person listening: send a note, capture the device, and see whether
anything arrives. Opt-in, because it makes a real synth make a real noise.

## 31. in `main()`

MEASURE, not just detect. The number wanted is how long after the note was sent
the audio actually arrived - USB MIDI transit, the synth's own response, the
capture path and our ring, end to end.

It necessarily includes the PATCH'S ATTACK: a slow pad crosses the threshold later
than a piano does, and no measurement can separate the two from outside. So this
is "time to audible onset" and it wants a percussive sound to mean anything.

## 32. in `main()`

Drive the plug-in's OWN Measure control, so its detection is exercised rather
than the harness's - they are different code and only one of them ships.

REPEATEDLY, IF ASKED. One measurement says nothing about whether the measurement
is repeatable, and repeatability is the property that matters: a figure that
moves by 8 ms between two runs on an unchanged synth is not a measurement. The
per-run detail goes to /tmp/genbridge.log (touch /tmp/genbridge-log first); what
is printed here is the spread of what the plug-in ended up reporting.

## 33. in `main()`

Five trips of settle + floor + response is about three seconds; 1900 blocks is 5.1, and the
120 after it are the pause the worker needs to store the result.

DRIVEN BLOCKS, NOT A usleep(). A pause here is a host that has stopped
calling process() while the capture device keeps filling the ring - 300 ms is
14400 frames into a 7680 frame ring, so it overflowed, the plug-in resynced,
and store_measurement() correctly threw away every run for happening over a
resync. Seven of eight runs discarded, and not one of them was the plug-in's
fault. A DAW does not stop calling process() and neither should this.
FIVE TRIPS of settle + floor + response is a little under three seconds, and
2200 blocks is 5.9 - the run has to be driven right through, plus enough
after it for the worker to store the result.

## 34. in `main()`

WHAT THE MEASUREMENT LANDED IN. The offset parameter is where the hardware
share ends up - the reported latency also carries the ring, which the block
retune can move underneath a run and would read as measurement scatter that
is nothing of the kind.

## 35. in `main()`

---- the save/reload cycle ----

A host saves the component's state and, on reload, hands the SAME bytes to the controller via
setComponentState so the panel can agree with the processor. A controller that ignores it
comes up on defaults - which is how two tracks saved with different devices both reopened
showing the first device in the list.

## 36. in `main()`

NO SUFFIX ON THE UID. The literal this replaced ended ",3" and it looked like a trailing
field; it is not - Apple's USB UIDs embed a comma themselves
("AppleUSBAudioEngine:Allen&Heath Ltd:QU-24:111000:2,3"), so the whole string including it
IS the UID. Appending another one produced a UID that matched nothing, and the plug-in
correctly declining to resolve it looked exactly like the silent-default bug this check
exists to catch.

## 37. in `main()`

── THE CALLBACK SIZE SURVIVES A SAVE ───────────────────────────────────────────────────

Its entire purpose is to be there at the FIRST open of the next session, so a round trip
is the only thing worth asserting about it. It is a PROCESSOR field - how much the host
takes per callback, not a parameter - so it goes through the component rather than the
controller checked above.

Without it the ring is sized from the declared block size, which on the host this was
built against under-states by half: it declares 256 and hands over 512, so the ring came
up at 560, discovered the truth a second later and retuned to 880 - at the cost of a
device reopen per instance.

## 38. in `main()`

THE CHECK'S OWN VIRTUAL DESTINATION, not a synth. This named "KRONOS SOUND" and so failed on
any day the Kronos was switched off - reporting the restore as broken when there was simply
nothing of that name to restore to. The sink is made before the plug-in loads and exists for
the whole run, so it is always there to be found by name.

## 39. in `main()`

---- the External-Instrument mode: capture from the HOST'S input ----

Capture Source = Host input takes the audio the host hands over and passes it through, instead
of opening a CoreAudio device: no ring, no resampler, no drift loop, because the host's input
and its output are the same clock. That is checkable here in a way the device path never was -
there is no hardware in it, so a known signal in must come back out.

WHAT WOULD OTHERWISE GO UNNOTICED: this path is on the audio thread and takes no lock, and the
failure it is most likely to have is silence - a null input treated as an error, a channel
index off by one, or the mode not being reached at all because the parameter did not arrive.

## 40. in `main()`

Ten blocks in DEVICE mode first, with the input present the whole time: nothing
should come back, because that mode reads the ring and the ring has no device
behind it. This is the control - without it, a passthrough that ignored the
parameter entirely would pass the check below.

## 41. in `main()`

THE MODE ADDS NOTHING OF ITS OWN, and the reported latency is where that
shows. With no device open there is no ring, no resampler and no converter to
account for, so what the host is told is the measured correction alone - zero
until something has been measured. A non-zero figure here would mean the
pipeline term is still being added over a pipeline that does not exist.

## 42. in `main()`

---- activation ----

A FRESH INSTANCE MUST OPEN NOTHING. Earlier versions grabbed a device on activation so as not
to look broken, and on a machine whose first input is an iPhone Continuity microphone that
meant waking it once per instance, on the host's main thread, during load. Ableton stopped
starting. Latency stays at zero until something is actually chosen.

## 43. in `main()`

---- drive a real parameter change, the way a host does ----

This is the path that failed in Ableton: the editor asks the host to change a parameter,
the host delivers it inside process(), and the processor acts on it. None of the earlier
checks touched it, which is precisely why the bug reached a DAW.

## 44. in `main()`

POLL UNTIL IT OPENS, rather than sleeping a fixed three seconds and hoping.

How long an open takes is a property of the DEVICE, not of the plug-in: a USB
interface that is already awake answers in well under a second, while slot 1 on
this machine became an iPhone Continuity microphone - which has to wake a phone
first, and does not reliably manage it inside three seconds. A fixed sleep turned
that into an intermittent failure of "a selection opens a device", which reads as
a plug-in fault and is not one.

Ten seconds is a ceiling, not a wait: the loop leaves as soon as the latency goes
non-zero, so a normal device costs no more than it did before.

## 45. in `main()`

---- the block-size retune ----

maxSamplesPerBlock was declared as whatever --block said, but a host may call
with far less; Ableton declares 512 and uses 128. The plug-in watches for a
couple of seconds and shrinks the ring to suit.

PACED AT REALTIME, deliberately. Driving process() as fast as the loop can go
would consume the ring far quicker than a real device fills it, the plug-in would
underrun, and the safety net would correctly undo the very thing being tested.

## 46. in `main()`

PACED AGAINST A DEADLINE, not usleep - the same fix as run_blocks(). A loop
of usleep(2666) runs slower than 48 kHz, so the capture device outruns the
harness and the ring backs up; now that the plug-in reports the delay it
ACTUALLY has rather than the one it aims at, that backlog shows up as 147 ms
of latency and fails the two checks below. The plug-in was telling the truth.

## 47. in `main()`

A REOPEN MUST NOT UNDO THE LESSON. Telling the host about a latency change makes
it reactivate the plug-in, which reopens the device; if that reset the
observation the two would chase each other for ever, tearing the device down
every couple of seconds. This is that loop, reproduced.

## 48. in `main()`

WITHIN A TOLERANCE, NOT EXACTLY. What this is guarding against is the retune
being UNDONE - the setpoint snapping back to the conservative floor, which on
this rig is several hundred samples. The reported figure now follows a live
measurement of the pipeline rather than a constant derived from the setpoint, so
it legitimately differs by a few samples across a reopen; demanding equality
would be testing the noise floor of a measurement, not the behaviour.
