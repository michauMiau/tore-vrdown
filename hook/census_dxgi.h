/* DXGI census wiring -- path B, per bisect.h:239-270.
 *
 * The class-vtable route is a measured dead end (48 slots instrumented, zero
 * calls, while the game rendered -- bisect.h:262). DXGI is underneath any D3D12
 * game, and dxgi.dll implements IDXGIFactory from one vtable, so the factory
 * this process creates itself is the one the game calls through. The claim is
 * cheap to make and worth nothing until a counter moves, so this instruments
 * every slot and reports which ones the live game actually uses.
 *
 * Slot numbers come from the headers, not from a comment. Measured by
 * sizeof(IDXGIFactoryVtbl)/sizeof(void*) and friends, cross-compiled:
 *   IDXGIFactory    12   IDXGIFactory1  14   IDXGIFactory2  25
 *   IDXGIFactory4   28   IDXGISwapChain 18   IDXGISwapChain3 40
 * so IDXGIFactory::CreateSwapChain is slot 10, and IDXGISwapChain::Present
 * is slot 8.
 *
 * Nothing here assumes which slot is the frame. A count is a fact; a guess at
 * which hot slot is endRender is not, and this project's entire dead end came
 * from making that guess and building on it.
 */

#ifndef TDVR_CENSUS_DXGI_H
#define TDVR_CENSUS_DXGI_H

#include "census.h"

/* Instrument the shared factory vtable. Called once, from the init thread,
 * after a factory exists. Everything forwards, so the game is unaffected. */
static void td_census_dxgi(void)
{
    HMODULE dxgi = GetModuleHandleA("dxgi.dll");
    if (!dxgi) { vr_log("  census: dxgi.dll is not in the process"); return; }

    HRESULT (WINAPI *CreateDXGIFactory1)(REFIID, void **) = NULL;
    /* Resolve by address, not by interface index: the address of the export is
     * what this code is after, and the header already gave us the slot. */
    CreateDXGIFactory1 = (HRESULT (WINAPI *)(REFIID, void **))
        (uintptr_t)GetProcAddress(dxgi, "CreateDXGIFactory1");
    if (!CreateDXGIFactory1) { vr_log("  census: no CreateDXGIFactory1 export"); return; }

    IDXGIFactory1 *fac = NULL;
    /* Not a variable name to guess and not a dependency on dxgi_probe.h's
     * copies: those live behind #if TDVR_DXGI_PROBE, and this file is built
     * with that flag off. Reaching across a feature flag for a GUID is how a
     * build ends up failing to compile for a reason that has nothing to do with
     * the change. This copy is the one tests/guid_check.c guards. */
    static const GUID IID_IDXGIFactory1_census =
        { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
    HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory1_census, (void **)&fac);
    vr_log("  census: CreateDXGIFactory1 hr=0x%08lX fac=%p", (unsigned long)hr, (void *)fac);
    if (FAILED(hr) || !fac) return;
    InterlockedIncrement(&g_census.factory_seen);

    void **vt = (void **)fac->lpVtbl;

    /* Instrument every slot the factory exposes. Slots past the real count are
     * skipped by the 0xC3/0xCC and null checks inside td_census_patch, and a
     * 25-slot factory is what IDXGIFactory2 defines, so 32 is a safe ceiling
     * that will not read past a shorter table. */
    for (int i = 0; i < TDVR_CENSUS_MAX_SLOTS; i++) {
        if (td_census_patch(vt, i)) {
            vr_log("  census: instrumented factory slot %d (real=%p)",
                   i, g_census.real[i]);
        }
    }
    vr_log("  census: %ld factory slot(s) instrumented; expecting the game's "
           "own CreateSwapChain (slot 10) to tick if it uses this factory",
           (long)g_census.installed);
    fac->lpVtbl->Release(fac);
}

#ifdef __cplusplus
}
#endif
#endif /* TDVR_CENSUS_DXGI_H */
