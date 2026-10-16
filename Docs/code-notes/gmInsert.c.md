# gmInsert.c notes

The longer comments for `monitor/gmInsert.c` and `gmInsert.h`, numbered; the code points at each as `// notes §k`.

## 1. Helix Native in the monitor's path (2026-10-16)

CT: host a plug-in in the Monitor for live guitar monitoring - app only, an Audio Unit to begin with, Helix Native
directly (a selector later), latency left for later (Helix reports 0 in every DAW tried). Found by its own
registration (aumf / 6HPA / LSIX); the Helix Native row reads "Not installed" when it is not there. Turning it on
or off restarts the routing, as any routing change does: the unit is loaded in `open_engine()` before the devices
start, at the OUTPUT's rate, and unloaded in `release()` after they stop, so the audio thread never races it.
A unit that will not load leaves the monitor running dry, and the panel says so.

## 2. Processing (`gm_insert_process()`)

Stereo, 32-bit float, one buffer a channel, in place: after the resampler and before the trim and the output
mapping. A mono input is fed to both sides. The unit pulls its input through a render callback that copies from
the same buffers it renders into (skipped when an in-place unit hands back our own pointers). Its latency is not
added to the panel's figure yet.

## 3. Its settings (`save_state()`, `restore_state()`)

Remembered the way a DAW remembers a plug-in: the unit's ClassInfo (its whole state - the preset and all) is
written to `helix-native.plist` beside the Monitor's settings when the unit unloads, on Stop, a routing change
or quit, and handed back to it when it next loads. Checked offline: 94 KB written on close.

## 4. Its editor (`show_insert_editor()` in gmApp.m)

"Edit Helix" appears while the unit is loaded and opens the unit's own Cocoa view in a window. The window is
closed before the unit unloads (`gm_insert_on_close()`), since the view must not outlive what it draws.
