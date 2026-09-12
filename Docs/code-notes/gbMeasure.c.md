# gbMeasure.c notes

The longer comments from `gbMeasure.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. file scope

MEASURING THE ROUND TRIP: play a note, time how long until anything comes back, and remember it.

The whole point is that a DAW cannot compensate for a delay it does not know about. Without this,
a part played through the instrument records roughly ninety milliseconds behind the beat on the
rig this was built against, and no amount of buffer tuning touches it because most of it is the
hardware.

WHAT IS STORED IS THE HARDWARE'S SHARE, NOT THE TOTAL, and that distinction is what makes it
correct under a host. The measured onset includes the plug-in's own path, which the host is
ALREADY compensating for because getLatencySamples() reported it. Storing the total and then
reporting it would count our part twice - and worse, the stored figure would silently go wrong the
moment the ring was retuned. Subtracting our contribution at the moment of measurement leaves a
number that is purely the synth and the wire, which stays true whatever the buffer does afterwards.

IT CANNOT SEPARATE THE SYNTH FROM ITS PATCH. A slow pad crosses the threshold later than a piano,
and nothing measuring from outside can tell the difference. Hence the manual offset: the
measurement gets you within a few milliseconds and a person settles the rest.

## 2. `gb_start_measurement()`

---- latency measurement -------------------------------------------------------------------

Play a note, time how long until anything comes back, and remember it. The whole point is that
a DAW cannot compensate for a delay it does not know about: without this, a part played
through the instrument records roughly ninety milliseconds behind the beat on the rig this
was built against, and no amount of buffer tuning touches it because most of it is the
hardware.

WHAT IS STORED IS THE HARDWARE'S SHARE, NOT THE TOTAL, and that distinction is what makes it
correct under a host. The measured onset includes the plug-in's own path, which the host is
ALREADY compensating for because getLatencySamples() reported it. Storing the total and then
reporting it would count our part twice - and worse, the stored figure would silently go
wrong the moment the ring was retuned. Subtracting our contribution at the moment of
measurement leaves a number that is purely the synth and the wire, which stays true whatever
the buffer does afterwards.

IT CANNOT SEPARATE THE SYNTH FROM ITS PATCH. A slow pad crosses the threshold later than a
piano, and nothing measuring from outside can tell the difference. Hence the manual offset:
the measurement gets you within a few milliseconds and a person settles the rest.

## 3. in `gb_start_measurement()`

THE ALL-NOTES-OFF IS THE WORKER'S JOB, NOT THIS THREAD'S. It is 32 MIDISend calls - one
note-off and one All Notes Off on each of 16 channels - and every one of them is a mach
message to the MIDI server. Half a millisecond of a 2.7 ms block, spent on the audio
thread, at the one moment the ring must not be starved: it underran, the ring resynced,
and gb_store_measurement(self) then threw the run away for happening over a resync.

The settle phase exists precisely to let things go quiet, and it is 350 ms long - orders
of magnitude more than the worker needs to get to this.

## 4. `gb_send_all_notes_off()`

WORKER THREAD. Everything hanging on every channel we might have used, silenced: our own
test note is released explicitly and anything left by a previous attempt - or by playing -
goes with it.

Sent immediately rather than stamped, because this thread has no block timeline to stamp
against. A played note scheduled up to one block ahead could in principle be overtaken by it,
which is a stuck note; a panic pressed in the middle of playing is not a case worth carrying
machinery for, and the note that follows would clear it.

## 5. in `gb_run_measurement()`

QUIET, NOT MERELY ELAPSED - and it is what made four trips out of five disappear.

Between trips this phase is waiting for the note just played to DECAY, and 0.35 s is
nowhere near enough for a piano or a pad. The floor was then taken over a still
ringing note, the threshold came out eight times too high, the next note could not
cross it, and the trip timed out - which ends the run. Every measurement rested on
one reading and the averaging was decoration.

So: the settle ends when the input has actually gone quiet, with the old duration as
a MINIMUM and a hard ceiling so a noisy input cannot hang the run.

## 6. in `gb_run_measurement()`

Establish what silence looks like on this input before deciding what a note looks
like. A noisy preamp would otherwise register an onset immediately.

THE TRAILING PEAK, NOT THE PEAK OVER THE WHOLE WINDOW. A preset with reverb on it is
still decaying through this phase, so a peak held from the start of the window is a
measurement of the tail rather than of the floor - and the threshold is eight times
whatever this says. Too high a threshold does not fail loudly; it fires LATE into the
attack, which reads as a slower synth, which the host then over-compensates for, and
the take records early. A Kronos on a reverbed preset came out 5 ms adrift of an
Analog Rytm on a dry kick, both reporting the same figure.

Restarting the hold two thirds of the way through leaves the last third - the part
nearest the note, and the quietest part of any decay - as the answer.

## 7. in `gb_run_measurement()`

AT THE START OF THE NEXT BLOCK, which is the one instant here that is certain to
be in the FUTURE - nextBlockHostTime is this block's start plus its frames, and
the model never hands back a start earlier than the clock. CoreMIDI therefore
holds the packet and releases it exactly then, so the moment the note left is a
number this code knows rather than one it hopes for.

Stamping it at the block start instead put it in the past by however long the
block had taken to compute - gb_run_measurement(self) runs after the audio is rendered -
so it went out at once, at an instant nothing recorded. Tens of microseconds on
its own, but it was the reference EVERY later arithmetic was measured from.

## 8. in `gb_run_measurement()`

A very loud input would otherwise set a threshold no note could reach, and the
measurement would time out reporting "nothing came back" when the truth is "this input is
too noisy to measure". The ceiling turns that into a detection that at least tries, and
the logged floor tells the story afterwards.

## 9. in `gb_run_measurement()`

The FIRST block that crossed is the onset; the confirmation only decides whether to
believe it. Counting from the confirming block instead would add its duration to
every measurement.

TIMED ON THE CLOCK AT BOTH ENDS, not counted in blocks - and this is what stopped
the figure jumping about from run to run.

Counting elapsed FRAMES assumes blocks arrive evenly spaced, and under a host that
hands over four of them per audio callback they do not: they arrive in a burst and
then nothing for the rest of the cycle. So the answer depended on WHICH call of the
callback the note happened to be detected in - 0, 128, 256 or 384 frames of pure
artefact at a 512-frame cycle, which is up to 8 ms of jump between two runs measuring
the same unchanged synth.

Two real instants remove it: the note left at measureNoteTime, and this block's ring
read happened at blockActualHostTime. The RAW clock, deliberately - the model is
ahead of it inside a burst, and the ring holds only what physically arrived.

THE SAMPLE, NOT THE BLOCK, for the last part of it: which sample of this block first
crossed says how far into it the sound began - 0 to a full buffer, half of one on
average, and always SHORT if discarded. One extra pass over the block that crossed,
and only that one.

## 10. in `gb_run_measurement()`

OUR SHARE IS WORKED OUT HERE, AT THE ONSET, not two blocks later when the
confirmation arrives. It is built from three things read at one instant - the
fill, the clock, and when the last capture callback landed - and by the time the
confirmation comes in, the device has usually written again: lastWriteHostTime is
then AFTER the onset block, the "how long ago" term collapses to zero, and the
share comes out a whole device buffer short. It read as an 18 ms synth.

## 11. in `gb_run_measurement()`

gb_internal_latency(self), NOT gb_report_latency(self) - see the comment on that pair. Netting off
the reported figure would subtract the correction already in force as well as our own
buffering, so each run would return less than the last.

BUT FROM THE OCCUPANCY THE ONSET ACTUALLY CAME THROUGH, not from the setpoint. The
ring only sits AT its setpoint on average: a host that hands over four blocks per
audio callback drains it in a burst, so the fill at the fourth is a whole cycle below
the fill at the first, and which of them a note happened to land in moved the answer
by that much. That is where the run-to-run scatter came from. The setpoint remains
what the HOST is told, because that is the long-run figure; the measurement nets off
the real one, and the difference between the two is exactly the momentary deviation
it should not be reporting as hardware.
THE MEAN CALLBACK LEAD, ADDED AS A CONSTANT - which is the difference between the two
failed attempts at this and the right answer.

The round trip is invariant to WHICH call of a callback detects the onset, because
the position in the ring and that call's fill move together and cancel. That is why
adding the per-block burstBefore made it worse: it uncancelled them and put the
block's own 0..384 frames of variance straight into the answer.

But the AVERAGE of that term is not zero, and it is not in the subtraction at all -
so every measurement carried it as a constant bias. At a 512 frame callback delivered
in 128s that is (512 - 128) / 2 = 192 frames, 4 ms, and both a KRONOS and an Analog
Rytm recorded 4 ms early on exactly that. A constant has no variance, so this is the
one form of the term that removes the bias without reintroducing the scatter.

## 12. in `gb_run_measurement()`

NAMED IN THE LOG, because this term is invisible in the result and was wrong in the
host-input mode for a day. burst and call are what it is computed from: a host handing
over its whole callback in one block has no lead at all, and one splitting a 256 into
64s has (256 - 64) / 2 = 96 frames of it.

## 13. in `gb_run_measurement()`

A ROUND TRIP CANNOT BE FASTER THAN OUR OWN PIPELINE. If it looks like it was, the
onset is not the note - a threshold crossing on something else, or a ring that was
not at its setpoint when the arithmetic assumed it was. Reported rather than clamped
to zero: a silent 0 looked exactly like a device that had never been measured.

## 14. in `gb_run_measurement()`

THE TRIP IS ONLY KEPT IF THE CAPTURE WAS CLEAN THROUGH IT. This used to be judged
over the whole run and thrown away wholesale - one resync anywhere and every note was
discarded, which on a rig that resyncs at all means no measurement is ever possible.
Per trip, a disturbed one simply does not vote.

## 15. in `gb_store_measurement()`

A RESYNC OR AN UNDERRUN DURING THE MEASUREMENT INVALIDATES IT. Either one means the ring
was snapped or starved while we were counting, so the onset moved by however long the
disturbance lasted - and the resulting figure is a measurement of the glitch, not of the
hardware. Better to say so and let it be repeated than to store a number that looks
authoritative and is not.

## 16. in `gb_store_measurement()`

JUDGED PER TRIP NOW, not over the whole run. A resync anywhere used to throw away every
note in the run, so on a rig that resyncs at all no measurement was ever possible - and
the resync was often caused by the act of measuring. A disturbed trip simply does not
vote; the run is only refused when nothing clean survived.

## 17. in `gb_store_measurement()`

THE MEASUREMENT LANDS IN THE CORRECTION, which is the whole point of the arrangement: the
figure Measure produces is the one the panel then lets you nudge, rather than a number
sitting next to a separate trim that starts at zero.

Clamped, because offsetMs is a normalised VST3 parameter with a fixed range and a reading
outside it cannot be represented. Logged when that bites - a silently clamped round trip
would put every take in the wrong place with nothing on screen to say why.

## 18. in `gb_store_measurement()`

Underruns during the measurement matter: a gap in the capture delays the onset by
however long the gap was, so a figure taken while the ring was starving is not a
measurement of the hardware at all.
EVERY TRIP, NOT JUST THE SUMMARY. An average and a range still hide the shape: five
readings clustered with one wild outlier, and five spread evenly, produce the same two
numbers and mean quite different things. Safe to read here - the run is idle by the time
the worker gets to this, so the array is not being written.
