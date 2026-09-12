# gbPlugin.c notes

The longer comments from `gbPlugin.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. file scope

WHAT GENBRIDGE IS, AS FAR AS A PLUG-IN FORMAT NEEDS TO KNOW - and nothing about any format.

This is what gbVst3.cpp and gbEditor.mm used to be, less every line that was VST3. The bridge
(gbBridge.h) was already the plug-in with no VST3 in it; this file describes it to SynthLib's
wrappers, and `./do-plugin` builds GenBridge.vst3 and GenBridge.component from the same objects.
The COM plumbing, the controller, the factory and the IPlugView all live in SynthLib/plugin/ now,
shared with G2 Alike.

TWO VARIANTS, ONE BINARY: the effect and the instrument, exactly as the VST3 registered them -
same class ids, so a project saved against the old wrapper finds the same plug-in.

EVERY INSTANCE IS ITS OWN. Nothing here is file-scope except the descriptors: two GenBridges on two
tracks share no state, and whatever the plug-in tells the host names the instance it is about.

## 2. `gEffectProcessorUid`

THE FOUR CLASS IDS gbVst3.cpp DECLARED AS FUIDs, written out as the bytes they always were. A VST3
FUID built from four uint32s lays each one out big-endian on every platform but Windows, so
FUID(0x4A1C8E52, ...) is the bytes 4A 1C 8E 52 ... - the identical plug-in to a host that already
knows it. THESE MAY NEVER CHANGE: a project saved against them would reopen with an empty slot.

## 3. `gb_on_bridge_message()`

THE BRIDGE'S MESSAGES, which used to cross IConnectionPoint to the controller and are now said to
the wrapper directly. Almost all of them are the same thing - the plug-in has changed one of its own
parameters (a device that could not give the channel asked for, a restored project naming a
different slot) and the host must be told, or its copy of the value goes on disagreeing with ours.

ANY THREAD - the worker sends most of these. synthlib_plugin_param_edited() and _latency_changed()
post themselves to the main thread, which is where both formats want to hear about it.

## 4. `gb_param_info_cb()`

gbParams.c's description, in SynthLib's terms. NOTHING HERE IS SAVED BY THE WRAPPER: the bridge's
own blob already holds every setting, keyed by device - a device picker's INDEX means nothing
against a device list of a different shape, which is the whole of the microphone bug - and the
pass-throughs are events. See gb_state_params() for how the panel gets them back.

## 5. in `gb_param_points()`

THE MEASURE BUTTON IS A PRESS, NOT A LEVEL. The editor raises it and drops it again at once, a
host may deliver both in one block, and reading only the last point sees nothing but the
release - so the press is looked for among all of them, and whether it is still HELD is what
the last one says. See gb_bridge_measure_trigger().

## 6. `gb_state_params()`

WHAT A SAVED BLOB SAYS THE PARAMETERS ARE, with no instance to load it into - what the old
controller's setComponentState() did, moved where both formats can use it. A VST3 host hands the
processor's state to the controller precisely so the panel agrees with the capture; ignoring it is
how two tracks saved with a Kronos and a Helix both reopened showing Analog Keys.

A DEVICE IS RESTORED BY ITS UID, NOT ITS SLOT, since a slot is a position in a list that changes
shape whenever something is plugged in. gb_state_parse_active() resolves the UID; this turns it into
the slot it occupies today.

## 7. `gb_probe_host_input()`

THE SIDE-CHAIN PROBE (2026-09-09). Does anything actually arrive on the instrument's aux input under
a real host? The External-Instrument mode rests on the answer, and nothing in the code can give it -
so the shape of what was handed over is logged the first time and again whenever it changes. Once,
and on change: a line per block would be a file nobody can read and would perturb the timing this
plug-in measures.

## 8. `gb_sync()`

THE PANEL: SynthLib's shared view (synthlibPanelView.m) drawing gbDraw.c. What follows is all that
is GenBridge's about it - which draw calls, and what "this editor's state" means.

EVERY FRAME, BEFORE ANYTHING IS DRAWN OR HIT-TESTED: this editor's instance's status slot and values
into the draw layer, which keeps both file-scope. Pushing them only on a change let whichever of two
open editors pushed last speak for both.

THE HOST'S VALUES, not the bridge's - synthlib_plugin_param_value() answers with what the host's own
panel shows, which on VST3 is the controller's copy and moves the moment an edit is made.

WHICH VARIANT comes from the panel table rather than the instance, so it is right even for a view
with no instance behind it (see gb_create_view()): the hit test only offers Measure and Offset on
the instrument.
