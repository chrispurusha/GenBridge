# gbBridge.c notes

The longer comments from `gbBridge.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. file scope

THE BRIDGE ITSELF: a CoreAudio device on one clock, a DAW on another, and a ring, a resampler and
a drift loop between them. What used to be a 5,000 line C++ class is this file, gbMeasure.c and
gbState.c - plain C, with the VST3 wrapper reduced to the COM plumbing it has to be.

The substitution the whole plug-in rests on is smaller than it looks: process() consumes blocks on
a clock that is not the capture device's, which is exactly what the proof of concept's output
IOProc did. Everything underneath - SynthLib/audio/ring.c, poc/drift.c, poc/resampler.c - is the
same code, unchanged and shared with the command line tool.

## 2. `gb_lock_config_from_host()`

HOW LONG THE HOST'S OWN THREAD WAITED FOR configLock, said out loud when it is long enough to
see. getState(), setState() and setupProcessing() all take that lock, and the worker holds it
across an entire device close and open - so a reconfigure can block whichever thread the host
called on. In Ableton that thread is the main one, and it calls getState() for undo snapshots
on ordinary UI actions: the beachball CT reports.

This turns "occasional or regular spinning cursor" into a number with a cause beside it. If
these lines are absent while the cursor spins, the cause is somewhere else entirely and this
has ruled out the obvious suspect - which is worth as much.

## 3. `gb_publish_config_snapshot()`

WHAT THE PLUG-IN ITSELF ADDS, with no hardware correction in it. Split out of
gb_report_latency(self) because the measurement has to subtract our share from the onset it sees,
and subtracting the REPORTED figure meant subtracting the previous measurement along with it:
every re-measure came back short by whatever correction was already in force, so the value
walked towards zero the more times it was run. Only ever grows out of the ring and the
converters, so it is the honest thing to net off.
Call with configLock HELD, after any change to the four fields below. One place, so a new
writer cannot forget half of them.

## 4. in `gb_snapshot_latency_frames()`

THE SETPOINT, NOT THE MEASUREMENT - reverted 2026-09-08, and the reason is worth keeping.

Reporting a smoothed measurement removed a 4-9 ms over-compensation and created something
far worse: the figure MOVES, every move past the deadband tells the host its latency
changed, and a host answers that by reactivating the plug-in. Reactivation closes and
reopens the device, which perturbs the ring, which moves the figure again. Ableton sat in
that loop - reconfigure every 1.5 seconds, each holding configLock for 1460 ms, the
device flapping between 64 and its restored 512 - which is both the beachball and the
"I set 64 and get 512" in one.

A latency a host is told must be STABLE. pipelineAvg is still measured and still published
as "actual" beside this, so the gap that started this can be seen and dealt with by
shrinking it rather than by chasing it.

## 5. `gb_latency_frames_measured()`

THE PIPELINE'S ACTUAL DELAY AT THIS INSTANT, and it takes TWO numbers, not one.

Occupancy on its own is not an age. The device writes a whole buffer at a time, so the fill
jumps up by deviceFrames at each capture callback and falls as the host drains it - while the
audio already in the ring is getting older at exactly the same rate. The two move in
ANTI-PHASE and their sum is constant; take the fill alone and what is left is a sawtooth of a
full device buffer. At 512 frames that is 10.7 ms, which is precisely the range a repeated
measurement was wandering over.

So: how long ago the newest frame arrived, plus how many sit in front of it. deviceLatency
has the device's buffer inside it (device_latency_frames() sums bufferFrames + latency +
safety offset + stream latency), and that buffer term is what "how long ago" now measures
directly - counting it twice would put a whole buffer back on.

The setpoint, NOT this, is what the host is told: the loop holds the AVERAGE fill there by
design, and a latency that moved with every block would have the host redo its delay
compensation continuously. This is for the measurement, which needs the instant it happened.

## 6. in `gb_latency_frames_measured()`

NO CALLBACK-LEAD TERM HERE, and it was tried. The frames already handed over inside a
callback look like they belong - the fill at the fourth of four calls is 384 frames below
the fill at the first - but the ROUND TRIP this figure is subtracted from is invariant to
which call detects the onset, and adding the term destroys that.

The reason is worth writing down. If the onset sits at position p in the ring, the call
that reads it finds it at cross = p - (frames already read), and that call's fill is
lower by exactly the same amount. The two cancel: whichever call detects it, the answer
is the same. Adding the lead uncancels them, and a measurement that had settled to 0.2 ms
across runs went back to a 7.3 ms spread - the very lottery it was meant to remove.

The lead DOES belong in the delay from capture to where the host finally places the
audio, which is a different quantity - see the note on gb_internal_latency_frames(self) and
findings 2026-09-08 (5).
HOW LONG AGO THE LAST CAPTURE CALLBACK LANDED - CLAMPED, and it needs to be.

Before the first callback the timestamp is 0, and the gap from 0 is the machine's entire
uptime; after the device stops delivering it grows without limit. Either way the figure
stops meaning "how far into the current device period we are" and starts poisoning the
reported latency - it put 151 ms on the panel and failed two checks that had passed for
weeks. In normal running this term never exceeds one device period, so anything past a
few of them is not a measurement, it is a device that has gone quiet.

## 7. in `gb_report_latency()`

THE HARDWARE'S SHARE IS ADDED FOR THE INSTRUMENT, because that is what makes a recorded
part land on the beat. The host delays everything else to match, which is precisely the
job an External Instrument device does in Live with its Hardware Latency field.

Not for the effect: nothing is being played through it, so there is no round trip to
compensate and inflating its latency would only push a live input further out of place.

ONE TERM, NOT TWO. This used to add hardwareSamples and then offsetMs on top of it, which
made the panel incoherent: Measure wrote a figure you could not touch, beside a trim that
started at zero and existed only to correct it. The measurement now lands IN offsetMs, so
what is added is simply the correction in force - and adding hardwareSamples as well here
would count the round trip twice.

## 8. `gb_channel_for()`

WATCH THE BLOCK SIZE THE HOST ACTUALLY USES, which need not be the one it declared.

The ring's floor is computed from maxSamplesPerBlock, because that is all a plug-in is told
before it has to size anything. Ableton declares 512 and then calls with 128 - so the floor
came out at 768 frames where 384 would do, and the plug-in reported 26 ms of latency for a
job that needs 16.

So: watch for a couple of seconds, take the largest block actually seen, and if that leaves a
worthwhile amount on the table, ask the worker to retune. This runs on the audio thread and
therefore only ever raises a flag - the setpoint change and the message to the host both
allocate, and neither belongs here.

ONCE ONLY, and never upward. A latency change makes the host redo its delay compensation, so
doing it repeatedly would be worse than the latency it saves. If the gamble is wrong - a
larger block arrives later and underruns - the safety net below puts the conservative floor
back for the rest of the session and stops trying.
THE DAW PLAYS THE HARDWARE. Note data arrives as VST3 events and leaves as MIDI bytes on a
CoreMIDI destination; the audio comes back through the same capture path the effect uses.
That round trip is what makes this an instrument rather than a recorder.

Sent straight from process(), not queued. MIDISend is not strictly real-time safe, but the
alternative - handing the bytes to another thread - adds exactly the jitter that makes a
hardware synth feel loose, and every plug-in that drives external gear makes the same trade.

Continuous controllers, pitch bend and aftertouch do NOT arrive here: VST3 delivers those as
parameter changes via IMidiMapping, which is a separate piece of work and is why G2-Edit's
controller implements it. Notes first.
0 keeps whatever the host sent; anything else forces the channel.

## 9. `gb_block_host_time()`

WHERE THIS BLOCK BEGINS IN WALL TIME - AND IT IS NOT "NOW".

A host is entitled to hand over several blocks per audio callback, and the one this is built
against does: Ableton declares 512 and calls with 128, which is FOUR process() calls computed
back to back inside one 512-frame cycle, microseconds apart. Reading the clock at the top of
each and calling it the block's start time gives all four nearly the same answer - so blocks
two, three and four are stamped up to 384 frames early, and worse, their events INTERLEAVE
with the previous block's: a note at offset 0 of the second call is truly 128 frames after a
note at offset 127 of the first, but comes out stamped 127 frames BEFORE it. CoreMIDI
delivers in timestamp order, so that is a note-off overtaking its note-on. It sounds exactly
as bad as it reads.

THE FRAMES THEMSELVES CARRY THE PACING. A block starts where the frames already handed over
run out, so the model is one addition: carry the end of the last block forward and use it,
unless real time has already passed it - which is what a new device cycle looks like, and
what a late callback looks like too. Nothing has to classify cycles or measure a buffer.

MidiSyncTool reached the same place from the other end (its findings, 2026-09-02: Live splits
a buffer at a loop wrap into 500 frames and then 12). Its version has to identify the cycle
because it needs the cycle's own length for its telemetry; this one does not.

THE CAP IS FOR A HOST THAT RUNS FASTER THAN REALTIME. An offline bounce hands over blocks as
fast as it can compute them, and an unbounded model would schedule MIDI further and further
into the future. A quarter of a second is far longer than any single device cycle and far
shorter than a bounce takes to run away.

## 10. in `gb_block_host_time()`

THE RAW CLOCK IS KEPT AS WELL AS THE MODEL, and the two are not interchangeable. Outgoing
MIDI is stamped on the MODEL, because that is where a note belongs musically. Anything
measured about the audio COMING BACK has to use the raw one, because the ring holds what
physically arrived by this instant - and for the second, third and fourth call of a
callback the model is up to a whole cycle ahead of it. See gb_run_measurement(self).

## 11. in `gb_block_host_time()`

THE CYCLE BOUNDARY, decided here because both users need the same answer: this is where
the model is allowed to carry forward, and it is what sizes the ring in gb_observe_burst(self).

Two calls belong to one callback if the second arrives before half of the first could
have been played. Back to back they are microseconds apart; a real boundary is a whole
block. Three orders of magnitude between the regimes, so the threshold is not delicate.

## 12. in `gb_block_host_time()`

MONOTONIC, AND THAT IS THE WHOLE POINT OF IT.

The model may never hand back a time earlier than the end of the block before it. Letting
it re-anchor to the clock at every callback boundary looks right - the clock is the truth,
after all - and it inverts the event stream: audio callbacks jitter by a few hundred
microseconds, so a callback arriving EARLY gets a first stamp before the previous
callback's last one. CoreMIDI delivers in timestamp order, so a note-off can be handed
over ahead of the note-on it belongs to. Notes stick on, and the next ones are lost
behind them - which from the keyboard looks like the plug-in has stopped passing MIDI
through altogether, while a quantised clip, whose events are sparse and far apart, plays
perfectly.

So: carry forward, always, and let the ceiling below be the only thing that pulls it back.
A block boundary that jitters early simply waits; the grid does not move.

## 13. in `gb_block_host_time()`

A HARD CEILING ON TOP OF THE RULE, because the rule is a heuristic and this is not.

Within a callback the model is legitimately ahead of the clock, and by definition never
by more than one callback's worth of frames. If it ever is, the boundary test has missed
one - a host rendering faster than realtime hands over call after call with no gap
between them, and every one of them reads as a continuation. The model then walks into
the future without limit, and MIDI stamped from it is not late, it is GONE: CoreMIDI
holds each packet until a time that may be minutes away. Nothing sounds, and the symptom
is "the plug-in stopped passing notes through" with everything else working perfectly.

Two callbacks of slack, so ordinary jitter never touches it, and falling back to the
clock when it trips. The cost of the ceiling being wrong is one callback of MIDI timing;
the cost of no ceiling is silence.

It is also what makes the monotonic rule above safe. Never going backwards on its own
would let one fast burst push the grid permanently into the future; never going forwards
too far on its own inverts the stream. Together they say: follow the frames, and if that
has drifted implausibly far from the clock, admit it and start again.
GENEROUS, DELIBERATELY. The ceiling is a last resort against a runaway, not a tuning
knob, and tripping it IS a backwards step - the one thing the rule above exists to
prevent. Set it at twice the measured callback it came out at 256 frames while the host
was handing over bursts of 512, so it tripped on every burst and inverted the stream it
was supposed to protect.

A host taking more than eight of its own declared blocks in one callback is pathological,
and the measured burst is not always available - observedMaxFrames is only collected
while a device is running, and MIDI flows whether one is or not.

## 14. in `gb_block_host_time()`

AND IT HAS TO COME BACK, WITHOUT EVER GOING BACKWARDS.

The model advances by the host's NOMINAL frame count while the clock advances at the
device's real rate, and no two crystals agree - so a model that only ever carries forward
creeps ahead by a few parts per million for as long as the session lasts. Left alone it
walks into the ceiling, and the ceiling resets to the clock, which is the one backwards
step this whole arrangement exists to avoid.

So when it is further ahead than a callback can account for, each block advances by a
shade LESS than its own length. Two per cent of a block is 53 microseconds at 128 frames
- inaudible on any single block, and it closes a whole callback of excess inside half a
second. The advance stays positive, so the stream stays monotonic while it converges.

## 15. `gb_send_controller()`

Back from a normalised parameter to the wire.
ON THE SAME CLOCK AS THE NOTES, and that is not a nicety. Half a stream stamped into the
future and half sent immediately is a stream that arrives out of ORDER - CoreMIDI delivers by
timestamp, so an "immediate" controller overtakes every note still waiting for its offset.
A bend that lands before the note it belongs to is heard as a glitch, not as a timing error.

## 16. `gb_observe_burst()`

WHAT THE HOST TAKES PER CALLBACK, NOT PER CALL - and getting this wrong was a glitch you could
hear.

The ring has to cover the largest single demand made on it, and that demand is a whole audio
callback. Ableton declares 512, calls with 128, and takes all four back to back. The retune
read "the largest block actually seen" as 128 and shrank the setpoint to suit, so the ring
held 400 frames against a callback that drained 512 of them. Every cycle ran it dry: 45
underruns and 44 resyncs in two seconds on a KRONOS at a 64 frame device buffer, one audible
click each. It survived a 512 frame device buffer only because the setpoint then came out at
960 by accident, which is more than one callback - which is why this never showed up until
someone ran a small buffer.

The DECLARED maximum was right all along. What is measured here is not a correction to it so
much as a confirmation, and on a host that genuinely uses less than it declares - one block
per callback, smaller than declared - it still reclaims the difference.

CALLED BEFORE ANY EARLY RETURN, unlike gb_observe_block(self). Sitting inside that one, it saw
nothing at all while the ring was resyncing - which is exactly when the numbers are needed,
and is why the first attempt at this fix changed nothing.

The boundary itself is decided in gb_block_host_time(self); this only adds up what fell inside one.

## 17. in `gb_observe_burst()`

CLAMPED TO ONE CALLBACK. burstFrames only resets when a cycle boundary is detected, so a
boundary that is missed accumulates without limit - and this figure feeds the reported
latency. Nothing can legitimately have handed over more than a callback's worth before
the current call, by definition of what a callback is.

## 18. in `gb_observe_burst()`

A CEILING, because a detection that misfires would otherwise stick for the session.
Nothing legitimately takes more than a few of its declared blocks per callback, and
sizing a ring for a figure this side of that costs a few milliseconds where trusting
a runaway one costs seconds.

## 19. in `gb_observe_block()`

A RING TOO SMALL IS FIXED AT ONCE, AND NOT ONLY DOWNWARDS.

The retune below exists to RECLAIM frames once the host has been watched, and it is
deliberately slow and one-way about it. This is the opposite case and it cannot wait: a
ring sized for less than one callback is drained dry every cycle, which is an audible
click each time - and there was no path to it except gb_revert_retune(self), which only fires
AFTER an underrun has already been heard.

It reaches here because neither number available at the first open is right. CT's Live
DECLARES 256 and hands over 512 in one callback; the declaration under-states, and the
observation is still partial during a project load - which is the least undisturbed
moment there is and the one where a busy CPU splits a burst in two. Both said 256, the
ring came up at 560, and only switching the buffer away and back settled it at 880.

So: whenever the burst actually seen needs more ring than is in force, ask for it. Once
per increase, since observedMaxFrames only ever grows.

## 20. `gb_send_latency_changed()`

TELLING THE HOST ITS LATENCY CHANGED, which it will not ask about on its own.

A host reads getLatencySamples() once, shortly after activation, and caches it until told
otherwise. Now that a fresh instance opens nothing, that reading is always zero - so
selecting a device later left Ableton compensating for nothing at all, and every track fed by
the plug-in sat late by the whole buffer.

Only the CONTROLLER can say so: restartComponent lives on IComponentHandler, which the
processor never sees. So it goes over the same connection the status slot does.

## 21. `gb_capture_callback()`

MONO IS WIDENED HERE, on the way into the ring, rather than on the way out.

The ring, the resampler and the drift loop are all stereo, and keeping them that way means
one code path downstream instead of a channel count threaded through every one of them. The
cost is resampling a duplicated channel, which is a few hundred thousand multiplies a second
- nothing beside the clarity of not having a mono variant of the whole chain.

## 22. `gb_start_worker()`

---- device life cycle, worker thread only -------------------------------------------------

The device is opened IN PROCESS for now. The feeder process this eventually wants - spawned
with posix_spawn so it inherits the host's microphone consent - is a robustness and sharing
move, not a correctness one, and splitting it out before there is a working plug-in to feed
means debugging IPC and DAW hosting at the same time with no baseline.

Everything here runs on the worker, never on the audio thread and never on the host's UI
thread. Opening a Core Audio device allocates, talks to a driver and can block for tens of
milliseconds; process() only ever asks for a change and carries on.

## 23. in `gb_worker_loop()`

SETTLE BEFORE ACTING - see GB_DEVICE_SETTLE_MS. Waiting here rather than in the
parameter handler keeps every route to a device change on one path: the panel's
arrows, the host's generic control and an automation lane all arrive as
gb_request_device(self), and all of them get coalesced by the same clock.

deviceDirty is not consumed until the wait is over, so a request arriving mid-wait
simply pushes the deadline out rather than being lost or acted on twice.

## 24. in `gb_worker_loop()`

SETTLE THE OFFSET BEFORE ACTING ON IT, the same shape as the device debounce above and
for the same reason: a burst of clicks should cost the host one delay-compensation
pass, not one per click. offsetDirty is not consumed until the wait is over, so a
click arriving mid-wait pushes the deadline out instead of being lost.

## 25. in `gb_worker_loop()`

THE MEASURED PIPELINE HAS MOVED FAR ENOUGH TO SAY SO. The reported figure follows a
live average now rather than a constant, so something has to notice when it has
drifted past the deadband - a device swap and a ring that has finally settled both
land here. The audio thread raises the flag; this is the only place that acts on it.

## 26. in `gb_worker_loop()`

WHERE THE NUMBER COMES FROM, every time it changes. A total on its own starts an
argument that only the breakdown can settle - "43.5 ms" is a ring, a device
buffer, a resampler and a measured round trip, and which of them is the big one
decides what to do about it. Twice now a figure has been questioned and the
components were only on the panel, where they cannot be pasted into a message.

## 27. in `gb_worker_loop()`

BEFORE gb_send_latency_changed(self), deliberately. That call is what makes the host
reactivate us and reopen the device, and gb_reconfigure(self) decides on reopen whether
to re-seed the offset. Writing the new value into the table first means the pair
is already up to date by the time anything looks at it.

## 28. in `gb_reconfigure()`

THE PARAMETER IS THE ONLY SELECTOR. It used to be one of two, with a saved UID as the
other, and they disagreed: the panel drew the parameter while the processor had opened
whatever the UID named, so the header said it was capturing a QU-24 while the device row
said Analog Keys and the audio was a Kronos. Three answers, all sincerely held.

The UID is still stored, and still keys the per-device settings - it is simply no longer
allowed to decide WHICH device. One selector, one answer, and the panel cannot be wrong
about it.
THE SAVED DEVICE FIRST while a restore is still pending, whatever slot the parameter
names. Resolving it also puts the parameter right, so the panel and the host stop
disagreeing with what is actually open.

## 29. in `gb_reconfigure()`

NOTHING IS OPENED UNTIL SOMETHING HAS ACTUALLY BEEN CHOSEN.

Every previous version of this opened SOMETHING on a fresh instance - first a hard-coded
Kronos, then whatever sat at slot 0 - on the reasoning that a silent plug-in looks broken.
That reasoning was wrong, and expensively so. Slot 0 on this machine is an iPhone
Continuity microphone, so loading a set woke it once per instance, synchronously, on the
host's main thread during load. Ableton stopped starting.

A plug-in has no business seizing capture hardware nobody asked it to. Idle until chosen
is both safer and more honest, and the panel says "no device selected" rather than naming
something the user never picked.

## 30. `gb_resolve_slot()`

Slot to device, skipping anything with no inputs - the same filter, in the same order, that
the editor and the controller use. One function so the three cannot drift apart again.
SLOT 0 IS "NONE", and every real device sits one higher.

Before this there was no way to express "nothing", which made two different states share one
value: a fresh instance that had chosen nothing and an instance that had chosen the first
device both read 0. The panel showed that as the first device's name while its own header said
"no device selected" - the plug-in contradicting itself in two lines of the same window - and
there was no way back to nothing once a device had been picked.

Devices moved UP by one rather than "None" being bolted on at the end, because 0 is the value
a VST3 parameter defaults to and the value a host restores when a project names no device. It
costs nothing on load: setComponentState() resolves a saved project by its device UID and
recomputes the parameter from that, so a saved set still reopens on the device it named. What
does shift is a recorded AUTOMATION lane of the device parameter, which would now name the
device one place earlier - accepted deliberately at v0.1.0 for a setup control nobody
automates.

## 31. `gb_publish_measurement()`

No configLock here, and it must stay that way - it is called from the worker's idle path and
from gb_store_measurement(self) after that has released the lock. So it publishes only what the
snapshot already holds; the snapshot itself is refreshed by whoever changed the config, under
the lock they were already holding.

## 32. in `gb_open_capture_locked()`

CLAMP THE CHANNEL REQUEST TO WHAT THE DEVICE ACTUALLY HAS, rather than letting the open
fail.

The channel settings persist per device, but the PARAMETER is global to the instance - so
selecting a 32 input desk, choosing channels 17/18, then switching to a two-channel synth
asks for channels that do not exist. device_open() refused, nothing opened, and the plug-in
appeared stuck on the last device big enough to satisfy the request. Silently refusing to
change device is a far worse answer than capturing the nearest thing that exists and
showing what happened.

## 33. in `gb_open_capture_locked()`

SAY SO IF THE REQUEST WAS TRIMMED. The clamp above is the right behaviour - capturing the
nearest thing that exists beats refusing to open - but on its own it left the panel and
the host showing a channel the device does not have while a different one was being
captured. The editor's arrows cannot reach an impossible value any more; a DEVICE CHANGE
still can, because the parameter is global to the instance while the channel setting is
per device. Same message the device slot already uses on the same kind of mismatch.

## 34. in `gb_open_capture_locked()`

LEAVE A DEVICE SOMEBODY ELSE IS DRIVING ALONE.

Rate and buffer size are global to the device, so setting either reaches into every other
client of it - the host included. The default has always been to touch neither, but that
protection ended the moment a size was picked in the panel, and the case where it matters
most is the easiest to walk into: a mixer serving as the host's OWN output and as this
plug-in's capture source. There the device's buffer frame size IS the host's block size,
so imposing one is the plug-in setting its own process() call rate on hardware it does not
own - and the smaller the size, the harder every subsequent device change becomes.

Probed BEFORE device_open() below, so what it reports is other clients and never our own
stream. Reported rather than silently obeyed: the panel setting is still whatever the user
chose, and the log says why the device did not take it.
OUR OWN GHOST FIRST. CoreAudio tears an IOProc down asynchronously, so a device we closed
moments ago can still report that it is running - and the probe below reads that as
"another client has it" and declines to touch the buffer.

That is the whole of CT's "I'm attempting to set 64 and getting 512" on project load. The
sequence is: the device opens before the saved buffer size is known, so it comes up at
whatever it was - 512; the restored state then arrives and asks for 64; gb_reconfigure(self)
closes the stream and immediately probes; the probe sees the stream it just closed; the
set is skipped; and 512 stands for the session. Setting 64 by hand later works because by
then the ghost is long gone - which is exactly why retrying "fixed" it, and why the
device is plainly not really shared.

Bounded, so a device that is genuinely being driven by someone else still reaches the
shared path below after a fifth of a second rather than being waited on for ever.
120 ms, not longer, and the reason is the lock this runs under - see the todo about
gb_reconfigure(self)'s scope. Every millisecond spent here is a millisecond the host's main
thread can be blocked in getState().

## 35. in `gb_open_capture_locked()`

The result MATTERS now that it is confirmed rather than assumed - see the note on
device_set_buffer_frames(). A false here is a device that had the size asked of it,
took the call and never changed, which is what "something else already has it open"
looks like from this side.

## 36. in `gb_open_capture_locked()`

SAID OUT LOUD WHEN IT IS NOT WHAT WAS ASKED FOR. Everything downstream uses the real
size - the ring's floor and the device latency both - so the arithmetic was never wrong;
what was missing was anyone being told, and a device silently running eight times the
requested buffer is most of a plug-in's reported latency.

## 37. in `gb_open_capture_locked()`

TWO QUITE DIFFERENT CAUSES, and the range tells them apart. If the size asked for is
inside what the device says it supports, the driver took the call and ignored it -
which is what a device ALREADY OPEN by something else does, because the buffer belongs
to whoever opened it first. If it is outside, the driver simply cannot do it.

## 38. in `gb_open_capture_locked()`

AND IF THE OTHER HOLDER IS ONE OF US, SAY SO BY NAME. A host loads every plug-in
into one process, so the status block this instance publishes into is the same array
every other GenBridge in the session publishes into - which makes "something else has
it open" answerable rather than a shrug. Two instances pointed at one device is an
ordinary thing to end up with, and the second one silently inherits the first one's
buffer.

## 39. in `gb_open_capture_locked()`

THE DECLARATION IS THE FLOOR, AND THE OBSERVATION ONLY EVER RAISES IT HERE.

This used to trust the observation outright, which is wrong at the FIRST open: the burst
measurement needs a whole undisturbed callback to be right, and a project load is the
least undisturbed moment there is. A busy load separates back-to-back calls by more than
half a block, the boundary test reads that as two cycles, and the burst comes out half
what it really is.

Measured by CT: an Analog Rytm at a 64 frame device buffer came up with setpoint 560,
which is 1.25 * (256 + 64 + 128) - a 256 frame callback. Switching the buffer to 128 and
back settled it at 880, which is the same arithmetic on the true 512. Only the first
value was ever wrong, and it was wrong in the direction that underruns.

maxSamplesPerBlock is a contract and is safe to size for; the retune still reclaims the
difference once it has watched for a couple of seconds and can be believed.
ONCE THE OBSERVATION HAS SETTLED, it is the better number and is used as it stands - that
is the whole point of the retune, and a reopen must not throw the reclaimed frames away.
While it is still WATCHING, it is a partial count and only ever raises the declaration.

## 40. in `gb_open_capture_locked()`

THE FLOOR IS ADVICE, NOT A LIMIT — for a setting the user typed. Auto still takes it, and
still adds the margin; an explicit target is now honoured as given, however low.

It is advice because the floor is deliberately conservative and cannot be otherwise: it
covers the WORST phase alignment between two unrelated clocks and the LARGEST block the
host says it may ever ask for, neither of which is what a given session actually does. A
number that pessimistic is worth showing and wrong to impose — clamping silently replaced
what the user asked for with a figure they could not see, which reads as the control not
working.

Safe to allow because the failure is bounded and visible. ring_read() hands the device
silence on an underrun and does NOT advance the read cursor, so the loop still sees the
true depth and pulls to refill; nothing is corrupted and nothing runs away. The panel
already shows fill/setpoint, underruns and resyncs, so too tight a setting reports itself
in the one place the user is looking while they choose it.

## 41. in `gb_open_capture_locked()`

COUNTERS BELONG TO THIS OPEN, NOT TO THE INSTANCE. A buffer or rate change tears the
device down and rebuilds it, and the rebuild resyncs by design - so carrying the old
totals over reports a fault that was actually a setting being changed. Worse, it makes a
latency measurement refuse itself, because it checks those same counters to decide
whether the capture was clean.

## 42. in `gb_open_capture_locked()`

ONLY WHEN THE PAIR HAS ACTUALLY CHANGED - see offsetUid/offsetDest for why. A reopen
of the same device must leave the live correction alone, or applying it would undo
it. A genuinely different device gets that device's own stored value, or zero if it
has never been measured: a correction belongs to the rig it was taken from, and
carrying it to another one is a whole round trip of error, not a small trim.

## 43. in `gb_open_capture_locked()`

THE OBSERVATION SURVIVES A REOPEN, and this is what stops the retune eating itself.

Telling the host its latency changed makes it deactivate and reactivate the plug-in, which
reopens the device. Resetting the observation there meant: open wide, watch two seconds,
retune, tell the host, get reactivated, open wide again - for ever, with the device torn
down and rebuilt every few seconds and the audio in pieces throughout.

The real block size is a property of the HOST, not of the device, so once known it stays
known and the correct setpoint is used from the first frame. Nothing then changes after
activation, so nothing asks the host to restart anything.

## 44. in `gb_open_capture_locked()`

NOT ARMED FOR A MANUAL SETPOINT. Retuning exists to claw back latency the host's declared
block size overstates, and it does that by REPLACING the setpoint. Against a figure the
user typed that is not a saving, it is the plug-in overruling them a couple of seconds
after they set it — the same silent overrule the clamp above used to do, arriving late.

## 45. in `gb_close_capture_locked()`

HAND THE DEVICE BACK AS WE FOUND IT.

Rate and buffer size are global to the device and nothing put them back, so a device this
plug-in had merely PASSED THROUGH was left reconfigured for everything else on the machine
- including, when it is the host's own device, the host. Switching away from it therefore
did not release it in any meaningful sense.

AFTER device_close(), so our own stream is already gone and the device is not being
reconfigured underneath a running IOProc of ours. The rate is set without waiting for
confirmation: we are on our way out, nothing here depends on it having landed, and the
wait costs up to 800 ms of the caller's time - which on this path is a device change the
user is waiting on.
The snapshot describes a device that is now gone. Cleared here so nothing reports the
latency of a bridge that is no longer running.

## 46. in `gb_close_capture_locked()`

NOT WHEN WE ARE ABOUT TO REOPEN THE SAME DEVICE. gb_reconfigure(self) closes and reopens, and
handing the buffer back to what it was in between means every reconfigure drives the
device 64 -> 512 -> 64 for no one's benefit. On a USB interface each of those costs over
a second inside CoreAudio, and between the two the device really IS at 512 - which is what
gets read off the panel and reported as "I set 64 and I keep getting 512".

The RECORD is kept, so the eventual real close still hands the device back as it was
found. Only the pointless middle of a reopen is skipped.

## 47. in `gb_close_capture_locked()`

observedMaxFrames DELIBERATELY SURVIVES A CLOSE. The largest block the host has actually
asked for is a property of the HOST, not of the device being closed - so a device change,
or the reactivation that telling the host about a latency change itself provokes, must not
throw the observation away. Resetting it here is what made a reopen go straight back to
the conservative setpoint and re-learn from scratch, which vst3check catches as "a reopen
keeps the tuned setpoint". It is cleared in setupProcessing() instead, and only when the
host declares a different maximum.

## 48. `gb_minimum_setpoint()`

THE RING CANNOT GO TO ZERO, and it is worth being precise about why rather than treating it
as a tuning knob that happens to bottom out.

The device's callback and the host's process() are driven by different clocks and fire at
unrelated moments. In the worst phase alignment, process() is called immediately BEFORE the
device callback that would have supplied its samples - so the ring must already hold a whole
host block's worth of input, or that call underruns. It must also hold a device block, since
input arrives in whole blocks and nothing can be consumed from a block that is still being
filled. The filter needs its taps either side on top.

That floor is a property of block-based audio, not of this design: passing samples straight
through would mean a host block landing in a gap between device callbacks and getting
silence. It is also why hostMaxFrames is the host's MAXIMUM block size rather than its usual
one - the floor has to cover the largest block the host may ever ask for, even if it
normally asks for far less.

## 49. `gb_bridge_create()`

════════════════════════════════════════════════════════════════════════════
THE API THE WRAPPER CALLS

Everything above is the bridge talking to itself. What follows is the whole of its outside edge -
the calls gbPlugin.c makes, in the order a host makes them - and it is deliberately small. A block
becomes four calls, a saved state a block of bytes, and nothing in this file has ever heard of
either plug-in format.
════════════════════════════════════════════════════════════════════════════

## 50. `gb_bridge_set_active()`

ACTIVATION OPENS THE DEVICE SYNCHRONOUSLY, and that is the whole reason the host sees a sensible
latency figure.

A host asks getLatencySamples() shortly after activating a plug-in and then caches the answer; it
only asks again if told to, via IComponentHandler::restartComponent. Opening the device on the
worker meant latency was still 0 when Ableton asked, and it reported zero latency for ever after -
while a test harness that polls until it settles saw the real 2228 and looked perfectly healthy.
Both were right, which is what made it worth writing down.

setActive is not the audio thread, and it is where a plug-in is expected to do its expensive
set-up, so a blocking device open belongs here. The worker stays for CHANGES made while running,
which is where doing it asynchronously actually matters.

## 51. `gb_bridge_setup_processing()`

WHETHER THE HOST INTENDS TO RUN US FASTER THAN REALTIME, which it tells us here and nowhere else.
There is no reciprocal call - a plug-in cannot demand realtime, it can only declare OnlyRT in its
class subcategories and find out here whether that was honoured. Logged for exactly that reason:
it is the only evidence of what a host decided.

A bounce in offline mode cannot work. The ring is filled by a device running at one second per
second, so a host consuming it faster simply drains it, and the render comes out silent or in
pieces. Nothing in here can fix that; the flag exists so the panel can say so afterwards rather
than leaving a silent bounce to be puzzled over.

## 52. in `gb_bridge_setup_processing()`

UNDER THE LOCK. hostRate, hostMaxFrames, observedMaxFrames and observedFrames are all read by
the worker while it holds this - gb_minimum_setpoint_for() is built on the first two and
gb_retune() on the second two - and the worker is running by the time a host calls this. VST3
guarantees the AUDIO thread is stopped here, which is why the same fields are safe to touch
from gb_observe_block(); it guarantees nothing about a thread of the plug-in's own.

## 53. in `gb_bridge_setup_processing()`

A DIFFERENT DECLARED MAXIMUM INVALIDATES THE OBSERVATION, and nothing else does. What was
learned about one host block size says nothing about another, so this is the one place that
forgets it - see gb_close_capture_locked(), which used to.
NOT ON THE FIRST CALL, which would throw away a callback size just restored from the project -
setState() and setupProcessing() arrive in whichever order the host likes, and this used to
clear a good value simply for being the first to see a block size at all. A LATER change of
declared size is a genuine reconfiguration and does discard it.

## 54. in `gb_bridge_latency()`

THE PUBLISHED FIGURE, not a fresh computation. A host may call this at any time on any
thread; recomputing meant reading setpointFrames, deviceLatency and nominalRatio - all plain
fields the worker rewrites under configLock during a device swap - so the answer could be
assembled from a half-updated set, and nominalRatio is a DIVISOR: observed as 0 mid-swap it
yields inf or NaN, handed straight to the host as a latency.

## 55. in `gb_bridge_block_begin()`

BEFORE ANYTHING THAT SENDS, and once. Every MIDI byte this block produces - notes,
controllers, the measurement's own note and its panic - is stamped from this one instant, so
the whole stream stays in order however the host chops its blocks up.

It is deliberately NOT the moment the block is heard - the host's output latency sits in
between - but that term cancels here and does not belong in the compensation: the audio comes
back through a ring read at the top of the same call, so both ends of the round trip are
anchored to the same clock and the difference between them is free of it.

## 56. in `gb_bridge_block_begin()`

ONLY WHILE THERE IS AUDIO. Blocks handed over before anything opens say nothing about what the
ring will have to cover, and a host - or a checker - that drives a burst of them unpaced while
nothing is open would leave a fictitious cycle length behind that the retune then treats as
settled for the rest of the session.

BUT "AUDIO" IS NOT "A DEVICE", and testing self->running here cost 2.0 ms in the host-input
mode (CT, 2026-09-09: measured 19.8, correct at 17.8). What the burst observation measures is
how the HOST delivers blocks - its own property, as observedMaxFrames' note says - and
gb_mean_callback_lead() is built on it. With no device open the observation never ran, the
lead came out 0, and the constant bias it exists to remove went straight into every
measurement. The comment on that subtraction in gbMeasure.c describes the same symptom on the
same synth: "both a KRONOS and an Analog Rytm recorded 4 ms early on exactly that".

## 57. in `gb_bridge_parameter()`

THE FIRST ONE AFTER A RESTORE IS THE HOST'S, NOT THE USER'S, and that
distinction is the whole fix. A host saves this parameter as a SLOT INDEX, and
an index is a position in a list that changes shape the moment a device is
unplugged - so the value restored with the project names whatever has moved
into that position, which on this machine is a Continuity microphone. Honour
the saved UID for that first value and let every later change through: a
later change can only have come from the user or from automation, and both
are deliberate.

## 58. in `gb_bridge_parameter()`

NO SHARED BUFFER. This used to snprintf the destination's name into a member
char array - on the AUDIO THREAD - which getState() then read from the host's
thread and gb_parse_state(self) wrote from it. The name is derivable from the atomic
above wherever it is actually wanted (gb_current_midi_name(self)), so the buffer that
was being raced over simply does not need to exist.

## 59. in `gb_bridge_parameter()`

The correction is part of the reported figure, so the host has to be told - but
NOT on this click. See GB_OFFSET_SETTLE_MS: the worker waits for the value to stop
moving and then tells it once, because each telling costs the host a full delay
compensation pass and this control is dialled in a dozen clicks at a time.

Nothing is computed here. What the latency becomes depends on state the worker
owns, and this runs on the audio thread before process() has even taken its
trylock, so the whole decision belongs on the other side of the queue.

## 60. in `gb_bridge_parameter()`

THE WORKER DOES THE REST. Going to Host input has to CLOSE whatever device is open -
leaving it running would hold hardware nobody is listening to - and coming back has to
open one again. Both are gb_reconfigure()'s job, and it is debounced like every other
route to it.

## 61. in `gb_bridge_measure_trigger()`

TRIGGER FROM ANY POINT, BUT ARM FROM THE LAST ONE. Both halves matter and they are not the
same question: the press has to be found wherever it lands in the block, while whether the
button is still DOWN is whatever it settled at. Arming from "was there a press" latched the
flag true - the editor's own release arrives in the same block - so the button worked exactly
once and then never again.

## 62. in `gb_bridge_measure_trigger()`

NOT LOGGED FROM HERE. synthlib_log_line() opens and closes the file on every call, and this is
the audio thread - three syscalls in the middle of a 2.7 ms block, at the exact moment a
measurement is about to start. It showed up as a resync during the run and the run then
discarded itself for not being clean: a measurement failing because of the act of
measuring. The worker logs it.

## 63. `gb_bridge_note()`

THE DAW PLAYS THE HARDWARE. Note data arrives as VST3 events and leaves as MIDI bytes on a
CoreMIDI destination; the audio comes back through the same capture path the effect uses. That
round trip is what makes this an instrument rather than a recorder.

Sent straight from the audio thread, not queued. MIDISend is not strictly real-time safe, but the
alternative - handing the bytes to another thread - adds exactly the jitter that makes a hardware
synth feel loose, and every plug-in that drives external gear makes the same trade.

## 64. in `gb_bridge_note()`

STAMPED WITH ITS OWN OFFSET, not sent on arrival. See gb_midi_send_at(): a block's worth of
notes fired at the block boundary is a bias of half a buffer, always early, and it grows with
every increase in the host's buffer size.

COUNTED, BOTH SIDES. "Notes are not getting through" has three quite different causes - the
host is not delivering them, the plug-in is dropping them, or the send is failing - and from
outside they look identical. The panel shows in/out, so one glance says which half to look at.

## 65. `gb_render_from_host()`

── Capturing from the HOST rather than from a device ───────────────────────

The External-Instrument mode: Live's own interface input arrives on the plug-in's side-chain, and
all this has to do is hand it back. No ring, no resampler, no drift loop - the host's input and
its output are the same clock, and reconciling two clocks is the only reason any of that exists.

SILENCE IS THE HONEST ANSWER when the host has routed nothing. A user who has chosen Host input
and connected nothing should hear nothing, not whatever a device left in the ring.

## 66. in `gb_bridge_render()`

TRYLOCK, NEVER LOCK. The worker holds this while it tears down and rebuilds the ring,
the resampler and the device - during which none of them may be touched. Blocking here
would stall the host's audio thread on a CoreAudio device open, which is exactly the
kind of thing that makes a DAW drop out. Failing to acquire it means a device change is
in flight, and a block of silence is the right answer.

## 67. in `gb_bridge_render()`

WITH the callback lead, unlike the measurement's own subtraction. This figure
answers "how far behind the audio is where the host finally puts it", and the
frames already handed over inside this callback are part of that distance. The
measurement subtracts a round trip that is invariant to them - see the note on
gb_latency_frames_measured(self).

## 68. in `gb_bridge_render()`

BOUNDED BY THE ESTIMATE IT REPLACED, and this is not belt and braces.

A measured figure can be wrong in ways a constant cannot. The ring's occupancy is
read live, the frames-so-far-this-callback depend on a cycle boundary being
detected correctly, and either can be disturbed - a mis-detected boundary
accumulates a "callback" of arbitrary length, and a ring that has just been
primed or snapped reads full. The result reached a host as 41 ms of plug-in
latency where the setpoint says 20, which is far worse than the 4-9 ms error this
whole exercise set out to remove.

The pipeline genuinely cannot sit more than about one callback either side of
what the setpoint implies - the drift loop holds it there and the burst explains
the rest - so anything outside that band is a fault, not a measurement.

## 69. in `gb_bridge_render()`

NOTHING IS TOLD TO THE HOST FROM HERE ANY MORE. This used to raise a flag when
the measured pipeline drifted past the deadband, which is what drove the
reconfigure loop above: a latency change is an instruction to the host to
reactivate us. The measurement is for the panel and the log now, and the figure
the host is given changes only when something structural does - a device, a rate,
a buffer, a retune, the trim.
