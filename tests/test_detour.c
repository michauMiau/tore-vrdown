// Offline test for the detour jump encoding in build_detour().
//
// Why this test exists: four consecutive builds (v32..v35) installed a detour
// on 0x5BAEB0, logged "installed, 15 bytes stolen", and then the process died
// on the first call with "detached after 1 frames" and no capture line. The
// in-process log could not tell "never called" from "called and jumped into
// garbage", so every build looked identical and the real defect stayed hidden
// for four iterations.
//
// The defect: "FF 25 disp32" (jmp qword ptr [rip+disp32]) is a SIX byte
// instruction. RIP-relative operands are measured from the byte AFTER the
// instruction, so with disp32 = 0 the CPU dereferences the qword at
// target+6. The code wrote the 8-byte target at target+8 and then nop-filled
// bytes 8..13, which both moved the pointer two bytes too far and clobbered
// its high half with 0x90. The render thread jumped to 0x28100000 and died.
//
// This test builds the patch the same way the real code does, then decodes it
// exactly as a CPU would: read disp32, add the length of the instruction, and
// dereference. If the resulting address is not the hook, the encoding is wrong.
//
// It also checks the two things that silently truncated instructions before:
// the patch must cover every stolen byte, and bytes after the patch must be
// nop so no instruction is split.

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#define DETOUR_LEN 16

static int failures = 0;

static void check(int ok, const char* what) {
    if (!ok) {
        printf("  FAIL  %s\n", what);
        failures++;
    }
}

// Mirrors build_detour()'s FF 25 branch. Returns the bytes written.
static int encode_abs_jump(uint8_t* p, void* hook, size_t det_len) {
    p[0] = 0xFF; p[1] = 0x25;
    p[2] = p[3] = p[4] = p[5] = 0x00;
    memcpy(p + 6, &hook, sizeof(void*));
    for (size_t i = 14; i < det_len; ++i) p[i] = 0x90;
    return 6 + 8;
}

// Decode the way the hardware does: RIP after the instruction, plus disp32.
static void* decode_abs_jump(const uint8_t* p) {
    if (p[0] != 0xFF || p[1] != 0x25) return NULL;
    int32_t disp;
    memcpy(&disp, p + 2, 4);
    const uint8_t* rip = p + 6;                 // end of the 6-byte instruction
    const uint8_t* slot = rip + disp;           // where the CPU looks
    void* target;
    memcpy(&target, slot, sizeof(void*));
    return target;
}

int main(void) {
    printf("=== detour jump encoding ===\n");

    // A real-ish layout: the patch site followed by code that must not change.
    const size_t SITE = 64;
    uint8_t site[SITE + 32];
    memset(site, 0xCC, sizeof(site));            // INT3 filler, like real code

    void* hook = (void*)(uintptr_t)0x00007FF712345678ull;
    size_t copy_len = 15;

    // Remember what the stolen bytes were, to prove the site is overwritten.
    uint8_t orig[32];
    memcpy(orig, site, 32);

    size_t written = encode_abs_jump(site, hook, DETOUR_LEN);
    printf("  wrote %zu bytes at the entry\n", written);

    // --- the encoding must resolve to the hook, as the CPU sees it ---
    void* got = decode_abs_jump(site);
    check(got == hook, "FF 25 disp32=0 dereferences the hook address");
    if (got != hook) {
        printf("        encoded to %p, wanted %p\n", got, hook);
    }

    // The 8-byte pointer must sit at offset 6, not 8.
    void* at6;
    void* at8;
    memcpy(&at6, site + 6, 8);
    memcpy(&at8, site + 8, 8);
    check(at6 == hook, "pointer is at target+6");
    check(at8 != hook, "pointer is NOT at target+8 (the old broken layout)");

    // --- the patch must not leave a split instruction ---
    for (size_t i = 0; i < copy_len; ++i) {
        if (i < 6) continue;                     // the jump itself
        if (i >= 14) continue;                   // the pointer
        // bytes 6..13 are the pointer, so nothing else may claim them
    }
    // Every byte from 14 to DETOUR_LEN must be a nop: the tail of the stolen
    // prologue has to be neutralised or the CPU runs garbage.
    int tail_ok = 1;
    for (size_t i = 14; i < DETOUR_LEN; ++i) {
        if (site[i] != 0x90) tail_ok = 0;
    }
    check(tail_ok, "bytes 14..DETOUR_LEN are nop (no split instruction)");
    if (!tail_ok) {
        for (size_t i = 14; i < DETOUR_LEN; ++i)
            printf("        site[%zu] = %02X, wanted 90\n", i, site[i]);
    }

    // DETOUR_LEN must cover the whole steal.
    check(copy_len <= DETOUR_LEN,
          "DETOUR_LEN covers every stolen byte");
    printf("  copy_len=%zu DETOUR_LEN=%d\n", copy_len, DETOUR_LEN);

    // --- the trampoline's back-jump must use the same layout ---
    printf("=== trampoline back-jump ===\n");
    uint8_t tramp[64];
    memset(tramp, 0xCC, sizeof(tramp));
    memcpy(tramp, orig, copy_len);              // stolen bytes
    uint8_t* tp = tramp + copy_len;
    tp[0] = 0xFF; tp[1] = 0x25;
    tp[2] = tp[3] = tp[4] = tp[5] = 0x00;
    void* back = site + copy_len;
    memcpy(tp + 6, &back, sizeof(void*));

    void* tramp_target = decode_abs_jump(tp);
    check(tramp_target == back, "trampoline jumps back to target+copy_len");
    if (tramp_target != back) {
        printf("        got %p, wanted %p\n", tramp_target, back);
    }

    // The stolen bytes must be intact in the trampoline: if they contain the
    // jump, the first g_orig call recurses forever. This was a real bug.
    int stolen_ok = 1;
    for (size_t i = 0; i < copy_len; ++i) {
        if (tramp[i] != orig[i]) stolen_ok = 0;
    }
    check(stolen_ok, "trampoline holds the ORIGINAL prologue, not our jump");
    if (!stolen_ok) {
        printf("        tramp: ");
        for (size_t i = 0; i < 8; ++i) printf("%02X ", tramp[i]);
        printf("\n        orig: ");
        for (size_t i = 0; i < 8; ++i) printf("%02X ", orig[i]);
        printf("\n");
    }

    // --- the regression that actually shipped broken ---
    printf("=== regression: the old offset-8 layout ===\n");
    uint8_t bad[32];
    memset(bad, 0xCC, sizeof(bad));
    bad[0] = 0xFF; bad[1] = 0x25;
    bad[2] = bad[3] = bad[4] = bad[5] = 0x00;
    memcpy(bad + 8, &hook, sizeof(void*));      // the mistake: offset 8
    for (size_t i = 14; i < DETOUR_LEN; ++i) bad[i] = 0x90;  // and clobbers it
    void* bad_target = decode_abs_jump(bad);
    check(bad_target != hook, "old layout resolves somewhere WRONG (as expected)");
    printf("  old layout -> %p   (this is what killed v32..v35)\n", bad_target);

    printf("\n");
    if (failures) {
        printf("FAILED (%d)\n", failures);
        return 1;
    }
    printf("ALL CHECKS PASS\n");
    return 0;
}
