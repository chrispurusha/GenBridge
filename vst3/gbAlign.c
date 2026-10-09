/*
 * GenBridge - bridge any CoreAudio device into a DAW.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
// Notes: Docs/code-notes/gbAlign.c.md - "// notes §k" refers there.

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gbAlign.h"
#include "gbParams.h"
#include "synthlibLog.h"

// notes §2
#define GB_ALIGN_SENDERS             (16)
#define GB_ALIGN_RECEIVERS           (8)
#define GB_ALIGN_SENDER_HISTORY      (1 << 18)   // 2.7 s at 96 kHz: the longest lag plus a window
#define GB_ALIGN_RECEIVER_HISTORY    (1 << 16)
#define GB_ALIGN_RECORDS             (64)

#define GB_ALIGN_WINDOW_S            (0.1)
#define GB_ALIGN_DECIMATE            (8)
#define GB_ALIGN_EARLIEST            (4096)      // samples a sender may appear EARLY - notes §3
#define GB_ALIGN_SILENT_RMS          (1.0e-3)    // -60 dBFS
#define GB_ALIGN_MATCH               (0.95)      // notes §4
#define GB_ALIGN_MARGIN_RATIO        (4.0)       // the winner fits at least this much better than any other lag
#define GB_ALIGN_PASS_MS             (200)
#define GB_ALIGN_ALIGNED_PASS_MS     (1000)        // notes §10

#define NS_PER_S                     (1000000000ull)

typedef struct {
    int64_t  end;                                 // receiver samples written when the block ended
    double   rate;
    bool     recording;
    int64_t  senderEnd[GB_ALIGN_SENDERS];         // each sender's samples written at that moment
    uint32_t senderGen[GB_ALIGN_SENDERS];         // 0 = slot not in use
} tGbAlignRecord;

struct tGbAlignSender {
    _Atomic bool     inUse;
    _Atomic uint32_t generation;
    void *           owner;
    tGbAlignApply    apply;
    float *          history;                     // allocated once per slot and never freed - notes §2
    _Atomic int64_t  written;
    _Atomic double   rate;
    _Atomic double   extraMs;
    _Atomic int      statusSlot;
    _Atomic bool     autoSet;
};

struct tGbAlignReceiver {
    _Atomic bool     inUse;
    float *          history;
    _Atomic int64_t  written;
    tGbAlignRecord   records[GB_ALIGN_RECORDS];
    _Atomic uint64_t recordCount;
    _Atomic bool     realignRequested;

    // ---- the align thread's own ----
    int64_t          lastEnd;
    bool             haveCandidate;
    int              candidateSlot;
    int64_t          candidateLag;
    int              pairedSlot;
    uint32_t         pairedGen;
    bool             awaiting;                    // a correction sent, not yet in force
    double           target;
    uint64_t         sentAt;
    uint64_t         reachedAt;
    bool             verifying;                   // the next confirmed lag must differ from lagBefore
    int64_t          lagBefore;
    uint64_t         silentSince;
    uint64_t         nextPassAt;                  // notes §10

    pthread_mutex_t  statusLock;
    tGbAlignStatus   status;
};

static tGbAlignSender   gSenders[GB_ALIGN_SENDERS];
static tGbAlignReceiver gReceivers[GB_ALIGN_RECEIVERS];

static pthread_mutex_t  gLock             = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   gWake             = PTHREAD_COND_INITIALIZER;
static pthread_t        gThread;
static bool             gThreadRunning    = false;
static bool             gThreadStop       = false;

static float *          gScratchSender    = NULL; // align thread only
static float *          gScratchReceiver  = NULL;
static float *          gScratchDecimated = NULL;

static uint64_t now_ns(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

// ------------------------------------------------------------------------------------------------
// Senders
// ------------------------------------------------------------------------------------------------

tGbAlignSender * gb_align_sender_create(void * owner, tGbAlignApply apply) {
    tGbAlignSender * found = NULL;

    pthread_mutex_lock(&gLock);

    for (int i = 0; (i < GB_ALIGN_SENDERS) && (found == NULL); i++) {
        tGbAlignSender * s = &gSenders[i];

        if (atomic_load(&s->inUse)) {
            continue;
        }

        if (s->history == NULL) {
            s->history = (float *)calloc(GB_ALIGN_SENDER_HISTORY, sizeof(float));

            if (s->history == NULL) {
                break;
            }
        }
        s->owner = owner;
        s->apply = apply;
        atomic_store(&s->written, 0);
        atomic_store(&s->rate, 0.0);
        atomic_store(&s->extraMs, 0.0);
        atomic_store(&s->statusSlot, -1);
        atomic_store(&s->autoSet, false);
        atomic_fetch_add(&s->generation, 1u);
        atomic_store(&s->inUse, true);
        found    = s;
    }

    pthread_mutex_unlock(&gLock);
    return found;
}

void gb_align_sender_destroy(tGbAlignSender * sender) {
    if (sender == NULL) {
        return;
    }
    pthread_mutex_lock(&gLock);
    atomic_store(&sender->inUse, false);
    atomic_fetch_add(&sender->generation, 1u);
    sender->owner = NULL;
    sender->apply = NULL;
    pthread_mutex_unlock(&gLock);
}

void gb_align_sender_write(tGbAlignSender * sender, float *const * out, uint32_t channels,
                           uint32_t frames, double rate, double extraMs, int statusSlot) {
    if ((sender == NULL) || (out == NULL) || (channels == 0u) || (out[0] == NULL)) {
        return;
    }
    int64_t at    = atomic_load_explicit(&sender->written, memory_order_relaxed);
    float * right = ((channels > 1u) && (out[1] != NULL)) ? out[1] : out[0];

    for (uint32_t i = 0; i < frames; i++) {
        sender->history[(at + i) & (GB_ALIGN_SENDER_HISTORY - 1)] = 0.5f * (out[0][i] + right[i]);
    }

    atomic_store_explicit(&sender->rate, rate, memory_order_relaxed);
    atomic_store_explicit(&sender->extraMs, extraMs, memory_order_relaxed);
    atomic_store_explicit(&sender->statusSlot, statusSlot, memory_order_relaxed);
    atomic_store_explicit(&sender->written, at + frames, memory_order_release);
}

void gb_align_sender_manual(tGbAlignSender * sender) {
    if (sender != NULL) {
        atomic_store(&sender->autoSet, false);
    }
}

bool gb_align_sender_auto_set(const tGbAlignSender * sender) {
    return (sender != NULL) && atomic_load(&sender->autoSet);
}

// ------------------------------------------------------------------------------------------------
// The align thread
// ------------------------------------------------------------------------------------------------

static void publish(tGbAlignReceiver * r, tGbAlignState state, double lateMs, double extraMs, int slot) {
    pthread_mutex_lock(&r->statusLock);
    r->status.state = state;

    if (!isnan(lateMs)) {
        r->status.lateMs = lateMs;
    }

    if (!isnan(extraMs)) {
        r->status.extraMs = extraMs;
    }

    if (slot >= -1) {
        r->status.senderSlot = slot;
    }
    pthread_mutex_unlock(&r->statusLock);
}

static tGbAlignState current_state(tGbAlignReceiver * r) {
    pthread_mutex_lock(&r->statusLock);
    tGbAlignState state = r->status.state;

    pthread_mutex_unlock(&r->statusLock);
    return state;
}

static bool sender_live(int i, uint32_t gen) {
    return (gen != 0u) && atomic_load(&gSenders[i].inUse) && (atomic_load(&gSenders[i].generation) == gen);
}

static double rms(const float * x, int n) {
    double sum = 0.0;

    for (int i = 0; i < n; i++) {
        sum += (double)x[i] * (double)x[i];
    }

    return (n > 0) ? sqrt(sum / (double)n) : 0.0;
}

// Copies [from, from + n) of a ring, by absolute position.
static void ring_copy(const float * ring, int64_t mask, int64_t from, int n, float * to) {
    for (int i = 0; i < n; i++) {
        to[i] = ring[(from + i) & mask];
    }
}

// Is any sender playing something audible right now? Decides "nothing arrives" from "nothing played".
static bool any_sender_audible(double rate, int window, bool * anySender) {
    *anySender = false;

    for (int i = 0; i < GB_ALIGN_SENDERS; i++) {
        tGbAlignSender * s   = &gSenders[i];

        if (!atomic_load(&s->inUse) || (fabs(atomic_load(&s->rate) - rate) > 1.0)) {
            continue;
        }
        int64_t          end = atomic_load_explicit(&s->written, memory_order_acquire);

        if (end < window) {
            continue;
        }
        *anySender = true;
        ring_copy(s->history, GB_ALIGN_SENDER_HISTORY - 1, end - window, window, gScratchSender);

        if (rms(gScratchSender, window) > GB_ALIGN_SILENT_RMS) {
            return true;
        }
    }

    return false;
}

typedef struct {
    bool    found;
    int64_t lag;            // samples the receiver's window sits behind the sender - notes §3
    double  match;          // normalised correlation at that lag
    double  runnerUp;       // the best coarse match more than 1 ms away from it
} tGbLagResult;

// notes §5 - the lag of rw[0..w) within one sender's history, coarse then fine
static tGbLagResult find_lag(int slot, const tGbAlignRecord * rec, const float * rw, int w) {
    tGbLagResult     result = {false, 0, 0.0, 0.0};
    tGbAlignSender * s      = &gSenders[slot];
    int64_t          sEnd   = rec->senderEnd[slot];
    int64_t          now    = atomic_load_explicit(&s->written, memory_order_acquire);
    int64_t          lagMin = -GB_ALIGN_EARLIEST;
    int64_t          lagMax = (int64_t)(rec->rate * ((GB_EXTRA_LATENCY_MAX_MS / 1000.0) + 0.2));
    int64_t          oldest = now - GB_ALIGN_SENDER_HISTORY + 16384;   // what the writer has not reached yet

    if ((sEnd - lagMin) > now) {
        lagMin = sEnd - now;        // the sender has not written that far ahead yet
    }

    if ((sEnd - w - lagMax) < oldest) {
        lagMax = sEnd - w - oldest;
    }

    if ((sEnd - w - lagMax) < 0) {
        lagMax = sEnd - w;
    }

    if (lagMax <= lagMin) {
        return result;
    }
    // The span every lag reads from: position 0 is lag lagMax, position o is lag (lagMax - o).
    int              span   = (int)(w + lagMax - lagMin);

    ring_copy(s->history, GB_ALIGN_SENDER_HISTORY - 1, sEnd - w - lagMax, span, gScratchSender);

    if (rms(gScratchSender, span) < (GB_ALIGN_SILENT_RMS * 0.1)) {
        return result;
    }
    // ---- coarse, on sums of GB_ALIGN_DECIMATE samples ----
    const int        d      = GB_ALIGN_DECIMATE;
    int              kw     = w / d;
    int              ks     = span / d;
    float *          rd     = gScratchDecimated;
    float *          sd     = gScratchDecimated + kw;

    for (int k = 0; k < kw; k++) {
        float sum = 0.0f;

        for (int j = 0; j < d; j++) {
            sum += rw[(k * d) + j];
        }

        rd[k] = sum;
    }

    for (int k = 0; k < ks; k++) {
        float sum = 0.0f;

        for (int j = 0; j < d; j++) {
            sum += gScratchSender[(k * d) + j];
        }

        sd[k] = sum;
    }

    double   er        = 0.0;

    for (int k = 0; k < kw; k++) {
        er += (double)rd[k] * (double)rd[k];
    }

    if (er <= 0.0) {
        return result;
    }
    int      positions = ks - kw + 1;
    double   es        = 0.0;

    for (int k = 0; k < kw; k++) {
        es += (double)sd[k] * (double)sd[k];
    }

    int      peak[3]   = {-1, -1, -1};
    double   peakC[3]  = {-2.0, -2.0, -2.0};
    double * coarse    = (double *)malloc((size_t)positions * sizeof(double));

    if (coarse == NULL) {
        return result;
    }

    for (int m = 0; m < positions; m++) {
        double dot = 0.0;

        for (int k = 0; k < kw; k++) {
            dot += (double)rd[k] * (double)sd[m + k];
        }

        coarse[m] = (es > 0.0) ? (dot / sqrt(er * es)) : 0.0;

        // the next position's energy: one sum leaves the window, one enters
        es       -= (double)sd[m] * (double)sd[m];

        if ((m + kw) < ks) {
            es += (double)sd[m + kw] * (double)sd[m + kw];
        }
        es        = (es < 0.0) ? 0.0 : es;
    }

    // The three highest local maxima, at least four coarse steps apart.
    for (int m = 0; m < positions; m++) {
        bool isMax = ((m == 0) || (coarse[m] >= coarse[m - 1]))
                     && ((m == positions - 1) || (coarse[m] >= coarse[m + 1]));
        int  near  = -1;

        if (!isMax) {
            continue;
        }

        for (int p = 0; p < 3; p++) {
            if ((peak[p] >= 0) && (abs(peak[p] - m) < 4)) {
                near = p;
            }
        }

        if (near >= 0) {
            if (coarse[m] > peakC[near]) {
                peak[near]  = m;
                peakC[near] = coarse[m];
            }
        } else if (coarse[m] > peakC[2]) {
            peak[2]  = m;
            peakC[2] = coarse[m];
        }

        // kept highest first, so the last place is always the one to give up
        for (int p = 2; p > 0; p--) {
            if (peakC[p] > peakC[p - 1]) {
                int    tm = peak[p];
                double tc = peakC[p];

                peak[p]      = peak[p - 1];
                peakC[p]     = peakC[p - 1];
                peak[p - 1]  = tm;
                peakC[p - 1] = tc;
            }
        }
    }

    // ---- fine, at full rate, around each coarse peak ----
    double erFull   = 0.0;

    for (int i = 0; i < w; i++) {
        erFull += (double)rw[i] * (double)rw[i];
    }

    int    fineO[3] = {-1, -1, -1};
    double fineC[3] = {-2.0, -2.0, -2.0};

    for (int p = 0; p < 3; p++) {
        if (peak[p] < 0) {
            continue;
        }

        for (int o = (peak[p] * d) - (d + 4); o <= (peak[p] * d) + (d + 4); o++) {
            if ((o < 0) || ((o + w) > span)) {
                continue;
            }
            double dot = 0.0;
            double es2 = 0.0;

            for (int i = 0; i < w; i++) {
                double v = (double)gScratchSender[o + i];

                dot += (double)rw[i] * v;
                es2 += v * v;
            }

            double c   = (es2 > 0.0) ? (dot / sqrt(erFull * es2)) : 0.0;

            if (c > fineC[p]) {
                fineC[p] = c;
                fineO[p] = o;
            }
        }
    }

    int    best     = 0;

    for (int p = 1; p < 3; p++) {
        best = (fineC[p] > fineC[best]) ? p : best;
    }

    if (fineO[best] < 0) {
        free(coarse);
        return result;
    }
    // notes §4 - the runner-up at full rate if a candidate more than 1 ms away was examined, else coarse
    int    away     = (int)(rec->rate * 0.001);
    double runnerUp = -2.0;

    for (int p = 0; p < 3; p++) {
        if ((p != best) && (fineO[p] >= 0) && (abs(fineO[p] - fineO[best]) > away) && (fineC[p] > runnerUp)) {
            runnerUp = fineC[p];
        }
    }

    if (runnerUp <= -2.0) {
        for (int m = 0; m < positions; m++) {
            if ((abs((m * d) - fineO[best]) > away) && (coarse[m] > runnerUp)) {
                runnerUp = coarse[m];
            }
        }
    }
    free(coarse);

    result.lag      = lagMax - fineO[best];
    result.match    = fineC[best];
    result.runnerUp = runnerUp;
    result.found    = ((1.0 - result.match) * GB_ALIGN_MARGIN_RATIO) <= (1.0 - runnerUp);
    return result;
}

// notes §6
static void realign(tGbAlignReceiver * r) {
    bool any = false;

    for (int i = 0; i < GB_ALIGN_SENDERS; i++) {
        tGbAlignSender * s = &gSenders[i];

        if (!atomic_load(&s->inUse) || (s->apply == NULL)) {
            continue;
        }

        // The paired sender if there is one, otherwise every sender that is running.
        if ((r->pairedSlot >= 0) && sender_live(r->pairedSlot, r->pairedGen) && (i != r->pairedSlot)) {
            continue;
        }

        if (atomic_load(&s->written) == 0) {
            continue;
        }
        s->apply(s->owner, 0.0);
        atomic_store(&s->autoSet, false);
        synthlib_log_line("align: re-align - sender %d's Extra Latency back to 0, measuring afresh", i);
        any = true;

        if ((r->pairedSlot < 0) || !sender_live(r->pairedSlot, r->pairedGen)) {
            r->pairedSlot = i;
            r->pairedGen  = atomic_load(&s->generation);
        }
    }

    r->haveCandidate = false;
    r->verifying     = false;

    if (any) {
        r->awaiting  = true;
        r->target    = 0.0;
        r->sentAt    = now_ns();
        r->reachedAt = 0;
        publish(r, eGbAlignAdjusting, NAN, 0.0, -2);
    }
}

static void analyse(tGbAlignReceiver * r) {
    uint64_t       now   = now_ns();

    if (atomic_exchange(&r->realignRequested, false)) {
        realign(r);
        return;
    }

    // notes §10 - once aligned, look once a second
    if (current_state(r) == eGbAlignAligned) {
        if (now < r->nextPassAt) {
            return;
        }
        r->nextPassAt = now + ((uint64_t)GB_ALIGN_ALIGNED_PASS_MS * 1000000ull);
    }
    uint64_t       count = atomic_load_explicit(&r->recordCount, memory_order_acquire);

    if (count == 0u) {
        return;
    }
    tGbAlignRecord rec   = r->records[(count - 1u) % GB_ALIGN_RECORDS];

    if ((atomic_load(&r->recordCount) - count) > (GB_ALIGN_RECORDS - 8)) {
        return;     // overwritten while being copied
    }

    if (rec.end == r->lastEnd) {
        return;     // the host has stopped calling us - nothing new to look at
    }
    r->lastEnd = rec.end;

    int            w     = (int)(rec.rate * GB_ALIGN_WINDOW_S);

    if ((w < 64) || (rec.end < w)) {
        return;
    }

    // ---- a correction in flight: wait for it to be in force, then let the host settle ----
    if (r->awaiting) {
        tGbAlignSender * s = &gSenders[r->pairedSlot];

        if (!sender_live(r->pairedSlot, r->pairedGen)) {
            r->awaiting = false;
        } else if (fabs(atomic_load(&s->extraMs) - r->target) > 0.5) {
            if ((now - r->sentAt) > (10u * NS_PER_S)) {
                synthlib_log_line("align: the sender never took %.1f ms - giving up waiting", r->target);
                r->awaiting = false;
            }
            return;
        } else {
            if (r->reachedAt == 0u) {
                r->reachedAt = now;
            }

            if ((now - r->reachedAt) < (3u * NS_PER_S / 2u)) {
                return;
            }
            r->awaiting = false;
        }
    }
    ring_copy(r->history, GB_ALIGN_RECEIVER_HISTORY - 1, rec.end - w, w, gScratchReceiver);

    if (rms(gScratchReceiver, w) < GB_ALIGN_SILENT_RMS) {
        bool anySender = false;
        bool audible   = any_sender_audible(rec.rate, w, &anySender);

        if (!anySender) {
            publish(r, eGbAlignNoSender, NAN, NAN, -1);
        } else if (audible) {
            if (r->silentSince == 0u) {
                r->silentSince = now;
            } else if ((now - r->silentSince) > (3u * NS_PER_S)) {
                publish(r, eGbAlignNoAudio, NAN, NAN, -2);
            }
        }
        return;
    }
    r->silentSince = 0;

    // ---- which sender, and how late ----
    tGbLagResult best     = {false, 0, 0.0, 0.0};
    int          bestSlot = -1;
    double       second   = -2.0;    // the next sender's match - notes §4

    for (int i = 0; i < GB_ALIGN_SENDERS; i++) {
        if (!sender_live(i, rec.senderGen[i]) || (fabs(atomic_load(&gSenders[i].rate) - rec.rate) > 1.0)) {
            continue;
        }
        tGbLagResult one = find_lag(i, &rec, gScratchReceiver, w);

        if (one.found && (one.match > best.match)) {
            second   = (bestSlot >= 0) ? best.match : second;
            best     = one;
            bestSlot = i;
        } else if (one.found && (one.match > second)) {
            second = one.match;
        }
    }

    if ((bestSlot >= 0) && ((1.0 - second) <= (((1.0 - best.match) * GB_ALIGN_MARGIN_RATIO) + 1.0e-9))) {
        r->haveCandidate = false;
        publish(r, eGbAlignTwoSenders, NAN, NAN, -2);
        return;
    }

    if ((bestSlot < 0) || (best.match < GB_ALIGN_MATCH)) {
        r->haveCandidate = false;

        if (  (current_state(r) == eGbAlignListening) || (current_state(r) == eGbAlignNoAudio)
           || (current_state(r) == eGbAlignNoSender)) {
            publish(r, eGbAlignUnsure, NAN, NAN, -2);
        }
        return;
    }

    // notes §4 - two windows must agree before anything is done about it
    if (!r->haveCandidate || (r->candidateSlot != bestSlot) || (llabs(r->candidateLag - best.lag) > 2)) {
        r->haveCandidate = true;
        r->candidateSlot = bestSlot;
        r->candidateLag  = best.lag;
        return;
    }
    r->haveCandidate = false;

    tGbAlignSender * s      = &gSenders[bestSlot];
    double           lateMs = ((double)best.lag / rec.rate) * 1000.0;
    double           extra  = atomic_load(&s->extraMs);
    int              slot   = atomic_load(&s->statusSlot);
    int64_t          close  = (int64_t)fmax(16.0, rec.rate * 0.0003);

    r->pairedSlot    = bestSlot;
    r->pairedGen     = rec.senderGen[bestSlot];

    if (best.lag <= close) {
        if (current_state(r) != eGbAlignAligned) {
            synthlib_log_line("align: aligned - sender %d arrives %+lld samples (%.2f ms), match %.4f, Extra Latency %.1f ms",
                              bestSlot, (long long)best.lag, lateMs, best.match, extra);
        }
        r->verifying = false;
        publish(r, eGbAlignAligned, lateMs, extra, slot);
        return;
    }

    // notes §7 - sent, in force, and the delay did not move: the host has not re-read our latency
    if (r->verifying && (llabs(best.lag - r->lagBefore) <= (close * 2))) {
        publish(r, eGbAlignHostStuck, lateMs, extra, slot);
        return;
    }
    r->verifying     = false;

    if (rec.recording) {
        publish(r, eGbAlignHeldRecording, lateMs, extra, slot);
        return;
    }
    double        target = extra + lateMs;
    tGbAlignState state  = eGbAlignAdjusting;

    if (target > GB_EXTRA_LATENCY_MAX_MS) {
        target = GB_EXTRA_LATENCY_MAX_MS;
        state  = eGbAlignAtLimit;
    }

    // Already as far as it goes: nothing to send, and nothing to verify
    if ((state == eGbAlignAtLimit) && (extra >= (GB_EXTRA_LATENCY_MAX_MS - 0.5))) {
        publish(r, eGbAlignAtLimit, lateMs, extra, slot);
        return;
    }

    if (s->apply == NULL) {
        return;
    }
    synthlib_log_line("align: sender %d arrives %lld samples (%.2f ms) late, match %.4f (runner-up %.3f) - Extra Latency %.1f -> %.1f ms",
                      bestSlot, (long long)best.lag, lateMs, best.match, best.runnerUp, extra, target);
    s->apply(s->owner, target);
    atomic_store(&s->autoSet, true);

    r->awaiting  = true;
    r->target    = target;
    r->sentAt    = now;
    r->reachedAt = 0;
    r->verifying = true;
    r->lagBefore = best.lag;
    publish(r, state, lateMs, target, slot);
}

static void * align_thread(void * unused) {
    (void)unused;
    pthread_setname_np("GenBridge align");
    pthread_mutex_lock(&gLock);

    while (!gThreadStop) {
        struct timespec until;

        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += (long)GB_ALIGN_PASS_MS * 1000000L;
        until.tv_sec  += until.tv_nsec / 1000000000L;
        until.tv_nsec %= 1000000000L;
        pthread_cond_timedwait(&gWake, &gLock, &until);

        for (int i = 0; (i < GB_ALIGN_RECEIVERS) && !gThreadStop; i++) {
            if (atomic_load(&gReceivers[i].inUse)) {
                analyse(&gReceivers[i]);
            }
        }
    }
    pthread_mutex_unlock(&gLock);
    return NULL;
}

// ------------------------------------------------------------------------------------------------
// Receivers
// ------------------------------------------------------------------------------------------------

tGbAlignReceiver * gb_align_receiver_create(void) {
    tGbAlignReceiver * found = NULL;

    pthread_mutex_lock(&gLock);

    if (gScratchSender == NULL) {
        gScratchSender    = (float *)calloc(GB_ALIGN_SENDER_HISTORY, sizeof(float));
        gScratchReceiver  = (float *)calloc(GB_ALIGN_RECEIVER_HISTORY, sizeof(float));
        gScratchDecimated = (float *)calloc(GB_ALIGN_SENDER_HISTORY, sizeof(float));
    }

    for (int i = 0; (i < GB_ALIGN_RECEIVERS) && (found == NULL) && (gScratchDecimated != NULL); i++) {
        tGbAlignReceiver * r = &gReceivers[i];

        if (atomic_load(&r->inUse)) {
            continue;
        }

        if (r->history == NULL) {
            r->history = (float *)calloc(GB_ALIGN_RECEIVER_HISTORY, sizeof(float));

            if (r->history == NULL) {
                break;
            }
            pthread_mutex_init(&r->statusLock, NULL);
        }
        atomic_store(&r->written, 0);
        atomic_store(&r->recordCount, 0u);
        atomic_store(&r->realignRequested, false);
        r->lastEnd       = -1;
        r->haveCandidate = false;
        r->pairedSlot    = -1;
        r->pairedGen     = 0;
        r->awaiting      = false;
        r->verifying     = false;
        r->silentSince   = 0;
        r->nextPassAt    = 0;
        r->status        = (tGbAlignStatus){
            eGbAlignListening, 0.0, 0.0, -1
        };
        atomic_store(&r->inUse, true);
        found            = r;
    }

    if ((found != NULL) && !gThreadRunning) {
        gThreadStop    = false;
        gThreadRunning = (pthread_create(&gThread, NULL, align_thread, NULL) == 0);
    }
    pthread_mutex_unlock(&gLock);
    return found;
}

void gb_align_receiver_destroy(tGbAlignReceiver * receiver) {
    bool stop    = false;

    if (receiver == NULL) {
        return;
    }
    pthread_mutex_lock(&gLock);
    atomic_store(&receiver->inUse, false);

    bool anyLeft = false;

    for (int i = 0; i < GB_ALIGN_RECEIVERS; i++) {
        anyLeft = anyLeft || atomic_load(&gReceivers[i].inUse);
    }

    // notes §8 - the thread must be gone before the last instance is, or the dylib can unload under it
    if (!anyLeft && gThreadRunning) {
        gThreadStop = true;
        pthread_cond_signal(&gWake);
        stop        = true;
    }
    pthread_mutex_unlock(&gLock);

    if (stop) {
        pthread_join(gThread, NULL);
        pthread_mutex_lock(&gLock);
        gThreadRunning = false;
        pthread_mutex_unlock(&gLock);
    }
}

void gb_align_receiver_write(tGbAlignReceiver * receiver, const float *const * in, uint32_t channels,
                             uint32_t frames, double rate, bool recording) {
    if ((receiver == NULL) || (frames == 0u)) {
        return;
    }
    int64_t          at  = atomic_load_explicit(&receiver->written, memory_order_relaxed);

    for (uint32_t i = 0; i < frames; i++) {
        float v = 0.0f;

        if ((in != NULL) && (channels > 0u) && (in[0] != NULL)) {
            const float * right = ((channels > 1u) && (in[1] != NULL)) ? in[1] : in[0];

            v = 0.5f * (in[0][i] + right[i]);
        }
        receiver->history[(at + i) & (GB_ALIGN_RECEIVER_HISTORY - 1)] = v;
    }

    atomic_store_explicit(&receiver->written, at + frames, memory_order_release);

    // notes §3 - every sender's position at this moment, which is what makes the lag exact
    uint64_t         n   = atomic_load_explicit(&receiver->recordCount, memory_order_relaxed);
    tGbAlignRecord * rec = &receiver->records[n % GB_ALIGN_RECORDS];

    rec->end       = at + frames;
    rec->rate      = rate;
    rec->recording = recording;

    for (int i = 0; i < GB_ALIGN_SENDERS; i++) {
        bool live = atomic_load_explicit(&gSenders[i].inUse, memory_order_relaxed);

        rec->senderGen[i] = live ? atomic_load_explicit(&gSenders[i].generation, memory_order_relaxed) : 0u;
        rec->senderEnd[i] = live ? atomic_load_explicit(&gSenders[i].written, memory_order_acquire) : 0;
    }

    atomic_store_explicit(&receiver->recordCount, n + 1u, memory_order_release);
}

void gb_align_receiver_realign(tGbAlignReceiver * receiver) {
    if (receiver != NULL) {
        atomic_store(&receiver->realignRequested, true);
        pthread_cond_signal(&gWake);
    }
}

void gb_align_receiver_status(tGbAlignReceiver * receiver, tGbAlignStatus * out) {
    if ((receiver == NULL) || (out == NULL)) {
        if (out != NULL) {
            *out = (tGbAlignStatus){
                eGbAlignListening, 0.0, 0.0, -1
            };
        }
        return;
    }
    pthread_mutex_lock(&receiver->statusLock);
    *out = receiver->status;
    pthread_mutex_unlock(&receiver->statusLock);
}
