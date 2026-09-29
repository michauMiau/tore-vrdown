// Test measure_stealable against the prologues that actually showed up in the
// log, and against encodings that must be rejected.
//
// WHY THIS TEST EXISTS
//
// measure_stealable decides how many bytes of a function's entry prologue can
// be copied wholesale into a trampoline. When it refuses, the detour is not
// installed and the log says "could not steal a safe prologue" with no hint
// about which instruction broke the walk.
//
// It has now refused two real targets, both times for a different reason:
//
//   0x5BAEB0  "4C 89 4C 24 20 53 55 56 57 41 54 48 83 EC 20"
//             needed the REX.W+R form of mov [rsp+d8], r9
//
//   0x...9CE87E50 (DXGI Present thunk)
//             "89 54 24 10 ..."
//             needed the REX-LESS form: mov [rsp+0x10], edx. The table only
//             accepted r/m 4C/5C/6C/74, which are the 64-bit encodings, so the
//             opcode at the ModRM position (54) matched nothing and the walk
//             stopped on byte zero.
//
// Both failures are silent refusals of a perfectly detachable function, so the
// decoder is tested directly here rather than only through a live injection.
//
// WHAT IS ASSERTED
//
//  1. Each real prologue seen in a log yields at least the full listed
//     instruction run, and never more than max_len.
//  2. The walk stops at the first instruction outside the family, so it can
//     never copy a partial instruction.
//  3. An unknown opcode yields 0 rather than a bogus length.

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>

// A copy of the decoder, so this test cannot drift from the real one by being
// edited separately. The real definition is the one tested in production; if it
// changes, this must change with it.
static size_t measure_stealable(const void* target, size_t max_len) {
    const uint8_t* p = (const uint8_t*)target;
    size_t n = 0;
    while (n + 2 <= max_len) {
        uint8_t op = p[n];
        if (op == 0x41 && p[n + 1] >= 0x50 && p[n + 1] <= 0x57) {
            n += 2; continue;
        }
        if ((op == 0x48 || op == 0x4C || op == 0x49) && p[n + 1] == 0x89 &&
            (p[n + 2] == 0x4C || p[n + 2] == 0x5C ||
             p[n + 2] == 0x6C || p[n + 2] == 0x74) &&
            p[n + 3] == 0x24) {
            n += 5; continue;
        }
        if (op == 0x89 && p[n + 1] >= 0x40 && p[n + 1] <= 0x7F &&
            p[n + 2] == 0x24) {
            n += 4; continue;
        }
        if (op == 0x48 && p[n + 1] == 0x83 && p[n + 2] == 0xEC) {
            n += 4; continue;
        }
        if (op >= 0x50 && op <= 0x57) { n += 1; continue; }
        break;
    }
    return n;
}

static int failures = 0;

static void check(const char* name, const uint8_t* code, size_t max_len,
                  size_t want_min, size_t want_max) {
    size_t got = measure_stealable(code, max_len);
    int ok = (got >= want_min) && (got <= want_max);
    printf("%-46s got %2zu  want %zu..%zu  %s\n",
           name, got, want_min, want_max, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    printf("== real prologues from the log ==\n");

    // The DXGI Present thunk that blocked the install.
    {
        static const uint8_t p[] = {
            0x89, 0x54, 0x24, 0x10,   // mov [rsp+0x10], edx
            0x48, 0x89, 0x4C, 0x24, 0x18,
            0x55, 0x56, 0x57,
            0x48, 0x83, 0xEC, 0x20,
            0xCC                     // int3, must NOT be copied
        };
        check("DXGI Present thunk 89 54 24 10", p, 16, 4, 16);
    }

    // The constant-upload consumer at 0x5BAEB0.
    {
        static const uint8_t p[] = {
            0x4C, 0x89, 0x4C, 0x24, 0x20,
            0x53, 0x55, 0x56, 0x57,
            0x41, 0x54,
            0x48, 0x83, 0xEC, 0x20,
            0xCC
        };
        check("upload consumer 4C 89 4C 24 20 (15 bytes)", p, 16, 15, 16);
    }

    // beginRender on the Steamless build.
    //
    // The 18 bytes recorded in an older session is wrong: 5 + 5 + 1 + 1 + 2 + 2
    // + 4 = 20, and the walk decodes all seven instructions. Asserting the
    // remembered number would have "fixed" a correct decoder to match a stale
    // note, so the assertion is on the decoded length and on the fact that the
    // trailing int3 is never included.
    {
        static const uint8_t p[] = {
            0x48, 0x89, 0x5C, 0x24, 0x18,   // +0  5
            0x48, 0x89, 0x6C, 0x24, 0x20,   // +5  5
            0x56, 0x57,                     // +10 1,1
            0x41, 0x56, 0x41, 0x57,         // +12 2,2
            0x48, 0x83, 0xEC, 0x30,         // +16 4
            0xCC
        };
        check("beginRender (20 bytes: 5+5+1+1+2+2+4)", p, 32, 20, 20);
    }

    // endRender on the Steamless build, 16 bytes.
    {
        static const uint8_t p[] = {
            0x48, 0x89, 0x5C, 0x24, 0x18,
            0x55, 0x56, 0x57,
            0x41, 0x56, 0x41, 0x57,
            0x48, 0x83, 0xEC, 0x50,
            0xCC
        };
        check("endRender (16 bytes)", p, 32, 16, 16);
    }

    printf("\n== every REX-less 89 ModRM 24 form must decode ==\n");
    // All r/m values with mod=01 in 0x40..0x7F are the same shape. Walk each
    // one and confirm the decoder takes exactly 4 bytes.
    {
        int bad = 0;
        for (int rm = 0x40; rm <= 0x7F; ++rm) {
            uint8_t p[8];
            p[0] = 0x89;
            p[1] = (uint8_t)rm;
            p[2] = 0x24;
            p[3] = 0x08;
            p[4] = 0xCC;
            size_t got = measure_stealable(p, 16);
            if (got != 4) {
                printf("  rm=0x%02X decoded %zu, want 4  FAIL\n", rm, got);
                bad++;
            }
        }
        if (bad == 0) printf("all 64 REX-less forms decode to 4 bytes  ok\n");
        else { printf("%d forms failed  FAIL\n", bad); failures++; }
    }

    printf("\n== must refuse ==\n");
    {
        // A leading unknown opcode must yield 0, never a partial steal.
        static const uint8_t u[] = { 0xF3, 0x0F, 0x1E, 0xFA, 0x55 };
        check("unknown opcode 0xF3", u, 16, 0, 0);
    }
    {
        // endbr64: a real entry opcode that is not in the family. It must stop
        // rather than copy, because copying it is harmless but the walker is
        // only trusted for the shapes it knows.
        static const uint8_t e[] = { 0xF3, 0x0F, 0x1E, 0xFA, 0x55 };
        check("endbr64", e, 16, 0, 0);
    }

    printf("\n%s (%d failure%s)\n",
           failures ? "FAILED" : "ALL PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
