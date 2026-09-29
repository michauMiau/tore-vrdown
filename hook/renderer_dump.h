// ---------------------------------------------------------------------------
// renderer_dump.h - snapshot the renderer object while a frame is in flight.
//
// Every attempt to find the SceneDynamicBuffer by address has failed for the
// same underlying reason: it was looking during a moment when the buffer's
// CPU-side copy does not exist, or exists somewhere other than where the
// address-based reasoning assumed. Static analysis of 0x087F40 — the one
// function in the image that reads renderer+0xB90 — produced a correct hook that
// was installed at the right address and never fired once in 13200 frames.
// The block is straight-line, so its neighbour 0x9E130 is unreachable too,
// which is why the earlier entry hook also saw zero.
//
// This inverts the approach. endRender runs every frame — it is on the vtable
// and it is where Present is called. Inside it, walk a bounded region of the
// renderer and copy it out. Then search the copy offline for the SDB by its
// contents rather than by its address: five float4x4 identity matrices in a
// row is a signature no other field in the object will have.
//
// The dump is the measurement that the whole project was missing. Everything
// before it was inference about a buffer nobody had ever read.
// ---------------------------------------------------------------------------
#ifndef TDVR_RENDERER_DUMP_H
#define TDVR_RENDERER_DUMP_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TDVR_DUMP_BYTES   0x3000      // first 12 KB of the renderer
#define TDVR_DUMP_MAGIC   "TDRDUMP1"

// Header written before the payload, so the reader can tell a truncated dump
// from a complete one.
#pragma pack(push, 1)
typedef struct {
    char     magic[8];
    uint64_t renderer;
    uint64_t when_frames;
    uint32_t size;
    uint32_t count;
} tdvr_dump_header;
#pragma pack(pop)

// Copy a bounded region of the renderer. Called from the endRender detour,
// where a frame is definitely in progress and a heap snapshot is safe.
//
// Reading live memory can fault if the region is not committed, so read in
// 4 KB steps and count how many actually succeeded. A short dump is still
// useful; a crashed process is not.
static void tdvr_dump_renderer(void* renderer, uint64_t frame) {
    tdvr_dump_header h;
    memcpy(h.magic, TDVR_DUMP_MAGIC, 8);
    h.renderer = (uint64_t)(uintptr_t)renderer;
    h.when_frames = frame;
    h.size = TDVR_DUMP_BYTES;
    h.count = 0;

    // Read in 4 KB steps, checking each region is committed and readable first.
    //
    // The obvious choice here is __try/__except, but this DLL is built with
    // mingw, where SEH is not available in user code without extra ceremony, so
    // the guard is done with VirtualQuery instead. That is weaker in one
    // respect — a race could still fault — and stronger in another: no SEH at
    // all, so nothing can longjmp out of the middle of a frame's render call.
    static unsigned char buf[TDVR_DUMP_BYTES];
    uint32_t got = 0;
    const unsigned char* base = (const unsigned char*)renderer;
    for (uint32_t off = 0; off < TDVR_DUMP_BYTES; off += 0x1000) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(base + off, &mbi, sizeof mbi) != sizeof mbi) break;
        if (mbi.State != MEM_COMMIT) break;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) break;
        if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                             PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            break;
        // Bytes left in this region from the current address. If a page ends
        // the region early, read only what is there — comparing against
        // RegionSize directly is wrong, because off counts from the start of
        // the object, not from BaseAddress.
        size_t left = (size_t)((const unsigned char*)mbi.BaseAddress
                               + mbi.RegionSize) - (size_t)(base + off);
        size_t take = left < 0x1000 ? left : 0x1000;
        if (take == 0) break;
        memcpy(buf + off, base + off, take);
        got += (uint32_t)take;
        if (take < 0x1000) break;   // region ended, nothing more to read
    }
    h.count = got;
    h.size = got;

    // Overwrite: keep the newest dump, one file. The offline search is the
    // expensive part, so it runs once against this.
    FILE* f = NULL;
    if (fopen_s(&f, "renderer_dump.bin", "wb") != 0 || !f) return;
    fwrite(&h, sizeof h, 1, f);
    fwrite(buf, got, 1, f);
    fclose(f);
}

#endif /* TDVR_RENDERER_DUMP_H */
