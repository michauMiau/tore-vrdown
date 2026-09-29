// ---------------------------------------------------------------------------
// hot.h - count what actually runs per frame.
//
// Everything in this project has been an inference about which function is on
// the frame path, and every one of them has been wrong. 0x9E130 built the SDB
// with a perfect signature match and was never called. 0x087F40 was the only
// function in the image reading renderer+0xB90, hooked at the verified address,
// and it fired 0 times in 13200 frames. The v18 dump then showed +0xB80..+0xB98
// all null — the buffers do not exist in that object at all.
//
// So stop inferring. Hook a spread of call sites across the plausible render
// and update paths, let the game run, and read the counters. The functions that
// count in the thousands are the frame path; the ones that stay at zero are
// setup or dead code, and the whole project stops guessing about them.
//
// Every hook here is on a call site (E8 rel32, five bytes) or a vtable slot, so
// there is no prologue to steal and no instruction to misalign. A refused or
// unmatched site is logged and skipped rather than patched on a guess.
// ---------------------------------------------------------------------------
#ifndef TDVR_HOT_H
#define TDVR_HOT_H

#include <stdint.h>

#define TD_HOT_MAX 40

typedef struct {
    uint32_t rva;        // the call site, for reporting
    uint32_t target;     // what it calls, for reporting
    const char* label;
    volatile LONG* hits;
    void* orig;
} td_hot_site;

// One counter per site. The table is small and fixed so the log can print it
// wholesale, which is the point: a single dump answers "what is per frame".
extern td_hot_site g_hot[TD_HOT_MAX];
extern volatile LONG g_hot_n;

// Disassembly-verified call sites, chosen to cover every hypothesis about where
// the frame path lives.
//
// Every entry here was checked offline against the real image: the byte at the
// offset must be E8 and rel32 must resolve to the stated target. That check
// rejected six of the twenty-one candidates — the 0x5B3E76 / 0x5B49AF / 0x5B4F00
// entries were addresses of `mov r8d, 0x40` immediates, not call sites, because
// an earlier note had conflated "where the size is set" with "where the call
// is". A wrong entry here is a detour on the wrong code, which is how the
// earlier callee trace crashed the game.
//
// If a seed's target stops matching at runtime, that seed is skipped with a
// log line, not patched. A stale RVA must not become a wrong patch.
typedef struct { uint32_t call_rva, target_rva; const char* label; } td_hot_seed;

static const td_hot_seed TD_HOT_SEEDS[] = {
    // From the live disassembly of the two frame hooks.
    { 0x5B35BC, 0x511FA0, "beginFrame: alloc, stores this+0xF70" },
    { 0x5B35FB, 0x5AA4C0, "beginFrame: 0x5AA4C0" },
    { 0x5B6E72, 0x5BB3B0, "endFrame: 0x5BB3B0" },
    { 0x5B6E8F, 0x5BAEB0, "endFrame: 0x5BAEB0" },
    { 0x5B6EA2, 0x5BA8B0, "endFrame: 0x5BA8B0" },
    { 0x5B6EF5, 0x6997C0, "endFrame: 0x6997C0" },
    { 0x5B6F11, 0x5982C0, "endFrame: 0x5982C0" },
    { 0x5B6F16, 0x699270, "endFrame: 0x699270" },
    { 0x5B6F3A, 0x4FFCA0, "endFrame: 0x4FFCA0" },
    { 0x5B70E7, 0x5BC180, "endFrame: 0x5BC180" },

    // From 0x087F40, the function that owns the scene buffers statically.
    { 0x088129, 0x080D10, "0x087F40: 0x080D10" },
    { 0x088696, 0x092B80, "0x087F40: 0x092B80 uploader" },
    { 0x08869E, 0x09E130, "0x087F40: 0x9E130 SDB builder" },
    { 0x088732, 0x5EB8C0, "0x087F40: 0x5EB8C0" },
    { 0x08873D, 0x5EB8B0, "0x087F40: 0x5EB8B0" },
};

#define TD_HOT_N ((int)(sizeof TD_HOT_SEEDS / sizeof TD_HOT_SEEDS[0]))

#endif /* TDVR_HOT_H */
