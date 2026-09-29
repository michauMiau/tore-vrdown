// ---------------------------------------------------------------------------
// IDXGISwapChain capture -- path B, the part that actually matters.
//
// WHERE THIS COMES FROM
//
// The renderer vtable this project was built on had 48 slots and none of them
// was ever called, so the engine's object graph is unreachable by name. The DXGI
// layer underneath it is not, because dxgi.dll implements IDXGIFactory2 from a
// SINGLE shared vtable: the factory this DLL creates itself has the same vtable
// pointer the game's factory has. So patching a slot on the vtable we already
// measured patches it for the game too. That is a claim to be confirmed by the
// hook firing, not assumed -- everything this project assumed about vtables was
// wrong.
//
// The slots, from dxgi.idl, all 26 confirmed inside dxgi.dll by the probe:
//     10 CreateSwapChain          (DXGI_SWAP_CHAIN_DESC,  52 bytes)
//     15 CreateSwapChainForHwnd   (DXGI_SWAP_CHAIN_DESC1, 48 bytes)
//
// THE TIMING PROBLEM
//
// The game creates its device and swapchain during startup, about ten seconds
// before this DLL is injected, so a create-time hook catches nothing on the
// first pass. It is still worth having: a swapchain is recreated on every window
// resize, and the worker forces one by nudging the game window with SetWindowPos.
// Whether the hook fires is a measurement, not an assumption.
//
// THE THREAD RULE
//
// These hooks run on the render thread. A hook that logs a dozen lines can drop
// a frame, and one that calls GetDesc/GetBuffer on a driver object is worse. So
// the hook does the minimum -- copy out the arguments, call the original,
// publish the returned pointer, return. The vtable dump, GetDesc, GetBuffer and
// the backbuffer descriptor all happen on a worker thread. A render-thread hook
// that does real work is how this project got its unexplained freezes.
//
// ENABLE: tdvr_flags.txt containing "sc" (space separated, case-insensitive).
// Without it nothing is installed and a normal run is unaffected.
// ---------------------------------------------------------------------------

#include "bisect.h"

// Forward declaration: inspect_chain() is defined above the implementation of
// this helper, and the two were reordered once already.
static int sc_readable(void* a, size_t n);
static DWORD WINAPI tdvr_sc_worker(LPVOID p);
void* tdvr_factory_from_device(void* dev);
void  tdvr_factory_walk_later(void* dev);

// Mode 14 needs two things from the XR side that live in xr_session.h, which is
// included before this file in some orders and after it in others. The struct
// type and the exact signature are copied from xr_session.h (tdvr_xr_try_session
// takes tdvr_xr*, not void* -- guessing that gives a conflicting-type error).
struct tdvr_xr;
int  tdvr_xr_make_device_and_queue(void** out_dev, void** out_queue);
int  tdvr_xr_try_session(struct tdvr_xr* X, void* device, void* queue);
extern struct tdvr_xr g_xr;

// The worker is the only thread that may do driver-lock work in mode 14, so the
// render thread has to be able to ask whether it is alive before handing the
// request over. Handing a request to a thread that does not exist is how mode 14
// looked like a no-op for two runs.
int  tdvr_sc_worker_running(void);
void tdvr_sc_wake_event(void);

// Set by the render thread in mode 14 (TDVR_XR_OWN_DEVICE) and consumed by the
// worker above. A flag rather than a queue because there is exactly one request
// per process -- the device either gets made or it does not, and retrying would
// only add a second driver lock behind a frame.
extern volatile LONG tdvr_own_device_requested;
volatile LONG tdvr_own_device_requested = 0;

// Is this address readable, and are `n` bytes of it inside one committed region?
//
// A vtable is only safe to index after this passes. This is NOT the same test as
// in_dxgi(): the device and adapter vtables live in D3D12Core.dll or D3D12.dll,
// so requiring a dxgi address here would reject valid objects -- which is the
// mistake that made an earlier "swapchain" probe accept a D3D12Core table and
// call GetBuffer on something that was not a swapchain at all.
static int sc_readable(void* a, size_t n) {
    typedef SIZE_T (WINAPI *PFN_VQ)(void*, MEMORY_BASIC_INFORMATION*, SIZE_T);
    HMODULE k = GetModuleHandleA("kernel32.dll");
    PFN_VQ vq = k ? (PFN_VQ)(void*)GetProcAddress(k, "VirtualQuery") : NULL;
    if (!vq || !a) return 0;
    MEMORY_BASIC_INFORMATION mbi;
    if (vq(a, &mbi, sizeof mbi) != sizeof mbi) return 0;
    uintptr_t base = (uintptr_t)mbi.BaseAddress;
    if (base + mbi.RegionSize <= (uintptr_t)a) return 0;
    if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                         PAGE_EXECUTE_WRITECOPY))) return 0;
    if ((uintptr_t)a + n > base + mbi.RegionSize) return 0;
    return 1;
}


#if TDVR_PRESENT_HOOK

// Spelled out from dxgi.idl, not from memory. Every id in this project that was
// written from memory was wrong, and a wrong id returns E_NOINTERFACE, which
// reads as "this is not a D3D12 game" rather than "I mistyped a hex digit".
//
//   IID_IDXGISwapChain = {310D36A0-D2E7-4C0A-AA04-6A9D23B8886A}
//   IID_ID3D12Resource = {CD3F4F26-E516-4591-A6BA-48FD235AAEA5}
static const GUID IID_IDXGISwapChain_h =
    { 0x310d36a0, 0xd2e7, 0x4c0a, { 0xaa, 0x04, 0x6a, 0x9d, 0x23, 0xb8, 0x88, 0x6a } };
static const GUID IID_ID3D12Resource_h =
    { 0x696442be, 0xa72e, 0x4059, { 0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad } };

#define DXGI_SLOT_CREATESWAPCHAIN    10
#define DXGI_SLOT_CREATESWCHAINHWND  15

// IDXGISwapChain inherits IDXGIDeviceSubObject, so slot 7 is GetDevice and the
// swapchain's own block starts at 8. Present is slot 8, GetBuffer 9, GetDesc 12.
// The probe on a factory does not confirm these -- they are the swapchain's own
// vtable, which is a different table in a different object. The range check
// against dxgi.dll is what confirms them, and the worker prints the whole table
// so the numbering can be checked against the log rather than assumed.
#define SC_SLOT_GETDEVICE     7
#define SC_SLOT_PRESENT       8
#define SC_SLOT_GETBUFFER     9
#define SC_SLOT_GETDESC      12

// ID3D12Resource inherits ID3D12Object (0-6) and ID3D12DeviceChild (7):
// 8 Map, 9 Unmap, 10 GetDesc, 11 SetEvictPriority, ... 17 GetHeap.
#define RES_SLOT_GETDESC     10

typedef HRESULT (WINAPI *PFN_CreateSwapChain)(void*, void*, const void*, void**);
typedef HRESULT (WINAPI *PFN_CreateSwapChainForHwnd)(void*, void*, void*,
                                                    const void*, const void*, void**);
typedef ULONG   (WINAPI *PFN_Release)(void*);

static PFN_CreateSwapChain        g_orig_csc  = NULL;
static PFN_CreateSwapChainForHwnd g_orig_cscw = NULL;
static void*  g_vtable_patched = NULL;
static HANDLE g_evt    = NULL;
static HANDLE g_worker = NULL;
static volatile LONG g_new_chain = 0;
static void*  g_walk_device = NULL;   // device queued by the render thread
static void*  g_last_chain = NULL;
static uint8_t* dxlo = NULL;
static uint8_t* dxhi = NULL;

static int in_dxgi(void* a) {
    return a && dxlo && (uint8_t*)a >= dxlo && (uint8_t*)a < dxhi;
}

// The nudge test is gone too. Moving the game window with SetWindowPos was
// measured and did NOT make the game recreate its swapchain (D3D12 flip-model
// chains are not rebuilt on resize), so the whole window-enumeration helper set
// -- EnumWindows, the pick callback and the statics it needed -- was deleted
// rather than left dormant. Dormant code that once crashed this project is worse
// than no code.
//
// ---- hooks: minimum work, render thread -----------------------------------
static HRESULT WINAPI td_csc(void* fac, void* dev, const void* desc, void** out) {
    HRESULT hr = g_orig_csc ? g_orig_csc(fac, dev, desc, out) : E_FAIL;
    if (SUCCEEDED(hr) && out && *out) {
        g_last_chain = *out;
        InterlockedIncrement(&g_new_chain);
        if (g_evt) SetEvent(g_evt);
    }
    vr_log("[SC] CreateSwapChain dev=%p hr=0x%08lX chain=%p", dev,
           (unsigned long)hr, (out && *out) ? *out : NULL);
    return hr;
}

static HRESULT WINAPI td_cscw(void* fac, void* dev, HWND hwnd,
                              const void* d1, const void* rate, void** out) {
    HRESULT hr = g_orig_cscw ? g_orig_cscw(fac, dev, hwnd, d1, rate, out) : E_FAIL;
    if (d1) {
        // DXGI_SWAP_CHAIN_DESC1: Width 0, Height 4, Format 8, Stereo 12,
        // SampleDesc{16,20}, BufferUsage 24, BufferCount 28, Scaling 32,
        // SwapEffect 36, AlphaMode 40, Flags 44; size 48.
        const int* s = (const int*)d1;
        const unsigned* u = (const unsigned*)d1;
        vr_log("[SC] DESC1 %ux%u fmt=%u stereo=%d samples=%u usage=0x%X count=%u "
               "scaling=%u effect=%u alpha=%u flags=0x%X",
               u[0], u[1], u[2], s[3], u[4], u[6], u[7], u[8], u[9], u[10], u[11]);
    }
    if (SUCCEEDED(hr) && out && *out) {
        g_last_chain = *out;
        InterlockedIncrement(&g_new_chain);
        if (g_evt) SetEvent(g_evt);
    }
    vr_log("[SC] CreateSwapChainForHwnd dev=%p hwnd=%p hr=0x%08lX chain=%p",
           dev, hwnd, (unsigned long)hr, (out && *out) ? *out : NULL);
    return hr;
}

// ---- inspection, worker thread only ---------------------------------------
static void inspect_chain(void* sc) {
    if (!sc) return;
    void** vt = *(void***)sc;
    vr_log("[SC] chain=%p vtable=%p", sc, vt);
    if (!vt || vt == (void**)sc) { vr_log("[SC] not a COM object"); return; }

    static const char* nm[15] = {
        "QueryInterface", "AddRef", "Release",
        "SetPrivateData", "SetPrivateDataInterface", "GetPrivateData",
        "GetParent", "GetDevice(sub)", "Present", "GetBuffer",
        "SetFullscreenState", "GetFullscreenState", "GetDesc", "ResizeBuffers",
        "ResizeTarget"
    };
    for (int i = 0; i < 15; i++)
        vr_log("[SC]   sc %2d %-24s %p%s", i, nm[i], vt[i],
               in_dxgi(vt[i]) ? "" : "   <-- not dxgi.dll");

    void* gd = vt[SC_SLOT_GETDESC];
    // The slot must be a committed pointer, not specifically a dxgi.dll one.
    // A swapchain created through the Agility SDK has its table in D3D12Core.dll,
    // so requiring dxgi here would reject the very object this function is for.
    if (!sc_readable(gd, sizeof(void*))) { vr_log("[SC] GetDesc not callable, stopping"); return; }
    // DXGI_SWAP_CHAIN_DESC: BufferCount 0, ModeDesc{4..31}, BufferUsage 32,
    // OutputWindow 36, Windowed 40, SwapEffect 44, Flags 48; size 52.
    unsigned char d[64];
    for (int i = 0; i < 64; i++) d[i] = 0;
    typedef HRESULT (WINAPI *PFN_GetDesc)(void*, void*);
    vr_log("[SC] >> swapchain GetDesc");
    HRESULT hr = ((PFN_GetDesc)gd)(sc, d);
    vr_log("[SC] << swapchain GetDesc hr=0x%08lX", (unsigned long)hr);
    if (hr != S_OK) return;
    const unsigned* u = (const unsigned*)d;
    vr_log("[SC] DESC count=%u %ux%u refresh=%u/%u scan=%u/%u fmt=%u usage=0x%X "
           "hwnd=%p windowed=%d effect=%u flags=0x%X",
           u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[8],
           (void*)(uintptr_t)u[9], (int)u[10], u[11], u[12]);

    void* gb = vt[SC_SLOT_GETBUFFER];
    if (!sc_readable(gb, sizeof(void*))) { vr_log("[SC] GetBuffer not callable, stopping"); return; }
    void* res = NULL;
    typedef HRESULT (WINAPI *PFN_GetBuffer)(void*, UINT, const GUID*, void**);
    vr_log("[SC] >> GetBuffer(0, IID_ID3D12Resource)");
    hr = ((PFN_GetBuffer)gb)(sc, 0, &IID_ID3D12Resource_h, &res);
    vr_log("[SC] << GetBuffer hr=0x%08lX resource=%p", (unsigned long)hr, res);
    if (hr != S_OK || !res) {
        // E_NOINTERFACE (0x80004002). A D3D11 swapchain hands back an
        // ID3D11Texture2D here, and so does a typo'd GUID -- which is why the id
        // is spelled out above rather than recalled.
        vr_log("[SC] no ID3D12Resource: hr=0x%08lX (0x80004002 = E_NOINTERFACE, "
               "so either a D3D11 swapchain or a wrong id)", (unsigned long)hr);
        return;
    }

    void** rv = *(void***)res;
    vr_log("[SC] resource=%p vtable=%p", res, rv);
    if (!rv || rv == (void**)res) return;
    void* rgd = rv[RES_SLOT_GETDESC];
    if (rgd) {
        // D3D12_RESOURCE_DESC: Dimension 0, Alignment 4, Width 8, Height 12,
        // DepthOrArraySize 16, MipLevels 20, Format 24, SampleCount 28, Layout 32.
        unsigned char rd[64];
        for (int i = 0; i < 64; i++) rd[i] = 0;
        typedef void (WINAPI *PFN_RGetDesc)(void*, void*);
        vr_log("[SC] >> resource GetDesc");
        ((PFN_RGetDesc)rgd)(res, rd);
        vr_log("[SC] << resource GetDesc");
        const unsigned* r = (const unsigned*)rd;
        vr_log("[SC] RESOURCE dim=%u align=%u %ux%u depth=%u mips=%u fmt=%u "
               "samples=%u layout=%u",
               r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8]);
    }
    PFN_Release rel = (PFN_Release)rv[2];
    if (rel) rel(res);
}

// ---- scanning the heap for the swapchain: REJECTED, DO NOT RE-ENABLE ------
//
// This existed and it killed the game. It found 40 candidate objects across 21
// distinct vtables -- dxgi.dll's own internal factories, adapters, output
// objects, none of them the game's swapchain -- and called QueryInterface on
// them. Calling an interface method on an object you have merely pattern-matched
// is not safe, and the process died with no minidump.
//
// It is kept here as a record of the measured failure, not as code. The answer
// is not "search harder": it is "be loaded before the game is", which
// C:\tdvr\golive_inner.bat now does by attaching the moment the process exists.

// Queue the factory walk onto the worker thread.
//
// The render thread must not call GetAdapter: it takes a driver lock and can
// block behind the frame currently being presented. The first version called it
// inline and the log simply stopped at "[SC] device=..." every run -- no crash,
// no minidump, the thread never returned, which reads as a GPU stall rather than
// the lock it was.
//
// The worker is joined to the existing one, so there is only ever one thread
// touching DXGI objects. AddRef is taken here, on the render thread, because the
// reference the caller holds is released as soon as the session binding finishes
// and a later release from the worker could otherwise free the object first.
void tdvr_factory_walk_later(void* dev) {
    if (!dev) return;
    static volatile LONG g_walk_pending = 0;
    if (InterlockedCompareExchange(&g_walk_pending, 1, 0) != 0) {
        vr_log("[SC] factory walk already queued, ignoring this one");
        return;
    }
    void** dv = *(void***)dev;
    if (!dv || dv == (void**)dev) {
        vr_log("[SC] device is not a COM object, not queueing the walk");
        InterlockedExchange(&g_walk_pending, 0);
        return;
    }
    PFN_Release addref = (PFN_Release)(void*)dv[1];   // slot 1 is AddRef
    if (addref) addref(dev);

    if (!g_evt) g_evt = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!g_worker) {
        g_worker = CreateThread(NULL, 0, tdvr_sc_worker, NULL, 0, NULL);
        if (!g_worker) {
            vr_log("[SC] no worker thread; not walking the factory graph");
            PFN_Release rel = (PFN_Release)(void*)dv[2];
            if (rel) rel(dev);
            InterlockedExchange(&g_walk_pending, 0);
            return;
        }
        vr_log("[SC] worker thread %p", g_worker);
    }
    g_walk_device = dev;
    SetEvent(g_evt);
    vr_log("[SC] factory walk queued for the worker (device=%p)", dev);
}

static DWORD WINAPI tdvr_sc_worker(LPVOID p) {
    (void)p;
    // Nothing to look for up front any more: the hook is now installed before the
    // game starts, so the swapchain creation is either already behind us (the
    // injector was still slow) or still to come. Poll quietly and inspect each
    // new chain as it appears, and do the device->adapter->factory walk here
    // rather than on the render thread that queued it.
    LONG seen = 0;
    int  walked = 0;
    int  own_done = 0;
    for (int i = 0; i < 600; i++) {          // 5 minutes at 500 ms
        LONG n = InterlockedCompareExchange(&g_new_chain, 0, 0);
        if (n != seen) {
            seen = n;
            vr_log("[SC] worker: swapchain #%ld = %p", (long)seen, g_last_chain);
            inspect_chain(g_last_chain);
        }
        // The pointer is cleared BEFORE the walk, so a second device arriving
        // while this one is being walked queues again instead of being dropped
        // and never retried.
        void* dev = g_walk_device;
        if (dev && !walked) {
            g_walk_device = NULL;
            walked = 1;
            vr_log("[SC] worker: walking the factory graph from device=%p", dev);
            void* fac = tdvr_factory_from_device(dev);
            if (fac) vr_log("[SC] worker: got the game's factory at %p", fac);
            // Balance the AddRef taken by tdvr_factory_walk_later.
            void** dv = *(void***)dev;
            if (dv && dv != (void***)dev) {
                PFN_Release rel = (PFN_Release)(void*)dv[2];
                if (rel) rel(dev);
            }
        }

#if TDVR_XR_OWN_DEVICE
        // Mode 14. The render thread cannot create the device itself:
        // D3D12CreateDevice takes a driver lock, and calling it there with a
        // frame in flight is the same stall shape the factory walk was moved off
        // to escape. It is a heavier lock than GetAdapter was. So the request
        // arrives here, on a thread that has no frame to block.
        if (tdvr_own_device_requested && !own_done) {
            own_done = 1;
            vr_log("[SC] worker: TDVR_XR_OWN_DEVICE -- creating a private "
                   "device+queue here, off the render thread");
            void* odev = NULL; void* oq = NULL;
            if (tdvr_xr_make_device_and_queue(&odev, &oq)) {
                vr_log("[SC] worker: private device=%p queue=%p -- binding the "
                       "session to it", odev, oq);
                tdvr_xr_try_session(&g_xr, odev, oq);
            } else {
                vr_log("[SC] worker: no private device+queue; the session will "
                       "not be bound at all in this mode");
            }
        }
#endif
        Sleep(500);
    }
    vr_log("[SC] worker: finished, saw %ld creation(s), walked=%d", (long)seen, walked);
    return 0;
}

// --- mode 14 handoff -------------------------------------------------------
//
// The worker's polling loop is driven by Sleep, not by the event, so waking it
// early is only an optimisation. What actually matters is that the render thread
// does not hand the request to a thread that was never started -- and it is
// started lazily, by the first chain capture, which in mode 14 may never happen.
int tdvr_sc_worker_running(void) {
    return g_worker != NULL;
}

void tdvr_sc_wake_event(void) {
    if (g_evt) SetEvent(g_evt);
    else       vr_log("[SC] no worker event yet; the worker will poll anyway");
}

// ---- vtable patch, with a mandatory read-back ----------------------------

// Is this address a committed, readable pointer-sized location?
//
// That is the real precondition for patching a vtable slot, and it is looser than
// in_dxgi() on purpose: the game's factory table is probably D3D12Core.dll's, and
// demanding dxgi would refuse to patch the exact table worth patching. The
// read-back afterwards is the authoritative check that the write landed --
// VirtualProtect succeeding proves nothing, which this file has already proven
// once by logging "installed" for a patch that never took effect.
static int patch_slot(void** vt, int n, void* hook, const char* name) {
    if (!sc_readable(&vt[n], sizeof(void*))) {
        vr_log("[SC] slot %2d %-22s unreadable -- refusing to patch", n, name);
        return 0;
    }
    void* expect = vt[n];
    if (!sc_readable(expect, sizeof(void*))) {
        vr_log("[SC] slot %2d %-22s = %p not in a committed region "
               "-- refusing to patch", n, name, expect);
        return 0;
    }
    DWORD old = 0;
    if (!VirtualProtect(vt, sizeof(void*) * 4, PAGE_READWRITE, &old)) {
        vr_log("[SC] slot %2d VirtualProtect failed %lu", n,
               (unsigned long)GetLastError());
        return 0;
    }
    vt[n] = hook;
    DWORD ign;
    VirtualProtect(vt, sizeof(void*) * 4, old, &ign);
    void* got = vt[n];
    int ok = (got == hook);
    vr_log("[SC] slot %2d %-22s %p -> %p  %s", n, name, expect, got,
           ok ? "VERIFIED" : "*** READ-BACK MISMATCH ***");
    return ok;
}

// ---- entry point ----------------------------------------------------------

// Walk DOWN from the device to the factory the game actually uses.
//
// Why this exists: patching dxgi.dll's shared factory vtable does not reach the
// game. Measured, with the patch installed two log lines before the game's first
// frame and D3D12 confirmed on, neither CreateSwapChain slot ever fires -- so the
// game's factory is a different object on a different table, very likely
// D3D12Core.dll's, because the Agility SDK implements its own.
//
// ID3D12Device DOES NOT INHERIT IDXGIDevice. From d3d12.h:
//     ID3D12Object : public IUnknown          (slots 0-2 only)
//     ID3D12Device : public ID3D12Object
// so there is no GetAdapter anywhere on an ID3D12Device, and calling vtable slot
// 10 as GetAdapter invoked some unrelated D3D12 method with mismatched arguments.
// It hung: the log stopped dead at "[SC] device=..." with no GetAdapter result,
// no crash and no minidump, for every run. A wrong slot here is not a crash, it is
// a hang, which is why the slot number is not something to be recalled.
//
// The correct route is the documented one:
//     ID3D12Device --QueryInterface(IID_IDXGIDevice)--> IDXGIDevice
//     IDXGIDevice  --GetAdapter(slot 10)-->            IDXGIAdapter
//     IDXGIAdapter --GetParent(slot 6, IID_IDXGIFactory4)--> the game's factory
// QueryInterface is slot 0 and is correct on every COM object by definition, so
// the only number that has to be right before anything is called is the GUID.
//
// IID_IDXGIAdapter1 was wrong here: the last-but-one byte was 0x1b where
// dxgi.h:2185 says 0x1a. Fifteen of sixteen bytes matched, so it reads as
// correct. Caught by a compiler comparison against the header
// (INITGUID + dxgi.h, then memcmp), not by eye.
static const GUID IID_IDXGIAdapter1_h =
    { 0x29038f61, 0x3839, 0x4626, { 0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05 } };
static const GUID IID_IDXGIDevice_h =
    { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };
// From d3d12.h, for the identity probe below. ID3D12Object is the base of
// ID3D12Device, so a device MUST answer this one; a queue must not.
// Value verified against d3d12.h line 3041. An earlier draft here had
// 9dbc-e448-4c2a-fc, which was recalled rather than read, and would have made
// the probe lie in the opposite direction.
static const GUID IID_ID3D12Object_h =
    { 0xc4fec28f, 0x7966, 0x4e95, { 0x9f, 0x94, 0xf4, 0x31, 0xcb, 0x56, 0xc3, 0xb8 } };

// IID_IDXGIFactory4, from dxgi.h. The root object of the adapter is this, and
// asking for the most derived factory is what makes GetParent return the real
// factory rather than an intermediate.
static const GUID IID_IDXGIFactory4_h =
    { 0x1bc6ea02, 0xef36, 0x464f, { 0xbf, 0x0c, 0x21, 0xca, 0x39, 0xe5, 0x16, 0x8a } };
static const GUID IID_IDXGIFactory2_h =
    { 0x50c83a1c, 0xe072, 0x4c48, { 0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0 } };

#define DXDEV_SLOT_GETADAPTER  10
#define ADP_SLOT_GETPARENT      6

typedef HRESULT (WINAPI *PFN_QI2)(void*, const GUID*, void**);
typedef HRESULT (WINAPI *PFN_GetAdapter)(void*, UINT, const GUID*, void**);
typedef HRESULT (WINAPI *PFN_GetParent)(void*, const GUID*, void**);

void tdvr_early_present_hook(void);
int  tdvr_install_present_hook(void);

// Returns the factory the game created, or NULL. Runs on the worker thread, not
// the render thread: the render thread is what hands over the device.
void* tdvr_factory_from_device(void* dev) {
    if (!dev) { vr_log("[SC] no device to walk from"); return NULL; }
    void** dv = *(void***)dev;
    if (!dv || dv == (void**)dev) { vr_log("[SC] device pointer is not a COM object"); return NULL; }
    if (!sc_readable(dv, sizeof(void*) * 20)) { vr_log("[SC] device vtable unreadable"); return NULL; }
    vr_log("[SC] device=%p vtable=%p", dev, (void*)dv);

    // Step 1: the DXGI view of the device. QI is slot 0 and cannot be wrong.
    //
    // But it returned E_NOINTERFACE on an object that had ALREADY answered
    // GetDevice(IID_ID3D12Device) with S_OK and CreateCommandQueue with S_OK and
    // reported feature level 0xB000. Those three are only consistent for a real
    // ID3D12Device, so a correct IID_IDXGIDevice should not fail here.
    //
    // One of those readings has to be a lie, and guessing which is how this
    // project has burned days. So ask the object what it actually is: QI a set
    // of interfaces that cannot all be true of the same object. Whatever comes
    // back decides the question, and the answer goes in the log.
    {
        struct { const char* nm; const GUID* id; } probe[] = {
            { "IID_IDXGIDevice",    &IID_IDXGIDevice_h },
            { "IID_ID3D12Device",   &IID_ID3D12Device    },
            { "IID_ID3D12Object",   &IID_ID3D12Object_h    },
            { "IID_ID3D12CommandQueue", &IID_ID3D12CommandQueue },
            { "IID_IDXGIFactory2",  &IID_IDXGIFactory2_h },
        };
        for (unsigned i = 0; i < sizeof(probe)/sizeof(probe[0]); i++) {
            void* o = NULL;
            HRESULT h = ((PFN_QI2)dv[0])(dev, probe[i].id, &o);
            vr_log("[SC]   QI %-24s hr=0x%08lX obj=%p", probe[i].nm,
                   (unsigned long)h, o);
            if (SUCCEEDED(h) && o) {
                void** ov = *(void***)o;
                vr_log("[SC]      -> vtable=%p in_dxgi=%d", (void*)ov, in_dxgi((void*)ov));
                PFN_Release rr = (PFN_Release)(void*)dv[2];
                if (rr) rr(o);
            }
        }
    }

    void* xdev = NULL;
    HRESULT hr = ((PFN_QI2)dv[0])(dev, &IID_IDXGIDevice_h, &xdev);
    vr_log("[SC] >> device QueryInterface(IID_IDXGIDevice) -> hr=0x%08lX obj=%p",
           (unsigned long)hr, xdev);
    if (hr != S_OK || !xdev) {
        vr_log("[SC] the device does not expose IDXGIDevice (hr=0x%08lX); "
               "0x80004002 = E_NOINTERFACE means the GUID is wrong or the object "
               "is not a D3D12 device", (unsigned long)hr);
        return NULL;
    }
    void** xv = *(void***)xdev;
    if (!xv || xv == (void**)xdev || !sc_readable(xv, sizeof(void*) * 16)) {
        vr_log("[SC] the IDXGIDevice vtable is not readable");
        PFN_Release r0 = (PFN_Release)(void*)dv[2];
        if (r0) r0(xdev);
        return NULL;
    }
    vr_log("[SC] IDXGIDevice=%p vtable=%p", xdev, (void*)xv);

    // Step 2: the adapter. Slot 10 on IDXGIDevice, counted through
    // IUnknown 0-2, IDXGIObject 3-6, IDXGIDeviceSubObject 7 GetDesc..., then
    // IDXGIDevice 8 GetAdapterLuid 9 GetNodeCount 10 GetAdapter.
    void* adp = NULL;
    hr = ((PFN_GetAdapter)xv[DXDEV_SLOT_GETADAPTER])(xdev, 0, &IID_IDXGIAdapter1_h, &adp);
    vr_log("[SC] << IDXGIDevice GetAdapter(0) -> hr=0x%08lX adapter=%p",
           (unsigned long)hr, adp);
    PFN_Release rx = (PFN_Release)(void*)xv[2];
    if (rx) rx(xdev);
    if (hr != S_OK || !adp) {
        vr_log("[SC] no adapter (hr=0x%08lX)", (unsigned long)hr);
        return NULL;
    }

    void** av = *(void***)adp;
    if (!av || av == (void**)adp || !sc_readable(av, sizeof(void*) * 16)) {
        vr_log("[SC] adapter vtable is not readable");
        PFN_Release r1 = (PFN_Release)(void*)dv[2];
        if (r1) r1(adp);
        return NULL;
    }
    vr_log("[SC] adapter=%p vtable=%p  in_dxgi=%d", adp, (void*)av, in_dxgi((void*)av));

    // Step 3: the root object IS the factory the game created.
    // IID_IDXGIFactory4 is declared at the top of this file.
    void* fac = NULL;
    hr = ((PFN_GetParent)av[ADP_SLOT_GETPARENT])(adp, &IID_IDXGIFactory4_h, &fac);
    vr_log("[SC] adapter GetParent(IID_IDXGIFactory4) -> hr=0x%08lX factory=%p",
           (unsigned long)hr, fac);
    PFN_Release rel = (PFN_Release)(void*)av[2];
    if (rel) rel(adp);

    if (hr != S_OK || !fac) {
        vr_log("[SC] the game's factory is not reachable via GetParent yet "
               "(hr=0x%08lX)", (unsigned long)hr);
        return NULL;
    }
    void** fv = *(void***)fac;
    vr_log("[SC] GAME'S FACTORY=%p vtable=%p  in_dxgi=%d  same-as-ours=%d",
           fac, (void*)fv, in_dxgi((void*)fv),
           fv == g_vtable_patched);

    // This is the object the game created, so this is the table it calls. Patch
    // it -- and only if the two slots really are functions in SOME loaded module,
    // which is the strongest test available before writing to a table.
    //
    // Note the guard changed on purpose. in_dxgi() required the slot to live in
    // dxgi.dll specifically, which is exactly the assumption that just failed:
    // the game's factory may well be D3D12Core.dll's. Requiring dxgi would have
    // silently refused to patch the very table this whole function exists to
    // find, and "refused to patch" looks identical to "hook installed" in a log
    // that only prints success.
    if (!fv || fv == (void**)fac || !sc_readable(fv, sizeof(void*) * 20)) {
        vr_log("[SC] the game's factory vtable is not readable -- not patching");
    } else {
        if (g_vtable_patched && g_vtable_patched == fv) {
            vr_log("[SC] this is the SAME table we already patched; "
                   "adding a second hook would make it call the first");
        } else {
            g_orig_csc  = (PFN_CreateSwapChain)fv[DXGI_SLOT_CREATESWAPCHAIN];
            g_orig_cscw = (PFN_CreateSwapChainForHwnd)fv[DXGI_SLOT_CREATESWCHAINHWND];
            vr_log("[SC] game's CreateSwapChain        = %p", (void*)g_orig_csc);
            vr_log("[SC] game's CreateSwapChainForHwnd = %p", (void*)g_orig_cscw);
            int a = patch_slot(fv, DXGI_SLOT_CREATESWAPCHAIN,
                               (void*)td_csc, "GAME CreateSwapChain");
            int b = patch_slot(fv, DXGI_SLOT_CREATESWCHAINHWND,
                               (void*)td_cscw, "GAME CreateSwapChainForHwnd");
            if (a || b) {
                g_vtable_patched = fv;
                vr_log("[SC] the GAME'S factory is now hooked: csc=%d cscw=%d", a, b);
                if (g_worker) {
                    SetEvent(g_evt);
                } else {
                    g_evt = CreateEventA(NULL, FALSE, FALSE, NULL);
                    g_worker = CreateThread(NULL, 0, tdvr_sc_worker, NULL, 0, NULL);
                    if (g_worker) vr_log("[SC] worker thread %p (late start)", g_worker);
                }
            } else {
                vr_log("[SC] neither of the game's slots could be patched");
            }
        }
    }
    return fac;
}

// The flag lives in the CWD of the game, not next to the DLL: the DLL is loaded
// by the injector into a process whose working directory is the game folder, and
// earlier runs proved the file is found there. Tokens are separated by spaces
// or commas, case-insensitive, and the WHOLE buffer is zeroed first so a short
// file cannot leave stale bytes for the matcher to trip over.
static int sc_flag_wanted(void) {
    char fb[64];
    memset(fb, 0, sizeof fb);
    FILE* ff = fopen("tdvr_flags.txt", "rb");
    if (!ff) return 0;
    size_t n = fread(fb, 1, sizeof(fb) - 1, ff);
    fclose(ff);
    fb[n < sizeof(fb) ? n : sizeof(fb) - 1] = 0;
    for (size_t i = 0; i + 2 <= n; i++) {
        if (fb[i] != 's' && fb[i] != 'S') continue;
        if (fb[i+1] != 'c' && fb[i+1] != 'C') continue;
        int before = (i == 0) || fb[i-1] == ' ' || fb[i-1] == ',' ||
                     fb[i-1] == '\n' || fb[i-1] == '\r' || fb[i-1] == '\t';
        char nx = fb[i+2];
        int after = (nx == 0) || nx == ' ' || nx == ',' ||
                    nx == '\n' || nx == '\r' || nx == '\t';
        if (before && after) return 1;
    }
    return 0;
}

// Forward declaration: tdvr_early_present_hook() is called from
// hook/teardown_vr.c's init_thread, and this header is included above it, so the
// definition below is not yet visible at the point of use.
int tdvr_install_present_hook(void);

// Called first thing in the worker entry, before the host-image search and
// before install_hooks(). The ordering is the whole point: this is a
// create-time hook and there is exactly one swapchain creation to catch.
void tdvr_early_present_hook(void) {
    int want = sc_flag_wanted();
    {
        char fb[64];
        memset(fb, 0, sizeof fb);
        FILE* ff = fopen("tdvr_flags.txt", "rb");
        if (ff) { size_t n = fread(fb, 1, sizeof(fb) - 1, ff); fclose(ff);
                  fb[n] = 0; }
        vr_log("[SC] tdvr_flags.txt='%s' -> present hook %s", fb,
               want ? "ENABLED" : "off");
    }
    if (want) tdvr_install_present_hook();
}

int tdvr_install_present_hook(void) {
    vr_log("[SC] === swapchain capture hook ===");

    // dxgi.dll range, for the module-membership test. From K32GetModuleInformation
    // or its re-export; nothing here imports dxgi.
    {
        typedef BOOL (WINAPI *PFN_GMI)(HMODULE, HMODULE, void*, DWORD);
        typedef struct { void* base; void* size; void* entry; } MI;
        HMODULE k = GetModuleHandleA("kernel32.dll");
        HMODULE dxgi = GetModuleHandleA("dxgi.dll");
        PFN_GMI gmi = NULL;
        if (k) {
            gmi = (PFN_GMI)(void*)GetProcAddress(k, "K32GetModuleInformation");
            if (!gmi) gmi = (PFN_GMI)(void*)GetProcAddress(k, "GetModuleInformation");
        }
        MI mi; memset(&mi, 0, sizeof mi);
        if (gmi && dxgi && gmi((HMODULE)GetCurrentProcess(), dxgi, &mi, (DWORD)sizeof mi)) {
            dxlo = (uint8_t*)mi.base;
            dxhi = dxlo + (size_t)(uintptr_t)mi.size;
        } else {
            vr_log("[SC] cannot get the dxgi.dll range -- not installing");
            return 0;
        }
        vr_log("[SC] dxgi range 0x%llX..0x%llX",
               (unsigned long long)(uintptr_t)dxlo, (unsigned long long)(uintptr_t)dxhi);
    }

    // The factory we create ourselves. Its vtable is the one dxgi.dll shares with
    // the game's factory, which is the ENTIRE basis for patching it.
    //
    // That basis is now measured, not assumed, and it FAILED. With the hook
    // installed two log lines before the game's first frame -- and with D3D12
    // confirmed on (D3D12Core.dll loaded, the Agility SDK) -- neither slot ever
    // fired. So the game's factory is not this table. D3D12Core.dll implements its
    // own DXGI factory when the Agility SDK is in play, and patching dxgi.dll's
    // table then patches a table the game never calls.
    //
    // The dump below is the evidence for that: if the first factory's slots and
    // the game's differ, the shared-table assumption is dead and the create
    // hook has to move to whichever module the game's factory vtable lives in --
    // which is only knowable from inside the process, from the real object.
    HMODULE dxgi = GetModuleHandleA("dxgi.dll");
    if (!dxgi) { vr_log("[SC] dxgi.dll is not loaded -- the game is not using it"); return 0; }
    typedef HRESULT (WINAPI *PFN_Mk)(const GUID*, void**);
    PFN_Mk mk = (PFN_Mk)(void*)GetProcAddress(dxgi, "CreateDXGIFactory1");
    if (!mk) { vr_log("[SC] no CreateDXGIFactory1"); return 0; }

    void* fac = NULL;
    HRESULT hr = mk(&IID_IDXGIFactory2_h, &fac);
    vr_log("[SC] CreateDXGIFactory1 -> hr=0x%08lX factory=%p",
           (unsigned long)hr, fac);
    if (hr != S_OK || !fac) { vr_log("[SC] cannot make a factory"); return 0; }

    void** vt = *(void***)fac;
    if (!vt || vt == (void**)fac) { vr_log("[SC] factory vtable is bogus"); return 0; }
    g_vtable_patched = vt;
    vr_log("[SC] factory vtable=%p (dxgi.dll's shared factory vtable)", vt);

    // Print the WHOLE table, attributed to a module where possible. The point is
    // to see whether slot 10 and slot 15 are dxgi thunks at all, and to have a
    // record to compare against a factory obtained from the game itself later.
    for (int i = 0; i < 26; i++)
        vr_log("[SC]   fac %2d %p%s", i, vt[i], in_dxgi(vt[i]) ? "" : "  <-- not dxgi");

    g_orig_csc  = (PFN_CreateSwapChain)vt[DXGI_SLOT_CREATESWAPCHAIN];
    g_orig_cscw = (PFN_CreateSwapChainForHwnd)vt[DXGI_SLOT_CREATESWCHAINHWND];
    vr_log("[SC] original CreateSwapChain        = %p", (void*)g_orig_csc);
    vr_log("[SC] original CreateSwapChainForHwnd = %p", (void*)g_orig_cscw);

    int a = patch_slot(vt, DXGI_SLOT_CREATESWAPCHAIN,
                       (void*)td_csc, "CreateSwapChain");
    int b = patch_slot(vt, DXGI_SLOT_CREATESWCHAINHWND,
                       (void*)td_cscw, "CreateSwapChainForHwnd");
    // Release our own factory reference; the vtable patch outlives the object,
    // which is fine -- dxgi.dll's vtables live in the module, not in the object.
    PFN_Release rel = (PFN_Release)vt[2];
    if (rel) rel(fac);

    if (!a && !b) { vr_log("[SC] no slot was patched -- giving up"); return 0; }
    vr_log("[SC] installed: csc=%d cscw=%d", a, b);

    g_evt = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_worker = CreateThread(NULL, 0, tdvr_sc_worker, NULL, 0, NULL);
    if (g_worker) vr_log("[SC] worker thread %p", g_worker);
    return 1;
}

#endif // TDVR_PRESENT_HOOK
