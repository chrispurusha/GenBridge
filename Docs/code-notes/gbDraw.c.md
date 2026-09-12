# gbDraw.c notes

The longer comments from `gbDraw.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. file scope

THIS DRAWS THROUGH SYNTHLIB'S RENDERER, not through AppKit. Everything below is render_text(),
render_rectangle() and draw_button() - the same calls the three sibling applications draw with -
so the panel cannot drift away from their look without the change showing up in all of them.

DROP-DOWNS FOR THE LONG LISTS, STEPPERS FOR THE SHORT ONES. Device, Rate and Buffer open a menu
and have no arrows; everything below them keeps its arrows and has no menu.

This comment used to say the opposite - that steppers were chosen because a drop-down "means
linking the popup, menu-bar and click-region machinery". That was measured and found untrue:
SynthLib's contextMenu.c includes nothing beyond the headers this file already uses, and
clickRegion.c was already in do-vst3's list (do-plugin's now). The build cost was one line.

What decided it was not cost but behaviour. A stepper walks THROUGH every value on the way to the
one you want, and each step here is a real device change - so stepping past a device opened it,
which is exactly what "nothing opens until explicitly chosen" exists to prevent. On this machine
that meant stepping down from a mixer woke an iPhone Continuity microphone in passing.

EVERY CONTROL IS A VST3 PARAMETER, and a click returns a request rather than acting. The host has
to be told through beginEdit/performEdit/endEdit or its automation and its saved state end up
disagreeing with what the plug-in is actually doing - see gb_on_edit() in gbPlugin.c.

## 2. `GB_MENU_BG`

── THE DROP-DOWNS ──────────────────────────────────────────────────────────────────────────────

The steppers stay. That is not indecision: a menu can only show as many entries as fit the canvas
(contextMenu.c has no scrolling, and clamp_menu_to_screen() only MOVES a menu that is too tall, so
the surplus runs off the bottom undrawn), while a pair of arrows can always reach every entry
however long the list runs. Replacing them would trade a slow control for an incomplete one on a
rig nobody can predict the size of. So the menu is the fast path and the arrows are the exhaustive
one, and no list length can make a device unreachable.

Where the pointer is, in the same logical space the menu is opened in. contextMenu.c asks for this
through synthlib_host_init() rather than declaring an extern, so the view feeds it in.
NOT RGB_GREY_3, which is what the sibling apps' menus use. In this panel's palette that macro and
RGB_BACKGROUND_GREY are the SAME value, {0.30, 0.30, 0.30} - so items painted with it vanished into
the panel and the menu appeared as text floating over the controls behind it. Darker than the
background and lighter than the control boxes ({0.16, 0.16, 0.18}), so it reads as sitting above
both. contrasting_text_colour() picks the label colour from this, so it only has to be right once.

## 3. `ROW_TOP`

One table, used by BOTH the drawing and the hit test. Two copies of these numbers is how a
control ends up looking like it is somewhere it cannot be clicked.
EVERY POSITION IS COMPUTED FROM ONE PLACE, because the instrument has a row the effect does not
and hard-coded coordinates simply drew the extra row on top of everything below it. The hit tests
call the same functions the drawing does, so a control cannot appear somewhere it cannot be
clicked.

## 4. `tGbRow`

THE ROWS, IN ORDER, AND THE ONE THAT DECIDES THE OTHERS COMES FIRST.

Capture Source is instrument-only and sits at the top because it settles what the four rows under
it are for: with Host input chosen there is no device to pick, no rate to ask for and no buffer to
set, and those rows grey out. The MIDI pair at the bottom is instrument-only too.

AN ENUM RATHER THAN LITERALS, because the row index is used in two places - the drawing and the
hit test - and a control that is drawn at one index and tested at another is a control that cannot
be clicked. That was already the note above; inserting a row at the top is exactly the change that
would have broken it.

## 5. `row_is_menu()`

ROWS 0-2 ARE DROP-DOWNS AND HAVE NO ARROWS. Two controls for one setting is clutter, and a menu
that lists every choice at once makes stepping past them pointless.

It does put the whole burden of reachability on the menu, which the arrows used to carry: whatever
the list length, the menu must be able to show all of it. That is what the column split in
gb_open_menu() is for, and it is why the count it can hold is worth keeping an eye on rather than
treating as settled - see the note there.

## 6. `row_live()`

IS THIS ROW STILL ABOUT ANYTHING? With Host input chosen there is no device being opened, so the
device, its rate, its buffer, its channel mode and its input pair are all describing something
that does not exist. They stay on screen - a control that vanishes is a control the user has to
go looking for - but they are drawn dim and they do not answer a click.

## 7. `GB_OFFSET_STEP_FINE_MS`

TWO STEP SIZES, because one cannot do both jobs - the same conclusion MidiSyncTool's panel came
to. The coarse step has to reach a synth's figure from nothing without a hundred clicks; the fine
step has to TRIM one, and what Measure dials in is whatever the hardware actually did - 11.83 ms,
not 11.8 - which a whole-millisecond step could only carry up and down the range keeping the .83
for ever. A tenth is also the row's own resolution, since the reading is printed to one decimal.

## 8. `OFFSET_GAP`

draw_button() paints a box DRAW_BUTTON_MARGIN wider on each side than the rect it is handed, so
four arrows spaced by the 6 px the rest of the panel uses came out with the coarse and fine boxes
touching - one four-glyph blob rather than two pairs. The gap has to clear the padding on both
neighbours before any of it is visible.

## 9. `NOTE_ARROW_W`

WHICH NOTE MEASURE PLAYS, on the same row as the button that plays it. Middle C is a fine default
for a keyboard and useless on a drum machine: an Analog Rytm has nothing there at all and wants
the lowest note there is. Two arrows and a name - the octave range is 128 wide, so a menu of it
would be a scroll rather than a choice.
AT THE RIGHT-HAND END OF THE ROW, not beside the button. The measured figure and its range are
printed from just after the button, and they are the longest thing on this row - put the stepper
at 196 and the two draw straight through each other.

FOUR ARROWS, the same arrangement as the offset row below it: outermost coarse, innermost fine,
reading in the middle. Coarse is an OCTAVE here rather than a bigger number of the same unit -
walking from middle C down to an Analog Rytm's C-2 is 48 semitone clicks and four octave ones,
and an octave is how anyone thinks about the distance anyway.

## 10. `offset_arrow()`

FOUR ARROWS AROUND THE READING, outermost coarse and innermost fine:

```
     [<<] [<]   -11.8 ms   [>] [>>]

```
so that distance from the reading reads as size of change. Slot 0 is the leftmost. The reading
sits BETWEEN the pairs rather than after them, which is what makes the arrangement legible - an
arrow that is not on the side it moves the value towards is a coin toss every time.

## 11. `gCachedList`

THE DEVICE LIST IS CACHED, and that is not an optimisation - it is a correctness fix.

device_enumerate() is a Core Audio HAL call that takes HAL locks. The panel repaints at 30 Hz and
was calling it on every frame, so the editor was hammering those locks continuously - contending
with the very open and close operations it was meant to be driving. Ableton became slow to load
the plug-in and had to be force quit rather than shutting down. Enumerating once a second is
still far more often than a device list actually changes.

## 12. `device_list()`

EVENT-DRIVEN, NOT TIMED, and that is the difference between a hitch a second and none.

The note above records what enumerating on every frame did to Ableton. Once a second was the fix
for that, and it is still a Core Audio HAL call on the HOST'S MAIN THREAD - every second, for as
long as the panel is open, whether or not anything has changed. On a machine with a dozen
interfaces, one of them a USB device that is slow to answer a property query, that is a beachball
you can set your watch by. CT: "something in the plugin is causing occasional or regular spinning
mouse cursor."

Nothing here needs a timer: device_watch_list() already tells us when the list changes, and
gb_device_list_invalidate() is what it calls. The long re-read is belt and braces so a missed
notification heals itself within half a minute rather than never.

## 13. `gb_input_device_channels()`

HOW MANY INPUT CHANNELS THE DEVICE IN THIS SLOT ACTUALLY HAS.

The first-channel parameter is normalised across a FIXED 32, because a VST3 parameter's range
cannot depend on which device happens to be selected without re-scaling every value already saved
in a project. So the range stays 32 and the ARROWS are limited instead - exactly what the device
stepper already does with gb_input_device_count().

Without it, a 2-channel synth let the arrows walk to "channel 17", the open clamped it back to 1,
and the panel went on displaying 17: the plug-in and its own display disagreeing about what was
being captured, which is the failure this editor exists to avoid.

0 means "no such slot, or nothing input-capable there" - callers treat that as "no limit known"
rather than as zero channels, so an empty cache never locks the control at channel 1.

## 14. `STAT_VALUE_DX`

A LABELLED FIGURE AT A FIXED COLUMN. Everything below used to be built with snprintf into one
string, so a fill of 480 and a fill of 1920 pushed everything to their right along by a character
- the drift figure never sat still long enough to read, which rather defeats the point of showing
it. Label and value each get their own x, so only the digits change.

## 15. `GB_CAPTION_GREY`

THE PANEL'S THIRD TEXT TIER - captions and hints, everything that supports a figure rather than
being one. It sits on RGB_BACKGROUND_GREY, which is 0.30 in every build that is not G2_EDIT, so
the old values were barely there: the stat captions ran at 0.45 (~1.8:1 against their own
background) and the offset hint at 0.50 (~2.1:1). 0.60 lifts both to ~3.3:1 while staying under
the 0.70 of the figure beside it, so the tier still reads as secondary instead of flattening
into it. Named because three places share it and a fourth would otherwise be a fourth literal.

## 16. in `gb_draw_frame()`

THE HOST-INPUT MODE HAS NO DEVICE TO REPORT ON, and every line below assumes there is one -
"no device selected" in amber is alarming and wrong here, since nothing is meant to be
selected. What matters instead is whether the host has routed anything in.

AND THAT IS WORTH SAYING OUT LOUD, because the commonest way to get this wrong is silent:
a side-chain pointed at the plug-in's OWN track is a feedback loop, so Live mutes it and
hands over buffers of nothing. Without this line that looks exactly like a dead synth, a
wrong MIDI destination or a broken plug-in.

## 17. in `gb_draw_frame()`

---- the three steppers ----

THE SAVED DEVICE WHILE WAITING FOR IT, not whatever the parameter's slot index happens to name
now. The index was recorded when the device list had a different shape, so with the device
unplugged it points at a stranger - and the row would calmly name a microphone underneath a
header saying we are waiting for an interface. The processor is honouring the saved device, so
the row shows the saved device.
THE SOURCE ROW COMES FIRST because it decides what the rows under it are for. Instrument only:
handing the host its own input back is not something an effect can usefully do.

## 18. in `gb_draw_frame()`

WHAT THE DEVICE TOOK, NOT ONLY WHAT WAS ASKED FOR. A CoreAudio device is entitled to refuse a
buffer size - its driver has a minimum, and a device another process already holds keeps the
size that process set. The plug-in handles that correctly: it reads the size back and sizes
the ring from the REAL one. The panel did not, and showed the request.

That cost a long evening. A KRONOS displaying "64 samples" was running at 512, which put
1440 frames in the ring and 660 in the device term - 44 ms of reported latency that looked
inexplicable against the setting on screen, because the setting on screen was fiction.

## 19. in `gb_draw_frame()`

NOT A REFUSAL - A DECISION. Rate and buffer size are global to a device, so GenBridge
deliberately leaves both alone when something else is already running it rather than
reaching into another client's settings; the host itself being that client is the
case that matters most. "device gave 512" reads as a fault and sends someone hunting
for one, when the answer is that the setting cannot apply while the device is shared.

## 20. in `gb_draw_frame()`

THE RANGE, NOT JUST THE AVERAGE. Measure plays several notes and averages the middle
of them; an average on its own cannot be told from one lucky shot. Five readings
within a millisecond of each other say the figure can be trusted, and the same
average out of readings 9 ms apart says it cannot - which is a judgement for the
person reading the panel, not one to hide.

## 21. in `gb_draw_frame()`

THE CORRECTION IN FORCE - what report_latency() actually adds, not a trim on top of it.
Measure seeds this row; the +/- moves it from there, because a measurement includes the
patch's own attack and cannot know it, so the last fraction of a millisecond stays a
judgement rather than a reading. The row above shows what was measured, so the two can be
compared and you can see how far you have moved from it.
"In use", not "Correction": labels are drawn at x=20 and the arrows start at LABEL_W (74),
so a label has 54 px. "Correction" ran straight under the < >. The longest label this
panel already proves fits is "MIDI Out" at eight characters - stay inside that.

## 22. in `gb_draw_frame()`

---- telemetry ----

The same figures the command line bridge prints. They are here because they are the only way
to see whether the drift loop is holding without attaching a debugger, and because a buffer
that is quietly resyncing every twenty seconds is otherwise indistinguishable from one that
is not.
WHERE THE LATENCY ACTUALLY GOES. One total tells you nothing actionable: on this rig two
thirds of it is usually the device's own buffer, which is fixed by a control three rows up,
and the ring is the only part the plug-in chooses. Showing the parts makes it obvious which
number to argue with.

## 23. in `gb_draw_frame()`

MILLISECONDS IN BRACKETS, as the Latency row above prints them - samples alone mean
nothing without the rate in your head, and this pair is the one people actually read.

"in use" moves to kCol[2], NOT kCol[1]. A stat's value is inset STAT_VALUE_DX (52) into a
column 125 wide, so it has 73 px before the next column's caption - enough for "221" and
not for "221 (4.6 ms)", which would have run straight under the caption beside it, the
same way "Correction" ran under its arrows. Columns 1 and 3 are unused on this row, so
spreading across the gap costs nothing and gives each value 198 px.

## 24. in `gb_draw_frame()`

fill/setpoint, and the recommended setpoint after it WHEN THE TWO DIFFER. The floor is advice
rather than a limit (see "THE FLOOR IS ADVICE" in gbBridge.c), and advice nobody can read is just a
silent override with extra steps — so a setpoint below the recommendation says so here, in
the same row as the underrun count that tells the user whether it is working.

## 25. in `gb_draw_frame()`

NOTES IN AND NOTES OUT, on the instrument only. "The keyboard does not play the synth" has
three causes that look identical from outside - the host is not handing the notes over, the
plug-in is dropping them, or the send is failing - and this row separates them in a glance:
a still 0 on the left is the host's end, in without out is ours.

## 26. in `gb_draw_click()`

Each row lists only what can actually be chosen on the CURRENT device, so the limits that
used to be enforced by clamping the arrows are now expressed by the list simply not
offering the impossible - which is the better place for them, because a control that never
offers a bad value never has to explain why it refused one.

## 27. in `gb_draw_click()`

The WIDTH comes off either way. With a device known the list ends at the last
start a pair still fits; with none known the fixed 32 is the bound instead -
and taking the width off that too is what stops a stereo list ending "32 - 33",
naming a channel no device can have.

## 28. in `gb_draw_click()`

NO STEREO ON A ONE-CHANNEL DEVICE. The open already copes - it captures the single
channel and widens it - but the panel then read "Stereo" over a mono capture, which is
the same disagreement between the display and the device that the channel limit above
exists to stop. A count of 0 means the cache cannot say, so both options stand.

## 29. in `gb_draw_click()`

A TENTH OF A MILLISECOND ON THE INNER PAIR, a whole one on the outer. The fine step was
once the only step, and before that the only step was 1 ms on the argument that a
millisecond is the resolution a person can judge - true of a latency heard on its own,
wrong for this control. The measurement already puts the hardware's share into
report_latency(), so what is left is the residual a take still sounds early or late by,
and a whole millisecond steps straight over it. 0.1 ms is 4.8 samples at 48k. But a
measurement that has not been run yet leaves the whole journey to the arrows, and 20 ms
of it at a tenth a click is two hundred clicks - hence the coarse pair beside it.

The range stays +/-100 ms deliberately: it is a normalised VST3 parameter, so narrowing
it would silently re-scale the offset saved in every existing host project.
