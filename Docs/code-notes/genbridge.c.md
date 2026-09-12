# genbridge.c notes

The longer comments from `genbridge.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. file scope

Proof of concept for the whole idea: run two independent CoreAudio devices against each other
and hold the buffer between them at a fixed depth indefinitely, with no xruns.

This is deliberately NOT a plug-in. The hard part of GenBridge is the drift loop, and a loop
that has to be reloaded into a DAW to be observed is a loop that will not get tuned. Here the
output device stands in for the DAW's clock - the substitution is honest, because from the
bridge's point of view a DAW is exactly that: something that consumes blocks on a clock which is
not the capture device's.

The telemetry is the deliverable. If 'fill' holds its setpoint and 'int' settles on the same
number as 'raw', the loop is tracking the real crystal offset and the design works.

## 2. in `output_callback()`

The raw cross-check, computed HERE rather than on the reporting thread. Both quantities are
owned by this callback - readPos is moved by ring_read above, framesOut a line ago - so they
are mutually consistent. Reading them from another thread instead lets one advance between
the two loads, and a single block of skew looks exactly like tens of ppm of drift.

## 3. in `main()`

Synthetic drift. Real hardware may or may not have any - a USB device running synchronous
to the host has none at all - so a loop that only ever sees well behaved devices has not
been tested. Deliberately mis-stating the ratio gives a drift whose exact size is known in
advance, which turns "it held steady" into a check with a right answer.

## 4. in `main()`

The setpoint has to clear one output block's worth of input plus the filter's reach, or the
very first read underruns and the loop spends its life recovering from a hole of its own
making.
Must clear one output block's worth of input, the filter's reach, AND a whole input block -
the fill sawtooths by that much from block granularity alone, so a setpoint below it dips
into underrun on the troughs even with the clocks in perfect agreement.
