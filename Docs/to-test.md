GenBridge TO TEST

Finished code that is built but not yet checked against a real host or real hardware.
Confirmed -> delete the line. Check failed -> move it to todo.md.

- ***VST3 AND AUDIO UNIT, ON SYNTHLIB'S SHARED WRAPPERS (2026-09-11)*** - gbVst3.cpp and gbEditor.mm
  are gone; vst3/gbPlugin.c describes the bridge to SynthLib/plugin/, and ./do-plugin builds
  GenBridge.vst3 and GenBridge.component (aufx GBfx CPur, aumu GBin CPur). Checked offline:
  tools/vst3check 91/91, auval clean on both components, both editors open in tools/vst3host and in
  G2-Edit's tools/auhost. STILL TO CHECK in Live: (1) an existing set reopens on the same devices, MIDI
  destination and offset - the class ids are unchanged and the saved state is byte-identical; (2) two
  instances on two tracks each show their OWN figures and their own values; (3) Measure arms and
  re-arms; (4) the editor comes back at the size each set left it; (5) pedal, bend and mod wheel
  reach the hardware on the right channel - pass-throughs now send EVERY point in a block rather
  than only the last. In Logic: the Audio Unit loads (sandboxSafe is false, so it may be hosted out
  of process), the instrument's side-chain can be fed, and a bounce reports "rendering offline".

- THE CALLBACK LEAD IN HOST-INPUT MODE (2026-09-09, predicted and unconfirmed). Re-measure the same
  synth in the same arrangement: the figure should now come back near 17.8 ms rather than 19.8, and
  the take should land with the trim at zero. /tmp/genbridge.log carries "measure: callback lead N
  frames (host burst B, this call C)" - B 256 and C 64 would confirm the arithmetic on the rig. If
  the figure is unchanged at 19.8, the lead is not the term and the 2.0 ms is something else.
- SUPERSEDED, kept for the reasoning - HOW EARLY, IN SAMPLES (2026-09-09). The External-Instrument mode measures 19.8 ms through a Kronos
  and the take lands slightly early - the double-count of Live's own input path that was predicted
  when the mode was built. Record a take against a click with the measured figure in force, read the
  offset in SAMPLES, and compare it against Live's buffer size. Equal means the term is identified
  and can be subtracted with a reason; not equal means the trim stays the answer and the figure is
  right as measured. This is the one measurement standing between the mode and being finished.
- MEASURE IN HOST-INPUT MODE (2026-09-09, second attempt - the first did nothing because the
  measurement tested "is a device open"). Press it with the synth routed in: the settle, floor and
  listen phases should run and a figure should come back. If it still does nothing, the log now has
  the reason - and if it says nothing at all, the trigger is not reaching the processor.
- THE ROUTING QUESTION THE TWO-TRACK SETUP RAISES: with a traditional External Instrument on the
  track feeding our side-chain, LIVE is compensating that track by its own Hardware Latency field
  while we measure through it - so the figure is the synth plus the interface MINUS whatever Live has
  already taken off. Set that field to 0 while measuring, or feed our side-chain from the hardware
  input directly, which is the arrangement the mode exists for.
- THE EXTERNAL-INSTRUMENT MODE, END TO END WITH A SYNTH (2026-09-09). Set Source to "Host input",
  route the interface input carrying the synth into the plug-in's side-chain, and play: audio should
  pass through with no device opened - the Device, Rate, Buffer, Mode and Input rows greyed, the
  header green and reading "external instrument". /tmp/genbridge.log should show a HOST INPUT line
  with signal PRESENT the moment audio flows.
- THEN PRESS MEASURE IN THAT MODE, which is the whole point of it: a figure should come back for the
  synth and the interface with no ring or converter of ours in it. Compare it against the same synth
  measured the old way (Source = an audio device, same interface): the two should differ by roughly
  the ring and the device buffer, and the host-input one should be the smaller.
- AND THE QUESTION THAT ONLY A RECORDING CAN ANSWER: whether the measured figure should be reported
  in FULL. The round trip in this mode includes the host's own input path, which Live may already
  compensate - so the correction may double-count it. Record a take against a click with the measured
  figure in force and look at where the transient sits; if it lands early by about Live's input
  buffer, that term has to come off. Live's "Keep Latency" setting is part of this question (see the
  2026-09-07 entry).
- THAT A SESSION IN THE MODE REOPENS IN IT: save with Source = Host input, reopen, and confirm the
  panel comes back on Host input with the device rows still greyed. The state carries a source=host
  key; a project saved before today has none and must come back on a device.
- THE SIDE-CHAIN PROBE IN LIVE (2026-09-09), which decides whether the External-Instrument mode is
  buildable at all. touch /tmp/genbridge-log, put the GenBridge INSTRUMENT on a MIDI track, and look
  first at whether Live offers the input at all - a side-chain / "Audio From" chooser on the
  device, naming an interface input. Route one in, play something audible into it, and read
  /tmp/genbridge.log for the HOST INPUT lines. Three outcomes, and they mean different things:
    numInputs 1, buffers yes, signal PRESENT  -> build the mode; the rest is passthrough and panel
    numInputs 1, buffers yes, signal none     -> Live routes but sends silence: check the routing,
                                                 then whether silenceFlags is set (it is logged)
    numInputs 0, buffers NO                   -> Live will not feed an instrument's aux bus. The
                                                 mode needs another shape - most likely the EFFECT
                                                 variant gaining the MIDI half rather than the
                                                 instrument gaining audio
  Also worth noting whether an activateBus line appears, and whether Live's own log complains about
  the bus layout: ~/Library/Preferences/Ableton/Live */Log.txt.
- THAT THE PROBE COSTS THE SHIPPING PLUG-IN NOTHING: the same session should behave exactly as
  before in every other respect - a device opens, audio flows, the measurement runs. The only new
  thing is a bus nothing is connected to, and one log line.
- THE C CONVERSION (2026-09-09), which is verified everywhere except in a host: vst3check is line
  for line what the C++ build produced, the editor screenshots are byte-identical and 35 of 50 moved
  functions normalise to the original exactly - but nothing has been loaded into Ableton. Open a
  saved session with two instances, confirm each opens its own device once, the panel figures move,
  the meters run, Measure produces a figure and the saved state comes back. See findings 2026-09-09.
- THAT THE SAVED FORMAT STILL ROUND-TRIPS THROUGH A REAL PROJECT, which is the one thing the
  conversion rewrote rather than moved: the blob parser and writer went from std::string to a byte
  walk. vst3check covers a v3 blob and the version-1 and -2 paths are UNEXERCISED - open a project
  saved by an older build if one still exists, and confirm the device, buffer and trim come back.
- Note timing vs host buffer size (the reason recorded parts crept earlier as the buffer grew), and the reordering that followed the first attempt at it: events are stamped with their own sampleOffset, blocks computed back to back in one callback are stamped a block apart rather than together, and CCs/bend/pressure go out on the same clock as the notes. Both halves have vst3check checks, so what needs a real host is the EAR: play repeated notes and a bend through a hardware synth at 128, 512 and 2048 and confirm nothing cuts, sticks or arrives before the note it belongs to. Then record a quantised clip at all three and confirm the transient sits in the same place
- Ring size at the FIRST open after a save: set the buffer, save, reload, and confirm from /tmp/genbridge.log that the ring opens straight at 880 with NO "retuned UP" line and no second reopen - the callback size is now saved with the project. Then change Live's buffer size and confirm it retunes to the new value rather than trusting the stale one
- THE 4 ms: record through both synths again with the measured figure in force and the trim at zero. The measurement no longer carries the mean callback lead as a bias, which on this rig moved an Analog Rytm from 12.5-13.5 ms to 8.9-9.9 - so a take that was 4 ms early should now land. If a residual remains it is the synth's own attack against the detector, which is what the trim is for
- Ring size on project load: confirm the Rytm now comes up at setpoint 880 for a 64 frame device buffer rather than 560, with no need to switch the buffer away and back, and that a retune still survives a reopen (the panel's ring figure should not jump back up after Live reactivates the plug-in)
- Project load with two instances: confirm from /tmp/genbridge.log that each device is opened ONCE at "frames 64" without a manual set, that "reopening the same device - leaving its buffer alone" appears instead of "restored device settings: 512 frames" between reconfigures, and that the panel never shows 512
- THE REGRESSION FIX: reload the project and confirm from /tmp/genbridge.log that there is now ONE reconfigure per instance rather than one every 1.5 seconds, that "latency now N samples" appears once and not repeatedly, and that the device stays at 64 instead of flapping to 512. The reported figure should sit around 1257 samples (26 ms) for the Rytm - ring 560 + device 64 + filter 32 + measured 601
- What in the Live session has the KRONOS running: most likely Live's own AUDIO INPUT DEVICE, which is chosen separately from its output, so a session on a QU-24 output can still hold the KRONOS as its input. Point Live's input elsewhere (or set Live's buffer to 64) and confirm GenBridge's panel then reads plain "64 samples" with reported latency around 1124 samples
- Buffer request confirmed rather than assumed: open the two-instance project and confirm the panel now shows "64 samples" with no "(device gave 512)" beside it, and that no manual set is needed. If the message still appears, it is now genuine contention rather than the async race - the log says which, and names the other GenBridge if that is what has it
- Spinning cursor: the panel no longer enumerates CoreAudio or CoreMIDI on a timer, only when a hot-plug notification says the list changed (with a 30 s belt-and-braces re-read). Leave a panel open in a busy session and confirm the beachball is gone, and separately that plugging a synth in or switching one on still updates both drop-downs within a second or two
- Ring underruns at a small device buffer - THE GLITCH. Reproduced outside Ableton on the KRONOS at buffer 64 with a four-block callback (45 underruns and 44 resyncs in two seconds) and zero after the fix, but that was vst3check driving the blocks. Run a real Ableton session at buffer 64 and watch the panel's under/resync counters stay at 0 while you play; then try 128 and 512 device buffers, and Live buffer sizes either side of 512
- Measure on a DRY percussive preset, and compare against a reverbed one. A decaying tail inflates the detector's floor, which fires it late into the attack and over-states the synth - that was 6 ms on a KRONOS (findings 2026-09-08 (5)). The floor is now taken from the last third of its window; confirm a reverbed preset now measures close to a dry one, and that "N of 5 trips used" in /tmp/genbridge.log reads 5 on a dry sound rather than 1
- THE ONE THAT MATTERS: record a quantised clip through both synths and confirm the transients land on the grid with the MEASURED figure in force and the trim at zero. The reported latency now follows a smoothed measurement of the pipeline instead of the ring's setpoint, which was 4-9 ms too big on a bursty host - that was the over-compensation. Watch "told" against "actual" on the panel while it runs: they should stay within a millisecond or two of each other
- Latency measurement repeatability: Measure now plays five notes, drops the extremes and averages the rest, and prints the range beside the figure. On the KRONOS three consecutive runs gave 9.73 / 9.94 / 9.73 ms with a per-run trip range around 1 ms. Confirm in Ableton that repeated presses agree, and that the range shown is the synth's own jitter rather than something moving underneath it - /tmp/genbridge.log has the per-run detail including "ring F of S"
- Test Note stepper on the Latency row: four arrows, [<<] an octave and [<] a semitone either way, C-2 to G8, note 0 named C-2 (middle C is C3). All four verified under vst3host (C3 -> C2, C3 -> C4, C3 -> B2). Set it to C-2 for an Analog Rytm and confirm Measure triggers a pad and returns a sensible figure; confirm the note survives a project save and reload, and that a host's generic panel shows the same name- Fine and coarse arrows on the In use row: [<<] [<] reading [>] [>>], 1 ms outer and 0.1 ms inner, snapping to a tenth. All four slots verified under vst3host, but confirm the hit areas feel right under a real pointer and that a coarse click on a measured (not round) figure lands on a tenth
- Absent saved device now waits instead of opening slot 0 - verified in vst3check against the plug-in's log, but NEVER on real hardware: open a project that uses the USB interface with it unplugged, confirm silence and a "waiting for <name>" header, then plug it in and confirm it connects on its own without touching a control
- Hot-plug watcher (device_watch_list, poc/device.c) - never exercised with a device actually appearing or vanishing; also confirm several GenBridge instances in one set all react
- Panel "waiting for <name>" header and the Device row showing the saved name - the code path was never rendered, since no absent-device state was reachable in vst3host
- Editor resize: bottom-edge drag should no longer fight back, and the CONTENTS should follow the window continuously rather than snapping at the end - the view-follows-parent half is proven (vst3host, parent grown with no onSize call), the no-implicit-animation half is not; needs a real live drag
- SynthLib CATransaction change affects G2-Edit and SynthEdit too, both of which resize a real window - check neither has picked up a redraw or scaling fault
- Whole window jumping to another size and back mid-drag: not seen since checkSizeConstraint() was made history-free (CT, 2026-09-01, "no longer seems to be oscillating"). Intermittent to begin with, so keep an eye on it - if it returns, touch /tmp/genbridge-log, reproduce, and read the [editor] lines in /tmp/genbridge.log
- Untried lead if it is still rough: gPresentInTransaction is TRUE for the plug-in and FALSE for G2-Edit, so only the plug-in blocks on waitUntilScheduled inside AppKit's drag loop - try the async present path, accepting that geometry and content then decouple
- attached() asking for the remembered size through plugFrame->resizeView() - does nothing in a host that honours getSize(), so it needs a host that does not
- Editor stops repainting when it cannot be seen (gbView.m): start/stop plumbing traced under vst3host, but the OCCLUSION TRANSITION itself was never scripted - minimise the plug-in window in a real host and watch Activity Monitor
- Editor size remembered across sessions - round-trips in vst3check, but hosts differ over when they call controller setState/getState and some manage window size themselves; needs a real Ableton session closed and reopened
- editorView use-after-free fix (editor_gone callback) - close path runs clean under vst3host, but the crash it prevents was in the status/parameter-notify paths after the editor closes; open the editor, close it, leave the set running and change a device
- Offset debounce (GB_OFFSET_SETTLE_MS, 400ms): the value should follow every click immediately while the HOST is told only once the clicking stops. Nudge the offset a dozen times quickly on an instrument with a device open and confirm the readout keeps up, the session does not hitch per click, and the correction has actually landed once you stop
- Drop-downs on ALL SEVEN rows (Device/Rate/Buffer/Mode/Input/MIDI Out/Channel), arrows only on the offset, and "None" as the default and as a selectable device: menu render, selection and the return to None are all proven under vst3host, but HOVER HIGHLIGHTING is not - vst3host's synthetic click posts no pointer motion, so the NSTrackingArea path has never run. Open the menu in a real host and confirm items light under the cursor, and that the highlight does not stick after the menu closes
- Selecting "None" on a RUNNING bridge should close the device and leave the panel showing None with no audio - resolve_slot(0) returns false so reconfigure() takes its existing close-and-stay-closed path, but vst3host never activates a processor so that half was never executed
- Menu scrolling (SynthLib contextMenu.c): open the instrument's MIDI Out list, confirm the chevron strips appear only in the direction there is more, that HOVERING an edge scrolls continuously (never exercised - vst3host's synthetic click posts no pointer motion), that clicking a strip pages, and that the last item is clickable once the bottom strip goes inactive
- vst3check now proves the instrument's MIDI path with NO hardware: it creates its own virtual CoreMIDI destination, points the plug-in at it by name, pushes a note event through process() and reads back what arrived (2026-09-08). Currently returns 90 40 66 / 80 40 00 - note on and off, right pitch, right velocity. If host MIDI ever appears not to reach the hardware, run this FIRST: it separates the plug-in's half from the host's routing, which is the half no test here can see.
