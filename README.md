# GenBridge

Bridge any Core Audio device into a DAW, without an aggregate device.

The problem this exists for: an aggregate device is macOS's answer to using two interfaces at once,
and it is unreliable — devices fail to enumerate, and the clock reconciliation is not really there.
The alternative is a plug-in that opens the device itself and hands its audio to the host, which is
what Elektron OverBridge does for Elektron Overbridge-style hardware.

**Status: working beta.** The plug-in builds, loads and captures — it appears in a host as two
devices, an effect and an instrument, and `./do-release` packages a `.dmg`. The `poc/` directory is
still here and still useful: it holds the command line program that proved the one part nobody hands
you for free, the clock drift loop, and it remains the place to measure that loop without a DAW in
the way.

Binary beta releases: https://github.com/chrispurusha/GenBridge/releases

If anyone is interested in helping, please drop me a line.

Since I'm now incurring costs (I recently started using LLMs) which would be good to at least cover, I now have a Buy Me a Coffee page:

https://buymeacoffee.com/chrispurusha

Thanks for any donations!

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
| `--filter-s <s>` | time constant of the low-pass on the fill measurement (default 2.0) |
| `--in-rate <hz>` / `--out-rate <hz>` | set device rates before opening; restored on exit |
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

All on real hardware. `integ` is the loop's own estimate of the crystal offset; `raw` is the same
figure derived independently from the ring's read cursor.

### Does the loop work

Korg Kronos in, U28E850 (DisplayPort) out, both 48 kHz, drift injected with `--inject-ppm`.

| Test | Result |
|---|---|
| `--inject-ppm 50`, 120 s | integrator converged to **-49.99 ppm**, raw 0.000, fill exact, 0 xruns |
| `--inject-ppm 2000 --max-ppm 3000`, 90 s | converged to -1995 ppm, fill exact, 0 xruns |
| `--inject-ppm 2000 --no-correct`, 45 s | underrun and resync every ~20 s, indefinitely |

That last row is the JUCE behaviour, and the rows above it are what replaces it.

### Rate conversion

Roland TD-50X against the U28E850, run both ways.

| Direction | Ratio | Result |
|---|---|---|
| 44.1k -> 48k (upsampling) | 0.918750 | fill held to +/-1.5 frames, integ -16.7 ppm, 0 xruns |
| 48k -> 44.1k (downsampling, anti-alias engaged) | 1.088435 | fill held to +/-1 frame, integ -17.3 ppm, 0 xruns |

Both directions report the same drift for the same pair of crystals, which is the answer they must
give: the offset is a property of the hardware, not of which way the conversion runs.

### Resampler quality

`genbridge --self-test` measures the resampler offline against known signals - no hardware, no
listening. THD+N in dB; more negative is better.

| ratio | 0.01fs | 0.10fs | 0.25fs | 0.40fs | 0.45fs |
|---|---|---|---|---|---|
| 1:1 (48k -> 48k) | -139.8 | -146.3 | -310.0 | -148.8 | -139.6 |
| up 0.919 (44.1k -> 48k) | -105.0 | -104.4 | -113.1 | -108.6 | **-94.8** |
| down 1.088 (48k -> 44.1k) | -109.4 | -114.1 | -115.9 | -103.9 | -90.5 |
| up 0.5 (24k -> 48k) | -107.3 | -107.8 | -115.0 | -107.4 | -94.0 |

The 0.45fs column is the one that matters: it is where a resampler with no transition band images
audibly. It originally read **-27.1 dB**, because the upsampling path set its anti-alias cutoff to
the full input Nyquist and so left the filter nowhere to roll off. Widening the filter to 64 taps
and pulling the passband edge back to 0.95 of Nyquist fixed it, at no audible cost - 0.95 still
keeps the passband above 20 kHz either side of a 44.1/48 conversion.

Note that at ratio 1.0 the resampler is transparent (-140 dB and below), so it costs nothing in
quality when there is no conversion to do.

### Which devices actually drift

Measured against the U28E850. This matters more than it sounds.

| Device | Rates | Drift | Note |
|---|---|---|---|
| Roland TD-50X | 44.1 / 48 / 96k | **~-17 ppm** | free-running; the loop earns its keep |
| Korg Kronos (USB) | 48k only | ~0 ppm | host-synchronous |
| Elektron Analog Keys | 48k only | ~0 ppm | host-synchronous |
| Line 6 Helix | 48k only | 0.00 ppm | host-synchronous; exactly zero over 143 s |

**A USB device running synchronous to the host is slaved to the host's clock and does not drift at
all.** Three of the four devices here behave that way - the Helix is locked so tightly that frames
consumed equalled frames produced exactly, across 6.9 million of them - so a bridge tested only
against those looks perfect while never exercising the loop.

`kAudioDevicePropertyClockDomain` is the closest thing to a way of detecting this, and it is not
close enough: devices sharing a non-zero domain are hardware-synchronised, but **every USB device
here reports 0, meaning "unknown"**. Only the DisplayPort output and the built-in audio report a
real domain, and they share it - so the Mac's own clock is the reference every drift figure above is
measured against. The drift machinery therefore has to run unconditionally; a device cannot be asked
whether it will need it.

### The one device that drifts has a vendor driver

Worth noting, because it is a plausible explanation rather than a coincidence. Every zero-drift
device runs on Apple's class-compliant driver (`AppleUSBAudioEngine:...` UIDs). The TD-50X does not:
it uses `/Library/Audio/Plug-Ins/HAL/RDUSB0264Audio.driver`, and it looks unusual from the outside -
transport type `????` rather than `usb`, and a safety offset of 6 frames where every real USB device
reports 74.

It does **not**, however, declare any rate conversion: `kAudioStreamPropertyPhysicalFormat` and
`kAudioStreamPropertyVirtualFormat` agree with each other at both 44.1 and 48 kHz, which is the
mechanism a resampling driver would use to say so. The ~-17 ppm is consistent across both
conversion directions, which is what a fixed crystal offset looks like.

The practical consequence either way: if a vendor driver *does* convert internally at a non-native
rate, running the device there means two conversions - the driver's, then GenBridge's. Prefer the
device's native rate and let GenBridge do the single conversion to whatever the DAW wants. That is why `--inject-ppm` exists: it manufactures a
drift of known size, turning "it held steady" into a check with a right answer. The TD-50X is the
one device here that drifts on its own.

## The plug-in

```
./do-vst3
```

That builds **and installs** to `/Library/Audio/Plug-Ins/VST3/`, clearing the quarantine flag on the
way - a plug-in that is not where a host looks for it has not really been built, and the alternative
is remembering a copy command after every build. The installed bundle is *replaced* rather than
copied over, because `cp -R` onto an existing bundle merges: a file dropped from the build would
survive in the installed copy and go on being loaded. `GENBRIDGE_NO_INSTALL=1` builds without
touching it, and `GENBRIDGE_VST3_INSTALL` points somewhere else.

The **system-wide** folder, not the per-user one, matching what the `.dmg` tells users to do. It is
`root:admin` and group-writable, so an administrator account writes to it without `sudo`, and it is
where every commercial installer puts its plug-ins. Hosts scan both folders, so the build warns if a
copy is sitting in the other one - two copies means the host may list the plug-in twice or load
whichever it reaches first, which is a convincing way to spend an afternoon re-fixing a bug that was
already fixed in the copy the host is not loading.

A host that is already running keeps the copy it loaded until it rescans or restarts.

`./do-release` packages a `.dmg` for a GitHub release - same interface as G2-Edit's, versions from
semver git tags, output to the Desktop, and it never creates the tag. It builds with
`GENBRIDGE_NO_INSTALL=1` so cutting a release cannot replace the copy you are testing with, checks
the version reached both the `Info.plist` and the compiled-in class info, and refuses to ship
without the licence notices FreeType and the VST3 SDK require - see `THIRD_PARTY.md`.

Built the way G2-Edit's is: a script rather than an Xcode target, against `pluginterfaces/` only,
with no CMake, no vstgui and no `public.sdk` helper classes beyond the four translation units that
do nothing but instantiate interface IDs - `funknown.cpp`, `coreiids.cpp`, `vstinitiids.cpp` and
`commoniids.cpp`. The bridge core in `poc/` is compiled in unchanged - only the CLI and
the self-test stay behind.

`tools/do-vst3host` builds two harnesses for exercising the plug-in outside a DAW:

- **`vst3host`** opens a window and shows the editor view. Copied from G2-Edit, whose own copy was
  written into a scratchpad twice and lost twice.
- **`vst3check`** is headless and answers different questions - factory shape, state round trips,
  whether activation really opens a device. It reports pass/fail and returns non-zero on failure, so
  it can gate a change.

Neither proves a DAW will accept the plug-in. An earlier version of `vst3host` asked only for
`IPluginFactory` and so never noticed `IPluginFactory2` was missing, which is exactly what Ableton
rejected G2-Edit for. `~/Library/Preferences/Ableton/Live */Log.txt` names a rejection cause
precisely and remains the last word.

Both are plug-in agnostic and shared with the sibling projects, so they eventually want a home of
their own rather than a copy per repository.

### Building the dependencies

`SynthLib/ThirdParty/{glfw,freetype,libusb}` are **nested submodules**, not directories - which is
why files must never be copied into them, as a copy collides with the next `submodule update`.

```
cd SynthLib && git submodule update --init ThirdParty/glfw ThirdParty/freetype
```

`libusb` is deliberately left out: it is G2-Edit's USB transport and GenBridge has no use for it.
Only freetype needs building, and it must be built at the same 11.5 deployment target as everything
else or the linker warns that the object file was built for a newer macOS. The cmake invocation is
in `G2-Edit/Docs/Third Party build notes.txt`; the build directory must be deleted before re-running
cmake, because the value is cached. glfw is needed for its **headers only** - SynthLib's key and
mouse constants come from them - so it is not built at all.

**It registers as two devices from one bundle** - four factory classes, a processor and a
controller each:

| Device | Subcategories |
|---|---|
| `GenBridge` | `Fx\|NoOfflineProcess\|OnlyRT\|Tools` |
| `GenBridge Instrument` | `Instrument\|Synth\|OnlyRT` |

There is one file to install and both appear after a rescan. The effect captures a device into a
track; the instrument does that *and* sends MIDI out, so a hardware synth can be played from a clip
and recorded back as one device.

The effect came first, and its category was chosen to dodge a trap: an effect *has* an input bus, so
the "no valid audio input bus" rejection that forced G2-Edit into `IPluginFactory2` with
`kInstrumentSynth` never arises. The input is declared and ignored. The instrument has to face that
question head-on, and answers it the way G2-Edit did - `Instrument|Synth`, which is the string known
to work here. `Instrument|External` is the literal description ("External Instrument (wrapped
Hardware)") and would be more honest, but a plug-in that loads beats one that is better described.

**`NoOfflineProcess` and `OnlyRT` are not the same flag, and only one of them is about bouncing.**
`NoOfflineProcess` opts out of a host's *offline-processing feature* - applying a plug-in
destructively to a clip - and says nothing about how a mixdown is rendered. `OnlyRT` is the one that
means "no processing faster than realtime", which is the literal truth here: the audio comes off a
wire at one second per second and a host consuming it faster simply drains the ring.

Both are declared. **Ableton Live 12.4.5 reads `OnlyRT` and ignores it** - its plug-in database
stores the subcategory string verbatim, and it still calls `setupProcessing` with `kOffline`. So an
offline bounce of a GenBridge track comes out silent, and no VST3 mechanism prevents it. The plug-in
detects `kOffline`, logs it, and the panel says "host is rendering offline - bounce in real time"
rather than leaving a quiet empty render to be puzzled over. The workaround is to record the track
onto an audio track in real time and export that.

Processor and controller are separate registered classes, for the reason G2-Edit found the hard way:
Ableton instantiates the class named by `getControllerClassId()` and will not ask the component for
`IEditController`.

### The editor

`vst3/` holds a Metal editor drawn through **SynthLib's own renderer** - the same `render_text()`,
`render_rectangle()` and `draw_button()` the sibling applications use, so it cannot drift away from
their look without the change showing up in all of them. Steppers rather than drop-downs, which
keeps the link surface to a handful of SynthLib files instead of the whole popup and menu-bar
system.

It shows device, sample rate, device buffer, mono/stereo, input channel, output trim, level meters,
and the drift telemetry - fill against setpoint, ppm, underruns, resyncs. That last row is the only
way to see whether the loop is holding without attaching a debugger.

**Two editors can be open at once**, which needed a change in SynthLib: its Metal backend kept the
layer, render targets, surface size, command buffer and scissor in file-scope globals, so a second
`gfx_attach_window()` simply overwrote the first window's layer and left it drawing nowhere. Those
are now a per-window context, selected by `gfx_attach_window()` and released by the new
`gfx_detach_window()`; the device, queue, pipeline, samplers and texture table stay shared, since
they belong to the GPU rather than to any surface and one glyph atlas should serve every editor.

### Processor and controller talk over IConnectionPoint

They are separate registered classes precisely so a host MAY keep them apart, and nothing else
bridges them. Two things need that bridge:

- **Which status slot to read.** The live figures are per instance. They were once a single global,
  and with two plug-ins in a set the editors read whichever processor wrote last - so a panel
  showing a microphone reported that it was capturing a Kronos.
- **Telling the host its latency changed.** A host reads `getLatencySamples()` shortly after
  activation and caches it. Since a fresh instance opens nothing, that reading is always zero, so
  choosing a device later left the host compensating for nothing at all. Only the controller holds
  the `IComponentHandler` that `restartComponent(kLatencyChanged)` lives on.

The slot number travels as a message once; the meters are then read from shared memory, because a
message per frame per instance would be a lot of allocation for advisory numbers.

### Choosing a device

Until there is an editor, the capture device is a **stepped list parameter**, which a host renders
as a drop-down in its generic panel. That makes the plug-in usable with no editor at all, and it
stays useful afterwards because it is automatable and the host saves it.

**A fresh instance opens nothing at all**, and that is deliberate. Three earlier versions opened
something on the reasoning that a silent plug-in looks broken - first a hard-coded device, then
whatever sat at slot 0. Every one was wrong. Slot 0 on the development machine is an iPhone
Continuity microphone, so loading a set woke it once per instance, synchronously, on the host's main
thread; Ableton stopped starting. A plug-in has no business seizing capture hardware nobody asked it
to. Idle until chosen, and the panel says `no device selected`.

**Nor does it retune devices.** Setting a nominal rate or buffer size is a global operation
affecting every client of that device, including the host itself if it happens to be the same
interface. Both default to "leave the device alone" and are written only when deliberately changed.

**A selection that fails to resolve stays shut.** A silent substitution turns a clear failure into a
confusing one - the panel said one device while the speakers played another. Failures are logged and
the panel shows `device unavailable` in amber.

**Channel requests are clamped to what the device has.** The settings are per device but the
parameter is per instance, so choosing channels 17/18 on a 32 input desk and then switching to a two
channel synth asks for channels that do not exist. That used to fail the open, leaving the plug-in
apparently stuck on the last device big enough to satisfy it.

### Device changes never happen on the audio thread

Opening a Core Audio device allocates, talks to a driver and can block for tens of milliseconds.
Doing that inside `process()` would stall the host's audio thread - which is how a DAW drops out. So
every open and close runs on a worker thread, and `process()` only ever raises a flag. The two are
kept apart by a mutex the audio thread **trylocks**, never locks: failing to take it means a device
swap is in flight, and a block of silence is the right answer.

### Diagnostics

```
touch /tmp/genbridge-log     # then reload the plug-in
cat /tmp/genbridge.log
```

Gated on a FILE rather than an environment variable, because a host launched from the Dock inherits
no shell environment - the one situation where the log is actually wanted. It records every device
resolution, every parameter arrival, every clamp and every failed open with its reason. Nearly every
bug in the plug-in so far was found by reading it rather than by guessing.

### Remembering settings per device

The host-saved state carries a small table keyed by device UID, not just the active device, so
switching away and back does not lose how a device was set up - a 32 channel drum module and a
stereo synth want different buffer sizes and channel pairs.

The format is versioned and line based, and it is that way now rather than later because a state
format becomes expensive to change the moment a session has been saved against it. Unknown keys are
skipped, so an older build can read a newer file - which is why new keys arrive without a version
bump, and only a change to how an existing line *splits* has ever needed one. The device UID is
written **last** on each line and parsed as the whole remainder, because real UIDs contain commas -
`AppleUSBAudioEngine:CalDigit, Inc.:...` - and splitting on them truncates it.

```
GENBRIDGE3
active=AppleUSBAudioEngine:KORG INC.:KRONOS:1140000:2,1
midi=KRONOS SOUND
midich=0
hw=236,4.917,12,KRONOS SOUNDAppleUSBAudioEngine:KORG INC.:KRONOS:1140000:2,1
dev=128,48000.0,25.500,2,2,0.7500,AppleUSBAudioEngine:CalDigit, Inc.:CalDigit Thunderbolt 3 Audio:20200000:2
dev=512,48000.0,40.000,0,2,1.0000,AppleUSBAudioEngine:KORG INC.:KRONOS:1140000:2,1
```

`dev=` is frames, rate, target ms, first channel, channel count, trim, UID. `midi=`/`midich=` are
the instrument's destination, stored **by name** rather than index because the MIDI list shifts
whenever a device is powered on or off.

`hw=` is the latency record for one *pair* - see below - and is the one line that cannot put its
free text last, because it carries two names. It counts the first off by length instead: samples,
correction in ms, destination-name length, then the destination and the UID run together. A MIDI
destination is quite entitled to contain a comma, and a third comma would have been a guess that
fails on somebody's interface.

### Measuring the hardware round trip

The instrument sends MIDI out and takes audio back, so a recorded part lands late by however long
the synth and the wire take. **Latency > Measure** sends a note, watches its own output for the
onset, and nets off the plug-in's own share of the delay.

**What it nets off is `internal_latency()`, not `report_latency()`, and the difference is the whole
correctness of it.** The reported figure *includes* the correction already in force, so subtracting
it meant subtracting the previous measurement too - every re-measure came back short by whatever was
already applied, and the value walked towards zero the more times it was run. The internal figure is
the ring, the resampler and the converters, which is the honest thing to net off.

**The measurement lands in the value you can edit.** There is one number, `In use`, seeded by
Measure and nudged by hand at 0.1 ms a click - about 4.8 samples at 48 kHz, so a real move rather
than a rounding. It used to be two: a measured figure you could not touch beside a trim starting at
zero whose only job was to correct it, which is backwards. `measured` is still shown next to it as
the raw reading, so you can see how far you have moved from it, but only `In use` is added to the
reported latency.

A measurement cannot separate the synth from its patch - a slow pad crosses the threshold later than
a piano - so the last fraction of a millisecond stays a judgement. That is what the +/- is for.

**The correction belongs to a (audio device, MIDI destination) pair**, not to the plug-in. The same
synth reached over USB and over DIN answers at different speeds, and two synths on one interface are
not comparable. Carrying one rig's figure to another is a whole round trip of error rather than a
small trim.

**It survives its own application.** Changing the reported latency makes the host reactivate the
plug-in, which reopens the device - so re-seeding the correction on device open would undo every
nudge the instant it took effect. Re-seeding is gated on the pair having actually changed. A reopen
of the same rig leaves the value alone; a genuinely different one brings its own, or zero if it has
never been measured.

A measurement that comes back with an onset *earlier* than the plug-in's own pipeline is reported
rather than clamped to zero. It cannot be a real round trip, so it means the threshold was crossed
by something other than the test note, or the ring was not at its setpoint - and a silent zero was
indistinguishable from a device that had never been measured at all.

## Scope: a HAL client, not a driver

GenBridge takes whatever Core Audio offers and asks no questions about how it got there. That is
the whole value: it needs no per-device knowledge and works with any device that has a driver,
rather than being a driver for one device.

The TD-50X is the case that proves the boundary is in the right place. All four of its USB
interfaces are `bInterfaceClass = 255` - vendor specific, not USB Audio Class - so Apple's
class-compliant driver cannot claim it and Roland ships its own HAL plug-in
(`/Library/Audio/Plug-Ins/HAL/RDUSB0264Audio.driver`). Reaching its "native" data would mean
unloading that driver, claiming the interfaces through IOKit, reverse engineering an undocumented
vendor protocol and implementing isochronous USB streaming from user space. That is writing a device
driver, for one drum module.

Roland wrote a driver. Elektron wrote a driver - `se.elektron.overbridge.driverkit.driver` is how
Overbridge gets its channel counts. Vendors who want native access write drivers, because they own
the hardware. Adding one device's proprietary protocol here would trade away the thing that makes
this useful for every other device.

**A device's "class-compliant mode" may not include audio at all.** Switching the TD-50X from Vendor
to Generic and power cycling it does make it enumerate as USB Audio Class - and it then exposes an
AudioControl interface (subclass 1) and MIDIStreaming (subclass 3) with **no AudioStreaming
interface (subclass 2) behind them**. The result is a working MIDI device that has vanished from
Core Audio entirely, which looks like a bug rather than a setting. On this module, Generic means
MIDI only, so Roland's proprietary protocol is the only route to its audio and there is no
class-compliant path to compare it against.

## Design notes

**The ring uses absolute frame counts as cursors, not buffer indices.** Taken from JUCE's combiner,
and the most useful idea in that file: overflow and underrun become arithmetic on two monotonic
numbers rather than a wrapped-gap case analysis, and the fill depth is meaningful before either side
has run.

**Priming snaps, it does not wait.** Waiting for the ring to reach its setpoint races the two
devices — the DisplayPort output here takes about 160 ms to deliver its first callback, letting the
input run 5738 frames past the setpoint, an opening error the loop then needs four minutes to walk
off. Snapping the read cursor makes the opening error zero by construction.

**The fill measurement is low-passed before the controller sees it, and that is not optional.**
Input arrives in whole device blocks while output consumes a fractional number of them, on a
different callback rate: at 44.1 kHz in and 48 kHz out, blocks of 256 arrive at 172.3 Hz while about
235 frames leave at 187.5 Hz. The instantaneous depth therefore sawtooths by up to a whole input
block however good the clocks are. That is quantisation, not drift, and it is big enough to matter -
measured at +/-110 frames, which the proportional term answers with more than 500 ppm, pinning the
correction to its clamp on nearly every update. At a ratio of exactly 1.0 the callbacks interleave
evenly and none of this shows, so a same-rate test will not find it. A one-pole filter fixes it for
nothing, because the noise is at block rate and the loop bandwidth is hundredths of a hertz.

**The setpoint must clear a whole input block**, for the same reason: the sawtooth dips that far
below the mean even with the clocks in perfect agreement.

**The loop bandwidth is deliberately low (0.02 Hz).** The correction is a pitch shift, so a loop
that chases the error quickly turns buffer jitter into wow and flutter. It must be slow enough that
its own output is inaudible; a real crystal offset does not change faster than that anyway. The cost
is convergence measured in tens of seconds.

**The resampler's anti-alias cutoff is fixed from the nominal ratio, not the live one.** The live
ratio differs by well under 0.1%; rebuilding a filter table in the audio callback to track that
would buy nothing audible.

## Still to do

- **Measure the resampler's audio quality.** Everything above validates the buffer arithmetic and
  the control loop; none of it measures what happens to a signal. An offline self-test - known
  inputs at several ratios, reporting SNR, THD and worst spurious component - would also give the
  number needed to judge whether libsoxr is worth vendoring at all, which is currently a blind call.
- Replace the built-in resampler with **libsoxr** (`soxr_set_io_ratio`, the variable-rate engine).
  It is not in Homebrew core and will need vendoring into `ThirdParty/`, built at the 11.5
  deployment target like the sibling projects' libraries.
- Split the capture side into a separate feeder process, spawned with `posix_spawn` so it inherits
  the host's microphone consent, talking to the plug-in over a framed pipe protocol.
- **Make an offline bounce work.** Live ignores `OnlyRT`, so the only real fix is capture and
  replay: write the captured audio to disk during a realtime pass, keyed to
  `ProcessContext::projectTimeSamples`, and play that back during an offline render instead of
  reading the live ring. Needs that timeline behaviour proving in an offline pass first.

## Licence

GPLv3, matching the sibling projects and SynthLib.

**Do not copy code from JUCE into this repository.** JUCE 8 is dual licensed under **AGPLv3** and a
commercial licence — not GPLv3, which is what JUCE 6 and earlier used. AGPLv3 code cannot be taken
into a GPLv3 project and left GPLv3; the combined work would have to become AGPLv3. Nothing here is
copied from JUCE: the ideas above were read from it and reimplemented, which is what keeps this
GPLv3.

Dependencies are compatible: the VST3 SDK is MIT (since 2025), and libsoxr is LGPLv2.1-or-later.
