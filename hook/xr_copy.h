// xr_copy.h -- putting real pixels into the XR swapchain images
//
// The projection layer is accepted by the runtime (lastLayerCount = 1, measured)
// but the images behind it are still whatever the runtime left there, because a
// swapchain handle is not drawable. Only the ID3D12Resource objects behind it
// are, and they only appear if you ask:
//
//     xrEnumerateSwapchainImages(swapchain, capacity, &count, buf)  -> resources
//
// The D3D12 half of this file had every slot number wrong at first, which is
// worth writing down because the failure is silent and looks like a hang.
//
// Three facts, all measured against the SDK headers rather than remembered:
//
//  1. **ID3D12Device has 44 vtable entries, not ~130.** The methods usually
//     quoted at slot 100+ are on ID3D12Device1..4, which are SEPARATE
//     interfaces reached through QueryInterface. A device created through
//     ID3D12Device::CreateDevice is a plain ID3D12Device, so only the base
//     vtable is in play. Reading slot 109 out of a 44-entry table returned
//     whatever bytes followed it in memory, and calling that address froze the
//     game with no error anywhere -- "CreateFence" worked right up until it
//     jumped into garbage.
//
//  2. **ID3D12Device has no CopyResourceRegion.** That method does not exist on
//     the device at all; copies live on ID3D12GraphicsCommandList as
//     CopyResource (slot 17). This is a very common assumption and it is wrong.
//
//  3. **Signal and Wait are on ID3D12CommandQueue** (slots 14 and 15), not on
//     the device. A fence is signalled by the queue that executed the work.
//
// So a copy is: a command list, Reset, CopyResource, Close, then
// ExecuteCommandLists on the game's own queue. The queue is the game's, so the
// copy is ordered against the frame being presented -- no extra
// synchronisation, and no risk of reading a backbuffer the game is writing.

#ifndef TDVR_XR_COPY_H
#define TDVR_XR_COPY_H

#include "xr_session.h"

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------------
// Vtable access.
//
// The DLL imports nothing from d3d12.dll, so every call is a slot index read out
// of the object. td_read validates the address, so a bad object yields NULL
// instead of an access violation inside the renderer.
// ------------------------------------------------------------------
static void* tdvr_vslot(void* obj, int n) {
    if (!obj) return NULL;
    void* vt = NULL;
    if (!td_read((uint64_t)(uintptr_t)obj, &vt, sizeof vt) || !vt) return NULL;
    void* fn = NULL;
    if (!td_read((uint64_t)(uintptr_t)vt + (uint64_t)n * 8, &fn, sizeof fn)) return NULL;
    return fn;
}

static void* tdvr_slot(void* obj, int n) {
    if (!obj) return NULL;
    void* f = NULL;
    if (!td_read((uint64_t)(uintptr_t)tdvr_vslot(obj, n), &f, sizeof f) || !f) return NULL;
    return f;
}

// A vtable entry that is not a real code pointer is a silent killer.
//
// The measurement that forced this: minidump, EXCEPTION_ACCESS_VIOLATION at
// rva 0x5e15 inside tdvr_xr_bind_thread, with param1 = 0xFFFFFFFFFFFFFFFF. The
// instruction is `call *%rax` right after a slot lookup, so the lookup returned
// -1 and the `if (!f)` guard passed -- because -1 is not zero. Calling it jumped
// to address 0xFFFFFFFFFFFFFFFF and the process died inside our own DLL.
//
// So the guard is not "is it NULL", it is "is it a plausible code address". A
// module address has the high bits set and is nowhere near the top of the
// address space, and anything ending in FFFF is a sentinel, not a function.
static void* tdvr_slot_checked(void* obj, int n) {
    void* f = tdvr_slot(obj, n);
    if (!f) return NULL;
    uint64_t a = (uint64_t)(uintptr_t)f;
    // A rejected slot is worth saying out loud, with the raw value: it names the
    // real cause (wrong interface, object of the wrong type) instead of leaving
    // a blank preview with no clue why.
    if (a < 0x10000ULL || a > 0x00007FFFFFFF0000ULL || (a & 0xFFFFULL) == 0xFFFFULL) {
        static volatile long g_slot_rejects = 0;
        if (g_slot_rejects < 12) {
            vr_log("OpenXR: vtable slot %d on %p is not code (0x%llx) -- the "
                   "object is probably not the interface assumed",
                   n, obj, (unsigned long long)a);
        }
        g_slot_rejects++;
        return NULL;
    }
    return f;
}

// ------------------------------------------------------------------
// The command list that does the copy.
//
// Reused every frame: Reset, CopyResource, Close, Execute. Reusing one list is
// the normal pattern and avoids a per-frame allocation on the render thread.
//
// Measured slots (see the header note about why these are small numbers):
//   ID3D12GraphicsCommandList  10 Reset, 17 CopyResource,  9 Close
//   ID3D12CommandQueue         10 ExecuteCommandLists, 14 Signal
//   ID3D12Device                9 CreateCommandAllocator, 12 CreateCommandList,
//                              36 CreateFence
// ------------------------------------------------------------------

#define TDVR_CL_RESET         10
#define TDVR_CL_COPY_RESOURCE 17
#define TDVR_CL_CLOSE          9
#define TDVR_Q_EXECUTE        10
#define TDVR_DEV_ALLOC         9
#define TDVR_DEV_CREATE_CL    12
#define TDVR_DEV_CREATE_FENCE 36

// The IIDs, as raw bytes.
//
// The DLL must not import anything from d3d12.dll, so it cannot name
// IID_ID3D12CommandAllocator or IID_ID3D12Fence. These are the two constants
// involved, written out. They are interface identifiers, fixed by the Windows
// SDK and never changing, so spelling them out is safe -- and passing the wrong
// one would fail loudly rather than corrupt anything.
static const unsigned char kIID_ID3D12CommandAllocator[16] = {
    0xB5, 0x0B, 0x9B, 0x4B, 0x1C, 0x2C, 0x49, 0xAA,
    0x9E, 0x35, 0x24, 0x44, 0x53, 0x7C, 0x2C, 0xF2
};
static const unsigned char kIID_ID3D12Fence[16] = {
    0xF3, 0xE9, 0x75, 0x4F, 0xA2, 0xF1, 0x4C, 0x4A,
    0x99, 0x10, 0x27, 0x7B, 0x4C, 0x98, 0x28, 0x86
};

// ID3D12Device::CreateCommandAllocator -- slot 9
static void* tdvr_create_allocator(void* device) {
    typedef int (STDMETHODCALLTYPE *PFN_t)(void*, int, void*, void*);
    void* f = tdvr_slot_checked(device, TDVR_DEV_ALLOC);
    if (!f) return NULL;
    void* alloc = NULL;
    // D3D12_COMMAND_LIST_TYPE_DIRECT == 3
    if (((PFN_t)f)(device, 3, (void*)kIID_ID3D12CommandAllocator, &alloc) < 0 || !alloc)
        return NULL;
    return alloc;
}

// ID3D12Device::CreateCommandList -- slot 12
static void* tdvr_create_command_list(void* device, void* queue) {
    typedef int (STDMETHODCALLTYPE *PFN_t)(void*, int, int, void*, int, void*);
    void* f = tdvr_slot_checked(device, TDVR_DEV_CREATE_CL);
    if (!f) return NULL;
    void* list = NULL;
    // D3D12_COMMAND_LIST_STATE_NONE == 0
    if (((PFN_t)f)(device, 3, 0, queue, 0, &list) < 0 || !list) return NULL;
    return list;
}

// ID3D12Device::CreateFence -- slot 36
static void* tdvr_create_fence(void* device, uint64_t initial) {
    typedef int (STDMETHODCALLTYPE *PFN_t)(void*, uint64_t, int, void*, void*);
    void* f = tdvr_slot_checked(device, TDVR_DEV_CREATE_FENCE);
    if (!f) return NULL;
    void* fence = NULL;
    // D3D12_FENCE_FLAG_NONE == 0
    if (((PFN_t)f)(device, initial, 0, (void*)kIID_ID3D12Fence, &fence) < 0 || !fence)
        return NULL;
    return fence;
}

// ID3D12GraphicsCommandList::Reset -- slot 10
static int tdvr_cl_reset(void* list, void* alloc) {
    typedef int (STDMETHODCALLTYPE *PFN_t)(void*, void*);
    void* f = tdvr_slot_checked(list, TDVR_CL_RESET);
    if (!f) return 0;
    return ((PFN_t)f)(list, alloc) >= 0;
}

// ID3D12GraphicsCommandList::CopyResource -- slot 17
static int tdvr_cl_copy(void* list, void* dst, void* src) {
    typedef void (STDMETHODCALLTYPE *PFN_t)(void*, void*, void*);
    void* f = tdvr_slot_checked(list, TDVR_CL_COPY_RESOURCE);
    if (!f) return 0;
    ((PFN_t)f)(list, dst, src);
    return 1;
}

// ID3D12GraphicsCommandList::Close -- slot 9
static int tdvr_cl_close(void* list) {
    typedef int (STDMETHODCALLTYPE *PFN_t)(void*);
    void* f = tdvr_slot_checked(list, TDVR_CL_CLOSE);
    if (!f) return 0;
    return ((PFN_t)f)(list) >= 0;
}

// ID3D12CommandQueue::ExecuteCommandLists -- slot 10
static int tdvr_q_execute(void* queue, void* list) {
    typedef void (STDMETHODCALLTYPE *PFN_t)(void*, int, void**);
    void* f = tdvr_slot_checked(queue, TDVR_Q_EXECUTE);
    if (!f) return 0;
    ((PFN_t)f)(queue, 1, &list);
    return 1;
}

// ID3D12CommandQueue::Signal -- slot 14. A fence is signalled by the queue that
// ran the work, which is why this is not on the device.
static int tdvr_q_signal(void* queue, void* fence, uint64_t value) {
    typedef int (STDMETHODCALLTYPE *PFN_t)(void*, void*, uint64_t);
    void* f = tdvr_slot_checked(queue, 14);
    if (!f) return 0;
    return ((PFN_t)f)(queue, fence, value) >= 0;
}

// ------------------------------------------------------------------
// Bind the colour targets behind both swapchains.
//
// Run OFF the render thread: xrEnumerateSwapchainImages blocks there, measured,
// the same way xrEndFrame does. Returns the number of eyes bound.
// ------------------------------------------------------------------
static int tdvr_xr_bind_images(void* Xp) {
    tdvr_xr* X = (tdvr_xr*)Xp;
    if (!X->EnumerateSwapchainImages) {
        vr_log("OpenXR: xrEnumerateSwapchainImages unresolved -- the XR images "
               "cannot be written");
        return 0;
    }

    int bound = 0;
    for (int e = 0; e < 2; e++) {
        if (!X->sc_made[e]) continue;

        // The buffer is sized for the payload the runtime WRITES, which is
        // XrSwapchainImageD3D12KHR (24 bytes: type, next, texture) and not the
        // XrSwapchainImageBaseHeader the signature is declared with (16 bytes).
        // Allocating the base header let the runtime write 192 bytes into 128
        // and the game froze with no error anywhere.
        //
        // The two-call idiom (NULL first, then the real buffer) is what the spec
        // describes, but this runtime BLOCKS on the NULL call, so the array is
        // passed straight away with the capacity it is sized for. That is legal
        // -- imageCapacityInput is honoured either way.
        XrSwapchainImageD3D12KHR imgs[4];
        uint32_t count = 4;
        for (uint32_t i = 0; i < 4; i++) {
            memset(&imgs[i], 0, sizeof imgs[i]);
            imgs[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
        }
        XrResult r = X->EnumerateSwapchainImages(
            X->sc[e], 4, &count, (XrSwapchainImageBaseHeader*)imgs);
        if (XR_FAILED(r) || count == 0) {
            vr_log("OpenXR: xrEnumerateSwapchainImages(eye %d) -> %s (count=%u)",
                   e, tdvr_xr_err(X, r), count);
            continue;
        }

        int ok = 0;
        for (uint32_t i = 0; i < count && i < 4; i++) {
            if (imgs[i].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR) continue;
            if (!imgs[i].texture) continue;
            X->sc_res[e] = (void*)imgs[i].texture;
            ok = 1;
            break;
        }
        if (!ok) {
            vr_log("OpenXR: eye %d has %u images but not D3D12 (first type=%d)",
                   e, count, (int)imgs[0].type);
            continue;
        }
        bound++;
        vr_log("OpenXR: eye %d bound to texture %p (of %u images)", e,
               X->sc_res[e], count);
    }

    // The copy path, if the pieces are all there. A missing piece is logged and
    // the layer still submits -- an empty preview, not a crash.
    //
    // With TDVR_XR_COPY_ENABLED == 0 none of this is created at all. Creating a
    // command list, an allocator and a fence costs a real allocation on the
    // render thread and buys nothing while the copy is switched off, and the
    // whole point of the switch is that the run touches as little as possible.
#if TDVR_XR_COPY_ENABLED
    if (bound && X->gfx_device && X->gfx_queue) {
        X->cmd_alloc = tdvr_create_allocator(X->gfx_device);
        X->cmd_list  = tdvr_create_command_list(X->gfx_device, X->gfx_queue);
        X->gfx_fence = tdvr_create_fence(X->gfx_device, 0);
        vr_log("OpenXR: copy path: alloc=%p list=%p fence=%p queue=%p",
               X->cmd_alloc, X->cmd_list, X->gfx_fence, X->gfx_queue);
    }
#else
    (void)bound;
    vr_log("OpenXR: copy is OFF by default (TDVR_XR_COPY_ENABLED=0) -- the layer "
           "submits with an empty image, which is the stable configuration");
#endif
    return bound;
}

// Copy the game's backbuffer into both XR images, once per frame, after the
// images are acquired.
//
// One command list does both eyes and is executed on the game's own queue, so
// the copy is ordered against the frame being presented. No fence wait is needed
// here: the queue serialises it, and the runtime reads the image during
// xrEndFrame, which comes after this execute on the same queue.
static void tdvr_xr_copy_images(void* Xp) {
    tdvr_xr* X = (tdvr_xr*)Xp;
#if !TDVR_XR_COPY_ENABLED
    (void)X;
    return;                      // the switch is off; nothing is submitted
#endif
    if (!X->cmd_list || !X->cmd_alloc || !X->gfx_queue) return;
    if (!X->sc_res[0] && !X->sc_res[1]) return;
    void* src = g_xr_backbuffer;
    if (!src) return;

    if (!tdvr_cl_reset(X->cmd_list, X->cmd_alloc)) return;
    if (X->sc_res[0]) tdvr_cl_copy(X->cmd_list, X->sc_res[0], src);
#if !TDVR_XR_COPY_EYE1
    if (X->sc_res[1]) tdvr_cl_copy(X->cmd_list, X->sc_res[1], src);
#endif
    if (!tdvr_cl_close(X->cmd_list)) return;
    tdvr_q_execute(X->gfx_queue, X->cmd_list);
    X->copy_frames++;
}

// ------------------------------------------------------------------
// Running the bind off the render thread.
// ------------------------------------------------------------------

volatile long g_bind_done = 0;
static HANDLE        g_bind_thread = NULL;

static DWORD WINAPI tdvr_xr_bind_thread(LPVOID arg) {
    tdvr_xr_bind_images(arg);
    InterlockedExchange(&g_bind_done, 1);
    return 0;
}

static void tdvr_xr_start_bind(void* Xp) {
    if (g_bind_thread || g_bind_done) return;
    g_bind_thread = CreateThread(NULL, 0, tdvr_xr_bind_thread, Xp, 0, NULL);
    vr_log("OpenXR: bind thread %p (xrEnumerateSwapchainImages cannot run on "
           "the render thread)", (void*)g_bind_thread);
}

#ifdef __cplusplus
}
#endif
#endif /* TDVR_XR_COPY_H */
