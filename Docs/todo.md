GenBridge TODO

Things to do. ONE LINE PER ITEM - keep it that way.
Measurements, reasoning and completed-work narrative go in findings.md, NOT here.
Built-but-unchecked work goes in to-test.md.

Shared with the sibling projects

- Need to be able to host multiple instances of this plugin on a DAW. Can we make sure we're OK on that? I've had the Kronos stop sending audio 'til I rebooted it, which happened after I added a track for TB03.
- Roland TB03 doesn't seem to allow round-trip latency measurement, but latency seems to be compensated for quite well via the Roland driver. If the measurement not possible scenario is correct, maybe we need to either warn or grey-out the measurement? TB03 does seem to be limited to 44.1kHz and doesn't seem to support 48kHz. Might be related to that? It supports 96kHz, but GenBridge breaks up when trying to deal with that rate.
- I'm not sure we're defaulting to stereo audio connection? We should be, if not.
- DONE 2026-09-09 for the three identical files (device, ring, stubs -> SynthLib/audio and SynthLib/plugin), and 2026-09-11 for the editor window: gbEditor.mm is gone, replaced by SynthLib's shared IPlugView and AU view; and the same day the panel view: gbView.m/msView.m (91% alike) became SynthLib/plugin/synthlibPanelView.m, driven by a per-project tSynthLibPanel in gbPlugin.c
- gbStatus and msStatus share the slot mechanism and differ only in the payload struct: worth one SynthLib slot allocator with a project-supplied payload rather than two
- vst3/ holds the plug-in layer for BOTH formats since 2026-09-11 and is misnamed - G2-Edit renamed its own to plugin/; move it with do-plugin's list, do-vst3host's -I and vst3check's includes in one change

Features

- BUILT 2026-09-09, not yet played through a synth - see to-test.md. Live routes into the instrument's aux bus (logged from Live itself), the passthrough and the parameter are in, vst3check covers the audio path, and the open question is whether the measured figure should be reported in full or with the host's own input path taken off it. The shape, for reference: a new "Capture source: Device | Host input" PARAMETER at the end of the id list - NOT a "Host" entry in the device slots, because those are positions in the device list and renumbering them silently repoints saved automation; reconfigure() opens nothing in that mode; gb_bridge_render() branches to a passthrough with the trim applied; reported latency becomes the measured offset with no pipeline term; and the panel greys device, rate, buffer, first channel and mono/stereo, along with the ring, drift and underrun figures, which would be meaningless rather than zero

Bugs

- A fresh panel now reads Buffer 32 samples, the default GenBridge has always registered with hosts; the old gbEditor.mm panel hard-coded 64 and disagreed with it (2026-09-11). Decide which the default should be and make the registered value say it
- The device buffer size is stored per PROJECT, but it is arguably a property of the INTERFACE - the same value is wanted in every project on the machine, and a setting chosen but not saved is silently lost (findings 2026-09-08 (17), which cost an evening). Consider a machine-wide preference keyed by device UID, with the project state overriding it where present
- A device already open elsewhere silently keeps ITS buffer size, and GenBridge's request is accepted and ignored - the panel now says so, but nothing tries to recover. Worth deciding what should happen: re-request when the other holder lets go, or say plainly on the panel that the setting cannot take effect while something else has the device. See findings 2026-09-08 (10)
- GB_AUTO_MARGIN (1.25) has never been justified by measurement - it is 6.3 ms of headroom at a 1024 frame host buffer, on top of a floor that is already the worst case. Sweep it toward 1.05 against the underrun counter and see what it actually needs; and consider exposing the manual setpoint (targetMs), which is honoured today but has no control
- RING SEEN BACKING UP ABOVE ITS SETPOINT: fill climbing 1318 -> 2003 against a setpoint of 1440 on an Analog Rytm, drift wandering rather than settling. If that is real rather than a rig artefact it makes the honest pipeline delay genuinely larger than the setpoint implies, which is the reporting question from the other end. See findings 2026-09-08 (8)
- send_controller() fires on every parameter change the host delivers, with no test for whether the value actually moved - a host that re-sends its CC parameters every block would put a MIDI message per parameter per block on the wire. Suspected, not measured; it is the other way notes could be crowded off a link
- Tell the host kParamTitlesChanged when the device list changes - hot-plug currently updates our cache only, so the host's generic panel keeps the old names; see findings 2026-09-02
- Expose a kIsBypass parameter, as JUCE always does - nothing is broken without one, but the host should be able to hand bypass to us
- device_enumerate() silently stops at DEVICE_MAX (64) and the device parameter is normalised across that same constant, so raising it re-scales every saved project - see findings 2026-09-02
- FX output can't be recorded on its own Ableton audio track - attributes now cleared as the cause (see findings 2026-09-02), so the question is whether to document the routing workaround or build a virtual driver
- On Cubase, helix audio is playing when Kronox is selected. Says capturing Helix, but Kronos actually selected. Seems OK on Ableton as-is.
- Ideally should be able to freeze / bounce when we're using GenBridge as an instrument. Although, Elektron OverBridge doesn't do that, so not high priority.
- Only seen once. When dragging GenBridge onto Ableton session screen, audio track was created which didn't send to main.
- Locking-up if I select a QU24 input with a frame size less than 64 - shared-device guard, restore-on-close and the reconfigure debounce all landed 2026-09-02, so RETEST before chasing further
- An ABSENT saved MIDI destination silently falls through to the first port in the list, so opening a project with the synth switched off sends notes to whatever is first - the same fault as the audio "absent saved device fell through to a microphone" (findings 2026-09-01), which was fixed on the audio side only. Needs the same treatment: hold the wanted NAME, send nothing until it appears; a None slot was rejected for audio because it renumbers saved values, so a resolved flag is the likely shape
- AND vst3check CANNOT SEE IT: its "MIDI destination restored by name" check names KRONOS SOUND, so it fails when that synth is off and passes when it is on - which is the device's presence, not the fallback. It went green on 2026-09-09 the moment the KRONOS was switched on, with nothing fixed. A check for the fallback has to restore a name that is deliberately ABSENT and assert that NOTHING is selected
