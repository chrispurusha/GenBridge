# gbAlign.c notes

The longer comments for `gbAlign.c` and `gbAlign.h`. The code points at each as `// notes §k`.

## 1. file scope - the Align role

WHY IT EXISTS (2026-10-09, CT). Recording GenBridge into Live goes through a second track ("Audio From:
the GenBridge track, Post FX"). Live holds every track's output back to line up with the slowest chain
in the set, so in a busy set the take landed ~300 ms late; telling Live that much latency through Extra
Latency (gbParams.h notes §5) put takes on the beat AND made monitoring immediate, because Live had
nothing left to hold that track back by. CT confirmed both by ear the same day. The figure depends on
the set, which the plug-in cannot read, so it is measured.

HOW. Every instance (effect or instrument, in the Capture role) is a SENDER: each block it writes the
mono sum of its output into its own history. An effect in the Align role is a RECEIVER, placed on the
recording track: it passes that track's audio through untouched, keeps its own history, and a
background thread finds where in some sender's history the arriving audio sits. That lag is how far
Live delays the route. The sender is asked to add it to its Extra Latency - through the host, as a
parameter edit, so it is saved with the project and shown on both panels - and once in force the lag
should read zero ("aligned").

PAIRING IS BY CONTENT. Nothing is chosen on the panel: the sender whose audio matches what arrives is
the partner, so several capture/align pairs in one set do not interfere. Two senders with identical
audio (two GenBridges on the same input) match equally and are reported, not guessed between (§4).

An aligning instance reports 0 latency of its own, captures nothing, and its device rows go grey.

## 2. the registries

Static slots, 16 senders and 8 receivers per host process, each slot's history allocated on first use
and NEVER freed. A receiver's audio thread reads every sender's write position each block; with
slots that are never freed it can do so with no lock and no chance of reading freed memory. A
generation number per slot tells a reused slot from the instance that held it before.

Sender history is 2^18 samples (2.7 s at 96 kHz): the longest lag searched (Extra Latency's 1000 ms plus
200 ms) plus a window, plus slack for the time between a receiver's block and the analysis of it.

## 3. why the lag is exact

Live must process the sending track before the track that takes its output, within one audio cycle.
So when the receiver's block runs, each sender's write position is exactly "the end of this cycle's
block" in that sender's stream, and the receiver records it beside its own position. A receiver
window ending at its position E corresponds, with no delay, to the sender's stream ending at the
recorded position - any lag is the route's delay, to the sample. Wall-clock stamps would see nothing:
Live's compensation is a delay line run inside the same cycle.

A small negative range (GB_ALIGN_EARLIEST) is searched anyway, in case a host breaks the ordering
assumption; a consistently negative result would show it.

## 4. what counts as a match

THE ROUTE IS NORMALLY BIT-EXACT: Post FX is taken before the mixer, so the receiver gets the sender's
samples scaled at most - a normalised correlation of 1.0000. A match must reach GB_ALIGN_MATCH (0.95),
fit at least GB_ALIGN_MARGIN_RATIO (4) times better than the best lag more than 1 ms away (otherwise
it is a periodic sound and any period would do), and TWO successive windows must agree within 2
samples before anything is done. The margin was first tested on the decimated (coarse) correlation,
which a sustained note makes look periodic - it refused every window of a plucked test signal while
the full-rate match sat at 1.0000 against 0.999 for the next period. It is tested at full rate now.

The same ratio decides between senders: a second sender matching as well as the first is
eGbAlignTwoSenders.

## 5. `find_lag()`

Coarse then fine. Coarse: the receiver's 100 ms window and the sender's span summed in eights, a
normalised correlation at every eighth lag, the three best local maxima kept. Fine: full-rate
correlation within +-12 samples of each. About 9 M multiply-adds per sender per pass at 48 kHz, on the
align thread every 200 ms - cheap enough not to need an FFT.

## 6. Re-align

Live's figure only grows the sender's Extra Latency; when the set gets LIGHTER (a heavy plug-in
removed) the sender over-reports, and the route delay that would show it is already zero - nothing
to measure. Re-align sets the paired sender's Extra Latency to 0 (every running sender if none is
paired yet), waits for it to be in force, and measures afresh.

## 7. the host did not re-read the latency

After a correction the receiver waits until the sender reports the new figure and 1.5 s more, then
measures again. If the lag has not moved, the host has not re-read our latency; adding it again would
double it. It says so instead (eGbAlignHostStuck) and acts again only when the lag changes. No
correction is made while the host says it is recording - the take would move mid-way.

## 8. the thread

One align thread for the process, started with the first receiver and joined when the last one goes:
a thread still running when the host unloads the plug-in's dylib crashes the host.

## 9. tested offline

A harness (not in the repo) drives a sender and a receiver through a simulated Live - route delay =
the set's latency minus what the sender reports - at 48 kHz in 128-frame blocks: 300 ms and 37.5 ms
converge exactly in about 2 s; 1150 ms stops at the 1000 ms limit and says so; a host that never
re-reads reports "stuck" after one correction; a set that drops to 120 ms is found by Re-align; a
silent receiver reports "nothing arriving"; a quiet unrelated sender is never chosen; two senders with
the same audio are reported. Not yet run in Live.

## 10. once aligned, once a second

CT, 2026-10-09: while playing, a pass every 200 ms cost about 2% of a core per capturing instance (offline
harness, 48 kHz). Once "aligned" it looks once a second instead; a Re-align press still acts on the next
200 ms pass. Two agreeing windows are still needed, so a change in the set (a heavy plug-in added) is
acted on about 2 s after it shows, rather than about 0.4 s.
