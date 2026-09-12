# gbStatus.h notes

The longer comments from `gbStatus.h`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `tGbStatus`

Live figures the processor publishes and the editor reads.

ONE OF THESE PER PLUG-IN INSTANCE, not one per process. It used to be a single global, and with
two instances in a set the editors read whichever processor wrote last - so a panel showing the
microphone reported that it was capturing a Kronos. Two answers, one of them a lie.

The processor claims a slot on construction and tells its controller which one through
IConnectionPoint, the channel VST3 provides for exactly this - the two are separate registered
classes precisely so a host MAY keep them apart, and nothing else bridges them.

Messages carry the slot number ONCE. The meters and drift figures are then read straight out of
this shared structure, because a message per frame per instance would be a great deal of
allocation on a UI timer for numbers that are only ever advisory.

Everything here is written by the audio or worker thread and read by the UI thread, so it is all
atomic and none of it is a pointer. The device name is the exception - a fixed buffer copied
under no lock at all, on the grounds that the worst case is a torn string in a readout that
refreshes thirty times a second.

## 2. file scope

The saved device is named in the project but is not plugged in. Distinct from "nothing
selected": one is a plug-in waiting for hardware it has been told to use, the other is one
that has never been told anything, and answering the first with the second is what let a
missing USB interface fall through to whatever sat at slot 0 - a microphone.

waitingName is written once, before the flag is raised, and read only while it is up, so it
needs no more protection than deviceName above.

## 3. file scope

WHAT THE PIPELINE'S DELAY ACTUALLY IS at this instant, against latencySamples which is what
the host was TOLD. The reported figure is built from the ring's setpoint because a latency
that moved every block would have the host redo delay compensation continuously; this is
built from the occupancy the audio really came through. If the two disagree on average, every
recording is displaced by the difference - see findings 2026-09-08 (5).

## 4. file scope

The device was already running for another client when it was opened, so its rate and buffer
size were LEFT ALONE deliberately - see the note in reconfigure(). Published because from the
panel that is indistinguishable from a device refusing the setting, and the two want quite
different responses from whoever is reading it.

## 5. file scope

IS THE HOST ACTUALLY SENDING ANYTHING? Only meaningful in the host-input mode, where the
difference between "routed and quiet" and "not routed at all" is invisible from the panel and
is exactly what a user hits first: a side-chain pointed at the plug-in's OWN track is a
feedback loop, so Live mutes it and the result is silence with nothing on screen to explain it.
1 while audio has arrived recently, 0 after about two seconds of nothing.
