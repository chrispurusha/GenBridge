# gbParams.h notes

The longer comments from `gbParams.h`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. file scope

WHAT THE PARAMETERS ARE AND WHAT THEIR NUMBERS MEAN - in C, and in ONE place.

This is the plug-in's own description of itself, not the VST3 API: an id, a range, a name and how
a value reads as text. The wrapper turns it into ParameterInfo and String128, which is the only
part of it that needs C++ at all.

It lives here because three pieces of code have to agree about every scale on this page - the
panel that draws a control, the wrapper that tells the host what the control is, and the
processor that acts on the value. They did not: GB_CHANNEL_SLOTS, GB_MAX_FIRST_CHANNEL and the
offset range were each defined twice, in gbDraw.c and in the wrapper, and a change to one was a
silent disagreement with the other. gb_device_slot() already carries the note about what that
costs - "the symptom was hearing a Kronos while the panel said Analog Keys".

## 2. `GB_PARAMS_INSTRUMENT_ONLY`

HOW MANY OF THEM THE EFFECT LEAVES OUT. The effect has no MIDI out, so no destination, no channel,
no measurement, no correction to one and no test note - and no capture source either, since taking
the host's input and handing it back is not a thing an effect can usefully do: SIX of the twelve.

THE INSTRUMENT-ONLY ENTRIES MUST STAY LAST, because that is the whole of this arithmetic. A new
parameter that both variants have goes BEFORE kParamMidiDest; one only the instrument has goes at
the end, and this number goes up with it.

IT SAID FOUR UNTIL 2026-09-09, so the effect advertised seven parameters and had six - and the
seventh, kParamMidiDest, answered getParameterInfo() with kInvalidArgument. A host is entitled to
walk 0..getParameterCount()-1 and expect every one of them to exist; what it does with the refusal
is its own business, and none of the possibilities are good. Found by putting the count and the
table in one file, which is the argument for having done that.

## 3. `GB_SOURCE_DEVICE`

WHERE THE AUDIO COMES FROM. Two values, and the second is the External-Instrument mode: instead of
opening a CoreAudio device, take the host's own input - Live's interface, routed into the plug-in's
side-chain - pass it through, and keep the MIDI and the latency measurement exactly as they are.

In that mode the ring, the resampler and the drift loop are all switched off, because the host's
input and its output are the same clock. Reconciling two clocks is the only reason they exist.

## 4. `GB_CC_AFTERTOUCH`

── Continuous controllers ──────────────────────────────────────────────────

A DAMPER PEDAL IS NOT AN EVENT. VST3 delivers note on and note off as events, and everything else
a keyboard produces - sustain, mod wheel, expression, pitch bend, aftertouch - as PARAMETER
changes, routed through IMidiMapping. A plug-in that only walks the event list therefore passes
notes to the hardware and silently drops the pedal, which is exactly what this one did.

So a parameter is reserved for every controller on every channel, and the processor turns any
change on one of them back into the MIDI message it came from. They are hidden: a host must know
they exist to deliver values, but nobody wants two thousand entries in an automation menu.

Per channel rather than flattened, because a bridge carries whatever the DAW sends and a
multitimbral synth is the obvious use for one. Notes already carry their channel, so flattening
controllers would make the pedal arrive on a different channel from the notes it belongs to.

THE THREE NUMBERS BELOW ARE THE SDK'S, WRITTEN OUT. They are Vst::kAfterTouch, Vst::kPitchBend
and Vst::kCountCtrlNumber from ivstmidicontrollers.h, which is a C++ header - so SynthLib's VST3
wrapper asserts its own SYNTHLIB_MIDI_* numbers against the SDK, and gbPlugin.c asserts these
against those. If the SDK ever renumbers them, the build stops rather than this file guessing.
