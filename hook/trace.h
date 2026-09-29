// ---------------------------------------------------------------------------
// trace.h - find out what the engine actually does every frame, from inside
// beginRender, without guessing.
//
// Static analysis has stalled. Correct code was found at 0x9E130 that builds the
// SceneDynamicBuffer and uploads it, and it turned out to be the resource-init
// path: two hooks on it, one at the entry and one on the interior upload, both
// stayed at zero over thousands of frames. Meanwhile the call graph from
// beginRender only reaches 57 addresses, which cannot be the whole render path
// — beginRender must call through pointers for the rest.
//
// So rather than read more disassembly and guess which helper uploads, this
// traces the calls that actually happen, in order, with their arguments. Two
// runs of the game produce two traces, and the calls that repeat once per frame
// are the render path. A call that appears once is init, and a call that
// appears in a repeating pattern with two alternating argument sets is very
// likely the per-eye or per-pass split the stereo work needs.
//
// The trace is bounded: a ring of records, logged in batches, so the log stays
// readable and the game does not stall on fprintf.
// ---------------------------------------------------------------------------
#ifndef TDVR_TRACE_H
#define TDVR_TRACE_H

#include <stdint.h>

#define TDVR_TRACE_SLOTS 24

typedef struct {
    uint64_t call_site;
    uint64_t rdx, r8, r9;    // the arguments that vary and matter
    uint32_t count;          // how many times this signature was seen
} tdvr_trace_rec;

// One slot per (call_site, arg pattern). Keyed on a cheap hash of the three
// arguments, so repeated identical calls collapse into one record and the
// interesting thing — a call whose arguments alternate — stands out.
static tdvr_trace_rec g_trace[TDVR_TRACE_SLOTS];
static volatile LONG    g_trace_n = 0;
static volatile LONG    g_trace_frames = 0;

static uint64_t tdvr_hash3(uint64_t a, uint64_t b, uint64_t c) {
    uint64_t h = a * 0x9E3779B97F4A7C15ull;
    h ^= (b + 0x165667B19E3779F9ull + (h << 6) + (h >> 2));
    h ^= (c + 0x27D4EB2F165667C5ull + (h << 6) + (h >> 2));
    return h;
}

static void tdvr_trace_note(uint64_t site, uint64_t a2, uint64_t a3, uint64_t a4) {
    uint64_t key = tdvr_hash3(site, a2, a3) ^ tdvr_hash3(a4, 0, 17);
    for (int i = 0; i < TDVR_TRACE_SLOTS; ++i) {
        if (g_trace[i].call_site == site + 1 &&
            g_trace[i].rdx == a2 && g_trace[i].r8 == a3 && g_trace[i].r9 == a4) {
            g_trace[i].count++;
            return;
        }
    }
    LONG idx = InterlockedIncrement(&g_trace_n) - 1;
    if (idx >= TDVR_TRACE_SLOTS) return;
    g_trace[idx].call_site = site + 1;
    g_trace[idx].rdx = a2;
    g_trace[idx].r8  = a3;
    g_trace[idx].r9  = a4;
    g_trace[idx].count = 1;
}

static void tdvr_trace_dump(void (*logf)(const char*, ...)) {
    LONG n = g_trace_n;
    if (n > TDVR_TRACE_SLOTS) n = TDVR_TRACE_SLOTS;
    logf("--- trace: %d distinct (site,args) signatures over %ld frames ---",
         n, g_trace_frames);
    for (LONG i = 0; i < n; ++i) {
        if (!g_trace[i].call_site) continue;
        logf("  site=%p n=%-5u rdx=%p r8=%p r9=%p",
             (void*)(g_trace[i].call_site - 1),
             g_trace[i].count,
             (void*)g_trace[i].rdx, (void*)g_trace[i].r8,
             (void*)g_trace[i].r9);
    }
}

static void tdvr_trace_reset(void) {
    memset(g_trace, 0, sizeof g_trace);
    g_trace_n = 0;
    g_trace_frames = 0;
}

#endif /* TDVR_TRACE_H */
