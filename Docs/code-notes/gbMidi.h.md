# gbMidi.h notes

The longer comments from `gbMidi.h`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `GB_MIDI_NAME_LEN`

MIDI out, for the instrument variant: the DAW plays the hardware and GenBridge captures what
comes back. That round trip is the whole point of an instrument version - without it the plug-in
can only record something a person is playing by hand.

Packet construction and MIDISend belong to SynthLib (synthlibMidi.c); what is here is the client,
the output port and the destination list.

## 2. `gb_midi_send_at()`

THE SAME SEND, STAMPED - and the difference is a note's position INSIDE the host's block.

A host hands over a block of events with a sample offset apiece, and a note at offset 900 of a
1024-frame block belongs 900 frames after that block begins. Firing everything the moment
process() is entered puts every note up to a whole block EARLY, which at 128 frames is inaudible
and at 2048 is 42 ms - so a part recorded back through the capture path drifts earlier the larger
the host's buffer gets, and no latency figure can correct it because the error is per-note.

CoreMIDI delivers a packet at its timestamp rather than on arrival, so the offset can simply be
added. A hostTime of 0 means "now", as it does in CoreMIDI itself, and a time already past is
delivered immediately - which is what makes this safe when a callback runs late.
