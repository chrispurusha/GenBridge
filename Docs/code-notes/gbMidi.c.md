# gbMidi.c notes

The longer comments from `gbMidi.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `gCount`

PUBLISHED LAST, AND ATOMIC, because this table is process-global and every GenBridge in the host
shares it - the audio threads of instances that are not the one asking for names read it while
this one is rebuilding it.

refresh() used to set gCount = 0 and then spend milliseconds in CoreMIDI. For that entire window
every send in the process saw "index >= gCount" and returned false: notes silently dropped, on
every instance, once a second for as long as any panel was open. Two instances in one project
made it twice as likely and looked exactly like the two interfering with each other.

The list is built into a shadow and copied in, and the COUNT goes last with a release. A reader
indexing below the count it loaded therefore sees entries that were written before it. A slot can
still name a different device after a rebuild, which is the reason destinations are saved and
restored by NAME rather than by index.

## 2. in `gb_midi_init()`

A plug-in may be instantiated many times; one client and one port serve all of them, which is
also what keeps the host's MIDI panel from filling with duplicates.
A NOTIFY PROC, so the destination list does not have to be polled. Passing NULL here is what
left refresh() with a one-second timer as its only way of noticing a synth being switched on -
and that timer ran on the host's main thread, inside CoreMIDI, for as long as a panel was
open. CoreMIDI will now tell us instead.

## 3. `gb_midi_send_at()`

ONE PATH, AND IT IS LOCAL RATHER THAN SynthLib'S. synthlib_midi_send_to() stamps every packet 0,
which is the one thing this cannot do; and it builds the packet in a shared static buffer behind
a mutex, so routing notes through it would take a lock per event on the audio thread. The packet
list here is on the stack and the send takes nothing.
