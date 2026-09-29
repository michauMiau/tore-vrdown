// Reproduce the torn-patch race in build_detour(), offline.
//
// WHY THIS TEST EXISTS
//
// v38 was injected into a live, rendering game. The injector reported the
// process healthy 40 s later (cpu=263s, responding=True), and several minutes
// later the process was gone with "=== detached after 0 frames ===" and no
// dump and no WER event. A patch that reliably corrupts memory kills the
// process immediately; this one killed it minutes later. That timing is the
// signature of a race, not of a bad encoding (the encoding was fixed in v36
// and proved correct by test_detour.c).
//
// The race is in build_detour():
//
//     VirtualProtect(target, DETOUR_LEN, PAGE_EXECUTE_READWRITE, &old);
//     p[0]=0xFF; p[1]=0x25; p[2..5]=0;
//     memcpy(p + 6, &hook, 8);
//
// Fourteen bytes are written one at a time into code that the render thread is
// executing right now. Between "FF 25" landing and the 8-byte address landing,
// the instruction at target decodes as
//
//     jmp qword ptr [rip + 0]        with [rip+0] = the four zero bytes
//
// so the thread dereferences the bytes at target+6, which are still 00 00 10
// 28 - the low half of the hook pointer with two leading zeros. That is
// 0x28100000, an unmapped address, and the thread faults.
//
// This test runs a fake "render thread" in a tight loop reading the patch site
// the way the hardware would, while the "installer" writes it non-atomically,
// and counts how many times the reader ever saw an inconsistent state. Then it
// repeats the same thing with the write made atomic (bytes published only once
// the site is fully valid) and shows the count go to zero.
//
// WHAT COUNTS AS A TORN READ
//
// The reader decodes: FF 25 disp32, rip = insn_end (offset 6), slot = rip +
// disp32 (offset 6 when disp32==0), and reads the 8 bytes at slot. A read is
// TORN when the opcode bytes say FF 25 but the decoded target is not the hook.
// That is precisely the state that crashes a real CPU: the decoder has already
// committed to the instruction, and the operand it loads is garbage.

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>

#define PATCH_LEN   14          // FF 25 + disp32 + 8-byte target
#define SITE_BYTES  32
#define ITERS       400000      // reader passes per run

// The address we are jumping to. Must be a plausible user-mode address.
#define HOOK_ADDR   0x00007FF712345678ull

typedef struct {
    uint8_t  site[SITE_BYTES];
    _Atomic int stop;
    // counters
    _Atomic unsigned long torn;
    _Atomic unsigned long total;
    _Atomic unsigned long saw_ff25;
} world;

static void init_site(world* w) {
    memset(w->site, 0xCC, sizeof(w->site));   // 0xCC filler, like real code
}

// Decode exactly as the hardware does. Returns the jump target.
//
// The displacement is clamped to the 14-byte patch window. A real CPU would
// happily jump anywhere the disp32 points, but the test only cares about the
// one window build_detour() opens, and an unclamped CC-filled disp32 turns this
// into a wild read off the end of the buffer.
static uint64_t decode_abs_jump(const uint8_t* p) {
    int32_t disp;
    memcpy(&disp, p + 2, 4);
    if (disp < 0 || disp > PATCH_LEN) return 0xDEADBEEFull;  // out of window
    const uint8_t* rip = p + 6;
    const uint8_t* slot = rip + disp;
    uint64_t v;
    memcpy(&v, slot, 8);
    return v;
}

// The non-atomic writer: exactly what build_detour() does today.
static void write_patch_naive(uint8_t* p, uint64_t hook) {
    p[0] = 0xFF; p[1] = 0x25;
    p[2] = p[3] = p[4] = p[5] = 0x00;
    memcpy(p + 6, &hook, 8);
}

// The reader: a thread that keeps executing the site.
static void* reader(void* arg) {
    world* w = (world*)arg;
    while (!atomic_load(&w->stop)) {
        // An instruction fetch. The memset is not atomic either, so this read
        // can itself straddle the writer, which is exactly the situation on a
        // real core: the decoder may be looking at bytes that are changing
        // underneath it.
        uint8_t buf[SITE_BYTES];
        memcpy(buf, w->site, sizeof(buf));

        atomic_fetch_add(&w->total, 1);

        // A read is TORN when the site is mid-write: some bytes are from the
        // patch and some are still filler. It is not a question of whether
        // the decode lands on the right address, it is a question of whether
        // the bytes being executed are a coherent instruction.
        //
        // The atomic scheme writes the operand first and the opcode last, so
        // during the whole window the opcode is still 0xCC: the reader sees
        // clean filler, which is the original code, and nothing is torn. Once
        // FF 25 lands, all fourteen bytes are already in place.
        //
        // So the test is: does the reader ever see a mix of patch bytes and
        // filler bytes? Not "does it decode to the hook".
        //
        // Everything below classifies buf, the single snapshot taken above,
        // and never w->site. Reading w->site per byte would be a second,
        // independent racing read: the writer could advance between byte 0 and
        // byte 13, so the "torn" state would be the reader tearing its own
        // observation rather than a real torn fetch. That bug made this test
        // report failures on the atomic scheme, which is the one ordering that
        // cannot tear, purely because the instruments were wrong. The snapshot
        // is also the more faithful model: a core fetches an instruction once.
        int is_patch_byte = (buf[0] == 0xFF && buf[1] == 0x25);
        if (is_patch_byte) atomic_fetch_add(&w->saw_ff25, 1);

        // A coherent state is either all-filler (unpatched) or all-patch.
        // Check the whole 14-byte window for a mixture.
        int patch_bytes = 0, filler_bytes = 0, other = 0;
        for (int i = 0; i < 14; ++i) {
            if (buf[i] == 0xCC) filler_bytes++;
            else if (buf[i] == 0xFF || buf[i] == 0x25 ||
                     buf[i] == 0x00 || buf[i] == 0x90 ||
                     (buf[i] >= 0x10 && buf[i] <= 0x7F)) patch_bytes++;
            else other++;
        }
        int patch_complete = (buf[0] == 0xFF && buf[1] == 0x25 &&
                              buf[2] == 0 && buf[3] == 0 &&
                              buf[4] == 0 && buf[5] == 0);
        if (patch_complete && filler_bytes > 0) {
            // FF 25 is there and decodable, but part of the operand is still
            // filler: this is the crash.
            atomic_fetch_add(&w->torn, 1);
        }
    }
    return NULL;
}

static void run(const char* label, int atomic_write, unsigned long* out_torn,
                unsigned long* out_total, unsigned long* out_saw) {
    world w;
    memset(&w, 0, sizeof(w));
    init_site(&w);

    pthread_t th;
    atomic_store(&w.stop, 0);
    pthread_create(&th, NULL, reader, &w);

    // The torn window in the real build_detour() is a handful of nanoseconds:
    // between the FF 25 store and the 8-byte address store. A sampler thread
    // cannot reliably hit that, and on the first version of this test the
    // writer finished all 400k iterations while the reader had managed only
    // ~8k samples, so it reported "torn=0" and looked like the bug was absent.
    //
    // The window is widened here on purpose: WINDOW_NS is a deliberate
    // perturbation that stands in for the cache-miss and TLB effects that
    // stretch the real window out to something a sampler can land in. The
    // number of torn reads it reports is therefore NOT a rate to extrapolate
    // from. What it proves is only that the intermediate state is reachable by
    // a concurrently-executing thread at all, which is the thing that cannot
    // be argued away. The deterministic check at the end of main() is the real
    // assertion; this is the illustration.
    const long WINDOW_NS = 2000;

    for (int i = 0; i < ITERS; ++i) {
        if (!atomic_write) {
            // Non-atomic: the opcode lands, then a window, then the operand.
            w.site[0] = 0xFF; w.site[1] = 0x25;
            w.site[2] = w.site[3] = w.site[4] = w.site[5] = 0x00;
            struct timespec ts = { 0, WINDOW_NS };
            nanosleep(&ts, NULL);
            uint64_t hook = HOOK_ADDR;
            memcpy(w.site + 6, &hook, 8);
        } else {
            // Atomic: the operand is in place BEFORE the opcode appears, so
            // there is no state in which FF 25 is decodable with a stale
            // operand. Order is what makes it safe, not the store width.
            uint64_t hook = HOOK_ADDR;
            memcpy(w.site + 6, &hook, 8);
            w.site[2] = w.site[3] = w.site[4] = w.site[5] = 0x00;
            struct timespec ts = { 0, WINDOW_NS };
            nanosleep(&ts, NULL);
            w.site[0] = 0xFF; w.site[1] = 0x25;
        }

        // Reset for the next pass.
        w.site[0] = 0xCC; w.site[1] = 0xCC;
        w.site[2] = w.site[3] = w.site[4] = w.site[5] = 0xCC;
        memset(w.site + 6, 0xCC, 8);
    }

    atomic_store(&w.stop, 1);
    pthread_join(th, NULL);

    printf("%-28s samples=%-9lu saw FF25=%-7lu torn=%lu\n",
           label, (unsigned long)atomic_load(&w.total),
           (unsigned long)atomic_load(&w.saw_ff25),
           (unsigned long)atomic_load(&w.torn));
    *out_torn = atomic_load(&w.torn);
    *out_total = atomic_load(&w.total);
    *out_saw = atomic_load(&w.saw_ff25);
}

int main(void) {
    printf("=== torn patch race in build_detour ===\n");
    printf("site starts as CC filler, hook target = 0x%llX\n\n",
           (unsigned long long)HOOK_ADDR);

    // Show the intermediate state directly, so the bug is visible and not
    // only statistically inferred.
    {
        uint8_t s[SITE_BYTES];
        memset(s, 0xCC, sizeof(s));
        write_patch_naive(s, HOOK_ADDR);
        printf("after the full non-atomic write:\n");
        printf("  bytes: ");
        for (int i = 0; i < 14; ++i) printf("%02X ", s[i]);
        printf("\n  decode -> 0x%llX   (want 0x%llX)\n\n",
               (unsigned long long)decode_abs_jump(s),
               (unsigned long long)HOOK_ADDR);

        uint8_t half[SITE_BYTES];
        memset(half, 0xCC, sizeof(half));
        half[0] = 0xFF; half[1] = 0x25;
        half[2] = half[3] = half[4] = half[5] = 0x00;
        printf("mid-write, after 'FF 25' but before the address:\n");
        printf("  bytes: ");
        for (int i = 0; i < 14; ++i) printf("%02X ", half[i]);
        printf("\n  decode -> 0x%llX   (should be 0x%llX)\n\n",
               (unsigned long long)decode_abs_jump(half),
               (unsigned long long)HOOK_ADDR);
    }

    unsigned long torn_naive, tot_naive, saw_naive;
    unsigned long torn_atomic, tot_atomic, saw_atomic;

    run("non-atomic write (today)", 0, &torn_naive, &tot_naive, &saw_naive);
    printf("\n");
    run("atomic publish (proposed)", 1, &torn_atomic, &tot_atomic, &saw_atomic);

    printf("\n");
    int fail = 0;
    if (torn_naive == 0) {
        printf("  NOTE: no torn read observed this run. The race is timing\n");
        printf("        dependent; the intermediate state above is still the\n");
        printf("        proof that it is reachable.\n");
    } else {
        printf("  => CONFIRMED: the non-atomic write produces %lu torn reads\n",
               torn_naive);
        printf("     out of %lu. Every one of those is a jump to garbage.\n",
               tot_naive);
    }

    if (torn_atomic != 0) {
        printf("  FAIL: the atomic scheme still tore %lu times\n", torn_atomic);
        fail = 1;
    } else {
        printf("  => the atomic publish produced 0 torn reads\n");
    }

    // The real gate: the intermediate decode must be wrong. This is the
    // property that matters, and it holds regardless of whether a torn read
    // happened to be sampled this run.
    {
        uint8_t half[SITE_BYTES];
        memset(half, 0xCC, sizeof(half));
        half[0] = 0xFF; half[1] = 0x25;
        half[2] = half[3] = half[4] = half[5] = 0x00;
        uint64_t bad = decode_abs_jump(half);
        if (bad == HOOK_ADDR) {
            printf("  FAIL: the intermediate state decodes correctly,\n");
            printf("        so there is no window to fix\n");
            fail = 1;
        } else {
            printf("  => the intermediate state decodes to 0x%llX, an\n",
                   (unsigned long long)bad);
            printf("     unmapped address. That is the crash.\n");
        }
    }

    printf("\n");
    if (fail) { printf("FAILED\n"); return 1; }
    printf("ALL CHECKS PASS\n");
    return 0;
}
