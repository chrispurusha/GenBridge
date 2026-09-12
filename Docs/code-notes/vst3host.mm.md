# vst3host.mm notes

The longer comments from `vst3host.mm`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. file scope

── A minimal VST3 host, for looking at our own editor ──────────────────────────────────────────

Taken from G2-Edit's tools/vst3host.mm, which is where it was written and where its history is.
It is plug-in agnostic - it takes a bundle path - so this is a copy rather than a fork, and the
right eventual home for it is SynthLib, alongside everything else the projects share.

Loads a .vst3, instantiates the controller, asks it for its editor view, and puts that view in a
window it owns — which is the one relationship with a plug-in view that matters and the one that
cannot be tested any other way. Optionally screenshots the window and exits, so a rendering
change in the plug-in can be diffed the same way one in the application is.

THIS EXISTED TWICE BEFORE AND WAS LOST TWICE, because both times it was written into a scratchpad
rather than the repository (see G2-Edit's todo.md, "the hand-written test host in the
scratchpad"). It is in the repository for that reason as much as any other - and a third copy was
very nearly written into a scratchpad here before this one was found.

WHAT IT PROVES, AND WHAT IT DOES NOT. It proves the plug-in loads, instantiates, and that its
editor draws. It does NOT prove a host will accept it: an earlier version of this harness asked
only for IPluginFactory and so never noticed that IPluginFactory2 was absent — which is exactly
what Ableton rejected the plug-in for. It asks for IPluginFactory2 now and reports what it finds,
but the general warning stands. Ableton's own ~/Library/Preferences/Ableton/Live */Log.txt names
a rejection cause precisely and remains the last word.

```
Build: ./do-vst3host    (see tools/README.md)
```

## 2. file scope

IPlugFrame's IID is not instantiated by any of the SDK's *iids.cpp files, because those cover
what a PLUG-IN implements and IPlugFrame is the one interface the HOST implements. Defining it
here is the documented way round that and is why this file, not the build script, ends up owning
it.

## 3. in `main()`

WHERE TO CLICK BEFORE THE SHOT. A drop-down only exists after a click, and driving that with
a screen-coordinate clicker means guessing which window is under the pointer - which, tried
once, put a click into an unrelated application. Posting the event to the plug-in's own view
cannot miss, needs no window to be frontmost, and works with the screen locked.
Up to four, applied in order half a second apart - enough to open a drop-down and then choose
from it, which is the only way to test that a menu selection reaches the parameter at all.

## 4. in `main()`

A PLUG-IN CAN REGISTER MORE THAN ONE. This one registers an effect and an
instrument, and taking whichever came first meant the instrument's own rows - and
therefore its longest menus - could not be looked at from here at all. --audio N
picks by position among the audio classes; the default of 0 keeps the old
behaviour of taking the first.

## 5. in `main()`

THE COMPONENT FIRST, and it matters more than it looks. A real host creates the component,
initialises it, and only then goes to the class its getControllerClassId() names. Ours
loads the patch in IComponent::initialize(), so a harness that skipped straight to the
controller got an editor drawing an EMPTY DATABASE — a correct-looking window with no
modules in it, which is a very convincing way to not notice that nothing was loaded.

## 6. in `main()`

THE CONTROLLER THE COMPONENT NAMES, as a real host finds it - not the last controller class
in the factory. With two variants registered those are different things: the loop above
took the INSTRUMENT's controller while --audio 0 took the EFFECT's processor, and since
2026-09-11 an editor draws from its own processor's instance, so the mismatched pair drew
defaults. getControllerClassId() is what every host uses; this now does too.

## 7. in `main()`

STOPPING NEEDS AN EVENT TO LAND ON, which is the whole of a bug that made this
window refuse to close until the mouse was moved.

-[NSApplication stop:] does not end the run loop. It sets a flag that is only acted
on once the CURRENT event finishes being dispatched — so with the pointer sitting
still, -run stays blocked in nextEventMatchingMask: and the flag is never reached.
Any stray event releases it, which is why moving the mouse appeared to "let" the
window close.

Posting a dummy application-defined event immediately afterwards gives the run loop
the event it is waiting for, and -run returns at once whether or not anything else
is happening.
