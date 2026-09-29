// ---------------------------------------------------------------------------
// DXGI factory probe -- path B.
//
// WHY THIS FILE EXISTS
//
// The whole project was built on two vtable slots, TD_SLOT_BEGIN_RENDER=2 and
// TD_SLOT_END_RENDER=3, taken from an older build and never re-measured. At pid
// 8624 the renderer's RTTI vtable was patched in 48 slots with counting stubs
// and NOT ONE of them was ever called, while the game itself was unambiguously
// running (grow3s=8.19s, 2.7 GB, 59 threads). d3d12.dll, dxgi.dll, d3d11.dll
// and nvapi64.dll are all loaded into the process, and the adapter is an
// RTX 4070.
//
// So the C++ object graph Teardown uses is not reachable by name from here, but
// the DXGI layer underneath it is: every swapchain in a D3D12 game is created by
// an IDXGIFactory, and IDXGISwapChain::Present is the one call that happens once
// per frame no matter what the engine does internally.
//
// A factory we create OURSELVES is the key to the whole thing, because its
// vtable is a known, documented interface rather than something to be guessed.
// The game's own factory is then found by hooking the CreateDXGIFactory1 export
// that dxgi.dll already has in its import table, and every swapchain it returns
// is a measured object. So this file does the measurement half: create a factory,
// verify it, dump the interface it really is, and report the addresses so the
// hook targets are measured values rather than remembered ones.
//
// A NOTE ON THE GUIDs AND SLOT NUMBERS, WHICH IS THE POINT OF THIS FILE
//
// Every interface id and every slot index in the first version of this file was
// written from memory, and every single one of them was wrong. That is why
// CreateDXGIFactory1 returned hr=0x80004002 (E_NOTIMPL) with a NULL factory:
// the code asked for an interface id that does not exist. It read exactly like
// "DXGI is not available in this process", and it is not -- dxgi.dll is loaded
// and the export resolved fine.
//
// The correct values, from dxgi.idl:
//
//     IID_IDXGIFactory  = {7B7166EC-21C7-44AE-B21A-C9AE321AE369}
//     IID_IDXGIFactory1 = {770AAE78-F26F-4DBA-A829-253C83D1B387}
//     IID_IDXGIFactory2 = {50C83A1C-E072-4C48-87B0-3630FA36A6D0}
//     IID_IDXGIAdapter1 = {29038F61-3839-4626-91FD-086879011A05}
//
// What was written instead:
//     Factory  = {7b7166ec-21c7-46ae-8e06-1a3b0e6c3e6c}  46ae, not 44ae
//     Factory1 = same, i.e. a duplicate of Factory
//     Factory2 = {50c83a1c-e072-4c93-898b-fbb639d95310}  4c48/87b0, not 4c93/898b
//
// Slot numbering, from the inheritance chain in dxgi.idl rather than guessed:
//
//     IUnknown         0 QueryInterface  1 AddRef  2 Release
//     IDXGIObject      3 SetPrivateData  4 SetPrivateDataInterface
//                      5 GetPrivateData  6 GetParent
//     IDXGIFactory     7 EnumAdapters    8 MakeWindowAssociation
//                      9 GetWindowAssociation  10 CreateSwapChain
//                      11 CreateSoftwareAdapter
//     IDXGIFactory1   12 EnumAdapters1  13 IsCurrent
//     IDXGIFactory2   14 IsWindowedStereo  15 CreateSwapChainForHwnd
//                      16 CreateSwapChainForCoreWindow
//                      17 GetSharedResourceAdapterLuid ... 25 GetCompositionQueueInformation
//
// and IDXGISwapChain, which inherits IDXGIDeviceSubObject:
//     8  Present        9  GetBuffer    10 SetFullscreenState
//     11 GetFullscreenState  12 GetDesc  13 ResizeBuffers  14 ResizeTarget
//     15 GetContainingOutput  16 GetFrameStatistics  17 GetLastPresentCount
//
// The old file had CreateSwapChain at 8 (it is 10) and called
// ((void**)ad)[5] to release an adapter, which is AddRef, not Release, because
// slot n of a vtable is ((void**)obj)[n+1]. Both would have been silent.
//
// This matters far beyond this file. The standing "D3D12CreateDevice returns
// E_NOTIMPL on every adapter" result -- recorded as the reason xrCreateSession
// refused the binding, and as the reason to install the Agility SDK -- has
// exactly the same shape: it was measured by code that spelled interfaces and
// slots from memory. Until it is re-measured with ids taken from a header,
// treat E_NOTIMPL from any of these calls as "suspect the id first", and treat
// any vtable index in this project as unmeasured until a read-back proves it.
// ---------------------------------------------------------------------------

#include "bisect.h"

#if TDVR_DXGI_PROBE

// The DLL imports nothing from dxgi.dll or d3d12.dll, by policy: the old
// project died of loader error 126 twice from accidental dependencies, and the
// point of this path is full control over what is loaded and when.
#ifndef IID_IDXGIFactory_local
#define IID_IDXGIFactory_local
static const GUID IID_IDXGIFactory_l =
    { 0x7b7166ec, 0x21c7, 0x44ae, { 0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69 } };
#define IID_IDXGIFactory IID_IDXGIFactory_l
#endif

#ifndef IID_IDXGIFactory1_local
#define IID_IDXGIFactory1_local
static const GUID IID_IDXGIFactory1_l =
    { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
#define IID_IDXGIFactory1 IID_IDXGIFactory1_l
#endif

#ifndef IID_IDXGIFactory2_local
#define IID_IDXGIFactory2_local
static const GUID IID_IDXGIFactory2_l =
    { 0x50c83a1c, 0xe072, 0x4c48, { 0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0 } };
#define IID_IDXGIFactory2 IID_IDXGIFactory2_l
#endif

#ifndef IID_IDXGIAdapter1_local
#define IID_IDXGIAdapter1_local
static const GUID IID_IDXGIAdapter1_l =
    { 0x29038f61, 0x3839, 0x4626, { 0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05 } };
#define IID_IDXGIAdapter1 IID_IDXGIAdapter1_l
#endif

typedef HRESULT (WINAPI *PFN_CreateDXGIFactory1)(REFIID, void**);
typedef ULONG   (WINAPI *PFN_ComRelease)(void*);

// Factory slot numbers, from the layout in the header comment.
#define DXGI_SLOT_SETPRIVATEDATA       3
#define DXGI_SLOT_SETPRIVATEDATAIFACE  4
#define DXGI_SLOT_GETPRIVATEDATA       5
#define DXGI_SLOT_GETPARENT            6
#define DXGI_SLOT_ENUMADAPTERS         7
#define DXGI_SLOT_MAKEWINDOWASSOC      8
#define DXGI_SLOT_GETWINDOWASSOC       9
#define DXGI_SLOT_CREATESWAPCHAIN     10
#define DXGI_SLOT_SOFTWAREADAPTER     11
#define DXGI_SLOT_ENUMADAPTERS1       12
#define DXGI_SLOT_ISCURRENT           13
#define DXGI_SLOT_WINDOWEDSTEREO      14
#define DXGI_SLOT_CREATESWCHAINHWND   15

// IDXGIAdapter1 inherits IDXGIAdapter, whose GetDesc is slot 8.
#define DXGI_SLOT_ADAPTER_GETDESC      8

// A COM object looks like this in memory:
//     [0]  pointer to the vtable array
//     [1..] the object's own fields
// So the vtable is obj[0] and slot n of the interface is obj->vtable[n].
// Passing obj[0] here and adding 1 reads the object's DATA, not its methods --
// which is how GetDesc came back as 0xFFFFFFFF while the same vtable dumped
// one line earlier showed a perfectly good 0x7FFE9DFC5780 in slot 8. Two
// measurements of one interface disagreeing is the signal; neither was trusted.
static void** dx_vtable(void** obj) {
    if (!obj) return NULL;
    return (void**)obj[0];
}

static void* dx_slot(void** vt, int n) {
    if (!vt) return NULL;
    return vt[n];
}

static void* dx_obj_slot(void** obj, int n) {
    void** vt = dx_vtable(obj);
    if (!vt || vt == obj) return NULL;   // refuse a self-referential read
    return dx_slot(vt, n);
}

int tdvr_dxgi_probe(void) {
    // Read the switch from the DLL's own environment block, not from a user
    // environment variable: the game is launched by a scheduled task, and that
    // task's process is built from the service's environment snapshot, so a
    // variable set in the user profile after boot does NOT reach it. Setting
    // TDVR_DXGI_TOUCH in the user environment and relaunching therefore kept
    // producing TDVR_DXGI_TOUCH=0, which looked like the flag not working.
    //
    // So read it from the injector-visible handover file instead, which is
    // written per run and copied into the game folder alongside the DLL. That
    // path was already proven to work (tdvr_host.txt is read the same way).
    char touchBuf[8] = {0};
    int touch = 0;
    {
        FILE* tf = fopen("tdvr_flags.txt", "rb");
        if (tf) {
            size_t n = fread(touchBuf, 1, sizeof(touchBuf) - 1, tf);
            fclose(tf);
            if (n) {
                if (touchBuf[0] == '1') touch = 1;
                vr_log("DXGI PROBE: tdvr_flags.txt said '%s' -> touch=%d",
                       touchBuf, touch);
            }
        }
    }
    if (!touchBuf[0]) {
        touch = getenv("TDVR_DXGI_TOUCH") != NULL;
        vr_log("DXGI PROBE: no tdvr_flags.txt, env TDVR_DXGI_TOUCH -> touch=%d",
               touch);
    }

    vr_log("=== DXGI PROBE ===  TDVR_DXGI_TOUCH=%d ===", touch);
    vr_log("DXGI PROBE: Factory ={7b7166ec-21c7-44ae-b21a-c9ae321ae369}");
    vr_log("DXGI PROBE: Factory1={770aae78-f26f-4dba-a829-253c83d1b387}");
    vr_log("DXGI PROBE: Factory2={50c83a1c-e072-4c48-87b0-3630fa36a6d0}");

    HMODULE dxgi = GetModuleHandleA("dxgi.dll");
    if (!dxgi) dxgi = LoadLibraryA("dxgi.dll");
    if (!dxgi) { vr_log("DXGI PROBE: dxgi.dll not loadable"); return 0; }
    vr_log("DXGI PROBE: dxgi.dll=%p", (void*)dxgi);

    PFN_CreateDXGIFactory1 mk =
        (PFN_CreateDXGIFactory1)(void*)GetProcAddress(dxgi, "CreateDXGIFactory1");
    if (!mk) {
        // The same export without the 1 is the other spelling.
        mk = (PFN_CreateDXGIFactory1)(void*)GetProcAddress(dxgi, "CreateDXGIFactory");
        vr_log("DXGI PROBE: CreateDXGIFactory1=%p (CreateDXGIFactory fallback)",
               (void*)mk);
    } else {
        vr_log("DXGI PROBE: CreateDXGIFactory1=%p", (void*)mk);
    }
    if (!mk) { vr_log("DXGI PROBE: no factory entry point"); return 0; }

    if (!touch) {
        vr_log("DXGI PROBE: TDVR_DXGI_TOUCH unset -- NOT calling DXGI. "
               "Export address measured, nothing created.");
        return 1;
    }
    vr_log("DXGI PROBE: TDVR_DXGI_TOUCH set -- creating our own factory");

    // Three known-good ids, so a failure on all three means something real
    // rather than a typo, and a success on the second or third proves which id
    // was the problem.
    static const GUID* const ids[] = {
        &IID_IDXGIFactory2, &IID_IDXGIFactory1, &IID_IDXGIFactory, NULL
    };
    void* factory = NULL;
    for (int i = 0; ids[i] && !factory; i++) {
        void* f = NULL;
        HRESULT r = mk(ids[i], &f);
        vr_log("DXGI PROBE: CreateDXGIFactory1(id#%d) -> hr=0x%08lX factory=%p",
               i, (unsigned long)r, f);
        if (SUCCEEDED(r) && f) factory = f;
    }
    if (!factory) {
        vr_log("DXGI PROBE: E_NOTIMPL with all three ids -- DXGI really is "
               "unavailable here, or CreateDXGIFactory1 is the wrong entry point");
        return 0;
    }

    void** vt = dx_vtable((void**)factory);
    vr_log("DXGI PROBE: factory=%p vtable=%p", factory, (void*)vt);
    if (vt == (void**)factory) {
        // Still impossible for a real COM object, and now a meaningful test:
        // we have actually read the first field, and it points at itself.
        vr_log("DXGI PROBE: ABORT -- first field == object address, not a vtable");
        return 0;
    }
    if (!vt) { vr_log("DXGI PROBE: ABORT -- null vtable"); return 0; }

    // The module range, so a slot can be proven to belong to dxgi.dll rather
    // than assumed to. GetModuleInformation is re-exported by kernel32 on every
    // supported Windows, under both the K32 prefix and the original name.
    uint8_t* lo = NULL;
    uint8_t* hi = NULL;
    {
        typedef BOOL (WINAPI *PFN_GetModuleInformation)(HMODULE, HMODULE, void*, DWORD);
        typedef struct { void* base; void* size; void* entry; } MODINFO;
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        PFN_GetModuleInformation gmi = NULL;
        if (k32) {
            gmi = (PFN_GetModuleInformation)(void*)
                  GetProcAddress(k32, "K32GetModuleInformation");
            if (!gmi)
                gmi = (PFN_GetModuleInformation)(void*)
                      GetProcAddress(k32, "GetModuleInformation");
        }
        MODINFO mi = {0};
        if (gmi && gmi((HMODULE)GetCurrentProcess(), dxgi, &mi, (DWORD)sizeof mi)) {
            lo = (uint8_t*)mi.base;
            hi = lo + (size_t)(uintptr_t)mi.size;
        }
    }

    static const char* names[26] = {
        "QueryInterface", "AddRef", "Release",
        "SetPrivateData", "SetPrivateDataInterface",
        "GetPrivateData", "GetParent",
        "EnumAdapters", "MakeWindowAssociation", "GetWindowAssociation",
        "CreateSwapChain", "CreateSoftwareAdapter",
        "EnumAdapters1", "IsCurrent",
        "IsWindowedStereo", "CreateSwapChainForHwnd",
        "CreateSwapChainForCoreWindow", "GetSharedResourceAdapterLuid",
        "RegisterStereoStatusWindow", "RegisterStereoStatusEvent",
        "UnregisterStereoStatus", "RegisterOcclusionStatusWindow",
        "RegisterOcclusionStatusEvent", "UnregisterOcclusionStatus",
        "CreateSwapChainForComposition", "GetCompositionQueueInformation"
    };
    vr_log("DXGI PROBE: dumping slots 0..25 of the factory interface");
    for (int i = 0; i < 26; i++) {
        void* f = dx_slot(vt, i);
        if (!f) { vr_log("  slot %2d %-32s <null>", i, names[i]); continue; }
        int in = lo && (uint8_t*)f >= lo && (uint8_t*)f < hi;
        vr_log("  slot %2d %-32s 0x%llX%s", i, names[i],
               (unsigned long long)(uintptr_t)f,
               in ? "  (dxgi.dll)" : "  (NOT dxgi.dll -- object is not this interface)");
    }

    // The two hooks this path is headed for, reported as measured values.
    void* csc  = dx_slot(vt, DXGI_SLOT_CREATESWAPCHAIN);
    void* cscw = dx_slot(vt, DXGI_SLOT_CREATESWCHAINHWND);
    void* ead1 = dx_slot(vt, DXGI_SLOT_ENUMADAPTERS1);
    vr_log("DXGI PROBE: CreateSwapChain        = 0x%llX",
           (unsigned long long)(uintptr_t)csc);
    vr_log("DXGI PROBE: CreateSwapChainForHwnd = 0x%llX",
           (unsigned long long)(uintptr_t)cscw);
    vr_log("DXGI PROBE: EnumAdapters1          = 0x%llX",
           (unsigned long long)(uintptr_t)ead1);

    // Which adapters does this process see, and what are they? Worth having
    // before touching the session binding, because the old D3D12CreateDevice
    // E_NOTIMPL was measured with a hand-rolled adapter walk that may have
    // been its own bug rather than a missing Agility SDK.
    if (ead1) {
        typedef HRESULT (WINAPI *PFN_EnumAdapters1)(void*, UINT, void**);
        typedef HRESULT (WINAPI *PFN_GetDesc1)(void*, void*);
        for (UINT i = 0; i < 4; i++) {
            void* ad = NULL;
            vr_log(">> EnumAdapters1(%u)", i);
            HRESULT r = ((PFN_EnumAdapters1)ead1)(factory, i, &ad);
            vr_log("<< EnumAdapters1(%u) -> hr=0x%08lX adapter=%p",
                   i, (unsigned long)r, ad);
            if (FAILED(r) || !ad) break;

            void** av = (void**)ad;
            void** avt = (void**)dx_obj_slot(av, 0);   // the adapter's vtable
            if (!avt || (void**)avt == av) {
                vr_log("DXGI PROBE: adapter %u has no usable vtable (%p)", i, avt);
            } else {
                // Measure the adapter's vtable instead of assuming slot numbers.
                // GetDesc looked like slot 8 from the inheritance chain, but
                // reading it returned 0xFFFFFFFF, and calling that froze the
                // game (the crash handler's dialog, process still alive, CPU
                // flat). Guessing an index is what caused this; so dump it and
                // let the module range prove each entry.
                static const char* an[13] = {
                    "QueryInterface", "AddRef", "Release",
                    "SetPrivateData", "SetPrivateDataInterface",
                    "GetPrivateData", "GetParent",
                    "EnumOutputs", "GetDesc",
                    "GetDesc1", "RegisterHardwareContentProtectionTeardownStatusEvent",
                    "UnregisterHardwareContentProtectionTeardownStatus",
                    "QueryVideoMemoryBudget"
                };
                vr_log("DXGI PROBE: adapter %u vtable=%p", i, (void*)avt);
                for (int k = 0; k < 13; k++) {
                    void* f = dx_slot(avt, k);
                    int in = lo && (uint8_t*)f >= lo && (uint8_t*)f < hi;
                    vr_log("  ad %2d %-46s 0x%llX%s", k, an[k],
                           (unsigned long long)(uintptr_t)f,
                           in ? "  (dxgi.dll)" : "  (NOT dxgi.dll)");
                }
            }

            void* gd = dx_obj_slot(av, DXGI_SLOT_ADAPTER_GETDESC);
            // Never call a pointer we have not proved is one. Check the whole
            // pointer, not just the all-ones case: the value that froze the
            // game here was 0x00000000FFFFFFFF, so a test against
            // (void*)(uintptr_t)-1 passes it straight through. The only
            // trustworthy test is the module range.
            int gd_ok = gd && lo && (uint8_t*)gd >= lo && (uint8_t*)gd < hi;
            if (!gd_ok) {
                vr_log("DXGI PROBE: adapter %u GetDesc slot %d = 0x%llX not in "
                       "dxgi.dll -- NOT calling it", i, DXGI_SLOT_ADAPTER_GETDESC,
                       (unsigned long long)(uintptr_t)gd);
            } else {
                // DXGI_ADAPTER_DESC1, laid out by offset:
                //     0  WCHAR Description[128]   256 bytes
                //   256  UINT  VendorId
                //   260  UINT  DeviceId
                //   264  UINT  SubSysId
                //   268  UINT  Revision
                //   272  SIZE_T DedicatedVideoMemory
                //   280  SIZE_T DedicatedSystemMemory
                //   288  SIZE_T SharedSystemMemory
                //   296  LUID  AdapterLuid
                //   304  UINT  Flags
                //   312  sizeof
                //
                // The buffer was 264 bytes here, so GetDesc wrote 48 bytes past
                // the end of it. The log stopped dead on the EnumAdapters1 line
                // and the process showed grow3s=0 -- a stack smash inside a
                // vendor driver call, not a DXGI problem. Size the buffer from
                // the struct, and log BEFORE the call so the last thing in the
                // file is always the line before the suspect call.
                unsigned char desc[312];
                for (int k = 0; k < 312; k++) desc[k] = 0xCD;
                vr_log("DXGI PROBE: adapter %u GetDesc ptr=0x%llX buf=312",
                       i, (unsigned long long)(uintptr_t)gd);
                HRESULT dr = ((PFN_GetDesc1)gd)(ad, desc);
                vr_log("DXGI PROBE: adapter %u GetDesc -> hr=0x%08lX",
                       i, (unsigned long)dr);
                if (dr == S_OK) {
                    char name[129];
                    for (int k = 0; k < 128; k++) name[k] = 0;
                    // UTF-16LE at offset 0; ASCII in practice for GPU names.
                    for (int k = 0; k < 128; k++)
                        if (desc[k*2] || desc[k*2+1])
                            name[k] = desc[k*2] ? (char)desc[k*2] : (char)desc[k*2+1];
                    unsigned vend = *(const unsigned*)(desc + 256);
                    unsigned long long dvm = *(const unsigned long long*)(desc + 272);
                    vr_log("  adapter %u: \"%s\" vendor=0x%04X dedicatedVram=%u MB",
                           i, name, vend, (unsigned)(dvm / (1024ull*1024ull)));
                } else {
                    vr_log("  adapter %u: GetDesc hr=0x%08lX", i, (unsigned long)dr);
                }
            }
            PFN_ComRelease rel = (PFN_ComRelease)dx_obj_slot(av, 2);
            if (rel) rel(ad);
        }
    }

    PFN_ComRelease rel = (PFN_ComRelease)dx_slot(vt, 2);
    if (rel) rel(factory);
    vr_log("=== DXGI PROBE done ===");
    return 1;
}

#endif // TDVR_DXGI_PROBE
