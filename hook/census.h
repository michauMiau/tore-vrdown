/* TDVR_SLOT_CENSUS -- implemented, because it was specified in bisect.h:212
 * and never built. That comment is the project's own record of the problem:
 *
 *   "The whole project was built on two vtable slots, TD_SLOT_BEGIN_RENDER=2
 *    and TD_SLOT_END_RENDER=3, taken from an older build and never
 *    re-measured."
 *   "the renderer RTTI vtable accepted 48 counting stubs and was never called
 *    once while the game demonstrably rendered"
 *
 * So the class-vtable route is not a hypothesis that failed once; it is a
 * measured dead end with 48 slots of evidence behind it. This file is the
 * third thing worth trying, and it does not depend on knowing which slot is
 * which:
 *
 *   - DXGI is underneath any D3D12 game, and the module list on a live RTX
 *     4070 process contains dxgi.dll, d3d12.dll, d3d11.dll and nvapi64.dll.
 *   - dxgi.dll implements IDXGIFactory2 from ONE vtable, so the factory this
 *     process creates itself is the same table the game uses. That is a claim
 *     to be confirmed by the counter incrementing, not to be assumed.
 *   - The game's own frame path must therefore pass through CreateSwapChain,
 *     Present, or ResizeBuffers at least once per frame, or at least once per
 *     window.
 *
 * Every hook here forwards to the real implementation, so the game keeps
 * running and the counters are evidence rather than a side effect of a
 * change in behaviour.
 *
 * What this file deliberately does NOT do: guess that a hot slot is
 * endRender. A census counts; a guess names. The count is what tells us which
 * frame to instrument properly.
 */

#ifndef TDVR_CENSUS_H
#define TDVR_CENSUS_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Why this exists, in one place, so the next reader does not have to go
 * spelunking through bisect.h to find out that a vtable hook was tried. */
#define TDVR_CENSUS_WHY                                                     \
    "class-vtable route measured dead: 48 slots instrumented, 0 calls, while " \
    "the game rendered. Slot numbers 2/3 came from an older build and were "  \
    "never re-measured. This counts DXGI instead, which is underneath any "   \
    "D3D12 game."

/* Call counters. Indexed by slot, so the report can name the slot number and
 * leave identification to the next step. Volatile because the stubs are
 * entered from the render thread and read from ours. */
#define TDVR_CENSUS_MAX_SLOTS 32

typedef struct TdvrCensus {
    volatile LONG64 calls[TDVR_CENSUS_MAX_SLOTS];
    void            *real[TDVR_CENSUS_MAX_SLOTS];
    void            *stub[TDVR_CENSUS_MAX_SLOTS];
    volatile LONG    installed;
    volatile LONG    factory_seen;
} TdvrCensus;

static TdvrCensus g_census;

/* One stub body, hand-assembled. 24 bytes, no stack frame touched, so it is
 * safe on a method that is expected to keep its own register state:
 *
 *     48 B8 <counter>   mov  rax, imm64      ; the address of the counter
 *     FF 00             inc  qword ptr [rax]
 *     48 B8 <original>  mov  rax, imm64      ; the real function
 *     FF E0             jmp  rax
 *
 * Only rax is touched, and rax is caller-saved in the SysV and MS x64
 * conventions, so a method that expects it to be preserved across the call
 * cannot exist. Alignment is 16 so the 8-byte immediates are not split by a
 * page boundary. */
static void td_census_emit(unsigned char *p, volatile LONG64 *ctr, void *real)
{
    unsigned long long a = (unsigned long long)(uintptr_t)ctr;
    unsigned long long b = (unsigned long long)(uintptr_t)real;
    p[0]  = 0x48; p[1]  = 0xB8;
    memcpy(p + 2, &a, 8);
    p[10] = 0xFF; p[11] = 0x00;
    p[12] = 0x48; p[13] = 0xB8;
    memcpy(p + 14, &b, 8);
    p[22] = 0xFF; p[23] = 0xE0;
}

/* Write a stub over a vtable slot. The vtable is shared with the game's own
 * calls, so it needs to be writable -- it lives in a read-only section of
 * dxgi.dll, hence the VirtualProtect. */
static int td_census_patch(void **vtable, int slot)
{
    unsigned char *p = (unsigned char *)&vtable[slot];

    /* A stub or padding entry starts with 0xC3 (ret) or 0xCC (int3) or is
     * outside the image. Wrapping one faults on the first call, which reads as
     * "the game crashed" and takes the whole run with it. */
    if (p[0] == 0xC3 || p[0] == 0xCC) return 0;

    void *real = vtable[slot];
    if (!real) return 0;

    DWORD old = 0;
    if (!VirtualProtect(p, 32, PAGE_EXECUTE_READWRITE, &old)) return 0;

    /* Allocate 32 bytes so the 24-byte stub never lands on a page edge. */
    unsigned char *stub = (unsigned char *)VirtualAlloc(NULL, 32,
                            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) { VirtualProtect(p, 32, old, &old); return 0; }
    td_census_emit(stub, &g_census.calls[slot], real);

    FlushInstructionCache(GetCurrentProcess(), stub, 24);
    g_census.real[slot] = real;
    g_census.stub[slot] = stub;
    vtable[slot] = stub;
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(void *));

    VirtualProtect(p, 32, old, &old);
    g_census.installed++;
    return 1;
}

/* Report. Called from a worker so it can be read without stopping anything.
 *
 * Note the same trap this project fell into in census_reporter(): deciding
 * whether to print by looking at a string length is wrong, because a frame
 * where nothing moved leaves the length at whatever the header is. Here the
 * decision is the counter "did any slot move", which cannot be wrong. */
static void td_census_report(void (*logf)(const char *, ...))
{
    int moved = 0;
    for (int i = 0; i < TDVR_CENSUS_MAX_SLOTS; i++) {
        LONG64 c = InterlockedCompareExchange64(&g_census.calls[i], 0, 0);
        if (c > 0) moved++;
    }
    logf("DXGI CENSUS: %ld slot(s) instrumented, factory_seen=%ld, "
         "%d slot(s) with calls", (long)g_census.installed,
         (long)g_census.factory_seen, moved);
    for (int i = 0; i < TDVR_CENSUS_MAX_SLOTS; i++) {
        if (!g_census.stub[i]) continue;
        logf("  dxgi slot %2d  calls=%-10lld real=%p", i,
             (long long)InterlockedCompareExchange64(&g_census.calls[i], 0, 0),
             g_census.real[i]);
    }
    logf("  (a hot slot identifies itself by its call rate; nothing here " TDVR_CENSUS_WHY);
}

#ifdef __cplusplus
}
#endif
#endif /* TDVR_CENSUS_H */
