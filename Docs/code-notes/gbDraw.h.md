# gbDraw.h notes

The longer comments from `gbDraw.h`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `GB_CANVAS_W`

The logical canvas. Every coordinate below is in these units and is scaled to whatever surface
the host gives us, so the panel is the same shape at any window size.
TALL ENOUGH FOR THE INSTRUMENT, which is the variant with the most rows - the effect simply ends
higher. It grew by one ROW_STEP on 2026-09-09 when the Capture Source row was added.

## 2. `gb_device_slot()`

HOW A NORMALISED DEVICE PARAMETER BECOMES A DEVICE, in one place.

This existed three times - in the editor, in the controller's value-to-string, and in the
processor - and two of them used a different scale. A VST3 stepped parameter has a FIXED step
count, decided at registration and cached by the host, so it cannot follow how many devices the
machine happens to have; the editor scaled across the real device count instead, which meant the
same normalised value named one device in the panel and opened another. The symptom was hearing
a Kronos while the panel said Analog Keys.

So: the slot count is fixed, every caller uses these two functions, and the only thing that
varies is how many of the slots currently point at a real device. GB_DEVICE_SLOTS is in
gbParams.h, with every other scale the panel and the wrapper have to agree about.
