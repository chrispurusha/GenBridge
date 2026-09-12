# gbBridge.h notes

The longer comments from `gbBridge.h`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `tGbHostOps`

THE PLUG-IN, WITH NO VST3 IN IT.

Everything GenBridge actually does is here and in gbBridge.c / gbMeasure.c / gbState.c: opening a
device, filling a ring from it, resampling onto the host's clock, tracking drift, playing the
hardware, measuring its round trip, remembering the settings and reporting a latency. None of
that needs C++ and none of it needs the SDK.

What a PLUG-IN FORMAT needs is not here either. SynthLib's shared wrappers (SynthLib/plugin/) are
the VST3 and the Audio Unit, and gbPlugin.c describes this bridge to them: a block becomes the
four calls below, a parameter is what gbParams.c says, and the saved state is a block of bytes.
When a question is "what does this plug-in do", the answer is in these files; when it is "what does
a format require", it is in SynthLib.

The struct is OPAQUE on purpose. It carries C11 _Atomic members, which C++ cannot parse, and
keeping the definition in gbBridgePrivate.h is what lets the C side use the right tool without
the wrapper's compiler ever seeing it.

## 2. `tGbHostOps`

── What only the wrapper can do ────────────────────────────────────────────

Two things the bridge needs and cannot reach: telling the host its latency changed, and telling
the controller a value it did not choose. Both go through the CONTROLLER - restartComponent lives
on IComponentHandler, which a processor never sees, and the messages travel over the
IConnectionPoint the host wires between the two ends.

So the bridge posts, and the wrapper delivers. The ids are the ones the controller's notify()
switches on: "gbStatusSlot", "gbDeviceSlot", "gbMode", "gbFirstChannel", "gbOffset", "gbLatency".

## 3. `gb_bridge_block_begin()`

── One block, in four calls ────────────────────────────────────────────────

The order is the order process() must make them in. gb_bridge_block_begin() decides where this
block sits in wall time - which is NOT "now" on a host that hands over four blocks per audio
callback - and everything stamped afterwards uses the answer, so it comes first and once.

## 4. `gb_bridge_measure_trigger()`

THE MEASURE BUTTON IS NOT AN ORDINARY PARAMETER, and it needs both halves of what the host
delivered. sawPress is "a press appeared anywhere in this block", because the editor raises the
control and drops it again immediately and a host may deliver both points at once; held is what
it SETTLED at, which is what decides whether the button is still down. Arming from the first
latched it true and the button then worked exactly once.

## 5. `gb_bridge_render()`

Fill the host's block. Silence is a perfectly good answer and is what comes back while a device
change is in flight.

`in` IS THE HOST'S OWN INPUT, or NULL when it gave none. It is used in one mode and ignored in the
other: capturing from a DEVICE fills the block from the ring and never looks at it, while
capturing from the HOST passes it straight through - see GB_SOURCE_HOST. Passing it on every
block rather than latching it keeps the decision in one place and costs a pointer.

## 6. `gb_bridge_set_state()`

── The saved state ─────────────────────────────────────────────────────────

The device UID is stored in the project, NOT the audio. Reopening a session should pick up
whatever the named device is now, not a frozen copy of what it was - the same reasoning as
G2-Edit's plug-in storing a patch PATH.

## 7. `tGbActive`

── What the controller needs from a blob ───────────────────────────────────

A VST3 host hands the component's saved state to the CONTROLLER as well, precisely so the two can
agree on what was loaded - and a controller that ignores it comes up showing defaults. That is
what made two tracks, saved with a Kronos and a Helix, both reopen as Analog Keys: the UID was in
the file, but nothing told the panel about it.
