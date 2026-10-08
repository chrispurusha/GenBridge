# gmDraw.c notes

The longer comments for `monitor/gmDraw.c`, numbered; the code points at each as `// notes §k`.

## 1. file scope

The monitor's panel, drawn with SynthLib inside SynthLib's panel view - the same machinery and look as
GenBridge's plug-in editor (vst3/gbDraw.c), whose patterns it follows: every row's value opens a drop-down,
the open menu takes the next click first, colours are told to the renderer through configure_synthlib_theme().

## 2. `refresh_devices()`

Read again whenever a device menu opens and after a choice, so a device plugged in since the last look is
offered. Frames do not re-read it: enumeration is not free and nothing needs it 30 times a second.

## 3. `channel_choices()`

Every start position the device has room for, not only odd pairs: the Helix's useful stereo pairs start on
odd channels, but other devices put a stereo return on 2-3. Counted from 1, as every interface's own manual
and panel count.

## 4. `meter()`

Drawn in dB, -60 to 0. A linear bar shows a guitar at a sensible level as a sliver.

## 5. the "in use elsewhere" line

See gmEngine notes §4. Shown so a Buffer setting that did not take effect explains itself.

## 6. trim snapping (`gm_draw_click()`)

A click within 2% of the middle of the bar sets exactly 0 dB, so unity is easy to get back to by eye. The bar
spans silence to +6 dB.

## 7. the Display toggle (`display_button()`)

Pauses all drawing to save the CPU it costs (2026-10-08, CT: "minimising CPU impact is a goal"). The panel
view stops its 30 Hz timer while the panel says it is paused (SynthLib tSynthLibPanel.paused), exactly as it
does for a hidden or covered window, and draws only after a click - so the toggle itself is how it comes
back. The last frame blanks the meters and figures and says the display is paused, because frozen numbers
would read as live. The routing is untouched either way. The button is blue while the display runs and grey
while it is paused.
