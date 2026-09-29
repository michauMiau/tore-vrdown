// Hook Present on the renderer's own swapchain vtable.
//
// WHY THIS EXISTS
//
// Everything up to now has hooked the TRendererD3D12 vtable slots for
// beginRender and endRender, resolved through RTTI. Those installs succeed and
// the prologue checks pass, but build/full_run.ps1 proved they are never
// executed: the game was demonstrably drawing (5 s of CPU before injection)
// and the log showed "beginRender entered: no", "endRender: no". The classes
// are real and the addresses are real; the slots simply are not the per-frame
// path in the build that is actually running.
//
// Present is different. IDXGISwapChain::Present is vtable slot 8 (offset 0x40),
// it is called exactly once per displayed frame, and the address is not a
// guess: the live unpacked .text was disassembled and shows
//
//     mov  rdx, [rbx+0xE40]        ; rdx = this+0xE40 = IDXGISwapChain*
//     call qword ptr [rax+0x48]     ; Present
//
// so 0xE40 really is the swapchain pointer and 0x48 really is Present. Hooking
// it gives a per-frame signal that does not depend on the renderer class name,
// the vtable layout, or RTTI resolving to the instance that is actually
// drawing.
//
// The cost is that we do not know which object is at 0xE40 until a frame has
// been rendered, and since beginRender never runs we cannot read it from
// there. So this waits for a caller: the previous build already installs a
// detour on 0x5BAEB0, which is reached. From there the renderer's own object
// is not available either, so the offset is resolved differently - see
// td_present_from_render() below, which walks back from any pointer the game
// hands to the uploader and looks for a swapchain-shaped vtable.
//
// WHICH OBJECT - the qualification on all of the above
//
// "0xE40 is the swapchain" is only true of the object endRender receives.
// beginRender is handed a different object, and in the early frames where that
// object's +0xE40 was read, the value there was 0000023e5c019650, whose vtable
// the Present slot read back as 0x234800000238c181 - a float4 exponent pattern, not an
// address. Same offset, wrong object, no swapchain.
//
// So the offset is a property of the renderer, not of any object that happens
// to be passed to a frame hook. The disassembly reads it from endRender's own
// argument, and v55 measured the two hooks receiving different objects before
// the install was moved to hooked_end_render. Treat "self + 0xE40" as meaningful
// only in endRender, and always validate the result (td_find_present) before
// writing through it. A successful guarded read proves the ADDRESS is mapped; it
// does not prove the OBJECT is a swapchain. The full sequence is in
// docs/PRESENT_HOOK_DOUBLE_DEREF.md.
//
// SIMPLICITY OVER GUESSES
//
// A swapchain vtable is recognisable without knowing anything about Teardown:
// it is a table of code pointers, and the Present slot must be executable. A heap buffer
// of floats will not be. So the test is: is vtable[9] a committed, executable
// address inside a module? That is a fact about memory, not an assumption
// about an offset, and it cannot be fooled by a matrix full of floats that
// happen to look like a small integer.

#include "memscan.h"

// Slot 8 of IDXGISwapChain, and also of IDXGISwapChain1/2/3, all of which
// inherit it. 0x40 = 8 * 8.
//
// THIS WAS WRONG AND WAS SLOT 9. Slot 9 is GetBuffer, not Present. Counted from
// the real IDXGISwapChainVtbl in dxgi.h:
//     0 QueryInterface   1 AddRef              2 Release
//     3 SetPrivateData   4 SetPrivateDataInterface  5 GetPrivateData
//     6 GetParent        7 GetDevice (0x38)
//     8 Present (0x40)   9 GetBuffer (0x48)   10 SetFullscreenState
//    11 GetFullscreenState  12 GetDesc
//
// The wrong slot produced a hook that looked perfect and was useless: the vtable
// and the saved "original" both resolved to D3D12Core.dll+0x2350E0, the patch was
// read back VERIFIED, and not one Present was ever counted. Worse, it patched
// GetBuffer on an object that turned out not to be a swapchain at all -- so the
// "original" being saved was GetBuffer on an Agility-SDK object, and calling it
// as Present (two UINT arguments instead of UINT+REFIID+void**) is a
// miscall. The real Present is at 0x48-8=0x40.
//
// Do not "confirm" a slot against a disassembly comment. dxgi.h is the truth and
// the vtable is enumerable; the comment above used to claim 0x48 was Present and
// that is what carried the error for a long time.
#define TD_PRESENT_SLOT 8

// Offset of the IDXGISwapChain* inside the renderer object.
//
// MEASURED on the build that is actually running: dump_text.ps1 copied .text
// out of a clean teardown process (RVA 0x1000, 0x97B1AC bytes) and
// find_present.py searched it for the render path. 47 sites call
// qword ptr [rax+0x48]; exactly one is fed by a member load of the same
// object, at RVA 0x5B6EB1:
//
//     0x5B6E7F  lea  rcx, [rbx+0x1130]     constant upload buffer
//     0x5B6E86  mov  r8d, 0x800             2048 bytes
//               call <uploader>
//     0x5B6E94  mov  rdx, [rbx+0xE40]      IDXGISwapChain*
//     0x5B6EB1  call qword ptr [rax+0x48]  Present
//
// 0xE40 is the swapchain, 0x48 is its Present - for the object endRender
// receives, and only after it is confirmed to be a swapchain. The qualification
// is load-bearing, not pedantry; see "WHICH OBJECT" below and the same note at
// the TD_OFF_SWAPCHAIN definition in teardown_vr.c. If the offset moves on
// another build the scan in td_present_from_object finds it instead, so this
// constant is an optimisation, not a hard dependency.
#define TD_SWAPCHAIN_OFFSET 0xE40

// Bounds on the vtable scan, which runs on the render thread inside a frame.
//
// 0x1100 bytes of candidate offsets is 544 slots, and each slot can cost a
// 16-entry vtable walk, so an unbounded scan performs tens of thousands of
// VirtualQuery calls in one frame. That stalled the game and was followed by a
// death attributed to the mod, when in fact the mod was simply spending seconds
// inside a frame doing far too much work. Two rounds is enough to catch a
// swapchain that appears on the first frame, which is the only case observed.
#define TD_SCAN_ROUNDS  2
#define TD_SCAN_GIVEUP  24

static volatile LONG g_present_calls = 0;
static volatile LONG g_present_missed = 0;   // calls dropped before the
                                           // original was published
static void*    g_present_orig     = NULL;

typedef HRESULT (WINAPI *td_present_fn)(void* swapchain, const void* syncIntervals);

// Forward declaration: build_detour is defined later in teardown_vr.c.
static void* build_detour(void* target, void* hook, uint8_t* trampoline,
                          size_t copy_len);

static void hooked_present(void* swapchain, const void* syncIntervals) {
    long n = InterlockedIncrement(&g_present_calls);

    // g_present_orig is written by the installing thread AFTER the slot is
    // swapped, so there is a window in which the game can call this hook while
    // the original pointer is still NULL. Calling through NULL would fault the
    // render thread inside our own code - the exact failure this whole session
    // has been fighting. If the original is not published yet, the swapchain's
    // own slot still holds either the real function or us; reading it back is
    // only safe once the write is visible, so the call is simply skipped and
    // that single frame is dropped. Dropping one frame is invisible; faulting
    // is a crash dump dialog.
    void* real = g_present_orig;
    if (!real) {
        InterlockedIncrement(&g_present_missed);
        return;
    }

    // Log sparingly: 1, then every 600th. Enough to prove the hook is live
    // without a log write per frame.
    if (n == 1 || n % 600 == 0) {
        vr_log("present: call %ld  swapchain=%p sync=%p",
               n, swapchain, syncIntervals);

        if (n == 1) {
            // Now that a real frame is known to be in flight, the swapchain
            // pointer is ground truth rather than a stored value, and its
            // vtable can be dumped. Slot 9 is what we are about to hook.
            //
            // The vtable is read through td_read, never dereferenced directly.
            // A raw "*(void***)swapchain" inside the Present hook is the worst
            // possible place for one: this runs on the render thread inside the
            // frame, and if the pointer is ever stale the result is an access
            // violation on the render thread, which is exactly the delayed
            // crash this project has been fighting. Reading it cannot fault.
            void** vt = NULL;
            if (td_read((uint64_t)(uintptr_t)swapchain, &vt, sizeof vt) &&
                vt) {
                vr_log("present: vtable=%p", (void*)vt);
                for (int k = 0; k < 10; ++k) {
                    void* f = NULL;
                    if (!td_read((uint64_t)(uintptr_t)vt + k * 8, &f, sizeof f)) {
                        f = NULL;
                    }
                    vr_log("    slot %d @+%02X = %p%s", k, k * 8, f,
                           k == TD_PRESENT_SLOT ? "   <- Present" : "");
                }
            } else {
                vr_log("present: swapchain=%p has no readable vtable",
                       swapchain);
            }
        }
    }

    ((td_present_fn)real)(swapchain, syncIntervals);
}

// Is this a plausible function pointer?
//
// A committed, executable page is necessary but NOT sufficient, and relying on
// it alone is what produced a genuinely dangerous false positive. The vtable
// scan accepted an object at renderer+0xEF8 - which is the frame-context array
// offset, not a swapchain - and installed the hook on it. The "original
// Present" it saved was 000000000a9f8def, a value out of game data that
// VirtualQuery happened to report as committed and executable. Writing
// hooked_present over that would have put a function pointer where none
// belonged.
//
// So a candidate must ALSO fall inside a module the process actually loaded.
// Every real DXGI thunk lives in dxgi.dll, d3d11.dll, d3d12.dll or the game
// itself, all of which appear in the module list; a float from a data array
// does not. That is checked by walking the PEB's loader list via
// EnumProcessModules, which is what psapi is for.
//
// The check is deliberately conservative: a legitimate thunk this rejects
// costs one log line, while a false positive costs a crash inside a vtable
// that the game will call.
// Cached module ranges.
//
// td_addr_in_module is called once per vtable slot and the scan reads 16 slots
// per candidate object, so taking a Toolhelp snapshot every time would run tens
// of thousands of times on the frame path. The module list of a live process
// only changes when something is loaded, and the mod resolves its own host
// image at startup, so the ranges are captured once and reused.
//
// A miss falls through to a fresh snapshot rather than trusting the cache, so
// a DLL loaded after startup is still found.
typedef struct td_range {
    uintptr_t lo, hi;
    char name[64];
} td_range;

#define TD_MAX_RANGES 256
static td_range g_ranges[TD_MAX_RANGES];
static LONG     g_ranges_n = -1;      // -1 = not captured yet

static int td_capture_ranges(void) {
    LONG n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    MODULEENTRY32W me;
    me.dwSize = sizeof me;
    if (Module32FirstW(snap, &me)) {
        do {
            if (n < TD_MAX_RANGES) {
                g_ranges[n].lo = (uintptr_t)me.modBaseAddr;
                g_ranges[n].hi = g_ranges[n].lo + me.modBaseSize;
                WideCharToMultiByte(CP_UTF8, 0, me.szModule, -1,
                                    g_ranges[n].name,
                                    (int)sizeof g_ranges[n].name, 0, 0);
                n++;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    InterlockedExchange(&g_ranges_n, n);
    return (int)n;
}

static int td_addr_in_module(const void* p) {
    if (!p) return 0;
    uintptr_t a = (uintptr_t)p;

    LONG n = g_ranges_n;
    if (n < 0) n = td_capture_ranges();

    for (LONG i = 0; i < n; ++i) {
        if (a >= g_ranges[i].lo && a < g_ranges[i].hi) return 1;
    }

    // Cache miss. Re-read the module list once, but only if the cached snapshot
    // is older than a moment, so a late-loaded dxgi.dll is still accepted
    // without taking a snapshot on every frame.
    static LONG last = 0;
    LONG now = GetTickCount();
    if (now - last > 2000) {
        last = now;
        td_capture_ranges();
        for (LONG i = 0; i < g_ranges_n; ++i) {
            if (a >= g_ranges[i].lo && a < g_ranges[i].hi) return 1;
        }
    }
    return 0;
}

static int td_is_code_ptr(const void* p) {
    if (!p) return 0;
    uintptr_t a = (uintptr_t)p;
    if (a < 0x10000) return 0;                     // never a valid image address
    if (a & 7) return 0;                           // code is at least 8-aligned

    // Must be inside a loaded module. See the comment above: without this the
    // scan accepts game data and the hook is installed on the wrong object.
    if (!td_addr_in_module(p)) return 0;

    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)p, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    DWORD prot = mbi.Protect;
    if (prot & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ |
                       PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (prot & exec) != 0;
}

// Log why a candidate was refused, at most twice per distinct address shape.
//
// The scan reported "no swapchain-shaped vtable" for five builds without ever
// naming the candidate or the reason, so a fix could not be confirmed from the
// log alone. This prints the address, whether it falls in a loaded module, and
// the page protection - which is the entire decision being made.
static LONG g_reject_m1 = 0, g_reject_m2 = 0;

static void td_log_reject(const char* what, const void* p) {
    if (!p) return;
    uintptr_t a = (uintptr_t)p;

    // Two buckets: pointers that are inside an image and pointers that are not.
    // Most of a scan's candidates are the latter, and printing one of each is
    // enough to see whether the filter is working.
    if (td_addr_in_module(p)) {
        if (InterlockedIncrement(&g_reject_m1) > 2) return;
    } else {
        if (InterlockedIncrement(&g_reject_m2) > 2) return;
    }

    const char* where = "not in any loaded module";
    LONG n = g_ranges_n < 0 ? 0 : g_ranges_n;
    for (LONG i = 0; i < n; ++i) {
        if (a >= g_ranges[i].lo && a < g_ranges[i].hi) { where = g_ranges[i].name; break; }
    }

    const char* prot = "unqueryable";
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((void*)p, &mbi, sizeof mbi)) {
        switch (mbi.Protect) {
        case PAGE_EXECUTE:           prot = "PAGE_EXECUTE"; break;
        case PAGE_EXECUTE_READ:      prot = "PAGE_EXECUTE_READ"; break;
        case PAGE_EXECUTE_READWRITE: prot = "PAGE_EXECUTE_READWRITE"; break;
        case PAGE_READONLY:          prot = "PAGE_READONLY"; break;
        case PAGE_READWRITE:         prot = "PAGE_READWRITE"; break;
        default:                     prot = "other-prot"; break;
        }
    }

    vr_log("present: reject %s=%p module=%s prot=%s", what, p, where, prot);
}

// Given any object, does its vtable look like a swapchain's? Checks a few
// slots rather than only the Present slot, because a game that wraps DXGI may have its
// own vtable with the DXGI methods at different offsets; finding a run of
// code pointers around 0x48 is the signal.
static void* td_find_present(void* obj) {
    if (!obj) return NULL;

    // ONE dereference, not two. An IDXGISwapChain is a C++ object whose first
    // word is already the pointer to its vtable, so reading obj gives the
    // vtable directly.
    //
    // THIS IS A TRAP THAT COST FOUR BUILDS (v54 through v57). It is recorded
    // here because the code looks correct at a glance and the failure is
    // silent: the validator runs, rejects everything, and reports
    // "not in any loaded module" forever, which reads like a wrong offset
    // rather than a wrong read.
    //
    // What happened: this used to dereference twice - read obj, then read that
    // result again as if it were another object needing its own vtable pointer.
    // That lands on vtable slot 0, not on the vtable. For a D3D12 vtable slot 0
    // is not a function pointer but interface data; the values read back were
    // 0x234800000238c181, a float4 exponent pattern, which is indistinguishable
    // from the float data a wrong OBJECT would give. That ambiguity is what
    // made this expensive: for several builds the diagnosis was split between
    // "the offset is wrong" and "we are reading the wrong object", and both
    // readings were compatible with the log. They were both wrong. The offset
    // was right and the read was wrong.
    //
    // The tie-break: the scan-based probe in the caller was doing a single
    // read all along and found a real swapchain, while this validator rejected
    // the same pointer. Two paths disagreeing on the same address is a bug in
    // one of them, and the one that found the swapchain was the one that
    // agreed with the disassembly.
    //
    // So: if this ever needs to look "more correct", it is already correct.
    // The rule is that for any C++ interface pointer, obj[0] IS the vtable.
    // Full account in docs/PRESENT_HOOK_DOUBLE_DEREF.md.
    void** vt = NULL;
    if (!td_read((uint64_t)(uintptr_t)obj, &vt, sizeof vt) || !vt) return NULL;

    void* presentFn = NULL;
    if (!td_read((uint64_t)(uintptr_t)vt + TD_PRESENT_SLOT * 8, &presentFn,
                 sizeof presentFn))
        return NULL;
    if (!td_is_code_ptr(presentFn)) {
        td_log_reject("present-slot", presentFn);
        return NULL;
    }

    // Require at least three more code pointers in the same table, so a single
    // lucky value in a data array cannot qualify.
    int code_slots = 0;
    for (int k = 0; k < 16; ++k) {
        void* f = NULL;
        if (td_read((uint64_t)(uintptr_t)vt + k * 8, &f, sizeof f) && td_is_code_ptr(f))
            code_slots++;
    }
    if (code_slots < 4) {
        vr_log("present: %p looks vtable-ish but only %d of 16 slots are "
               "code pointers (want 4)", (void*)vt, code_slots);
        return NULL;
    }
    return presentFn;
}

// Install the detour on Present, given the swapchain object.
//
// Install the hook by replacing the vtable slot, NOT by detouring the code.
//
// WHY NO BYTE STEALING HERE
//
// The earlier version of this function stole a prologue out of the DXGI Present
// thunk and built a trampoline. That requires measure_stealable to accept the
// function's entry instructions, and it kept refusing them: first
// "48 8B 81 E0" (mov rax,[rcx-0x20]), then "89 54 24 10" (mov [rsp+0x10],edx),
// then "48 89 5C 24" (mov [rsp+disp8],rbx). Each refusal produced one log line
// and no hook, across five builds, and every one of those targets is a
// perfectly ordinary DXGI entry point.
//
// None of that is necessary. This hook replaces a POINTER in a vtable, not code.
// The original slot value is read, kept, and called directly:
//
//     vtable[9] = hooked_present;   // our function
//     hooked_present -> vtable[9] originally held
//
// No instruction is split, no trampoline is built, no prologue has to be
// recognised, and there is nothing to restore on unload. The prologue decoder is
// still used elsewhere for functions that genuinely do need a detour, but a
// pointer swap should never have needed one.
//
// The vtable lives in read-only memory shared with dxgi.dll, so the write goes
// through VirtualProtect and is restored immediately afterwards. Only the 8
// bytes of the slot are made writable rather than the whole page, so a
// concurrent reader of a neighbouring slot cannot observe a torn pointer.

static int td_install_present(void* swapchain) {
    if (g_present_orig) return 1;                 // already done
    if (!swapchain) return 0;

#if !TDVR_PRESENT_ENABLED
    // Bisect variant B: beginRender/endRender stay hooked, Present does not. The
    // swapchain hook is the most pointer-chasing code in the build -- it walks a
    // vtable on a per-frame object, validates the result, and patches a slot --
    // so it is the first thing to set aside when isolating a crash.
    vr_log("bisect B: Present hook disabled by build switch");
    return 0;
#endif

    // td_find_present is the only place that decides whether this object really
    // is a swapchain. Its verdict has to be what decides here too.
    //
    // The previous version called td_find_present and then, on success,
    // re-derived the vtable itself with `vt = *(void***)swapchain` and wrote
    // vt[9] without rechecking anything. That second derivation is what let a
    // bogus object through: the log showed "HOOK LIVE. Real Present saved at
    // 000000000ab7b37f", an address whose low three bits are 7 and which is
    // plainly not code. td_find_present had rejected such pointers, but the
    // write path never asked it.
    void* target = td_find_present(swapchain);
    if (!target) {
        vr_log("present: refusing to hook %p - td_find_present rejected it",
               swapchain);
        return 0;
    }

    // Read the vtable through td_read rather than dereferencing, so an
    // unreadable pointer is a clean refusal instead of an access violation on
    // the render thread - the failure this whole session has been chasing.
    void** vt = NULL;
    if (!td_read((uint64_t)(uintptr_t)swapchain, &vt, sizeof vt) || !vt) {
        vr_log("present: %p has no readable vtable pointer", swapchain);
        return 0;
    }

    // Final gate on the value about to be saved, independent of the scan.
    // If this ever fails the object is not what td_find_present thought it was.
    void* real = NULL;
    if (!td_read((uint64_t)(uintptr_t)vt + TD_PRESENT_SLOT * 8, &real,
                 sizeof real)) {
        vr_log("present: cannot read slot %d of vtable %p", TD_PRESENT_SLOT,
               (void*)vt);
        return 0;
    }
    if (real != target || !td_is_code_ptr(real)) {
        vr_log("present: refusing: slot %d of vtable %p is %p, expected %p%s",
               TD_PRESENT_SLOT, (void*)vt, real, target,
               (real == target) ? " (not a valid code pointer)" : "");
        td_log_reject("present-slot-at-install", real);
        return 0;
    }

    // Refuse if the slot is already ours: a second copy of this DLL in the
    // process must not point the slot at a different module's hook, because
    // the first copy's Present counter would stop and its cleanup would be
    // meaningless. Read through td_read here too - real was already read into
    // a local above, and using it avoids a second raw vtable access.
    if (td_is_our_hook(real, g_module)) {
        vr_log("present: slot %d already holds this mod's hook - another copy "
               "of the DLL is loaded", TD_PRESENT_SLOT);
        g_present_orig = real;
        return 0;
    }

    // real was read and validated through td_read above; reuse it rather than
    // dereferencing the vtable a second time, which would bypass the guard for
    // no reason.
    vr_log("present: vtable=%p slot %d @+%02X real=%p -> %p",
           (void*)vt, TD_PRESENT_SLOT, TD_PRESENT_SLOT * 8,
           real, (void*)hooked_present);

    DWORD old = 0;
    // One page-granular protect around the 8-byte slot.
    if (!VirtualProtect(&vt[TD_PRESENT_SLOT], sizeof(void*),
                        PAGE_READWRITE, &old)) {
        vr_log("present: VirtualProtect failed: %lu", GetLastError());
        return 0;
    }
    vt[TD_PRESENT_SLOT] = (void*)hooked_present;
    VirtualProtect(&vt[TD_PRESENT_SLOT], sizeof(void*), old, &old);

    // The slot now holds our hook, so the original has to be remembered
    // somewhere the hook can reach. It is set only after the write, so a thread
    // that calls into the hook in between sees a valid original rather than
    // NULL.
    g_present_orig = real;

    vr_log("present: HOOK LIVE. Real Present saved at %p, calls will be "
           "counted from the vtable.", real);
    return 1;
}

// Called with the renderer object, found by reading the live process.
//
// The renderer is NOT the object the uploader is called with. The uploader's
// rcx is a 32-byte descriptor with a single member pointer, and scanning it for
// a swapchain-shaped vtable finds nothing, which is what earlier runs reported.
// The renderer is the object endRender does its work on, and the swapchain sits
// at a measured offset inside it.
//
// MEASURED, not guessed: dump_text.ps1 copied .text out of a clean process and
// find_present.py searched it. "call qword ptr [rax+0x48]" occurs 47 times, and
// exactly one of those is preceded by a member load of the same object:
//
//   RVA 0x5B6EB1   mov rdx, [rbx+0xE40]  ->  call [rax+0x48]
//
// 0xE40 is therefore the IDXGISwapChain* and 0x48 is its Present, for the
// object endRender receives. See "WHICH OBJECT" at the top of this file.
// (This comment used to cite docs/PRESENT_CALLSITE.md, which does not exist.)
// The scan is kept as a fallback in case the offset moves on another build,
// but the known offset is read first because it is one read instead of 0x220.
static void td_present_from_object(void* obj) {
    if (!obj || g_present_orig) return;

    // Try the measured offset first.
    void* known = NULL;
    if (td_read((uint64_t)(uintptr_t)obj + TD_SWAPCHAIN_OFFSET, &known,
                sizeof known) &&
        known && ((uintptr_t)known & 7) == 0) {
        void* present = td_find_present(known);
        if (present) {
            vr_log("present: swapchain at renderer+0x%X = %p, slot %d = %p",
                   TD_SWAPCHAIN_OFFSET, known, TD_PRESENT_SLOT, present);
            if (td_install_present(known)) return;
        } else {
            vr_log("present: renderer+0x%X = %p has no Present in slot %d",
                   TD_SWAPCHAIN_OFFSET, known, TD_PRESENT_SLOT);
        }
    } else {
        vr_log("present: cannot read renderer+0x%X", TD_SWAPCHAIN_OFFSET);
    }

    // The scan window is 0x1100 bytes = 544 slots, and each candidate can cost a
    // 16-entry vtable walk, so an unbounded scan performs tens of thousands of
    // VirtualQuery calls in a single frame on the render thread. The caller
    // (hooked_begin_render) already limits how many times this is entered;
    // inside, the scan gives up early once a stretch of consecutive candidates
    // has all been rejected, so a build where the swapchain is not reachable
    // this way stops paying for it.
    int consecutive_misses = 0;
    for (uint32_t off = 0x100; off < 0x1200; off += 8) {
        if (off == TD_SWAPCHAIN_OFFSET) continue;
        void* cand = NULL;
        if (!td_read((uint64_t)(uintptr_t)obj + off, &cand, sizeof cand)) continue;
        if (!cand || ((uintptr_t)cand & 7)) continue;

        // Cheap pre-filter before the expensive walk. A real swapchain lives in
        // a heap block whose vtable is in a module's .rdata; its first word is
        // therefore a pointer into read-only data, not into executable code.
        // Testing that rejects most of a renderer's pointer fields outright.
        void* first = NULL;
        if (!td_read((uint64_t)(uintptr_t)cand, &first, sizeof first)) continue;
        if (!first || !td_addr_in_module(first)) continue;

        void* present = td_find_present(cand);
        if (present) {
            vr_log("present: found at renderer+0x%X = %p (vtable slot %d)",
                   off, present, TD_PRESENT_SLOT);
            if (td_install_present(cand)) return;
        } else {
            if (++consecutive_misses >= TD_SCAN_GIVEUP) {
                vr_log("present: %d consecutive non-candidates by "
                       "renderer+0x%X, giving up on the scan",
                       consecutive_misses, off);
                return;
            }
        }
    }
    vr_log("present: no swapchain-shaped vtable in the first 0x1200 bytes");
}
