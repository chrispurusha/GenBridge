# trim.h notes

The longer comments for `poc/trim.h`, numbered; the code points at each as `// notes §k`.

## 1. The output trim's law

ONE SCALE FOR THE PLUG-IN AND THE MONITOR (2026-10-16). The control runs in decibels from TRIM_MIN_DB (-48) to
TRIM_MAX_DB (+12), straight across, and its very bottom is silence. 0 dB sits at 0.8 (`trim_unity()`).

It was a gain multiplier, 0 to 2, linear in gain - so at most +6 dB, and CT wanted more ("+12dB?"). A scale in
decibels gives the boost and keeps the useful range of cut readable, where linear gain spent most of the
control above -6 dB. Being linear in dB, the plug-in's PLAIN value is decibels (`eSynthLibUnitDecibels`), so a
host shows and automates it as such.

What is saved is unchanged in kind: the plug-in's device record and the Monitor's settings keep the GAIN, and
the control's position is worked out from it (`trim_normalized()`). A project saved before this keeps its gain
exactly; only the host's automation lane, which stores the control's position, reads differently - CT: not
heavily in use, no conversion wanted.
