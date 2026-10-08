# gmEngine.c notes

The longer comments for `monitor/gmEngine.c` and `gmEngine.h`, numbered; the code points at each as `// notes §k`.

## 1. `gm_engine_poll()` (gmEngine.h)

Device arrivals and departures come from CoreAudio on its own thread (`device_watch_list()`), which only
sets a flag. The app's half-second timer calls this on the main thread, which is where opening and closing
devices belongs. A running monitor whose input or output disappears drops to "waiting"; a waiting one opens
again as soon as both are present, so unplugging and replugging the Helix needs no click.

## 2. file scope

GenBridge Monitor (2026-10-08, CT): route one input pair - or one channel - of one device to an output pair
of another, with as little delay as two independent clocks allow. The use it was built for: play guitar
through a Line 6 Helix and hear it on the QU-24 (Helix USB 1/2 -> QU-24 USB returns 29/30) while GenBridge,
in the DAW, records the Helix's unprocessed channel.

It is the proof of concept's bridge (poc/genbridge.c) made into an app: SynthLib's ring between the two
devices' callbacks, the windowed-sinc resampler on the output side and the drift loop steering its ratio so
the ring holds a fixed depth for ever. Two devices are two crystals; without the loop the ring would empty or
overflow within minutes. The same device in both roles also works - ratio 1, no drift - but goes through the
same path; a direct copy inside one callback would save the ring's few milliseconds (todo).

## 3. the setpoint (`open_engine()`)

The floor is the proof of concept's: one output block's worth of input, plus a whole input block (the fill
sawtooths by that much from block granularity alone), plus the resampler's reach (2 x RESAMPLER_TAPS). x1.25
on top is GenBridge's margin (GB_AUTO_MARGIN), and the Safety row adds milliseconds beyond that. At 32-sample
buffers that is 240 frames, 5 ms, and 2.5 minutes Helix -> QU-24 ran with no dropout. The 2 x TAPS term is
the largest part and is probably needed only right after a resync, when the resampler has no history - a
tighter floor is in todo.

## 4. buffer sizes (`claim_buffer()`, `release()`)

A device's buffer size is global to the device. If another client already has it running - the DAW with
GenBridge on the same Helix - changing it would change it under them, so it is left alone and the panel says
so (GenBridge findings 2026-09-08 (10), the same rule). Otherwise the requested size is clamped to the
device's range and set, and the old size is put back when the monitor stops or quits. One device in both
roles is claimed once.

## 5. meters (`hold_peak()`, `gm_engine_status()`)

The callbacks keep the largest peak since it was last read; each status read takes it and zeroes it, so the
meter shows one frame's worth of peak at the panel's repaint rate. A load-and-store rather than an atomic
maximum: a lost update is one meter frame, not worth a compare-exchange loop on the audio thread.

## 6. channel mapping (`output_callback()`)

The ring carries the input's own width. Mono in to stereo out puts the channel on both sides; stereo in to
mono out sums at half each; otherwise straight through. The trim multiplies after the resampler, read once
per block, so moving it never reopens anything.

## 7. the latency figure (`open_engine()`)

What the devices report (each one's latency and safety offset, `device_latency_frames()`) plus one buffer at
each end, plus the bridge: the setpoint and the resampler's group delay. It is an estimate from reported
figures, not a measurement - a driver that under-reports shows a figure that is too good. The panel splits it
into "devices" and "bridge", because only the bridge part is the app's to reduce.
