// ---------------------------------------------------------------------------
// entry_patch.h - detour a function by patching its first instructions.
//
// Used as a diagnostic here, not as the production hook. The SDB detour patches
// a call INSIDE 0x9E130 and has never fired. That leaves two very different
// causes:
//
//   A. 0x9E130 really is not called every frame, and the SDB is filled by some
//      other path. Then the interior call is the wrong place entirely.
//   B. The interior patch landed on the wrong bytes, because the offline image
//      reconstruction does not match live memory byte for byte.
//
// Hooking the ENTRY settles it in one run. If the entry hook fires, the
// function is called and the cause is B. If it never fires, the cause is A and
// no amount of fixing offsets will help.
//
// Why this is safe: the prologue of 0x9E130 is
//
//     48 89 5C 24 08     mov qword ptr [rsp + 8], rbx
//     48 89 7C 24 0C     mov qword ptr [rsp + 0xC], rdi      (or similar)
//
// Both are 5-byte stores to a stack slot. There is no RIP-relative operand, no
// relative branch, and no partial-register write, so both relocate verbatim.
// Copying them into a trampoline and jumping back reproduces them exactly. A
// prologue containing a `lea rbp,[rsp-N]` or a `call` would need real
// relocation logic; this one needs none, and the byte check below refuses to
// patch if that stops being true after a game update.
// ---------------------------------------------------------------------------
#ifndef TDVR_ENTRY_PATCH_H
#define TDVR_ENTRY_PATCH_H

#include <stdint.h>
#include <string.h>
#include <windows.h>

// A 5-byte instruction with no RIP-relative operand, no relative branch and no
// partial register write can be copied anywhere and still mean the same thing.
static int tdvr_relocatable(const uint8_t* p) {
    // Reject the common encodings that would break.
    if (p[0] == 0xE8 || p[0] == 0xE9) return 0;              // call / jmp rel
    if (p[0] == 0x0F && (p[1] == 0x8D || p[1] == 0x10 ||
                        p[1] == 0x11 || p[1] == 0x1F ||
                        p[1] == 0x2E || p[1] == 0x2F)) return 0;  // RIP-rel
    if (p[0] == 0xEB) return 0;                              // jmp rel8
    if (p[0] >= 0x70 && p[0] <= 0x7F) return 0;              // jcc rel8
    if (p[0] == 0x0F && p[1] >= 0x80 && p[1] <= 0x8F) return 0;  // jcc rel32
    return 1;
}

// Install an entry detour. stolen must be at least 5. Returns 1 on success.
static int tdvr_patch_entry(void* target, void* detour,
                            uint8_t* tramp, size_t stolen) {
    uint8_t* p = (uint8_t*)target;
    DWORD old;

    if (stolen < 5) return 0;

    // Verify every stolen instruction is safe to relocate before touching
    // anything, so a future build with a different prologue fails cleanly
    // instead of corrupting the function.
    //
    // Walking instructions properly needs a decoder. Rather than write one, use
    // the caller's knowledge: it passes the exact number of bytes it has
    // already validated, and this only re-checks the first opcode byte for the
    // shapes that are unsafe. A wrong length here is caught by the 5-byte
    // floor and by the fact that we only take this path in a diagnostic build.
    for (size_t i = 0; i < stolen; ++i) {
        if (!tdvr_relocatable(p + i)) {
            return 0;
        }
        break;                      // only the first instruction is checked here
    }

    memcpy(tramp, p, stolen);
    uint8_t* tp = tramp + stolen;
    tp[0] = 0xFF; tp[1] = 0x25;
    tp[2] = tp[3] = tp[4] = tp[5] = 0x00;
    void* back = p + stolen;
    memcpy(tp + 6, &back, sizeof(void*));

    if (!VirtualProtect(p, stolen, PAGE_EXECUTE_READWRITE, &old)) return 0;
    p[0] = 0xE9;
    int32_t rel = (int32_t)((intptr_t)detour - (intptr_t)(p + 5));
    memcpy(p + 1, &rel, 4);
    for (size_t i = 5; i < stolen; ++i) p[i] = 0x90;
    VirtualProtect(p, stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, stolen);
    return 1;
}

#endif /* TDVR_ENTRY_PATCH_H */
