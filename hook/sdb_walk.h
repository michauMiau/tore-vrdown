// ---------------------------------------------------------------------------
// sdb_walk.h - follow the renderer's own pointers, looking for the SDB.
//
// The 12 KB renderer object holds 117 heap pointers. The SceneDynamicBuffer is
// not inline in it (the v18 dump proved +0xB80..+0xB98 are null), but the
// object that owns those buffers is reachable from something the renderer does
// point at. So: two levels deep. For each pointer the renderer holds, read its
// first 4 KB, collect the pointers found there, and test each of those as a
// candidate SDB.
//
// 117 pointers, 4 KB each, is 480 KB of reads. The second level multiplies that
// by however many pointers each of those objects has — that is the part that
// could stall a frame, so the budget below caps the total bytes walked and
// abandons the scan rather than hitching the game. The scan runs from the frame
// hook, so a budget that is too generous is visible immediately as a frame
// time spike; the cap is set so one pass cannot exceed a few hundred
// microseconds of work.
//
// It is also strictly read-only: no patch, no hook, no write. A bug here costs
// a bad log line, not a crash.
// ---------------------------------------------------------------------------
#ifndef TDVR_SDB_WALK_H
#define TDVR_SDB_WALK_H

#include <stdint.h>
#include "memscan.h"

#define TD_WALK_MAX_PTRS   32768
#define TD_WALK_MAX_HITS   16

// Objects read per pass, per frame.
//
// 512 for level 1 covers the renderer's 88-117 pointers with room to spare.
// 256 for level 2 is 1 MB of reads: at 144 Hz a frame is 7 ms, and 1 MB of
// memcpy plus 256 VirtualQuery calls is well under 1 ms, so the walk cannot
// dominate the frame. The v23 run tried to read 8627 objects at level 2 and
// stopped the render thread, so this cap is a thirtieth of what already failed.
#define TD_WALK_CAP_L1     512
#define TD_WALK_CAP_L2     256

// Total bytes per pass, as a backstop below the object caps. 4 MB is more than
// the caps can consume (512 + 256 objects = 3 MB), so in practice the object
// counts govern and this only fires if they are changed apart from each other.
#define TD_WALK_BUDGET     (4u * 1024u * 1024u)
#define TD_WALK_READ       4096u                    // per pointer

typedef struct {
    uint64_t    owner;      // the object that held the pointer
    uint64_t    addr;       // the candidate SDB
    uint32_t    flags;
    float       pd[4];
    float       max_vp;
} td_walk_hit;

static td_walk_hit  g_walk_hits[TD_WALK_MAX_HITS];
static volatile LONG g_walk_n;
static uint64_t     g_walk_bytes;
static uint64_t     g_walk_ptrs;
static uint64_t     g_walk_seq;

static int td_walk_add(uint64_t owner, uint64_t addr, const unsigned char* p) {
    td_scan_hit h;
    if (td_test_candidate(p, &h) != TD_SCAN_F_ALL) return 0;
    LONG n = g_walk_n;
    if (n >= TD_WALK_MAX_HITS) return 0;
    g_walk_hits[n].owner  = owner;
    g_walk_hits[n].addr   = addr;
    g_walk_hits[n].flags  = h.flags;
    g_walk_hits[n].max_vp = h.max_vp;
    memcpy(g_walk_hits[n].pd, h.pd_f, sizeof h.pd_f);
    g_walk_n = n + 1;
    return 1;
}

// Walk a page for SDB candidates and for pointers worth following.
//
// The struct is 500 bytes, not 500-aligned, so a buffer can sit at any 16-byte
// boundary. Stepping by 16 finds every one of them and keeps the cost down:
// one 4 KB page is 256 candidate tests, each a handful of comparisons.
//
// pptrs may be NULL, meaning "do not collect pointers this time" — the level-2
// pass only wants SDB hits, because following a third level of pointers can
// reach anywhere and the budget, not the logic, is what bounds it.
static uint32_t td_walk_page(uint64_t owner, uint64_t at, const unsigned char* buf,
                             uint64_t* pptrs, uint64_t* nptrs, uint64_t pcap) {
    uint32_t found = 0;
    for (uint32_t off = 0; off + 0x1F4 <= TD_WALK_READ; off += 16) {
        if (td_walk_add(owner, at + off, buf + off)) found++;
    }
    if (!pptrs || !nptrs) return found;
    for (uint32_t off = 0; off + 8 <= TD_WALK_READ; off += 8) {
        uint64_t v;
        memcpy(&v, buf + off, 8);
        // A plausible 64-bit heap or image pointer: above 64 KB, below the
        // user-mode ceiling, and 8-byte aligned as a real pointer is.
        if (v > 0x10000 && v < 0x00007FFFFFFF0000ULL && (v & 7) == 0) {
            if (*nptrs < pcap) pptrs[(*nptrs)++] = v;
        }
    }
    return found;
}

// Walk out from the renderer object, looking for a SceneDynamicBuffer.
//
// Three passes, each bounded:
//
//   0  the renderer object itself, for an inline SDB and for its pointers
//   1  every object the renderer points at
//   2  objects reachable from level 1
//
// The level-2 pointer count is the problem. The v23 run found 8627 pointers at
// level 2, which at 4 KB each is 34 MB of reads inside one frame — the render
// thread stopped making progress and the log froze mid-walk. The byte budget
// was set to 32 MB, which is exactly why it did not help: the budget was the
// wrong unit. 34 MB of memcpy plus 8600 VirtualQuery calls is hundreds of
// milliseconds, and a frame budget of 7 ms at 144 Hz does not contain that.
//
// So the bound is now on objects per pass, not bytes total, and level 2 is
// capped low. If the SDB is not reachable within a few thousand objects from
// the renderer, the walk reports that it ran out of scope rather than silently
// reading the whole heap. A walk that stops early can be reasoned about; one
// that stalls the game cannot be measured at all.
//
// Everything here is read-only. The cost of being wrong is a slow frame, not a
// crash, which is why the cap is a number and not an assertion.
static void td_walk_run(const void* renderer, size_t renderer_bytes) {
    unsigned char* page = g_walk_scratch;
    uint64_t n1 = 0, n2n = 0;
    uint64_t budget = TD_WALK_BUDGET;

    g_walk_n = 0;
    g_walk_bytes = 0;
    g_walk_ptrs = 0;
    g_walk_seq++;
    InterlockedExchange(&g_rpm_fail_logged, 0);

    // Pass 0: copy the renderer object through the same guarded read as
    // everything else, so the page check applies to it too. Reading the
    // caller's own object directly would be faster but would trust that it is
    // committed, which is exactly the assumption that is wrong in a packed
    // process where the hook may fire on an object mid-teardown.
    uint32_t want = (uint32_t)(renderer_bytes < TD_WALK_READ
                               ? renderer_bytes : TD_WALK_READ);
    if (td_read((uint64_t)(uintptr_t)renderer, page, want) != want) {
        MEMORY_BASIC_INFORMATION mbi;
        int got = (int)VirtualQuery((LPCVOID)(uintptr_t)renderer, &mbi,
                                    sizeof mbi);
        td_read_fail((uint64_t)(uintptr_t)renderer, want, "self", &mbi, got);
        return;
    }
    budget -= want;
    g_walk_bytes += want;
    td_walk_page(0, (uint64_t)(uintptr_t)renderer, page,
                 g_walk_ptrs_a, &n1, TD_WALK_PTR_CAP);
    g_walk_ptrs += n1;

    // Pass 1: every object the renderer points at. Collected pointers keep
    // their owner so a hit can be traced back to the field that reached it.
    //
    // Every pass logs its tallies. Without them a pass that reads nothing and a
    // pass that finds nothing look identical in the log, which is exactly the
    // ambiguity this walk has to avoid: "0 ptrs" has to mean "the level above
    // produced no pointers", never "the loop did not run".
    uint64_t r1ok = 0, r1fail = 0, l1 = n1 < TD_WALK_CAP_L1 ? n1 : TD_WALK_CAP_L1;
    for (uint64_t i = 0; i < l1 && budget >= TD_WALK_READ; ++i) {
        if (td_read(g_walk_ptrs_a[i], page, TD_WALK_READ) != TD_WALK_READ) {
            r1fail++;
            continue;
        }
        budget -= TD_WALK_READ;
        g_walk_bytes += TD_WALK_READ;
        r1ok++;
        td_walk_page(g_walk_ptrs_a[i], g_walk_ptrs_a[i], page,
                     g_walk_ptrs_b, &n2n, TD_WALK_PTR_CAP);
    }
    vr_log("  walk pass1: %llu of %llu read, %llu refused, n2n=%llu",
           (unsigned long long)r1ok, (unsigned long long)n1,
           (unsigned long long)r1fail, (unsigned long long)n2n);
    g_walk_ptrs += n2n;

    // Pass 2. No pointer collection: a third level of pointers can reach
    // anywhere in the address space, and the budget is a poor guard against
    // that when every object is 4 KB. SDB hits only.
    // Pass 1. Bounded by object count, not by bytes. v23 tried to read every
    // level-2 pointer and stopped the render thread; v24 capped it and still
    // died, so the cap was the wrong variable. What actually matters is how
    // many pages of the heap we are willing to touch inside one frame, and at
    // 144 Hz that is a small number.
    //
    // Level 2 walks only the first 256 level-2 pointers, which is 1 MB of
    // reads. If the SceneDynamicBuffer is not in the first 256 objects two
    // hops from the renderer, the walk will not find it — and saying so is
    // worth more than reading 30 MB to find out, because a walk that stalls
    // the game cannot be measured at all.
    uint64_t r2ok = 0, r2fail = 0, l2 = n2n < TD_WALK_CAP_L2 ? n2n : TD_WALK_CAP_L2;
    for (uint64_t i = 0; i < l2 && budget >= TD_WALK_READ; ++i) {
        if (td_read(g_walk_ptrs_b[i], page, TD_WALK_READ) != TD_WALK_READ) {
            r2fail++;
            continue;
        }
        budget -= TD_WALK_READ;
        g_walk_bytes += TD_WALK_READ;
        r2ok++;
        td_walk_page(g_walk_ptrs_b[i], g_walk_ptrs_b[i], page,
                     NULL, NULL, 0);
    }
    vr_log("  walk pass2: %llu of %llu read, %llu refused%s", (unsigned long long)r2ok,
           (unsigned long long)n2n, (unsigned long long)r2fail,
           n2n > TD_WALK_CAP_L2 ? "  [CAPPED]" : "");
}

static void td_walk_report(void) {
    // The walk runs every 300 frames and this report every 120, so a report
    // between two walks prints the previous walk's numbers. That reads as
    // "0 KB read" on a walk that has not happened yet, which cost an hour of
    // debugging already. Print the sequence number, and skip the report
    // entirely when no walk has run yet.
    if (g_walk_seq == 0) return;
    vr_log("--- sdb walk #%llu: %ld hits, %llu ptrs, %llu KB read ---",
           (unsigned long long)g_walk_seq,
           (long)g_walk_n, (unsigned long long)g_walk_ptrs,
           (unsigned long long)(g_walk_bytes / 1024));
    for (LONG i = 0; i < g_walk_n && i < TD_WALK_MAX_HITS; ++i) {
        vr_log("  SDB %016llX via %016llX  pd=(%.5f %.5f %.5f %.5f) maxVP=%.2f",
               (unsigned long long)g_walk_hits[i].addr,
               (unsigned long long)g_walk_hits[i].owner,
               (double)g_walk_hits[i].pd[0], (double)g_walk_hits[i].pd[1],
               (double)g_walk_hits[i].pd[2], (double)g_walk_hits[i].pd[3],
               (double)g_walk_hits[i].max_vp);
    }
}

#endif /* TDVR_SDB_WALK_H */
