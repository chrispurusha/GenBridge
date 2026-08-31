# GenBridge

Bridge any Core Audio device into a DAW, without an aggregate device.

The problem this exists for: an aggregate device is macOS's answer to using two interfaces at once,
and it is unreliable — devices fail to enumerate, and the clock reconciliation is not really there.
The alternative is a plug-in that opens the device itself and hands its audio to the host, which is
what AudioMovers' Inject does for Elektron Overbridge-style hardware.

**Status: proof of concept.** The `poc/` directory holds a command line program that proves the one
part nobody hands you for free — the clock drift loop. There is no plug-in yet.

---

## Why the drift loop is the whole problem

Two audio devices have two crystals, and no crystal is exact. A device nominally at 48 kHz may
really run at 48000.5 Hz. Bridge two of them and the buffer between them fills or empties at the
difference — about one frame every two seconds at 10 ppm — until it overflows or starves.

Neither Core Audio's aggregate device nor JUCE's software equivalent fixes this.
`AudioIODeviceCombiner` in `juce_CoreAudio_mac.cpp` detects the collision and calls `xrun()`:
it invalidates both device sample times, drops the block and resynchronises. That is correct, and
the glitch returns on a fixed schedule forever. There is no clock drift compensation anywhere in
JUCE — `grep -ri drift` across every module returns nothing but a jpeglib constant and an AAX error
enum.

The fix is to stop treating the rate ratio as a constant: resample by a ratio nudged by a few parts
per million, continuously, so the fill depth stays put. The integrator then converges on the true
ratio between the two crystals, which means the loop both cancels the drift and measures it.

## What the proof of concept does

Runs two independent Core Audio devices against each other and holds the ring between them at a
fixed depth. The output device stands in for the DAW's clock — an honest substitution, because from
the bridge's point of view a DAW is exactly that: something consuming blocks on a clock which is not
the capture device's.

```
./do-poc
./build/genbridge --list
./build/genbridge --in KRONOS --out U28E850
```

| Option | Meaning |
|---|---|
| `--in` / `--out` | device, matched as a substring of the name or UID |
| `--channels <n>` | channels to bridge (default 2) |
| `--frames <n>` | device buffer frames to request (default 256) |
| `--target-ms <ms>` | ring setpoint (default 40) |
| `--bandwidth <hz>` | drift loop bandwidth (default 0.02) |
| `--max-ppm <ppm>` | correction clamp (default 500) |
| `--inject-ppm <ppm>` | lie about the rate ratio, to give the loop a known drift to find |
| `--no-correct` | disable the loop — the control case |

### Reading the telemetry

```
   time       fill       err       corr      integ        raw  under   over resync
  120.4s     1920.0       0.0  -49.99p    -49.99p     +0.00p      0      0      0
```

- **fill / err** — ring depth in input frames, and its distance from the setpoint. This is what the
  loop steers.
- **corr** — the ratio correction being applied right now.
- **integ** — the controller's integrator. Once settled, *this is the measured offset between the
  two crystals*.
- **raw** — the same quantity derived independently, from the ring's read cursor against the output
  frame count. `integ` and `raw` arriving at the same number by different routes is the result worth
  having.
- **under / over / resync** — xruns, and how often the ring had to be snapped back to its setpoint.
  All three should stay at zero.

## Measured results

Korg Kronos (USB) in, U28E850 (DisplayPort) out, on real hardware.

**These two devices show essentially no relative drift** — a USB device running synchronous to the
host is slaved to the host's clock, so there is nothing to correct. That is worth knowing, and it is
also why the loop has to be tested with `--inject-ppm` rather than by trusting whatever hardware is
to hand.

| Test | Result |
|---|---|
| No injection, 90 s | fill held at setpoint, 0 xruns, correction +0.02 ppm |
| `--inject-ppm 50`, 120 s | integrator converged to **−49.99 ppm**, raw 0.000, fill exact, 0 xruns |
| `--inject-ppm 2000 --max-ppm 3000`, 90 s | converged to −1995 ppm, fill exact, 0 xruns |
| `--inject-ppm 2000 --no-correct`, 45 s | underrun and resync every ~20 s, indefinitely |

That last row is the JUCE behaviour, and the first three are what replaces it.

## Design notes

**The ring uses absolute frame counts as cursors, not buffer indices.** Taken from JUCE's combiner,
and the most useful idea in that file: overflow and underrun become arithmetic on two monotonic
numbers rather than a wrapped-gap case analysis, and the fill depth is meaningful before either side
has run.

**Priming snaps, it does not wait.** Waiting for the ring to reach its setpoint races the two
devices — the DisplayPort output here takes about 160 ms to deliver its first callback, letting the
input run 5738 frames past the setpoint, an opening error the loop then needs four minutes to walk
off. Snapping the read cursor makes the opening error zero by construction.

**The loop bandwidth is deliberately low (0.02 Hz).** The correction is a pitch shift, so a loop
that chases the error quickly turns buffer jitter into wow and flutter. It must be slow enough that
its own output is inaudible; a real crystal offset does not change faster than that anyway. The cost
is convergence measured in tens of seconds.

**The resampler's anti-alias cutoff is fixed from the nominal ratio, not the live one.** The live
ratio differs by well under 0.1%; rebuilding a filter table in the audio callback to track that
would buy nothing audible.

## Still to do

- Replace the built-in resampler with **libsoxr** (`soxr_set_io_ratio`, the variable-rate engine).
  It is not in Homebrew core and will need vendoring into `ThirdParty/`, built at the 11.5
  deployment target like the sibling projects' libraries.
- Split the capture side into a separate feeder process, spawned with `posix_spawn` so it inherits
  the host's microphone consent, talking to the plug-in over a framed pipe protocol.
- Wrap it as a VST3, reusing G2-Edit's hand-written COM plumbing and SynthLib's renderer-in-an-
  NSView. Register as an **effect** (`Fx|NoOfflineProcess|Tools`), not an instrument — an effect has
  an input bus, which sidesteps the host rejections an instrument category invites, and
  `NoOfflineProcess` keeps a live-input plug-in out of offline bounces.

## Licence

GPLv3, matching the sibling projects and SynthLib.

**Do not copy code from JUCE into this repository.** JUCE 8 is dual licensed under **AGPLv3** and a
commercial licence — not GPLv3, which is what JUCE 6 and earlier used. AGPLv3 code cannot be taken
into a GPLv3 project and left GPLv3; the combined work would have to become AGPLv3. Nothing here is
copied from JUCE: the ideas above were read from it and reimplemented, which is what keeps this
GPLv3.

Dependencies are compatible: the VST3 SDK is MIT (since 2025), and libsoxr is LGPLv2.1-or-later.
