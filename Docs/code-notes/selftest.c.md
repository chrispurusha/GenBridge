# selftest.c notes

The longer comments from `selftest.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. file scope

Offline measurement of the resampler, with no hardware and no listening involved.

The bridge's telemetry proves the buffer arithmetic and the control loop; it says nothing at all
about what happens to a signal. A run with zero xruns and a rock-steady fill can still be
destroying the audio. This puts a number on that, deterministically, so a change to the filter
can be shown to be an improvement rather than assumed to be one.

Method: a pure sine through the resampler at a given ratio, then a windowed FFT of the output.
Everything that is not the fundamental is, by definition, something the resampler invented -
imaging, aliasing, interpolation error or quantisation - so the ratio of that to the fundamental
is the figure of merit. Reported as THD+N in dB; more negative is better.

## 2. in `self_test()`

Above the OUTPUT Nyquist the signal is meant to be removed entirely, so "distortion
relative to the fundamental" is not a meaningful quantity - there should be no
fundamental. Reporting a number there measures alias rejection while looking like
THD, which is worse than reporting nothing.
