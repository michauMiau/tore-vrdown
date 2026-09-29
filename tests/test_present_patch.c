// Test the Present vtable patch logic without the game.
//
// This exists because the vtable write is the only place in the whole project
// where the mod writes to the game's memory. Everything else reads, forwards,
// or is a log line, so this one write deserves to be exercised against a
// synthetic vtable before it is ever pointed at a live process.
//
// It rebuilds the exact sequence td_install_present performs:
//   read the vtable pointer through the guarded reader
//   read slot 9
//   compare it against what the scan decided
//   refuse if it is not a valid code pointer
//   VirtualProtect the slot, write it, restore the protection
// and asserts the observable outcomes: a good swapchain gets hooked, a
// non-code pointer is refused, and a mismatched slot is refused.
//
// Build:  gcc -O1 -g -o test_present_patch test_present_patch.c
// Run:    ./test_present_patch

#include <stdio.h>
#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Stand-ins for the Windows API, so the test runs on the build machine.
typedef int     BOOL;
typedef void*   HANDLE_win;
typedef uintptr_t DWORD;
#define PAGE_READWRITE      0x04
#define PAGE_EXECUTE_READ   0x20

static int g_protect_calls = 0;
static int g_protect_failed = 0;

// Only the page containing this is treated as writable, which is what lets the
// test prove that the protect call actually happened and was restored.
// The fake vtable, the swapchain object and the code page must be three
// SEPARATE allocations. Leaving them to the same local array lets the stack
// layout overlap them, which made this test fail for reasons that had nothing
// to do with the code under test.
static uint8_t g_code_page[4096] __attribute__((aligned(64)));
static void*   g_vtable[16]      __attribute__((aligned(64)));
static uint64_t g_sc_storage[4]   __attribute__((aligned(64)));
static uint64_t g_sc2_storage[4]  __attribute__((aligned(64)));
static void*   g_other_vt[16]    __attribute__((aligned(64)));

static BOOL VirtualProtect(void* addr, size_t sz, DWORD np, DWORD* old) {
    g_protect_calls++;
    // Refuse anything that is not inside our fake page, so a bug that computed
    // a wrong address cannot silently "succeed".
    uint8_t* a = (uint8_t*)addr;
    // The write under test goes to the VTABLE, not to the code page, so the
    // fake VirtualProtect has to accept either location. Refusing the vtable
    // made the install path fail for a reason unrelated to its logic.
    const uint8_t* pages[3] = { g_code_page,
                                 (const uint8_t*)g_vtable,
                                 (const uint8_t*)g_other_vt };
    size_t sizes[3] = { sizeof g_code_page, sizeof g_vtable, sizeof g_other_vt };
    int inside = 0;
    for (int i = 0; i < 3; ++i) {
        if (a >= pages[i] && a + sz <= pages[i] + sizes[i]) { inside = 1; break; }
    }
    if (!inside) {
        g_protect_failed++;
        return 0;
    }
    *old = PAGE_EXECUTE_READ;
    return 1;
}

// The guarded reader. Stands in for td_read: succeeds only for addresses that
// fall inside a region we have registered, which is how the real one behaves
// with VirtualQuery.
// Five regions get registered: the code page, the swapchain, its vtable, the
// second swapchain and its rival vtable. Sized to match, because an undersized
// table silently overwrote the count and every later read saw garbage - which
// made this test fail for reasons unrelated to the code under test.
static uint64_t g_regions[8][2];
static int g_region_count = 0;

static void region_add(uintptr_t lo, uintptr_t hi) {
    g_regions[g_region_count][0] = lo;
    g_regions[g_region_count][1] = hi;
    g_region_count++;
}

static int td_read(uint64_t addr, void* out, size_t len) {
    for (int i = 0; i < g_region_count; ++i) {
        if (addr >= g_regions[i][0] &&
            addr + len <= g_regions[i][1]) {
            memcpy(out, (const void*)(uintptr_t)addr, len);
            return 1;
        }
    }
    return 0;
}

// A "code pointer" has to be aligned, non-null, inside a registered region and
// must not look like a float or a small integer. This mirrors the property the
// real filter relies on, without needing a module list.
static int td_is_code_ptr(const void* p) {
    uintptr_t a = (uintptr_t)p;
    if (!a) return 0;
    if (a & 7) return 0;           // code is aligned
    if (a < 0x10000) return 0;     // never a small integer or a float
    return td_read(a, &(uint32_t){0}, 4);
}

// ---------------------------------------------------------------------------
// The unit under test: a faithful copy of the install path in present.h.

#define TD_PRESENT_SLOT 9

static void* g_present_orig;

static int install_present(void* swapchain, void** expect_from_scan) {
    if (!swapchain) return 0;

    void* target = expect_from_scan ? expect_from_scan[TD_PRESENT_SLOT] : NULL;
    if (!target) {
        printf("  refuse: scan rejected %p\n", swapchain);
        return 0;
    }

    void** vt = NULL;
    if (!td_read((uint64_t)(uintptr_t)swapchain, &vt, sizeof vt) || !vt) {
        printf("  refuse: %p has no readable vtable pointer\n", swapchain);
        return 0;
    }

    void* real = NULL;
    if (!td_read((uint64_t)(uintptr_t)vt + TD_PRESENT_SLOT * 8, &real,
                 sizeof real)) {
        printf("  refuse: cannot read slot %d\n", TD_PRESENT_SLOT);
        return 0;
    }
    if (real != target || !td_is_code_ptr(real)) {
        printf("  refuse: slot %d is %p, expected %p\n",
               TD_PRESENT_SLOT, real, target);
        return 0;
    }

    DWORD old = 0;
    if (!VirtualProtect(&vt[TD_PRESENT_SLOT], sizeof(void*),
                        PAGE_READWRITE, &old)) {
        printf("  refuse: VirtualProtect failed\n");
        return 0;
    }
    vt[TD_PRESENT_SLOT] = (void*)(uintptr_t)0xDEADBEEF00;  // stand-in hook
    VirtualProtect(&vt[TD_PRESENT_SLOT], sizeof(void*), old, &old);

    g_present_orig = real;
    printf("  hooked: slot %d %p -> hook, protect calls=%d\n",
           TD_PRESENT_SLOT, real, g_protect_calls);
    return 1;
}

// ---------------------------------------------------------------------------

static int failures = 0;

static void check(const char* name, int cond) {
    printf("  %-46s %s\n", name, cond ? "ok" : "FAIL");
    if (!cond) failures++;
}

// A "swapchain" is just an object whose first word points at the vtable.
static uint64_t make_swapchain(uint64_t* storage, void** vt) {
    *storage = (uint64_t)(uintptr_t)vt;
    return (uint64_t)(uintptr_t)storage;
}

int main(void) {
    // Lay the fake vtable and its code region out once, and register them.
    uint64_t code_lo = (uint64_t)(uintptr_t)&g_code_page[2048];
    region_add(code_lo, code_lo + sizeof g_code_page);

    // Slot 9 initially points at a plausible address inside the code region.
    g_vtable[TD_PRESENT_SLOT] = (void*)(uintptr_t)(code_lo + 64);

    uint64_t sc = make_swapchain(g_sc_storage, g_vtable);

    region_add(sc, sc + sizeof g_sc_storage);
    // The vtable lives in a registered region so the slot read succeeds.
    region_add((uint64_t)(uintptr_t)g_vtable,
               (uint64_t)(uintptr_t)g_vtable + sizeof g_vtable);

    printf("test 1: a valid swapchain gets hooked\n");
    int rc = install_present((void*)(uintptr_t)sc, g_vtable);
    check("install returns success", rc == 1);
    check("slot now holds the hook",
          g_vtable[TD_PRESENT_SLOT] == (void*)(uintptr_t)0xDEADBEEF00);
    check("original was saved", g_present_orig != NULL);
    check("VirtualProtect was called twice (set and restore)",
          g_protect_calls == 2);
    check("VirtualProtect only ever touched test pages", g_protect_failed == 0);

    printf("\ntest 2: an unaligned pointer is not a code pointer\n");
    check("0x...7 rejected (the v41 signature)",
          td_is_code_ptr((void*)(uintptr_t)(code_lo + 7)) == 0);
    check("aligned address inside code accepted",
          td_is_code_ptr((void*)(uintptr_t)(code_lo + 8)) == 1);

    printf("\ntest 3: a slot the scan did not agree with is refused\n");
    uint64_t sc2 = make_swapchain(g_sc2_storage, g_vtable);
    region_add(sc2, sc2 + sizeof g_sc2_storage);

    void** other_vt = g_other_vt;
    memset(other_vt, 0, sizeof g_other_vt);
    other_vt[TD_PRESENT_SLOT] = (void*)(uintptr_t)(code_lo + 128);
    region_add((uint64_t)(uintptr_t)other_vt,
               (uint64_t)(uintptr_t)other_vt + sizeof g_other_vt);

    int before = g_protect_calls;
    rc = install_present((void*)(uintptr_t)sc2, other_vt);
    check("install refused a mismatched slot", rc == 0);
    check("no VirtualProtect happened on refusal",
          g_protect_calls == before);
    check("real slot was left untouched",
          g_vtable[TD_PRESENT_SLOT] == (void*)(uintptr_t)0xDEADBEEF00);

    printf("\ntest 4: an unreadable swapchain is refused, not dereferenced\n");
    rc = install_present((void*)(uintptr_t)0xDEAD0000, g_vtable);
    check("install refused an unmapped pointer", rc == 0);

    printf("\ntest 5: NULL swapchain is refused\n");
    rc = install_present(NULL, g_vtable);
    check("install refused NULL", rc == 0);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
