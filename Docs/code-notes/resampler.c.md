# resampler.c notes

The longer comments from `resampler.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. in `resampler_process()`

Compact: discard input the filter can no longer reach, keeping HALF_TAPS - 1 frames of
history behind the read point. The memmove is a few hundred frames per callback, which is
nothing beside the filter itself, and it keeps the buffer a plain linear array - the taps
can then be indexed directly with no wrap test in the innermost loop.
