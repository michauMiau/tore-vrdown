// ---------------------------------------------------------------------------
// sdb_probe.h - is 0x087F40 running at all, and if not, who is?
//
// The bind hook at 0x0887B0 is installed and correct — the offline test proved
// the bytes, and the runtime log shows it patched at the right address. But
// bind=0 over 13200 frames. Three explanations, and they need different fixes:
//
//   1. 0x087F40 never runs (wrong function, or only runs in a different mode)
//   2. 0x087F40 runs, but the SDB branch at 0x0887A5 is skipped this frame
//   3. 0x087F40 runs rarely — level load, camera cut, or a throttled path
//
// Counting entry hits on 0x087F40 and on its caller 0x070B20 separates these.
// If the entry count is 0 the whole branch is wrong; if it is large and bind
// stays 0, the branch at 0x0887A5 is conditional.
//
// 0x087F40's prologue cannot be stolen: it runs 28 bytes to `sub rsp, 0x450`.
// The probe therefore hooks the *call site* 0x071102 inside 0x070B20, which is
// a five-byte E8 rel32, and separately the call sites of 0x070B20 inside
// 0x071CE0. Call-site hooks need no prologue at all.
// ---------------------------------------------------------------------------
#ifndef TDVR_SDB_PROBE_H
#define TDVR_SDB_PROBE_H

#include <stdint.h>

// Locate a direct `call 0xNNNN` by its target, searching forward only from a
// seed inside the containing function. Returns the address of the E8 byte.
//
// Forward-only matters: an earlier version scanned backwards and found a
// similar-looking sequence 0xA2 bytes earlier, which produced a resolver that
// passed offline and hooked the wrong function at runtime.
static uint8_t* tdvr_find_call_to(uint8_t* image, size_t size,
                                  uint8_t* fn_start, size_t fn_len,
                                  uint32_t target_rva) {
    for (size_t i = 0; i + 5 <= fn_len; ++i) {
        uint8_t* p = fn_start + i;
        if (p[0] != 0xE8) continue;
        int32_t rel = (int32_t)((uint32_t)p[1] | ((uint32_t)p[2] << 8)
                                | ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24));
        uint8_t* dest = p + 5 + rel;
        if (dest < image || dest >= image + size) continue;
        if ((uint32_t)(dest - image) != target_rva) continue;
        return p;
    }
    return NULL;
}

#endif /* TDVR_SDB_PROBE_H */
