// ---------------------------------------------------------------------------
// scene_bind.h - install the per-frame SceneDynamicBuffer bind hook.
//
// The matcher lives in scene_bind_find.h so it can be compiled and tested on
// the analysis machine, which has no windows.h. It is pure byte matching: no
// Windows API, no globals, no side effects. This header only adds the install.
//
// The reasoning behind the hook target, in short, because getting this wrong
// cost the project a working-looking build:
//
// 0x9E130 matched a perfect signature — 500 bytes, the layout from the HLSL
// declaration, UpdateSubresources with size 0x1F4 — and two live hooks then
// showed it never runs per frame. 0 hits over 11760 frames. A green build, a
// healthy 144 FPS process, and no stereo.
//
// 0x087F40 is the function that actually runs, and it is the only one in the
// whole 33 MB image that reads renderer+0xB90. Full detail in
// scene_bind_find.h and docs/SDB_BREAKTHROUGH_RESOURCE_TYPE.md.
// ---------------------------------------------------------------------------
#ifndef TDVR_SCENE_BIND_H
#define TDVR_SCENE_BIND_H

#include <stdint.h>
#include <string.h>
#include <windows.h>

#include "scene_bind_find.h"

// Install the detour on the SDB descriptor bind.
//
// rcx is the 24-byte descriptor destination and rdx the SDB resource, both
// already live at the call site, so the detour forwards them unchanged and
// needs no register decoding beyond passing two arguments through.
//
// Returns 1 on success; out_orig receives the descriptor constructor, and
// out_site the patched call site, for logging.
static int tdvr_install_sdb_bind(void* image, size_t size, void* detour,
                                 void** out_orig, void** out_site) {
    void* ctor = NULL;
    uint8_t* site = tdvr_find_sdb_bind((uint8_t*)image, size, &ctor);
    if (!site) return 0;

    // Reuse the direct-call patcher: E8 rel32, five bytes, one instruction.
    // A prologue steal is not an option — 0x087F40's prologue runs to 0x087F5D,
    // 28 bytes and past `sub rsp, 0x450`, so copying part of it would leave the
    // stack frame unallocated.
    if (!tdvr_patch_direct_call(site, detour, out_orig)) return 0;
    if (out_site) *out_site = site;
    return 1;
}

#endif /* TDVR_SCENE_BIND_H */
