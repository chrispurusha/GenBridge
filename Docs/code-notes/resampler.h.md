# resampler.h notes

The longer comments from `resampler.h`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. file scope

A windowed-sinc polyphase resampler whose ratio may change on every block.

THE VARIABLE RATIO IS THE ENTIRE POINT, and it is what a fixed-ratio converter cannot do.
Apple's AudioConverter and JUCE's ResamplingAudioSource both want a ratio that is constant, or
at least rarely changed - JUCE rebuilds its low-pass whenever the ratio moves, behind a
SpinLock, which is neither cheap nor real-time safe. Here the ratio is expected to wander by a
few parts per million on every single callback, because that is how a clock drift is cancelled.

This is why the anti-alias cutoff is fixed at construction from the NOMINAL ratio, not from the
live one. The live ratio differs from nominal by well under 0.1%; moving a filter cutoff by that
much would change nothing audible while forcing a table rebuild in the audio callback. So the
table is built once, and the drift correction is carried purely by how fast the read position
advances through it.

libsoxr's variable-rate engine (soxr_set_io_ratio, the SOXR_VR flag) does the same job with a
better filter, and is the intended replacement once it is vendored - see README. The arithmetic
either side of it does not change, which is why this is behind an interface.

## 2. file scope

Passband edge as a fraction of the target Nyquist. A finite filter cannot go from passband to
stopband instantly, so it needs somewhere to roll off, and giving it none is what leaves imaging
sitting right below Nyquist.

These two numbers were chosen by measurement, not taste - `genbridge --self-test` prints the
table they came from. Upsampling THD+N at 0.45 fs, which is where the defect lives:

```
     taps  guard   THD+N          note
       32   1.00   -27 dB         no transition band at all: the original bug
       32   0.95   -50 dB         32 taps cannot reach the stopband in 5% of guard
       48   0.95   -90 dB
       64   0.95   -95 dB         chosen
      128   0.95   -103 dB        2 dB per doubling from here; not worth it

```
0.95 keeps the passband above 20 kHz either side of a 44.1/48 conversion, so nothing audible is
given up in exchange. The cost of 64 taps is twice the multiply-accumulates of 32 and twice the
group delay - 32 input frames, well under a millisecond - which matters only at high channel
counts, where a 32-channel device would be doing about 98 M MAC/s.
