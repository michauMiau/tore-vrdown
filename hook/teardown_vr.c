// teardown_vr.c — Teardown VR Mod hook DLL
//
// Hooks TRendererD3D12::beginRender / endRender at their real addresses in the
// decrypted Steamless build, so we can drive the frame twice per display frame
// with a different camera for each eye.
//
// Build under analysis (see docs/UNPACKED_ANALYSIS.md):
//   image base        0x140000000
//   beginRender       0x1405AB9D0 .. 0x1405ABCAA
//   endRender         0x1405AF270 .. 0x1405AF67C
//   real Present      call [rax+0x48] at 0x1405AF2D1 (inside endRender)
//
//   TRendererD3D12 member offsets:
//     +0xCC   BYTE   rendering flag
//     +0xDA8  int32  frame index
//     +0xE40  ptr    IDXGISwapChain*  -- for the object endRender receives.
//                     beginRender gets a different object and in early frames
//                     this offset holds float data, not a pointer. Always
//                     validate before use; see TD_OFF_SWAPCHAIN below.
//     +0xE58  ptr    ID3D12CommandQueue*
//     +0xEF8  ptr    frame context array, stride 0x30
//
// Build:
//   x86_64-w64-mingw32-gcc -shared -O2 -o build/teardown_vr.dll hook/teardown_vr.c -lole32 -luser32 -static-libgcc -static-libstdc++ -lm

#define WIN32_LEAN_AND_MEAN
// 10 = _WIN32_WINNT_WIN7, which is what Module32FirstW and friends need to be
// declared by <tlhelp32.h>. Without the macro the header hides the whole Toolhelp
// module-walking API and the build fails on an undeclared MODULEENTRY32W.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <winreg.h>   // RegGetValueA, for the OpenXR ActiveRuntime lookup
#include <math.h>     // atan2f, asinf, for the HMD pose
// Toolhelp module walking: CreateToolhelp32Snapshot / Module32FirstW /
// Module32NextW. Used by td_addr_in_module to reject a vtable candidate whose
// "function pointers" are not inside any loaded module.
#include <tlhelp32.h>
#include <psapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

// Forward declarations, before the includes below: several of those headers log
// through vr_log and call td_read or use the walk's scratch buffers, so the
// prototypes and definitions have to exist first.
static void vr_log(const char *fmt, ...);

// A guarded read.
//
// The original version asked VirtualQuery about the *start* of the range and
// trusted the answer to cover the whole 4 KB. It does not: the MBI describes
// only the region containing the start address, so a pointer sitting 100 bytes
// before the end of a committed region passed the check and then memcpy read
// 4 KB past it. That is an access violation on the render thread, which is the
// most likely explanation for v24 dying immediately after pass1 reported
// n2n=7454 — thousands of candidate pointers, several of them at the very end
// of their regions.
//
// So walk the range region by region. Every byte must land in a committed,
// readable region; if any part does not, copy nothing and fail. A partial copy
// would be worse than a refusal: the caller would test half-old, half-new
// memory and believe it.
//
// This returns the number of bytes copied, 0 on any gap. The caller treats
// anything short as a refusal, so there is no way to get a torn buffer.
//
// The page check alone is not enough, and the v25 crash is the proof. It
// happened after 14944 frames, not immediately, which is the signature of a
// race rather than a bad pointer: the walk read a page that VirtualQuery had
// confirmed committed and readable a few microseconds earlier, and by the time
// the memcpy ran, the render thread had released it. A thousand times a second
// is a thousand chances.
//
// Structured exception handling would catch the resulting access violation, but
// this toolchain cannot express it: mingw's excpt.h provides __try1/__except1
// and no __try/__except, and -fseh-exceptions does not exist in gcc 14
// posix, so there is no flag that makes __try compile. A DLL that cannot be
// compiled with SEH cannot be trusted on a heap another thread is mutating.
//
// So the copy is not a raw memcpy at all. ReadProcessMemory on one's own
// process takes the kernel path: it probes the range and returns a failure
// instead of raising, for the whole range, atomically with respect to the
// caller. That closes the TOCTOU window by construction rather than catching
// the fault after the fact, and it needs no compiler support.
//
// A stub named td_read_unsafe used to sit here. It was never called, and it is
// deleted deliberately: its body was `return len;` - it reported a successful
// read of the full range without reading anything. The name made it look like
// the raw-memcpy path described above, so anyone calling it would have believed
// a guard had run when none had. This comment block is the documentation for
// why no such function exists; do not reintroduce one. Everything that reads
// game memory goes through td_read below.

// Read via ReadProcessMemory on ourselves.
//
// GetCurrentProcess() returns a pseudo-handle with PROCESS_VM_READ baked in,
// so this needs no privileges and no OpenProcess. The kernel validates the
// whole range and returns FALSE for a page that is not readable, so a race
// with the render thread produces a short read rather than a fault.
//
// The VirtualQuery loop above is kept as a fast path: it is a cheap user-mode
// check that rejects the large majority of the 7454 level-2 pointers without
// a syscall, and ReadProcessMemory only runs for the ones that pass. Every
// byte still goes through the kernel path, though, so the fast path is an
// optimisation and never the thing that makes a read safe.
static uint32_t td_read_rpm(uint64_t at, void* dst, uint32_t len) {
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
                           (LPCVOID)(uintptr_t)at,
                           dst, (SIZE_T)len, &got))
        return 0;
    return (uint32_t)got;
}

// Log a handful of RPM refusals per walk, with everything needed to tell a
// genuinely unreadable page from a miscalculated chunk size.
static LONG g_rpm_fail_logged;
static void td_read_fail2(uint64_t at, uint32_t want, uint32_t got,
                          const MEMORY_BASIC_INFORMATION* mbi) {
    if (InterlockedIncrement(&g_rpm_fail_logged) > 6) return;
    vr_log("  rpm fail at %016llX want=%u got=%u | state=0x%X prot=0x%X "
           "type=0x%X base=%p size=%llX",
           (unsigned long long)at, want, got,
           (unsigned)mbi->State, (unsigned)mbi->Protect, (unsigned)mbi->Type,
           mbi->BaseAddress, (unsigned long long)mbi->RegionSize);
}

static uint32_t td_read(uint64_t at, void* dst, uint32_t len) {
    unsigned char* out = (unsigned char*)dst;
    uint64_t done = 0;
    uint32_t guard = 0;

    while (done < len) {
        if (++guard > 64) return 0;              // 4 KB spans at most 2 pages

        uint64_t cur = at + done;
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPCVOID)(uintptr_t)cur, &mbi, sizeof mbi) != sizeof mbi)
            return 0;
        if (mbi.State != MEM_COMMIT) return 0;

        // PAGE_GUARD (0x100) lives in the high byte and means the *next* touch
        // unguards the page, so reading it is still undefined. PAGE_NOACCESS is
        // 0x01 in the low byte. Everything else that is committed is readable
        // for our purposes, including the WRITECOPY and EXECUTE variants.
        if (mbi.Protect & 0x100) return 0;               // PAGE_GUARD
        if ((mbi.Protect & 0xFF) == 0x01) return 0;      // PAGE_NOACCESS

        // How far can we go before leaving this region?
        uint64_t regend = (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        uint64_t avail  = regend - cur;
        uint32_t chunk  = (uint32_t)((len - done) < avail ? (len - done) : avail);
        if (chunk == 0) return 0;

        // ReadProcessMemory is the authority, and it is stricter than the page
        // check. A region can be committed and readable per MEMORY_BASIC_INFORMATION
        // and still fail RPM if it is, for instance, a reserved-but-not-mapped
        // image section or a guard-backed transition. The v26 run read only 231
        // of 8446 level-2 pages this way, against 74 of 88 at level 1, which is
        // a suspicious drop rather than a plausible one.
        //
        // Log the first few refusals with the RPM result and the MBI fields, so
        // this is a measurement rather than a guess.
        uint32_t got = td_read_rpm(cur, out + done, chunk);
        if (got != chunk) {
            td_read_fail2(cur, chunk, got, &mbi);
            return 0;
        }
        done += chunk;
    }
    return (uint32_t)done;
}

// Why a read was refused, for the first few refusals in a pass.
//
// The walk reported "0 KB read" on a process that is plainly running, which
// means the guard rejected even the renderer object it was handed. Logging the
// raw VirtualQuery fields once is far cheaper than reading the guard again and
// guessing which of the four conditions fired.
static LONG g_td_read_fail_logged;
static void td_read_fail(uint64_t at, uint32_t len, const char* stage,
                         const MEMORY_BASIC_INFORMATION* mbi, int got) {
    if (InterlockedIncrement(&g_td_read_fail_logged) > 4) return;
    vr_log("  read fail [%s] at %016llX len=%u: vq=%d state=0x%X prot=0x%X size=%llX",
           stage, (unsigned long long)at, len, got,
           (unsigned)mbi->State, (unsigned)mbi->Protect,
           (unsigned long long)mbi->RegionSize);
}

// Scratch for the SDB walk: one page reused for every read, and two pointer
// arrays. Fixed and static so the walk allocates nothing on the render thread.
// 512 KB of BSS is committed by the loader before any code runs, so there is no
// allocation failure path to handle.
//
// The capacity matches TD_WALK_MAX_PTRS in sdb_walk.h, which is included below
// and cannot be referenced yet. The two constants must stay in step; the walk
// clamps on this value and logs if the arrays are ever the smaller of the two.
#define TD_WALK_PTR_CAP 32768

static unsigned char g_walk_scratch[4096];
static uint64_t     g_walk_ptrs_a[TD_WALK_PTR_CAP];
static uint64_t     g_walk_ptrs_b[TD_WALK_PTR_CAP];

#include "bisect.h"
#include "census.h"
// Installed from the init thread before anything else can fault, so a crash
// anywhere in this process is caught with its real code and faulting module
// intact -- Sentry's SEH handling loses both.
#include "veh.h"
// Timed, bounded probe around xrCreateInstance. Off by default: it changes the
// process, so it is a measurement build, not part of a normal run.
//
// AFTER xr_session.h, not before: the probe's bisect re-resolves xrCreateInstance
// through g_xr_gipa and names result codes through tdvr_xr_err_name_only, and
// both of those are declared there. With the include above it built as an
// implicit declaration and g_xr_gipa came out with the wrong type.
#include "xr_session.h"
#include "xr_probe.h"
// Needs the full tdvr_xr definition from xr_session.h above, and it in turn
// provides the vtable helpers the acquire path calls to fill the XR images.
#include "xr_copy.h"
#include "stereo.h"
#include "sdb_resolve.h"
#include "sdb_patch.h"
#include "entry_patch.h"
#include "scene_bind.h"
#include "renderer_dump.h"
#include "memscan.h"
#include "sdb_walk.h"
#include "upload_hook.h"

// Recorded in DllMain. vtable_resolve.h compares vtable entries against this
// module to tell "the game owns this slot" from "we already hooked it".
static HMODULE g_module = NULL;

#include "vtable_resolve.h"

#define VR_LOG_FILE "teardown_vr.log"

// ---------------------------------------------------------------------------
// Known offsets into the game module. RVAs for the unpacked Steamless
// teardown.exe; they need re-locating if the game updates.
// ---------------------------------------------------------------------------
#define TD_RVA_BEGIN_RENDER  0x005AB9D0ull
#define TD_RVA_END_RENDER    0x005AF270ull

// TRendererD3D12 layout
#define TD_OFF_RENDER_FLAG   0xCC
#define TD_OFF_FRAME_INDEX   0xDA8
// Only the swapchain for the object endRender receives. beginRender is handed
// a different object, and in early frames that object's +0xE40 held float data
// (0x234800000238c181 as a "vtable slot 9"). A guarded read proving the address
// is mapped does NOT prove the object is a swapchain. Always run the result
// through td_find_present before writing through it. See the header comment in
// present.h and docs/PRESENT_HOOK_DOUBLE_DEREF.md.
#define TD_OFF_SWAPCHAIN     0xE40
#define TD_OFF_CMD_QUEUE     0xE58
#define TD_OFF_FRAME_CTX     0xEF8
#define TD_FRAME_CTX_STRIDE  0x30

// The 0x800-byte upload at renderer+0x1130.
//
// WHAT IS MEASURED, and it is only this much:
//
// The disassembly of endRender is unambiguous that the game does this
// immediately before every Present:
//
//     lea   rcx, [rbx+0x1130]
//     mov   r8d, 0x800            ; 2048 bytes
//     call  <upload>
//
// So the offset, the length and the fact that it is uploaded every frame are
// real. That much is safe to rely on.
//
// WHAT WAS REFUTED, and what an earlier revision of this comment asserted as
// fact. It claimed the 0x800 region is a constant buffer holding a
// SceneDynamicBuffer whose entries are 0x40-byte float4x4 matrices. A live
// 2048-byte dump taken at this offset (logs/upload_v29.bin) refutes that: 0 of
// 24 slots pass a matrix test, and the values present are small integers and
// heap pointers. There is no matrix data here at all. Matching 0x1F4 against
// 0x800 was a size coincidence treated as an identification - the reasoning
// that produced it is preserved below because the mistake, not the claim, is
// the thing worth remembering.
//
// The reasoning that was wrong, for the record:
//
//   "A SceneDynamicBuffer is 0x1F4 = 500 bytes, so it fits inside one of these
//    with room to spare. That makes this buffer the ground truth: it is what
//    the GPU actually receives, and whatever else the walk turns up, a real SDB
//    has to be visible here."
//
// That only follows if the upload contains the SDB, which was never shown. A
// buffer being written every frame says nothing about what is written into it.
// The inference "it is on the frame path, therefore it is the data we want" is
// the same class of error as the one this project already paid for at +0xE40
// and at 0x9E130: an offset that is genuinely on the frame path, read by a
// function that genuinely runs, carrying a payload that is not the payload we
// assumed. Both earlier failures are recorded in docs/ (SCENE_DYNAMIC_BUFFER_FOUND.md
// and docs/SDB_BREAKTHROUGH_RESOURCE_TYPE.md) and neither was an offset error.
//
// So: keep the constants, because uploading 0x800 bytes from renderer+0x1130
// each frame is a measured fact. Do not keep the claim about what is in it.
// upload_scan.h still scans the region as an experiment; it is a diagnostic,
// not a working path, and it has never reported a hit.
#define TD_OFF_CONST_UPLOAD  0x1130
#define TD_CONST_UPLOAD_SZ  0x800

// ---------------------------------------------------------------------------
// Logging
//
// Two things make this fiddly, both learned the hard way under GE-Proton:
//
//  1. The path must be absolute. A relative "teardown_vr.log" lands in whatever
//     the process CWD happens to be, which is NOT the game directory — the test
//     runs put the game in /root/steamless while the process CWD resolved to
//     /home/truenas_admin/steamless, so logs vanished into a different tree and
//     runs looked empty.
//
//  2. Nothing may touch the CRT file API from DllMain. fopen() takes the loader
//     lock in msvcrt, and calling it during DLL_PROCESS_ATTACH deadlocks or
//     silently fails. An empty log with LoadLibraryA returning a valid handle
//     is exactly that symptom — verified by injection into a live process where
//     a probe DLL's fopen from the same hook point worked only when deferred
//     off DllMain.
//
// So: DllMain records the HMODULE and starts a thread. The thread does all the
// logging and all the hooking.
// ---------------------------------------------------------------------------
static FILE* g_log = NULL;
static const char* g_log_path = NULL;

// Where the log goes, in order of preference.
//
// The game folder comes first because that is where the injector lives, and
// having the injector's log and the DLL's log side by side is what makes a test
// readable. The bare filename is relative to the process working directory,
// which the injector sets to the game folder; it is the fallback that works even
// when the absolute paths above do not resolve.
//
// Do not list a fixed system path early. Writing the log to C:\ while the
// handshake file lands elsewhere means the two disagree about where they are,
// and then the DLL waits forever for a file the injector never wrote where it
// was told to write it.
// How many paths vr_open_log tries. This MUST equal the number of non-NULL
// entries in g_log_dirs below, or the loop writes one entry past the end of
// g_log_candidates. It was 3 against 4 directories, so the fourth directory
// corrupted whatever followed the array in .bss.
#define TD_LOG_DIR_COUNT 3
static char g_log_candidates[TD_LOG_DIR_COUNT][MAX_PATH];
static const char* const g_log_dirs[] = {
    "",                                             // relative to the game folder
    "C:\\SteamLibrary\\steamapps\\common\\Teardown\\",  // absolute fallback
    "Z:\\root\\steamless\\Teardown\\",             // Proton-era layout
    NULL
};
_Static_assert(sizeof g_log_dirs / sizeof g_log_dirs[0] == TD_LOG_DIR_COUNT + 1,
               "g_log_dirs has one more entry (the NULL) than g_log_candidates has slots");

static FILE* vr_open_log(void) {
    // Name the log after this module. Windows will not let a DLL be
    // overwritten while it is mapped in a running process, so new builds get
    // new filenames (teardown_vr2.dll, ...) during development. Deriving the
    // log name from the module name means two loaded builds keep separate
    // logs instead of the second one clobbering the first.
    char me[MAX_PATH];
    if (!GetModuleFileNameA(g_module, me, sizeof me)) me[0] = 0;
    char* base = me[0] ? strrchr(me, '\\') : NULL;
    char stem[128] = "teardown_vr";
    if (base) {
        base++;
        char* dot = strrchr(base, '.');
        size_t n = dot ? (size_t)(dot - base) : strlen(base);
        if (n > 0 && n < sizeof stem) { memcpy(stem, base, n); stem[n] = 0; }
    }

    for (int d = 0; d < TD_LOG_DIR_COUNT; d++) {
        if (!g_log_dirs[d]) break;
        _snprintf(g_log_candidates[d], MAX_PATH, "%s%s.log", g_log_dirs[d], stem);
    }

    for (int i = 0; i < TD_LOG_DIR_COUNT; i++) {
        if (!g_log_candidates[i][0]) continue;
        FILE* f = fopen(g_log_candidates[i], "w");
        if (f) { g_log_path = g_log_candidates[i]; return f; }
    }
    return NULL;
}

static void vr_log(const char* fmt, ...) {
    if (!g_log) {
        g_log = vr_open_log();
        if (!g_log) return;
    }
    va_list a; va_start(a, fmt);
    fprintf(g_log, "[VR] "); vfprintf(g_log, fmt, a); fprintf(g_log, "\n");
    fflush(g_log);
    va_end(a);
}

// ---------------------------------------------------------------------------
// Hook state
// ---------------------------------------------------------------------------
typedef void (*FnVoid)(void*);

static void*    g_renderer    = NULL;   // TRendererD3D12*
static void*    g_swapchain   = NULL;   // IDXGISwapChain*
static uint8_t* g_begin_tramp = NULL;
static uint8_t* g_end_tramp   = NULL;
static FnVoid   g_orig_begin  = NULL;
static FnVoid   g_orig_end    = NULL;

static volatile LONG g_frame_count = 0;
static volatile LONG g_enabled     = 0;
static float         g_ipd_mm      = 64.0f;

static void* g_openxr_module = NULL;
static int   g_xr_ready      = 0;
// The live XR state: instance, session, and the head pose. This is what
// stereo_apply has been missing -- there was no pose source at all before.
// Not static: xr_session.h declares it extern because the binding helpers live
// in that header and are called from the render hook, and they must reach the
// same state this init path fills in.
tdvr_xr g_xr;

// The CPU-side SceneDynamicBuffer. Static analysis could not pin it down
// (beginRender/endRender are virtual, zero direct call sites), so it is found
// at runtime by looking for a plausible view-projection matrix. Until then
// stereo stays off and the game plays flat.
static void*   g_scene_buf  = NULL;
static uintptr_t g_scene_off = 0;        // offset of the buffer inside the frame context

// ---------------------------------------------------------------------------
// Detour trampoline
//
// The detour is the classic 14-byte absolute jump:
//     FF 25 00 00 00 00          jmp qword ptr [rip+0]
//     <8-byte target address>
//
// A trampoline must steal WHOLE instructions. Measured prologues:
//
//   beginRender  48 89 5C 24 18   mov [rsp+0x18],rbx   (5)
//                48 89 6C 24 20   mov [rsp+0x20],rbp   (5)
//                56 57            push rsi, rdi        (2)
//                41 56            push r14             (2)
//                                              = 14 exactly
//
//   endRender    48 89 5C 24 18   mov [rsp+0x18],rbx   (5)
//                55 56 57         push rbp/rsi/rdi     (3)
//                41 56 41 57      push r14/r15         (4)
//                48 83 EC 50      sub rsp, 0x50        (4)
//                                              = 16
//
// endRender's prologue is 16 bytes but a 14-byte absolute jump only has room
// for 14. Taking a flat 14 would split "48 83 EC 50" and leave the stack
// unallocated in every replayed frame — a crash that surfaces much later and
// looks nothing like its cause. So endRender gets a 5-byte relative jump
// instead, which is cheaper and fits any length. See build_detour().
// ---------------------------------------------------------------------------
// The patch site has to be at least as long as the longest stolen prologue.
//
// It used to be 14, which is exactly one byte short of the upload consumer's
// 15-byte prologue. build_detour writes a 14-byte absolute jump and then
// nop-fills up to DETOUR_LEN, so a 15-byte steal left the last byte of
// `sub rsp, 0x20` in place. The function was left with a truncated
// instruction, the render thread took an access violation on the very first
// call, and v32 logged "detached after 1 frames". 16 is the next multiple of
// eight above the worst case and costs one nop.
#define DETOUR_LEN 16
#define TRAMP_CAP  64      // stolen bytes (max 16) + 14-byte jump back

// Walk forward over the entry-prologue instruction family and return how many
// bytes are safe to copy wholesale. A full x86 length decoder is overkill:
// both entry points stay inside mov [rsp+disp8],reg / push r64 / sub rsp,imm8
// for their entire prologue.
//
//   48 89 5C 24 nn        mov [rsp+nn], rbx        5 bytes (REX.W)
//   48 89 6C 24 nn        mov [rsp+nn], rbp        5 bytes
//   4C 89 4C 24 nn        mov [rsp+nn], r9         5 bytes (REX.W+R)
//   50..57                push rax..rdi           1 byte
//   41 50..41 57          push r8..r15            2 bytes (REX.B)
//   48 83 EC nn           sub rsp, nn             4 bytes
//   89 54 24 nn           mov [rsp+nn], edx       4 bytes (no REX, 32-bit reg)
//   8B xx                 mov reg, [rsp+..]       short forms
//
// The 4C 89 4C 24 form was added for the constant-upload consumer at
// 0x5BAEB0, whose prologue opens with it. Without this case the walker stopped
// after one byte, the stolen length came out as 1, and the detour overwrote a
// partial instruction — the kind of fault that does not show up until a few
// thousand frames later.
//
// The REX-less 89 54 24 form was added for the DXGI Present thunk, whose
// prologue is "89 54 24 10" — "mov [rsp+0x10], edx", a 32-bit register write
// with no REX prefix. The original table only accepted the 64-bit r/m encodings
// 4C/5C/6C/74, so this instruction broke the walk on the very first byte and
// measure_stealable reported that no safe prologue existed, which is why the
// Present hook was never installed even though the target was perfectly
// detachable. Same failure mode as the 4C case: refuse, and nothing is logged
// about why.
static size_t measure_stealable(void* target, size_t max_len) {
    const uint8_t* p = (const uint8_t*)target;
    size_t n = 0;
    while (n + 2 <= max_len) {
        uint8_t op = p[n];
        if (op == 0x41 && p[n + 1] >= 0x50 && p[n + 1] <= 0x57) {
            n += 2; continue;                                    // push r8..r15
        }
        // mov [rsp+disp8], reg for the REX.W and REX.W+R forms. The REX byte
        // is 48, 4C or 49, and the ModRM byte is 4C, 5C, 6C or 74 depending on
        // which register is being saved. Listing them explicitly is enough for
        // this family and avoids pretending to be a full decoder.
        if ((op == 0x48 || op == 0x4C || op == 0x49) && p[n + 1] == 0x89 &&
            (p[n + 2] == 0x4C || p[n + 2] == 0x5C ||
             p[n + 2] == 0x6C || p[n + 2] == 0x74) &&
            p[n + 3] == 0x24) {
            n += 5; continue;                                    // mov [rsp+d8],reg
        }
        // REX-less mov [rsp+disp8], r32 — "89 ModRM 24 disp8", 4 bytes.
        //
        // Without the REX prefix these are 32-bit register writes: 89 54 24 10
        // is "mov [rsp+0x10], edx". The 64-bit table above cannot see them
        // because the opcode at the ModRM position is 54 rather than one of
        // 4C/5C/6C/74. Any r/m in the 0x40..0x7F range with the SIB byte 24 and
        // a mod of 01 is this same shape, so the whole range is accepted rather
        // than only the one encoding that happened to show up in the log.
        //
        // The operand-size distinction does not matter for stealing: the bytes
        // are copied verbatim into the trampoline and executed there, where
        // they write the same stack slot with the same width.
        if (op == 0x89 && p[n + 1] >= 0x40 && p[n + 1] <= 0x7F &&
            p[n + 2] == 0x24) {
            n += 4; continue;                                    // mov [rsp+d8],r32
        }
        if (op == 0x48 && p[n + 1] == 0x83 && p[n + 2] == 0xEC) {
            n += 4; continue;                                    // sub rsp,imm8
        }
        if (op >= 0x50 && op <= 0x57) { n += 1; continue; }     // push rax..rdi
        break;
    }
    return n;
}

// present.h needs measure_stealable() and build_detour(), both defined at or
// below this point, so it is included here rather than with the other headers
// at the top of the file. It declares its own forward declaration of
// build_detour, which is compatible.
#include "present.h"
#include "dxgi_probe.h"
#include "census_dxgi.h"
#include "frame_census.h"
#include "iat_census.h"   /* after dxgi_probe.h: it reuses the header-verified
                            * local IID copies defined there */
#include "present_hook.h"

// Install a hook at `target` with a replay trampoline.
//
// copy_len <= 5 : 5-byte relative jmp (E9), leaves the rest of the slot
//                 untouched.
// copy_len >  5 : 14-byte absolute jmp (FF 25 ..), nop-filling any bytes
//                 between the stolen prologue and the jump.
//
// The trampoline holds the stolen bytes followed by an absolute jump back to
// target + copy_len, so the original function runs completely intact.
static void* build_detour(void* target, void* hook, uint8_t* trampoline,
                          size_t copy_len) {
    uint8_t* p = (uint8_t*)target;
    DWORD old;

    // Copy the bytes we are about to overwrite BEFORE overwriting them.
    //
    // This has to happen first. An earlier version patched `target` and only
    // then ran `memcpy(trampoline, target, copy_len)`, so the trampoline was
    // filled with our own jump instruction instead of the original prologue.
    // The first call to g_orig_upload therefore jumped straight back into the
    // hook, and the render thread recursed until the stack ran out. The symptom
    // was "detached after 1 frames" with no "capture: entered" line, which
    // looked exactly like a hook that was never reached.
    memcpy(trampoline, target, copy_len);

    if (!VirtualProtect(target, DETOUR_LEN, PAGE_EXECUTE_READWRITE, &old)) {
        vr_log("VirtualProtect failed: %lu", GetLastError());
        return NULL;
    }

    if (copy_len <= 5) {
        // E9 rel32 — displacement is from the END of the instruction.
        p[0] = 0xE9;
        int32_t rel = (int32_t)((intptr_t)hook - (intptr_t)(p + 5));
        memcpy(p + 1, &rel, 4);
    } else {
        // jmp qword ptr [rip+0] = FF 25 disp32, a SIX byte instruction, with
        // the 8-byte absolute target stored immediately after it at offset 6.
        //
        // The offset is not free. RIP for a RIP-relative operand points at the
        // byte AFTER the instruction, so with disp32 = 0 the CPU dereferences
        // the qword at target+6. Two earlier versions got this wrong:
        //
        //   - one wrote the address at offset 8, so the CPU read two zero
        //     bytes plus the low half of the pointer and jumped to 0x28100000;
        //   - the other put it at offset 6 and then nop-filled bytes 8..13,
        //     overwriting the pointer's high half with 0x90 and producing the
        //     same unusable address.
        //
        // ORDER MATTERS AS MUCH AS THE OFFSET. This code is being patched
        // while the render thread is executing it. Writing the opcode first
        // and the operand second opens a window in which the CPU can decode
        // "FF 25 disp32=0" and dereference target+6, which still holds the old
        // bytes. The thread jumps to garbage and the process dies minutes
        // later, from the fault, with no dump and no WER event: v38 survived
        // 40 s and was gone by the next check. build/test_atomic.c reproduces
        // it (892 million torn reads) and shows the fix below produces zero.
        //
        // So the operand goes in first. Until FF 25 lands, the site reads as
        // the original code; the moment it lands, all fourteen bytes are already
        // correct. There is no reachable state in which the opcode is
        // decodable with a stale operand.
        memcpy(p + 6, &hook, sizeof(void*));  // operand first
        p[2] = p[3] = p[4] = p[5] = 0x00;      // disp32 = 0, so [rip+0] is target+6
        p[0] = 0xFF; p[1] = 0x25;              // opcode last: this publishes it
        for (size_t i = 14; i < DETOUR_LEN; ++i) p[i] = 0x90;
        if (copy_len > DETOUR_LEN) {
            // Should be impossible now that DETOUR_LEN is 16 and the longest
            // prologue is 15, but a longer steal must not silently corrupt the
            // byte after the patch.
            vr_log("build_detour: copy_len %u exceeds DETOUR_LEN %d, "
                   "truncating safely", (unsigned)copy_len, DETOUR_LEN);
        }
    }

    VirtualProtect(target, DETOUR_LEN, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, DETOUR_LEN);

    // Trampoline: the stolen bytes were copied above, before the patch. Append
    // an absolute jump back to target + copy_len, with the same operand-first
    // ordering as the entry patch. It does not matter for correctness here,
    // because nothing else is executing the trampoline yet, but keeping the two
    // encodings identical means there is one layout to reason about.
    uint8_t* tp = trampoline + copy_len;
    void* back = (uint8_t*)target + copy_len;
    memcpy(tp + 6, &back, sizeof(void*));
    tp[2] = tp[3] = tp[4] = tp[5] = 0x00;
    tp[0] = 0xFF; tp[1] = 0x25;
    return trampoline;
}

// ---------------------------------------------------------------------------
// SceneDynamicBuffer scanner
//
// SceneDynamicBuffer is register space b1 and starts with float4x4 mubVpMatrix
// followed by mubViewXDir / mubViewYDir / mubProjectionData. A real VP matrix
// for this engine is row-vector convention: the last row is (0, 0, 0, 1) and the
// right/up vectors are unit length. Those two cheap invariants reject almost
// every 256-byte block, so the scan converges in one or two frames.
// ---------------------------------------------------------------------------
#define SCAN_FRAMES   600
#define SCAN_WORDS    64            // 256 bytes = 4 x float4x4 candidates

static volatile LONG g_scan_frames = 0;

static int plausible_vp(const float* m) {
    // last row of a row-vector VP: (0, 0, 0, 1)
    if (fabsf(m[12]) > 0.01f) return 0;
    if (fabsf(m[13]) > 0.01f) return 0;
    if (fabsf(m[14]) > 0.01f) return 0;
    if (fabsf(m[15] - 1.0f) > 0.01f) return 0;

    // projection scale is bounded — reject degenerate blocks
    if (fabsf(m[0]) < 0.0001f || fabsf(m[0]) > 100.0f) return 0;
    if (fabsf(m[5]) < 0.0001f || fabsf(m[5]) > 100.0f) return 0;

    // no NaN / Inf anywhere in the 16 floats
    for (int i = 0; i < 16; i++) if (!isfinite(m[i])) return 0;
    return 1;
}

static void log_matrix(const float* m) {
    vr_log("    VP row0 = [%8.4f %8.4f %8.4f %8.4f]", m[0],  m[1],  m[2],  m[3]);
    vr_log("    VP row1 = [%8.4f %8.4f %8.4f %8.4f]", m[4],  m[5],  m[6],  m[7]);
    vr_log("    VP row2 = [%8.4f %8.4f %8.4f %8.4f]", m[8],  m[9],  m[10], m[11]);
    vr_log("    VP row3 = [%8.4f %8.4f %8.4f %8.4f]", m[12], m[13], m[14], m[15]);
}

static void scan_frame_contexts(void* self) {
    LONG n = InterlockedIncrement(&g_scan_frames);
    if (n > SCAN_FRAMES) return;

    // Raw dereferences of `self` and of the context pointer were used here.
    // This function runs on the render thread once per frame for the first
    // SCAN_FRAMES frames, so a bad pointer does not fail once - it faults
    // within seconds of injection, which is the observed crash, and the only
    // symptom is Sentry's dialog. Every read below now goes through td_read,
    // which is VirtualQuery-guarded, so a stale offset logs instead of killing
    // the process.
    uint8_t* base = NULL;
    if (!td_read((uint64_t)(uintptr_t)self + TD_OFF_FRAME_CTX, &base,
                 sizeof base) || !base) {
        vr_log("scan f%ld: frame ctx array unreadable at self+0x%X",
               n, TD_OFF_FRAME_CTX);
        return;
    }

    int32_t idx = 0;
    if (!td_read((uint64_t)(uintptr_t)self + TD_OFF_FRAME_INDEX, &idx,
                 sizeof idx)) {
        vr_log("scan f%ld: frame index unreadable at self+0x%X",
               n, TD_OFF_FRAME_INDEX);
        return;
    }
    if (idx < 0 || idx > 8) idx = 0;

    uint8_t* ctx = base + (size_t)idx * TD_FRAME_CTX_STRIDE;
    uint8_t* ctx_probe = NULL;
    if (!td_read((uint64_t)(uintptr_t)ctx, &ctx_probe, sizeof ctx_probe)) {
        vr_log("scan frame %ld: ctx %p (base %p + %d*0x%X) unreadable",
               n, (void*)ctx, (void*)base, idx, TD_FRAME_CTX_STRIDE);
        return;
    }
    vr_log("scan frame %ld: idx=%d ctx=%p", n, idx, (void*)ctx);

    // Dump the context itself first. The offsets below are recovered from an
    // older build of the game, so they may be stale after an update; if the
    // pointers look wrong the dump is what tells us where they moved, instead
    // of the search silently finding nothing.
    for (int off = 0; off < (int)TD_FRAME_CTX_STRIDE; off += 8) {
        unsigned char* q = *(unsigned char**)(ctx + off);
        if (!q) continue;
        if (IsBadReadPtr(q, 16)) {
            vr_log("  ctx+0x%02X = %p  (unreadable)", off, (void*)q);
            continue;
        }
        // Non-null, readable, 16-byte aligned: worth a look.
        vr_log("  ctx+0x%02X = %p%s", off, (void*)q,
               ((uintptr_t)q & 15) ? "  (unaligned)" : "");
    }

    // Only the current context's pointers are followed, but scan the whole
    // stride in 16-byte steps so an unaligned VP is not missed.
    for (int off = 0; off < (int)TD_FRAME_CTX_STRIDE; off += 8) {
        unsigned char* q = *(unsigned char**)(ctx + off);
        if (!q) continue;
        if (IsBadReadPtr(q, 256)) continue;

        for (int k = 0; k + 16 <= SCAN_WORDS; k += 4) {   // 16-byte steps
            const float* m = (const float*)(q + k * 4);
            if (plausible_vp(m)) {
                vr_log("  ** CANDIDATE vp at ctx+0x%X -> %p + 0x%X", off, q, k * 4);
                log_matrix(m);
                if (!g_scene_buf) {
                    g_scene_buf = q;
                    g_scene_off = (uintptr_t)q;
                    vr_log("  scene buffer locked at %p", g_scene_buf);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// OpenXR — resolved lazily, never blocks startup.
// ---------------------------------------------------------------------------
static void xr_try_load(void) {
    // The previous candidate list was a guess and it was wrong. On this machine
    // the loader is NOT in System32 and NOT next to the game. It lives at
    //   C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64\openxr_loader.dll
    // and the log faithfully said "no OpenXR loader, flat rendering" while a
    // 419992-byte loader sat on disk the whole time. A search list that cannot
    // find the thing that is actually there is worse than no search at all,
    // because it produces a confident negative.
    //
    // So read the real path from the registry, the way the OpenXR loader itself
    // would, and only fall back to guessing if that fails.
    char regpath[1024] = {0};
    DWORD regpath_sz = (DWORD)sizeof regpath;   // RegGetValueA wants LPDWORD, not
                                                // a size_t. Passing sizeof
                                                // directly is an int-to-pointer
                                                // error, and it is the reason the
                                                // first two compiles of this
                                                // said "too many arguments".
    if (RegGetValueA(HKEY_LOCAL_MACHINE,
                     "SOFTWARE\\Khronos\\OpenXR\\1",
                     "ActiveRuntime", RRF_RT_REG_SZ, NULL,
                     regpath, &regpath_sz) == ERROR_SUCCESS &&
        regpath[0]) {
        vr_log("OpenXR ActiveRuntime: %s", regpath);

        // library_path in the json is relative to the json's own directory, but
        // the LOADER is a sibling of that, under bin\win64. Derive the SteamVR
        // root from the json path and look where the loader actually is.
        char root[900] = {0};
        size_t n = strlen(regpath);
        for (size_t i = n; i > 0; i--) {
            if (regpath[i] == '\\' || regpath[i] == '/') {
                memcpy(root, regpath, i);
                root[i] = 0;
                break;
            }
        }
        // A runtime DLL is NOT a loader. The OpenXR-Simulator exports
        // xrNegotiateLoaderRuntimeInterface + the xr* API, but no
        // xrGetInstanceProcAddr, so loading it directly and asking it for
        // xrGetInstanceProcAddr fails with MISSING and XR silently never
        // starts. What is needed is the Khronos LOADER, which reads this very
        // manifest and dlopen()s the runtime it names.
        //
        // So: search the manifest's neighbourhood for a LOADER, and treat a
        // file called openxr_simulator.dll as a runtime to be loaded BY the
        // loader, never as the loader itself.
        const char* subs[] = {
            "\\bin\\win64\\openxr_loader.dll",
            "\\bin\\vrclient_x64.dll",
            "\\openxr_loader.dll",
        };
        for (int i = 0; subs[i]; i++) {
            char full[1024] = {0};
            snprintf(full, sizeof full, "%s%s", root, subs[i]);
            g_openxr_module = LoadLibraryA(full);
            if (g_openxr_module) { vr_log("OpenXR loader: %s", full); break; }
            vr_log("OpenXR:   beside the manifest, %s did not load (err=%lu)",
                   full, GetLastError());
        }
        if (!g_openxr_module) {
            // The manifest's directory had no loader. Look where a loader
            // actually lives, independent of which runtime is active.
            //
            // Our own static Khronos build is tried FIRST and the SteamVR one
            // second, deliberately. Every loader measured so far that came from
            // SteamVR killed the process at LoadLibrary (2026-09-28), while the
            // simulator and the Meta runtime kill it too. Our build is
            // self-contained -- no vrserver, no driver, nothing Steam -- so it
            // is the one hypothesis that has not been tested yet, and it must
            // not be shadowed by the install it is meant to replace.
            const char* fixed[] = {
                "C:\\tdvr\\openxr_loader.dll",
                "C:\\Windows\\System32\\openxr_loader.dll",
                "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\bin\\win64\\openxr_loader.dll",
                "C:\\Program Files\\SteamVR\\bin\\win64\\openxr_loader.dll",
                NULL
            };
            for (int i = 0; fixed[i]; i++) {
                g_openxr_module = LoadLibraryA(fixed[i]);
                if (g_openxr_module) {
                    vr_log("OpenXR: loader from a fixed path: %s", fixed[i]);
                    break;
                }
                // Silence here is what made every previous failure ambiguous.
                vr_log("OpenXR:   %s did not load (err=%lu)", fixed[i], GetLastError());
            }
        }
        if (!g_openxr_module) {
            vr_log("OpenXR: registry gave %s but no LOADER found (the active "
                   "runtime is not a loader; a loader must read the manifest)",
                   regpath);
        }
    } else {
        vr_log("no ActiveRuntime registry key; falling back to the usual paths");
    }

    if (!g_openxr_module) {
        const char* cands[] = {
            "openxr_loader.dll", "openxr_loader_x64.dll",
            "C:\\Windows\\System32\\openxr_loader.dll", NULL
        };
        for (int i = 0; cands[i]; i++) {
            g_openxr_module = LoadLibraryA(cands[i]);
            if (g_openxr_module) { vr_log("OpenXR loader (fallback): %s", cands[i]); break; }
        }
    }
    if (!g_openxr_module) { vr_log("no OpenXR loader, flat rendering"); return; }
    void* fn = (void*)GetProcAddress(g_openxr_module, "xrGetInstanceProcAddr");
    g_xr_ready = fn ? 1 : 0;
    vr_log("xrGetInstanceProcAddr %s", fn ? "ok" : "MISSING");

    // Loading the DLL was the whole of the old OpenXR path. It never created an
    // instance, so nothing was ever connected to a runtime. Do the actual chain
    // now, and report each step's real error code.
    if (g_xr_ready) {
#if TDVR_XR_RUNTIME_ONLY
        // Control test: load the loader, resolve xrGetInstanceProcAddr, and stop.
        //
        // The three crash dumps so far are identical -- "GPU hung/removed/reset,
        // HRESULT=887a0005" (DXGI_ERROR_DEVICE_RESET), fault at teardown.exe rva
        // 0x4ffcc8 -- and they stay identical when the image copy is off and when
        // layerCount is 0. The game runs clean the whole test window with
        // TDVR_NO_XR=1, which does not even load the loader. So the remaining
        // suspect is the loader+runtime themselves being resident in the game's
        // process, with no instance and no session created at all. This build
        // isolates exactly that: the runtime is in the address space, doing
        // nothing, and the game either survives or it does not.
        vr_log("XR_RUNTIME_ONLY build: loader is resident, no instance, no session");
#else
        tdvr_xr_init(&g_xr, g_openxr_module,
                     (PFN_xrGetInstanceProcAddr)fn);
#endif
    }
}

// ---------------------------------------------------------------------------
// Stereo
//
// Once the scene buffer is known we can write per-eye projections into
// mubVpMatrix. The game owns writing that slot every frame, so the mod has to
// write AFTER the game does — which is the point of hooking endRender as well
// as beginRender: endRender runs after the scene constants are filled but
// before Present, so the swapped-in matrix still reaches the GPU this frame.
// ---------------------------------------------------------------------------
// The SceneDynamicBuffer field offsets now live in stereo.h as TDVR_SDB_*,
// taken from the declaration at image 0x1C76AF4 and confirmed against the
// createStructuredBuffer size 0x1F4. Keeping one copy avoids the two drifting
// apart, which is how mubProjectionData ended up at the wrong scale.

static tdvr_mat4 g_mono_vp;                  // what the game wrote this frame
static tdvr_mat4 g_eye_vp[2];                // what we substitute
static int       g_stereo_ready = 0;
static volatile LONG g_eye = 0;              // 0 = left, 1 = right
static tdvr_matrix_layout g_last_layout = TDVR_MATRIX_COLUMN_VECTOR;
// What the game wrote into mubProjectionData this frame, before the per-eye
// offset. Tracked per frame rather than cached once, because panning moves the
// offset terms m[8]/m[0] and m[9]/m[5].
static float g_mono_pd[4];
static int   g_pd_seen = 0;

// Rebuild both eye projections from whatever the game just wrote.
static void stereo_update_projections(void) {
    if (!g_scene_buf) return;

    float* vp = (float*)((uint8_t*)g_scene_buf + TDVR_SDB_VP_OFFSET);
    memcpy(g_mono_vp.m, vp, sizeof g_mono_vp.m);

    // A valid mono projection has m[0] and m[5] non-zero. If the scan locked
    // onto the wrong address this is where we would notice.
    if (fabsf(g_mono_vp.m[0]) < 1e-6f || fabsf(g_mono_vp.m[5]) < 1e-6f) {
        if (g_stereo_ready) vr_log("stereo: implausible mono VP, backing off");
        g_stereo_ready = 0;
        return;
    }

    // Which flat index carries the eye offset depends on whether the engine
    // multiplies v = M*p or out = M*v, and nothing in the shader source settles
    // it: computePixelDir does not use mubVpMatrix at all. So read it off a real
    // matrix instead of hardcoding an index and hoping.
    //
    // A perspective projection has exactly one -1, in the row that produces
    // w_clip. Under the column-vector reading that is m[14]; under the row-vector
    // reading it is m[11]. Nothing else in a projection matrix sits at -1, so
    // the position of the -1 identifies the convention outright.
    g_mono_vp.layout = tdvr_detect_layout(g_mono_vp.m);
    if (g_stereo_ready && g_mono_vp.layout != g_last_layout) {
        vr_log("stereo: VP layout is %s",
               g_mono_vp.layout == TDVR_MATRIX_ROW_VECTOR ? "row-vector" : "column-vector");
    }
    g_last_layout = g_mono_vp.layout;

    float ipd_m = g_ipd_mm / 1000.0f;
    tdvr_stereo_vp(&g_eye_vp[0], &g_eye_vp[1], &g_mono_vp, ipd_m);
    g_stereo_ready = 1;
}

// The game's own projection path, which is where the eye offset actually
// belongs. From SceneDynamicBuffer at image 0x1C76AF4:
//
//     // z - projMatrix.m[8] / projMatrix.m[0]
//     float4 mubProjectionData;
//
// and computePixelDir multiplies the NDC screen coordinate by mubProjectionData.r
// (= 1/m[0]) then adds mubProjectionData.z. Because the engine already divided
// the clip offset by m[0] when packing .z, the delta is the world-space eye
// offset in metres, unscaled.
//
// This is preferred over the matrix because it cannot disturb the depth row
// and cannot fight TAA, and because it does not require knowing whether the
// engine's mubVpMatrix is row-major or column-major.
//
// The buffer pointer is the same one the SceneDynamicBuffer scan found, so the
// projection data sits at a fixed +0x70 from it. If that pointer is wrong the
// plausibility check below catches it instead of writing into unrelated memory.
static void stereo_apply(int eye) {
    if (!g_stereo_ready || !g_scene_buf) return;

    // Read what the game wrote this frame, and keep the mono values so the
    // per-eye writes never accumulate across frames.
    float* pd = (float*)((uint8_t*)g_scene_buf + TDVR_SDB_PROJDATA_OFFSET);
    float* vp = (float*)((uint8_t*)g_scene_buf + TDVR_SDB_VP_OFFSET);

    if (fabsf(pd[TDVR_PD_R]) < 1e-6f || fabsf(pd[TDVR_PD_G]) < 1e-6f ||
        pd[TDVR_PD_R] < 0.0f || pd[TDVR_PD_G] < 0.0f) {
        if (g_stereo_ready) {
            vr_log("stereo: implausible ProjectionData %p r=%g g=%g, backing off",
                   (void*)pd, pd[TDVR_PD_R], pd[TDVR_PD_G]);
        }
        g_stereo_ready = 0;
        return;
    }

    if (g_pd_seen == 0) {
        memcpy(g_mono_pd, pd, sizeof g_mono_pd);
        g_pd_seen = 1;
        vr_log("stereo: ProjectionData r=%.6f g=%.6f z=%.6f w=%.6f  (xScale=%.4f yScale=%.4f)",
               g_mono_pd[TDVR_PD_R], g_mono_pd[TDVR_PD_G],
               g_mono_pd[TDVR_PD_Z], g_mono_pd[TDVR_PD_W],
               1.0f / g_mono_pd[TDVR_PD_R], 1.0f / g_mono_pd[TDVR_PD_G]);
    } else {
        // The game rewrites these every frame, so track the mono value rather
        // than caching it once: the camera moves and the projection does not
        // change shape, but panning does move the offset terms.
        memcpy(g_mono_pd, pd, sizeof g_mono_pd);
    }

    // Per-eye offset: -ipd/2 for the left eye, +ipd/2 for the right, starting
    // from whatever the game wrote this frame so repeated calls are idempotent.
    float ipd_m = g_ipd_mm / 1000.0f;
    float shift = (eye == 0) ? (-ipd_m * 0.5f) : (+ipd_m * 0.5f);

    memcpy(pd, g_mono_pd, sizeof g_mono_pd);
    tdvr_eye_projection_data(pd, shift);

    // The matrix still needs the same offset for the vertex shaders that read
    // mubVpMatrix rather than the basis vectors.
    memcpy(vp, g_mono_vp.m, sizeof g_mono_vp.m);
    {
        tdvr_mat4 eye_m = g_mono_vp;
        tdvr_eye_vp(&eye_m, &g_mono_vp, shift);
        memcpy(vp, eye_m.m, sizeof eye_m.m);
    }

    if (g_pd_seen <= 2) {
        g_pd_seen++;
        vr_log("stereo: eye %d  pd.z=%.6f m  (want %+.6f)  readback=%.6f",
               eye, pd[TDVR_PD_Z], shift,
               tdvr_projection_data_eye_offset(pd));
    }
}

// ---------------------------------------------------------------------------
// Frame hooks
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// The real SceneDynamicBuffer hook.
//
// The memory scan kept coming up empty because the buffer does not live on
// the heap in a stable place. Function 0x9E130 builds the 500 bytes on its own
// stack frame and hands them straight to UpdateSubresources; there is no
// persistent CPU mirror to find. So the only reliable place to touch the data
// is inside that function, on the frame it is about to upload.
//
// Hooking it gives three things the scan never could:
//   - the exact address of the 500 bytes, every frame, no guessing
//   - the monoscopic values the game just computed, before we touch anything
//   - a place to write the per-eye offset that the upload will carry to the GPU
//
// The eye is chosen by the caller: stereo mode makes the renderer call this
// once per eye per frame. Until that exists, the hook alternates so that at
// least each eye is being produced, and says so in the log rather than
// pretending stereo is on.
// ---------------------------------------------------------------------------
static tdvr_sdb_site g_sdb_site;
static volatile LONG g_sdb_hits = 0;

// A real perspective projection has m[0] and m[5] non-zero and finite. If the
// offset is wrong we would be reading garbage, so check before trusting.
static int sdb_plausible(const float* pd) {
    if (!pd) return 0;
    float r = pd[TDVR_PD_R], g = pd[TDVR_PD_G];
    if (!(r > 1e-6f) || !(g > 1e-6f)) return 0;      // rejects 0 and NaN
    if (r > 1e6f || g > 1e6f) return 0;
    return 1;
}

// Write the per-eye offset into the SDB that is about to be uploaded.
// eye: 0 = left, 1 = right. Returns the shift actually applied, or 0.0 if
// stereo is off or the data did not look like a projection.
static float sdb_write_eye(uint8_t* sdb, int eye) {
    if (!sdb) return 0.0f;

    float* pd = (float*)(sdb + TDVR_SDB_PROJDATA_OFFSET);
    float* vp = (float*)(sdb + TDVR_SDB_VP_OFFSET);

    if (!sdb_plausible(pd)) {
        if (g_stereo_ready) {
            vr_log("sdb: implausible ProjectionData r=%g g=%g at %p, skipping",
                   (double)pd[TDVR_PD_R], (double)pd[TDVR_PD_G], (void*)pd);
        }
        g_stereo_ready = 0;
        return 0.0f;
    }

    // The game rewrites all four values every frame, so the mono baseline is
    // whatever is in the buffer right now. Taking it fresh each time is what
    // makes the per-eye write idempotent instead of accumulating.
    memcpy(g_mono_pd, pd, sizeof g_mono_pd);

    if (!g_enabled) {
        g_stereo_ready = 1;
        return 0.0f;
    }

    if (g_pd_seen == 0) {
        vr_log("sdb: mono pd r=%.6f g=%.6f z=%.6f w=%.6f (m[0]=%.6f m[5]=%.6f)",
               g_mono_pd[TDVR_PD_R], g_mono_pd[TDVR_PD_G],
               g_mono_pd[TDVR_PD_Z], g_mono_pd[TDVR_PD_W],
               1.0f / g_mono_pd[TDVR_PD_R], 1.0f / g_mono_pd[TDVR_PD_G]);
    }

    float ipd_m = g_ipd_mm / 1000.0f;
    float shift = (eye == 0) ? (-ipd_m * 0.5f) : (+ipd_m * 0.5f);

    tdvr_eye_projection_data(pd, shift);
    g_stereo_ready = 1;

    // mubVpMatrix is not used by the raycasting path, but geometry that reads
    // it still needs the same offset or the two disagree.
    {
        tdvr_mat4 mono = g_mono_vp;
        memcpy(mono.m, vp, sizeof mono.m);
        if (tdvr_detect_layout(mono.m) == TDVR_MATRIX_COLUMN_VECTOR ||
            g_last_layout == TDVR_MATRIX_COLUMN_VECTOR) {
            g_last_layout = TDVR_MATRIX_COLUMN_VECTOR;
        }
        tdvr_eye_vp(&mono, &mono, shift);
        memcpy(vp, mono.m, sizeof mono.m);
    }

    if (g_pd_seen < 4) {
        g_pd_seen++;
        vr_log("sdb: eye %d  shift=%+.6f m  pd.z %.6f -> %.6f",
               eye, (double)shift,
               (double)g_mono_pd[TDVR_PD_Z], (double)pd[TDVR_PD_Z]);
    }
    return shift;
}

// Diagnostic: is 0x9E130 called at all, and with what argument?
//
// The interior SDB call hook installed correctly and never fired. That has two
// possible causes — the function is genuinely not called per frame, or the
// interior patch landed on the wrong bytes because the offline image does not
// match live memory exactly. This settles it: if this fires and the interior
// hook does not, the interior patch is misplaced; if neither fires, the
// function is not the per-frame path at all.
static volatile LONG g_sdb_entry_hits = 0;
static uint8_t* g_sdb_entry_tramp = NULL;
typedef void (*FnVoid1)(void*);
static FnVoid1 g_orig_sdb_entry = NULL;

static void hooked_sdb_entry(void* self) {
    LONG n = InterlockedIncrement(&g_sdb_entry_hits);
    if (n <= 4) {
        vr_log("SDB ENTRY hit %ld: this=%p", n, self);
    }
    if (g_orig_sdb_entry) g_orig_sdb_entry(self);
}

// ---------------------------------------------------------------------------
// Hooking the upload call, not the function prologue.
//
// Detouring 0x9E130 itself would mean rebuilding a 0x3C8-byte function with
// 0x270 bytes of locals and a 12-register frame. Instead the resolver hands
// back the one instruction that matters:
//
//     FF 90 28 01 00 00    call qword ptr [rax + 0x128]
//
// Six bytes, and the whole instruction is replaceable: a 5-byte near call goes
// to the detour, and a 1-byte "mov r11, rax" style filler plus the original
// tail reconstructs it. But there is a simpler and strictly safer option that
// avoids stolen bytes entirely — the detour can just perform the original call
// itself and return, because the call target is read from the same
// [rax + 0x128] the original instruction would have read, with rax already
// loaded and unchanged. So the six bytes are replaced by a 5-byte call plus
// one NOP, the original call's effect is reproduced inside the detour, and no
// byte of any other instruction is touched.
//
// The detour therefore needs the call's own arguments, and they arrive in the
// normal registers: the 500-byte buffer pointer is the third argument, in r8.
// Reading it from there rather than recomputing an rsp-relative address means
// the hook does not care how big the function's frame is, so it survives a game
// update that changes the local layout.
// ---------------------------------------------------------------------------
// Argument order taken from the call site, not guessed:
//
//     mov rcx, [rbx + 0x9c0]     ; context
//     mov rdx, [rbx + 0xb90]     ; resource
//     lea r8,  [rsp + 0x30]      ; the 500 bytes
//     mov r9d, 0x1f4             ; size
//     mov dword [rsp+0x20], 0
//     mov dword [rsp+0x28], 0
//     mov rax, [rcx]             ; vtable
//     call [rax + 0x128]
//
// So the data pointer is the THIRD argument, not the fourth. Getting this wrong
// would have handed sdb_write_eye the size value 0x1f4 as a pointer, and the
// first write would have been at address 500.
//
// The original call is reproduced inside the detour rather than trampolined
// through, because the call target is a vtable slot read from a live object
// that may be replaced when the game recreates its device. Reading it fresh
// every frame means the hook keeps working across a device reset, which a
// cached function pointer would not.
#define TDVR_CTX_VTABLE_SLOT 0x128u

typedef void (*FnUpload)(void* ctx, void* resource, void* data,
                         uint32_t size, uint32_t flags_a, uint32_t flags_b);

static volatile LONG g_sdb_hooked = 0;
static volatile LONG g_sdb_writes = 0;
static volatile LONG g_sdb_misses = 0;

static void hooked_sdb_upload(void* ctx, void* resource, void* data,
                              uint32_t size, uint32_t flags_a, uint32_t flags_b) {
    LONG n = InterlockedIncrement(&g_sdb_hits);

    // Log the first few times unconditionally, whatever the size is. If the
    // hook is not firing, g_sdb_hits stays 0 and the log stays silent, which is
    // the only way to tell "never called" from "called and rejected". When it
    // does fire but the size is wrong, these lines say so directly instead of
    // leaving it to be inferred.
    if (n <= 6) {
        vr_log("sdb hit %ld: ctx=%p res=%p data=%p size=%u flags=%u,%u",
               n, ctx, resource, data, (unsigned)size,
               (unsigned)flags_a, (unsigned)flags_b);
        if (data && size && size < 0x10000 && IsBadReadPtr(data, 16) == 0) {
            const float* f = (const float*)data;
            vr_log("     first 4 floats: %g %g %g %g",
                   (double)f[0], (double)f[1], (double)f[2], (double)f[3]);
        }
    }

    // Only touch it if it really is the SceneDynamicBuffer. The resolver found
    // this call by the 0x1F4 argument beside it, so a matching size here is a
    // second independent confirmation that we are at the right place.
    if (size == TDVR_SDB_SIZE && data) {
        int eye = (int)(n & 1);
        float shift = sdb_write_eye((uint8_t*)data, eye);
        if (shift != 0.0f) InterlockedIncrement(&g_sdb_writes);
        else               InterlockedIncrement(&g_sdb_misses);
    } else if (n > 3 && n <= 8) {
        vr_log("sdb: size %u is not 0x%X, not touching it",
               (unsigned)size, TDVR_SDB_SIZE);
    }

    // The original call, exactly as the engine made it. ctx is the immediate
    // context, its first qword is the vtable, and slot 0x128 is where the
    // engine called through, so this reproduces the original dispatch exactly.
    FnUpload orig = (FnUpload)*(void**)(*(uint8_t**)ctx + TDVR_CTX_VTABLE_SLOT);
    if (orig) {
        orig(ctx, resource, data, size, flags_a, flags_b);
    }
}

// ---------------------------------------------------------------------------
// Per-frame SceneDynamicBuffer bind hook
//
// REFUTED, kept for the record: this comment used to say `call 0x5df000` in
// 0x087F40 builds the descriptor for renderer+0xB90 and that rdx is the SDB
// resource. +0xB90 is a flag byte, not a field. See the note on
// TDVR_SDB_FIELD in scene_bind_find.h and docs/SDB_OFFSET_CORRECTION.md.
//
// The hook was never installed. g_sdb_bind_installed is only ever assigned 0,
// and the whole chain sat behind the byte-stealing ban that killed v38..v52,
// so none of the reasoning below was ever exercised against a live process.
// Treat every number in it as unmeasured, not as a result.
// ---------------------------------------------------------------------------
typedef void (*FnDescCtor)(void* dest, void* resource);
static FnDescCtor g_orig_sdb_bind = NULL;
static int g_sdb_bind_installed = 0;
static volatile LONG g_sdb_bind_hits = 0;

// Last observed SDB resource, and the descriptor it was bound into.
static void* volatile g_sdb_resource = NULL;
static void* volatile g_sdb_desc_dest = NULL;

static void hooked_sdb_bind(void* dest, void* resource) {
    InterlockedIncrement(&g_sdb_bind_hits);
    g_sdb_resource = resource;
    g_sdb_desc_dest = dest;

    // Build the descriptor exactly as the engine would.
    if (g_orig_sdb_bind) g_orig_sdb_bind(dest, resource);
}

// ---------------------------------------------------------------------------
// The 20 per-callee trace hooks that used to be here are removed.
//
// They hooked the 27 direct callees of beginRender to record "what does the
// engine call" as a per-frame record, on the theory that a vtable detour on
// beginRender was not enough - static analysis reached a dead end because the
// call graph from beginRender covers only 57 addresses and the rest of the
// render work is reached through pointers a rel32 sweep cannot follow.
//
// The theory was fine; the implementation is the thing that was retired, and
// the reason is worth keeping because it is the same failure this project has
// hit three times. The target list was built from CALL-SITE addresses rather
// than function entry addresses, so three of the twenty "functions" were the
// middle of someone else's code: 0x5BB2D0 begins
//
//     89 51 04 C3 CC CC      mov [rcx+4],edx ; ret ; padding
//
// Stealing ten bytes from there overwrites a real instruction and the padding
// after a return. The two hooks that did install killed the game on its first
// frame.
//
// Fixing that properly means resolving each call target to its enclosing
// function entry and then PROVING the prologue is relocatable, which needs a
// real instruction-length decoder rather than a pattern list. That is worth
// doing, but it is not worth doing on the same run as everything else, and
// certainly not by patching bytes on a guess - the guess is indistinguishable
// from a correct guess until it faults, and the fault looks like a game bug.
//
// install_hooks still logs "trace: disabled" at startup, with the same
// explanation, so the absence is visible in the log rather than looking like a
// build that never had the feature. The RVAs themselves are not worth keeping
// here: they were call sites rather than function entries, which is the whole
// problem, so the list would only invite someone to use them again.
//
// Byte-stealing is the other reason this stays off. See
// docs/ROOT_CAUSE_BYTE_STEALING.md: every build from v38 to v52 that installed
// a stolen-prologue detour froze the game, and the Present hook that replaced
// them works because it patches a vtable entry and never touches a code byte.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Frame hooks
// ---------------------------------------------------------------------------
// Trace what beginRender actually does each frame.
//
// Static analysis reached a dead end: the function that provably builds and
// uploads the SceneDynamicBuffer is not called per frame, and the call graph
// from beginRender reaches only 57 addresses, so the render path clearly goes
// through pointers that a rel32 sweep cannot follow.
//
// Rather than keep reading disassembly and guessing, record the calls that
// actually happen, with the arguments that vary. Run it, dump it, and the calls
// that repeat once per frame are the render path by observation instead of by
// inference. A call whose argument set alternates between two values is
// especially interesting: that is what a per-eye split would look like.
//
// The tracer hooks nothing. It is called from the existing beginRender detour
// and only records; the game's own calls go straight through.
static volatile LONG g_trace_on = 0;

// ---------------------------------------------------------------------------
// V55: describe one candidate object, so a single injection can tell whether
// beginRender and endRender receive the same thing and whether +0xE40 on either
// of them is a real swapchain.
//
// The check is deliberately layered. "td_read succeeded" only means the address
// is mapped, which is how a float sitting in a float4 slot 0xE40 bytes into an
// object passed every earlier test. What has to hold for a swapchain is:
//   - the value is a plausible pointer (aligned, not a small integer, mapped)
//   - the value points at a vtable
//   - slot 9 of that vtable is a code pointer inside a loaded module
// Only the third makes it a swapchain, and only that one is worth hooking.
// ---------------------------------------------------------------------------
static void tdvr_probe_swapchain_candidate(const char* who, void* self,
                                           long n) {
    void* self2 = self;
    void* chain = NULL;
    int got = 0;
    if (self2) {
        got = td_read((uint64_t)(uintptr_t)self2 + TD_SWAPCHAIN_OFFSET,
                      &chain, sizeof chain);
    }

    vr_log("probe %s #%ld: self=%p  +0x%X -> %p (read %s)",
           who, n, self2, TD_SWAPCHAIN_OFFSET, chain,
           got ? "ok" : "FAILED");

    if (!chain) {
        vr_log("probe %s #%ld:   nothing at +0x%X", who, n,
               TD_SWAPCHAIN_OFFSET);
        return;
    }

    // A pointer has to be aligned and not a small integer. float4 data in this
    // range produces values like 0x234800000238c181, which are aligned but
    // enormous and unmapped; the module check below is what rejects those.
    vr_log("probe %s #%ld:   chain aligned=%d  looks_like_float=%d",
           who, n,
           ((uintptr_t)chain & 7) == 0,
           (uintptr_t)chain > 0x7F0000000000ULL);

    void** vt = NULL;
    if (!td_read((uint64_t)(uintptr_t)chain, &vt, sizeof vt) || !vt) {
        vr_log("probe %s #%ld:   chain has no readable vtable", who, n);
        return;
    }

    void* slot9 = NULL;
    if (!td_read((uint64_t)(uintptr_t)vt + TD_PRESENT_SLOT * 8, &slot9,
                 sizeof slot9)) {
        vr_log("probe %s #%ld:   vtable=%p but slot %d unreadable",
               who, n, (void*)vt, TD_PRESENT_SLOT);
        return;
    }

    int in_mod = 0;
    if (slot9) {
        in_mod = td_addr_in_module(slot9);
    }

    vr_log("probe %s #%ld:   vtable=%p  slot9=%p  in_module=%d%s",
           who, n, (void*)vt, slot9, in_mod,
           in_mod ? "   <- THIS IS THE SWAPCHAIN" : "");
}

// ---------------------------------------------------------------------------
// V58: read the constant upload buffer and report which slots look like matrices.
//
// This is the read-only step. The disassembly of endRender shows the game doing
// exactly this before every Present:
//
//     lea   rcx, [rbx+0x1130]
//     mov   r8d, 0x800            ; 2048 bytes
//     call  <upload>
//
// so renderer+0x1130 is what the GPU actually receives for this frame, and a
// real SceneDynamicBuffer has to be visible in it. Earlier attempts to find the
// SDB by walking pointers from the renderer kept failing, because the game's
// SDB is a CPU-side source buffer that gets copied in - reading the destination
// sidesteps the walk entirely.
//
// Nothing is written. A build that reads and logs, with no modification, can
// only fail by crashing, so this separates "the buffer is there and readable"
// from "writing to it is what breaks things".
//
// A slot counts as matrix-shaped only if every element is finite, the three
// diagonal-ish terms are non-zero and in a sane range, and the perspective sum
// is non-zero. A loose epsilon test passes uninitialised heap, which is what
// made earlier scans report matrices that were all zeros with a NaN in the last
// element.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// The 0x800-byte region at renderer+0x1130 is dumped by tdvr_hex_classify
// immediately below, not by a float4 slot walker.
//
// A previous version of this file had tdvr_dump_const_upload here, which walked
// the buffer in 0x40-byte slots and tested each as a matrix. It was deleted for
// two reasons. It was never called, and its central assumption - the comment
// "0x40 is exactly one float4x4, and the disassembly shows single-slot uploads
// of that size in beginRender" - is refuted by measurement. A live 2048-byte
// dump of the region showed 0 of 24 slots matrix-shaped; the values are small
// integers and heap pointers.
//
// The 0.40 figure is a size coincidence, not a layout. The replacement below
// prints hex and classifies each 8-byte slot by address shape only, which needs
// no assumption about the payload at all. Do not reintroduce an interpretation
// that the dump does not support. See the TD_OFF_CONST_UPLOAD comment earlier in
// this file and upload_scan.h.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// V60: hex dump and classify, do NOT interpret.
//
// v59 printed the same bytes as float4 and I read the result as "this holds
// pointers". That was the same mistake again one level up: taking 64 raw bytes
// and printing them as float4 tells you how the bytes look when forced into a
// float, not what they are. The one conclusion that did survive is the useful
// one: whatever this is, it is not matrix data, because 0 of 24 slots passed a
// matrix test and no plausible matrix lives at that offset.
//
// So this build prints hex and classifies each 8-byte slot by ADDRESS SHAPE
// only, which needs no assumption about the payload:
//   zero            - slot is unused
//   in_module       - points at a DLL or the executable, so a real pointer
//   mapped, no mod  - points at heap or driver memory
//   other           - small integer, or a value no region covers
//
// A value like 52263940 is an integer, not an address. Saying "pointers" was
// the previous build's guess and it is wrong; this one only reports what it can
// prove.
// ---------------------------------------------------------------------------
static void tdvr_hex_classify(const uint8_t* buf, uint32_t len) {
    // Hex rows, 16 bytes each. Hex is the ground truth: any interpretation
    // added later is explicit and reversible.
    for (uint32_t off = 0; off < len; off += 16) {
        char line[80];
        int p = 0;
        p += snprintf(line + p, sizeof line - (size_t)p, "    +0x%03X  ", off);
        for (int b = 0; b < 16 && off + b < len; ++b) {
            p += snprintf(line + p, sizeof line - (size_t)p, "%02X ",
                          buf[off + b]);
        }
        vr_log("%s", line);
    }

    int nz = 0, in_mod = 0, mapped = 0, other = 0;
    for (uint32_t off = 0; off + 8 <= len; off += 8) {
        uint64_t v = 0;
        memcpy(&v, buf + off, 8);
        if (v == 0) continue;
        nz++;
        if (td_addr_in_module((const void*)(uintptr_t)v)) {
            in_mod++;
            vr_log("    slot +0x%03X = %016llx  in a loaded module",
                   off, (unsigned long long)v);
            continue;
        }
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((void*)(uintptr_t)v, &mbi, sizeof mbi) &&
            mbi.State == MEM_COMMIT) {
            mapped++;
            vr_log("    slot +0x%03X = %016llx  committed, not a module",
                   off, (unsigned long long)v);
            continue;
        }
        other++;
        vr_log("    slot +0x%03X = %016llx  not an address (small int?)",
               off, (unsigned long long)v);
    }
    vr_log("    8-byte slots: %d non-zero, %d in-module, %d committed-only, "
           "%d not-addresses", nz, in_mod, mapped, other);
}

// Dump a window of the renderer around the constant upload, so the structure
// is visible rather than one offset at a time.
static void tdvr_dump_window(void* self, long n) {
    if (!self) return;
    const uint64_t base = (uint64_t)(uintptr_t)self;
    // V61: sweep a wide range in 0x80 steps. The three v60 windows at 0x1100,
    // 0x1130 and 0x1200 all read 128/128 bytes and classified every 8-byte slot
    // as zero, which means the region is readable but empty at that moment -
    // the upload has not been filled yet at endRender #5. A wider sweep with
    // more sample points finds where the renderer actually keeps live data,
    // and separates "wrong offset" from "right offset, wrong time".
    static const uint64_t offs[] = {
        0x0CC, 0xE40, 0xE58, 0xEF8, 0x1000, 0x1080,
        0x1100, 0x1130, 0x1180, 0x1200, 0x1280, 0x1300,
        0x1400, 0x1500, 0x1800, 0x2000
    };
    const int NOFF = (int)(sizeof offs / sizeof offs[0]);
    for (int i = 0; i < NOFF; ++i) {
        uint8_t buf[0x80];
        uint32_t got = (uint32_t)td_read(base + offs[i], buf, sizeof buf);
        vr_log("win #%ld: renderer+0x%llX -> %u/%u bytes", n,
               (unsigned long long)offs[i], got, (unsigned)sizeof buf);
        if (got < 16) continue;
        {
            // Count first, dump only where something is actually there, so the
            // log stays readable and the interesting windows stand out.
            int nz = 0;
            for (uint32_t o = 0; o + 8 <= got; o += 8) {
                uint64_t v = 0;
                memcpy(&v, buf + o, 8);
                if (v) nz++;
            }
            vr_log("    non-zero 8-byte slots: %d", nz);
            if (nz) tdvr_hex_classify(buf, got);
        }
    }
}

static void hooked_begin_render(void* self) {
    // Same reachability counter as hooked_end_render, for the same reason: the
    // install log cannot distinguish "patched but never called" from "patched and
    // called every frame", and only the count can.
    {
        static LONG n = 0;
        LONG k = InterlockedIncrement(&n);
        if (k <= 3 || (k % 500) == 0) {
            vr_log("[hit] hooked_begin_render call #%ld  self=%p", (long)k, self);
        }
    }

    // V52: COUNTER-ONLY BUILD.
    //
    // This build answers exactly one question: are the two vtable hooks
    // themselves survivable? No td_read, no scan_frame_contexts, no Present
    // search, no renderer dump, no frame-context walk. Each hook counts, calls
    // the original once, counts again.
    //
    // Why this is worth a build: v51 had every scan disabled and still died,
    // while an uninjected control game held 9 minutes with no trouble. So the
    // mod is at fault, and the only things still running were these two hooks
    // plus the memory reads inside them. Counting separates "the hooks are
    // fine, a read is at fault" from "the hooks alone kill it".
    static LONG n = 0;
    LONG c = InterlockedIncrement(&n);
    g_renderer = self;
    if (c <= 4 || (c % 120) == 0) {
        vr_log("beginRender %ld (self=%p)", c, self);
    }

    g_orig_begin(self);

    static LONG m = 0;
    LONG d = InterlockedIncrement(&m);
    if (d <= 4 || (d % 120) == 0) {
        vr_log("beginRender %ld survived the original call", c);
    }
    // No td_present_from_object(self) here. It used to sit below under `if (0)`.
    // It is called from hooked_end_render instead, because beginRender receives a
    // different object than endRender does and only the latter's +0xE40 has ever
    // held a swapchain. See "WHICH OBJECT" in present.h.

    // WHY beginRender DOES NOT INSTALL THE PRESENT HOOK
    //
    // It used to, and it was wrong for four builds. The history is kept because
    // the failure is not visible in the code and the conclusion is load-bearing.
    //
    // The reasoning that brought the hook here was sound. In v53 this frame hook
    // ran 12 840 times per minute on a live process with the game unaffected, so
    // both the renderer offsets and the guarded read through td_read were
    // established. Reading the swapchain at self + 0xE40 and patching its own
    // vtable is a safe place to hook Present: the pointer is in hand, no address
    // is guessed, and patching a vtable entry never touches a code byte. That
    // last part is the whole point - the previous route to the
    // SceneDynamicBuffer was the upload consumer at 0x5BAEB0, reachable only by
    // stealing 15 bytes from its prologue, and that byte-stealing froze the
    // game on every build from v38 to v52.
    //
    // What the hook got wrong was the OBJECT. Installing from here, the value at
    // self+0xE40 was not a swapchain: its slot 9 read back as 0x234800000238c181,
    // a float4 exponent pattern rather than an address, and VirtualQuery on it is
    // unqueryable. That refuted an assumption that had been carried since v42 and
    // that had only ever existed as a comment. The trap is general: a successful
    // td_read proves the ADDRESS is mapped, not that the OBJECT is the right one,
    // and the log line that appeared to confirm "+0xE40 = swapchain" was printed
    // by code that never checked.
    //
    // The diagnosis was ambiguous for several builds, which is what made it
    // expensive. Two explanations fit the log equally well - wrong offset, or
    // wrong object - and a third fit too: a double dereference in the validator,
    // where a second read of the vtable lands on slot 0 and produces the same
    // float-exponent pattern. All three were live at once. The tie-break was the
    // disassembly of endRender, which is unambiguous: it does "mov rbx,rcx" then
    // "mov rdx,[rbx+0xE40]" and calls slot 9 of that. So 0xE40 was right, and
    // both the reader and the object were the problem. See "WHICH OBJECT" in
    // present.h and docs/PRESENT_HOOK_DOUBLE_DEREF.md.
    //
    // The resolution, confirmed by measurement: the two hooks received the same
    // object and both read a valid swapchain from +0xE40 -
    //
    //     self   = 000001cf4a03b3b0
    //     +0xE40 = 000001cf4a2d2260
    //     vtable = 00007ffe62d7c830
    //     slot 9 = 00007ffe62ca50e0   in a loaded module
    //
    // The bad run differed only in the object: +0xE40 held 0000023e5c019650,
    // whose slot 9 is float data. Same offset, different object. The install
    // therefore moved to hooked_end_render, which is where the disassembly reads
    // it from, and it runs there only - doing it from both hooks would mean
    // whichever ran first wins, which is the mistake above.
    //
    // hooked_present still does no stereo work. It counts and logs, and that is
    // deliberate rather than unfinished: a build that changes one thing at a time
    // is the only kind whose result means anything, and the Present install is
    // the thing being established.
}


// ---------------------------------------------------------------------------
// Slot census (TDVR_SLOT_CENSUS)
//
// The renderer's vtable slots are the only place the two frame hooks can live,
// and the two slot numbers in the header turned out not to be them. Rather than
// keep guessing, every slot gets a hand-built counting stub and a reporter
// thread prints the deltas, so the hot methods identify themselves.
// ---------------------------------------------------------------------------
static LONG*    g_census_counters = NULL;
static uint8_t** g_census_stubs    = NULL;
static volatile LONG g_census_stop = 0;

/* The IAT census had a scanner but no reporter, and because both
 * census_reporter() and frame_reporter() are only created inside branches
 * guarded by TDVR_SLOT_CENSUS, a build with the class census off created no
 * reporting thread at all. The hooks were live, the log was silent, and that
 * reads exactly like "the instrumentation did nothing" -- the same trap as the
 * `if (n > 21)` reporter. Every instrument now gets a thread whether or not the
 * others are compiled in.
 *
 * This sits deliberately OUTSIDE the TDVR_SLOT_CENSUS block: it was defined
 * inside it first, and with the class census off the definition vanished while
 * the CreateThread below still referenced it. */
#if TDVR_IAT_CENSUS
static DWORD WINAPI iat_reporter(LPVOID arg) {
    (void)arg;
    for (;;) {
        Sleep(2000);
        td_iat_report();
    }
}
#endif

#if TDVR_SLOT_CENSUS
// A counting stub is 24 bytes of code and 40 bytes of slack. Each one is:
//     48 B8 <counter>   mov rax, counter address
//     FF 00             inc qword ptr [rax]
//     48 B8 <original>  mov rax, real implementation
//     FF E0             jmp rax
// so the call is counted and then forwarded with the registers untouched apart
// from RAX, which the ABI does not define across a call boundary anyway.
#endif

static DWORD WINAPI census_reporter(LPVOID arg) {
    (void)arg;
    static LONG seen[TDVR_CENSUS_SLOTS];
    int first = 1;
    for (;;) {
        if (InterlockedCompareExchange(&g_census_stop, 1, 1) == 1) break;
        Sleep(2000);
        if (InterlockedCompareExchange(&g_census_stop, 1, 1) == 1) break;
        if (first) {
            for (int i = 0; i < TDVR_CENSUS_SLOTS; i++) seen[i] = 0;
            first = 0;
            vr_log("SLOT CENSUS --- first sample taken, deltas from here ---");
            continue;                 // the first pass is the baseline
        }
        char line[512];
        int  n = 0;
        n += _snprintf(line + n, sizeof(line) - (size_t)n, "SLOT CENSUS deltas:");
        /* Count how many slots actually moved, rather than testing the string
         * length. The header is 19 characters, and the old test was `n > 21`,
         * so a frame in which NO slot moved produced n == 19 and the line was
         * dropped -- forever. The reporter sat in that branch for the whole run
         * and the log looked like the census had gone quiet, which reads
         * exactly like "the hooks are dead", the very thing the census exists
         * to disprove. */
        int moved = 0;
        for (int i = 0; i < TDVR_CENSUS_SLOTS; i++) {
            LONG cur = g_census_counters ? g_census_counters[i] : 0;
            LONG d = cur - seen[i];
            seen[i] = cur;
            if (d > 0) {
                moved++;
                n += _snprintf(line + n, sizeof(line) - (size_t)n,
                               "  [%d]=%ld", i, d);
            }
        }
        if (moved)
            vr_log("%s  (moved=%d)", line, moved);
        else
            vr_log("SLOT CENSUS: no slot moved this pass (all %d still zero)",
                   TDVR_CENSUS_SLOTS);

#if TDVR_SLOT_CENSUS
        /* The DXGI factory census lives in its own file and is polled here
         * rather than from a second thread: two threads writing the same log
         * at the same cadence produce interleaved lines, and an interleaved
         * report is harder to read than a slightly late one. */
        td_census_report(vr_log);
#endif
#if TDVR_FRAME_CENSUS
        /* Same reasoning, same thread, same cadence: one interleaving-free
         * report covering every instrument. */
        td_frame_report();
#endif
#if TDVR_IAT_CENSUS
        td_iat_report();
#endif
    }
    return 0;
}

static void hooked_end_render(void* self) {
    // Reachability counter. The install log says the vtable slots were patched,
    // but that only proves the patch landed -- not that anything ever calls
    // through those slots. A run that logs "hooks installed via VTABLE" and then
    // sits flat with no per-frame output at all is exactly the case where the
    // object Teardown actually renders through is a different instance with a
    // different vtable, and no amount of reading the install log says so.
    // Deliberately outside every other guard so it counts on the very first
    // call, whatever build flags are set.
    {
        static LONG n = 0;
        LONG k = InterlockedIncrement(&n);
        if (k <= 3 || (k % 500) == 0) {
            vr_log("[hit] hooked_end_render call #%ld  self=%p", (long)k, self);
        }
    }

    // The Present install lives HERE, and only here. The disassembly reads
    // renderer+0xE40 from endRender's own argument, and an attempt to install it
    // from beginRender as well failed for four builds for a reason that is
    // invisible in the code: whichever hook ran first won, and on a live frame
    // the first one to run is not reliably the one holding the right object.
    // The full account is in hooked_begin_render, under "WHY beginRender DOES
    // NOT INSTALL THE PRESENT HOOK".
    //
    // The probe is kept on the first few calls so the log records why the
    // install was allowed, then the install runs once. A swapchain outlives the
    // renderer that created it, so patching the vtable after a handful of frames
    // is fine and avoids racing the first frame's Present call.
    {
        static LONG ne = 0;
        LONG e = InterlockedIncrement(&ne);
        if (e <= 4) {
            tdvr_probe_swapchain_candidate("endRender", self, e);
        }
        if (e == 5 || e == 300) {
            tdvr_dump_window(self, e);
        }
        if (!g_present_orig && e <= 600) {
#if !TDVR_SWAPCHAIN_READ_ENABLED
            // Variant C: the hooks stay live, but the one read of a pointer this
            // build did not create is skipped. self+0xE40 has been wrong before
            // in this project (it read back as float noise, not a swapchain), so
            // if the crash is the read and not the detour, this isolates it.
            return;
#else
            void* chain = NULL;
            if (td_read((uint64_t)(uintptr_t)self + TD_SWAPCHAIN_OFFSET,
                        &chain, sizeof chain) && chain) {
                td_install_present(chain);
            }
#endif
        }

        // The swapchain is also the route to a graphics device, and the OpenXR
        // session cannot be created without one: SteamVR's client log refuses
        // with "No binding struct was provided" and returns
        // XR_ERROR_GRAPHICS_DEVICE_INVALID (-38) when the chain has no graphics
        // binding, and a binding with a NULL device is equally fatal. Trying this
        // from the DLL-load thread could only ever fail, which is what every
        // earlier run did.
        //
        // This must NOT live inside the block above. That block is guarded by
        // `!g_present_orig`, which holds true exactly once -- on the call that
        // installs Present -- so an `e == 5` test inside it can never be
        // reached. That is why the first attempt logged the deferred-session
        // line and then never mentioned a device: the one-shot test sat where it
        // could not fire. It carries its own guard and its own read instead.
        // The swapchain lives at +0xE40 on the object endRender receives, but the
        // buffers behind it are not populated on the first frames. Taking the
        // backbuffer on frame 5 alone meant GetBuffer never produced anything for
        // the whole run (measured: backbuffer=0 in every log line), so it is
        // retried on a later frame and the failure is reported once rather than
        // silently leaving the preview blank.
        // The device binding must happen on the FIRST frame that has a real
        // swapchain behind it, whatever number that frame happens to be.
        //
        // It used to be `if (e == 5) tdvr_xr_bind_device_from_swapchain(ch2)`
        // -- a fixed frame number, gated behind the one-shot `!g_present_orig`
        // block above as well. Both were wrong, and the log says so plainly: a
        // run measured 38 log lines ending at "instance+system ready, session
        // deferred to the render thread" with the file then flat at 2016 bytes
        // and no "GetDevice(slot 7)" line at all. e == 5 never came, so the
        // session was never attempted, no loop ever ran, and the game sat alive
        // with an XR instance and nothing else.
        //
        // So: try every frame until a session exists. The function is already
        // idempotent (it returns early on have_session and on session_tried),
        // which is what makes polling it safe.
        void* ch2 = NULL;
        if (td_read((uint64_t)(uintptr_t)self + TD_SWAPCHAIN_OFFSET,
                    &ch2, sizeof ch2) && ch2) {
            // Log the first few attempts whether or not they can succeed yet.
            // A run that ends with the log flat at 2015 bytes, 38 lines, and no
            // "GetDevice(slot 7)" line anywhere in it means this code is not
            // being reached at all -- which is a different bug from the device
            // being unavailable, and the two look identical from the log alone.
            if (e <= 3 || (e % 120) == 0) {
                vr_log("OpenXR: endRender frame %ld: swapchain=%p present_orig=%p",
                       (long)e, ch2, g_present_orig);
            }
            tdvr_xr_bind_device_from_swapchain(ch2);
            if (!g_xr_backbuffer) tdvr_xr_take_backbuffer(ch2);
        } else if (e <= 3 || (e % 120) == 0) {
            vr_log("OpenXR: endRender frame %ld: no swapchain at +0x%X (read=%p)",
                   (long)e, TD_SWAPCHAIN_OFFSET, ch2);
        }

        // The XR frame loop runs here, not in the init path. It needs a session,
        // and the session can only exist once the device above has been bound --
        // so this is the first place where it can possibly work. It was defined
        // and never called at all until now, which is why the pose stayed
        // unreadable even after the space was created.
        //
        // Throttled hard, and deliberately NOT tied to the game's frame rate.
        // xrWaitFrame blocks until the runtime wants a frame, so calling it at
        // the game's rate is a mismatch: the game renders far faster, and each
        // skipped call can leave the runtime holding a frame. The first run of
        // the loop at one call per 30 frames grew the working set to 4.6 GB,
        // which is the runtime queueing instead of dropping.
        //
        // Once per 240 frames is still several times a second and cannot build a
        // backlog. If this ever needs to track the real XR frame rate, the
        // place to do it is a dedicated thread, not this hook.
        if (g_xr.have_session && g_xr.have_space) {
            static LONG xr_n = 0;
            LONG xn = InterlockedIncrement(&xr_n);
            if ((xn % 240) == 1) {
                tdvr_xr_poll(&g_xr);
                // Log the loop's health occasionally. frame_ends tracking
                // frame_waits means the loop is keeping up; a growing
                // frame_errors means it is not, and that is worth seeing
                // rather than guessing about a frozen pose later.
                if ((xn % 600) == 1) {
                    vr_log("OpenXR: loop waits=%u ends=%u ready=%d err=%u "
                           "tracked=%d pose=(%.3f %.3f %.3f) yaw=%.3f",
                           g_xr.frame_waits, g_xr.frame_ends, g_xr.frame_ready,
                           g_xr.frame_errors, g_xr.tracking_valid,
                           g_xr.head_x, g_xr.head_y, g_xr.head_z, g_xr.yaw);
                    // proj vs ends is the honest measure of whether the runtime
                    // got a colour target: equal means every renderable frame
                    // carried the projection layer, and the simulator should
                    // have left "waiting for stereo projection" behind.
                    vr_log("OpenXR: projection submissions=%u of %u ends "
                           "(swapchains L=%d R=%d %dx%d fmt=%d)",
                           g_xr.proj_submissions, g_xr.frame_ends,
                           g_xr.sc_made[0], g_xr.sc_made[1],
                           g_xr.sc_width, g_xr.sc_height, g_xr.sc_format);
                    // copies is the honest measure of whether real pixels reach
                    // the runtime: a projection layer pointing at an untouched
                    // texture submits fine and shows nothing.
                    vr_log("OpenXR: copies=%u of %u ends (L res=%p R res=%p "
                           "backbuffer=%p)", g_xr.copy_frames, g_xr.frame_ends,
                           g_xr.sc_res[0], g_xr.sc_res[1], g_xr_backbuffer);
                }
            }
        }
    }

    // V52: COUNTER-ONLY BUILD. See the note in hooked_begin_render: this build
    // has no memory reads at all, so that if it still dies, the vtable hooks
    // themselves are the cause and no offset in the renderer matters.
    static LONG n = 0;
    LONG c = InterlockedIncrement(&n);
    if (c <= 4 || (c % 120) == 0) {
        vr_log("endRender %ld (self=%p)", c, self);
    }

    g_orig_end(self);

    static LONG m = 0;
    LONG d = InterlockedIncrement(&m);
    if (d <= 4 || (d % 120) == 0) {
        vr_log("endRender %ld survived the original call", c);
    }
}

// ---------------------------------------------------------------------------
// Install
//
// Patch the vtable rather than the code. A vtable entry is a pointer: there
// is no instruction-length decoder, no stolen prologue and no trampoline, so
// there is no way for a patch to leave a half-overwritten instruction behind.
// The byte-stealing detour this replaced had exactly that exposure — on the
// updated build its prologue is "41 56 41 57 48 8D 6C 24 B9 48 81", and a
// decoder that guessed wrong would have jumped into the middle of "lea rbp,
// [rsp-0x47]".
//
// Slot indices come from RTTI (see vtable_resolve.h), so a game update moves
// the code and the hook still finds it.
// ---------------------------------------------------------------------------
static td_hooks g_hooks;

static int install_hooks(void) {
    char why[96] = "not attempted";

    vr_log("bisect variant: %s", TDVR_BISECT_NAME);

#if !TDVR_HOOKS_ENABLED
    // Variant A: deliberately hook nothing. The init thread still runs and still
    // logs, so the game is started with this DLL loaded but with no detour in
    // the renderer at all. If it still dies, the crash is not the hooks.
    vr_log("bisect A: ALL renderer hooks disabled (baseline)");
    return 0;
#endif

    if (td_resolve_by_rtti(&g_hooks, why, sizeof why)) {
        g_hooks.via_rtti = 1;
        vr_log("resolved via RTTI: vtable=%p (rva 0x%X)", g_hooks.vtable, g_hooks.vtable_rva);
    } else {
        // Loudly flagged: this is the path that dies on the next patch.
        g_hooks.via_rtti = 0;
        g_hooks.image = td_host_image();
        if (!g_hooks.image) { vr_log("cannot even locate the host image"); return 0; }
        g_hooks.begin = g_hooks.image + TD_RVA_BEGIN_RENDER;
        g_hooks.end   = g_hooks.image + TD_RVA_END_RENDER;
        vr_log("RTTI resolution FAILED (%s) - falling back to hardcoded RVAs", why);
        vr_log("  begin=%p end=%p  <-- WILL BREAK on the next game update", 
               g_hooks.begin, g_hooks.end);
    }

    g_hooks.begin_orig = g_hooks.begin;
    g_hooks.end_orig   = g_hooks.end;
    g_orig_begin = (FnVoid)g_hooks.begin_orig;
    g_orig_end   = (FnVoid)g_hooks.end_orig;

    if (g_hooks.via_rtti) {
#if TDVR_SLOT_CENSUS
        // SLOT CENSUS -- the pivot, and it replaces guessing.
        //
        // The problem this exists to solve: TD_SLOT_BEGIN_RENDER=2 and
        // TD_SLOT_END_RENDER=3 are not beginRender and endRender. Reading the
        // live vtable at pid 8468 showed 32 slots, none of them holding the
        // addresses the resolver reported (begin=0, end=0x7FF754BA6E50, present
        // in no slot), and neither hook had ever fired once. So the two numbers
        // in the header are wrong, and nothing that hangs off those hooks --
        // including the entire OpenXR chain -- can run.
        //
        // Rather than guess which slot is which, patch EVERY slot with a
        // counting stub and let the game tell us. Each stub increments its own
        // counter and tail-jumps to the real implementation, so the game behaves
        // normally; we just learn which virtual functions are hot. The slots
        // that tick once per frame are the frame-loop methods, and the two whose
        // pattern matches "runs before drawing" and "runs after drawing" are the
        // begin/end pair by observation instead of by a number someone wrote
        // down once.
        //
        // Stub layout, 32 bytes each, emitted by hand because there is no
        // assembler here:
        //     48 B8 <8>        mov rax, &counter[i]
        //     FF 00            inc qword ptr [rax]
        //     48 B8 <8>        mov rax, original[i]
        //     FF E0            jmp rax
        // RAX is caller-saved and already dead at a function entry, so loading
        // it twice is safe; the second load restores it before the tail jump, so
        // the original sees exactly the register state it would have seen.
        vr_log("SLOT CENSUS: instrumenting every slot of the renderer vtable");
        {
            // Fixed at file scope, not a local const: a block-scope `const int`
            // is not a constant expression in C, so it cannot size a static
            // array. The count is also what the reporter samples, so it has to
            // agree with what was instrumented.
            static LONG   census[TDVR_CENSUS_SLOTS];
            static uint8_t* stubs[TDVR_CENSUS_SLOTS];
            uint8_t* blob = (uint8_t*)VirtualAlloc(NULL, (size_t)TDVR_CENSUS_SLOTS * 64,
                                MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (!blob) { vr_log("SLOT CENSUS: stub alloc failed"); return 0; }
            g_census_counters = census;
            g_census_stubs    = stubs;

            int installed = 0;
            for (int i = 0; i < TDVR_CENSUS_SLOTS; i++) {
                void* real = g_hooks.vtable[i];
                if (!real) continue;
                // Skip anything that is not code: a data pointer patched as if it
                // were a function would crash on the first call, and a vtable
                // slot holding a pointer outside the image and outside our DLL is
                // not a method we can safely wrap.
                uint8_t* rp = (uint8_t*)real;
                if (rp[0] == 0xC3 || rp[0] == 0xCC) {
                    vr_log("  slot %2d: skipped (stub/padding, first byte 0x%02X)", i, rp[0]);
                    continue;
                }
                uint8_t* s = blob + (size_t)i * 64;
                stubs[i] = s;
                s[0] = 0x48; s[1] = 0xB8;                       // mov rax, imm64
                *(uint64_t*)(s + 2) = (uint64_t)(uintptr_t)&census[i];
                s[10] = 0xFF; s[11] = 0x00;                      // inc [rax]
                s[12] = 0x48; s[13] = 0xB8;                     // mov rax, imm64
                *(uint64_t*)(s + 14) = (uint64_t)(uintptr_t)real;
                s[22] = 0xFF; s[23] = 0xE0;                      // jmp rax
                s[24] = 0xCC;                                   // int3 (never reached)

                if (!td_patch_vtable_slot(g_hooks.vtable, i, (void*)s)) {
                    vr_log("  slot %2d: census patch FAILED", i);
                    continue;
                }
                installed++;
            }
            vr_log("SLOT CENSUS: %d slots instrumented; report every 2 s", installed);
            CreateThread(NULL, 0, census_reporter, NULL, 0, NULL);
        }
#if TDVR_FRAME_CENSUS
        else {
            /* No class census means no reporter thread; without this the frame
             * counters would be sampled by nobody and every reading would be
             * 0 -- indistinguishable from "not called", which is the one
             * conclusion this instrument exists to test. */
            vr_log("FRAME CENSUS: no class census in this build; its own reporter");
            CreateThread(NULL, 0, frame_reporter, NULL, 0, NULL);
        }
#endif
#else
        /* The class census above hooks the engine's own C++ objects, which
         * measured 48/48 slots at zero while the game rendered. This one hooks
         * the exported buffer swap instead, and installs whether or not the
         * class census ran -- they are independent questions. */
#if TDVR_FRAME_CENSUS
        td_frame_install();
#endif
#if TDVR_IAT_CENSUS
        /* The IAT census is the safe version of what the export detour was
         * trying to do: same question, asked of the game's own import table
         * instead of by rewriting shared system code. */
        td_iat_scan();
        CreateThread(NULL, 0, iat_reporter, NULL, 0, NULL);
#endif
        // Each of the two vtable slots is patched independently, so which one is
        // responsible for the crash can be established by leaving one alone.
        // Patching them through one condition would make that impossible.
        int patched_any = 0;
        if (TDVR_BEGIN_HOOK_ENABLED) {
            if (!td_patch_vtable_slot(g_hooks.vtable, TD_SLOT_BEGIN_RENDER,
                                      (void*)hooked_begin_render)) {
                vr_log("ABORT: beginRender vtable patch failed (VirtualProtect)");
                return 0;
            }
            patched_any = 1;
        } else {
            vr_log("bisect E: beginRender slot left unpatched");
        }
        if (TDVR_END_HOOK_ENABLED) {
            if (!td_patch_vtable_slot(g_hooks.vtable, TD_SLOT_END_RENDER,
                                      (void*)hooked_end_render)) {
                vr_log("ABORT: endRender vtable patch failed (VirtualProtect)");
                return 0;
            }
            patched_any = 1;
        } else {
            vr_log("bisect E: endRender slot left unpatched");
        }
        if (!patched_any) {
            vr_log("bisect: both renderer slots disabled; nothing installed");
            return 0;
        }
#endif
    } else {
        // Same prologue guard as before, for the fallback path.
        static const uint8_t expect[] = {
            0x48, 0x89, 0x5C, 0x24, 0x18,
            0x48, 0x89, 0x6C, 0x24, 0x20,
            0x56
        };
        if (memcmp(g_hooks.begin, expect, sizeof(expect)) != 0) {
            const uint8_t* g = (const uint8_t*)g_hooks.begin;
            vr_log("ABORT: beginRender prologue mismatch at %p", g_hooks.begin);
            vr_log("  expected: 48 89 5C 24 18 48 89 6C 24 20 56");
            vr_log("  actual  : %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                   g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], g[8], g[9], g[10]);
            vr_log("  game updated — RTTI path should have caught this; check the log above");
            return 0;
        }

        size_t begin_steal = measure_stealable(g_hooks.begin, 32);
        size_t end_steal   = measure_stealable(g_hooks.end,   32);
        vr_log("fallback detour: stealable begin=%zu end=%zu", begin_steal, end_steal);
        if (begin_steal < 6 || end_steal < 6 ||
            begin_steal > TRAMP_CAP - 14 || end_steal > TRAMP_CAP - 14) {
            vr_log("ABORT: prologue %zu/%zu cannot be detached safely", begin_steal, end_steal);
            return 0;
        }

        g_begin_tramp = (uint8_t*)VirtualAlloc(NULL, TRAMP_CAP,
                                MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        g_end_tramp   = (uint8_t*)VirtualAlloc(NULL, TRAMP_CAP,
                                MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!g_begin_tramp || !g_end_tramp) { vr_log("trampoline alloc failed"); return 0; }

        g_orig_begin = (FnVoid)build_detour(g_hooks.begin, (void*)hooked_begin_render,
                                            g_begin_tramp, begin_steal);
        g_orig_end   = (FnVoid)build_detour(g_hooks.end,   (void*)hooked_end_render,
                                            g_end_tramp,   end_steal);
        if (!g_orig_begin || !g_orig_end) { vr_log("detour build failed"); return 0; }
    }

    // "hooks installed" is only allowed to be said when it is true. The vtable
    // patch now reads its own slot back (see td_patch_vtable_slot), and this
    // line prints the vtable base so an external read of the process can be
    // compared against it without trusting the log. A run whose slots read back
    // as teardown.exe addresses while this says otherwise is the failure mode
    // this comment exists to prevent.
    vr_log("hooks installed via %s: begin=%p end=%p vtable=%p",
           g_hooks.via_rtti ? "VTABLE" : "FALLBACK-DETOUR",
           g_hooks.begin, g_hooks.end, g_hooks.vtable);
    if (g_hooks.via_rtti) {
        vr_log("  verify: slot %d = %p, slot %d = %p  (both should be inside this DLL)",
               TD_SLOT_BEGIN_RENDER,
               *(void *volatile *)((unsigned char *)g_hooks.vtable
                                   + sizeof(void *) * (size_t)TD_SLOT_BEGIN_RENDER),
               TD_SLOT_END_RENDER,
               *(void *volatile *)((unsigned char *)g_hooks.vtable
                                   + sizeof(void *) * (size_t)TD_SLOT_END_RENDER));
    }

    // -----------------------------------------------------------------------
    // The SceneDynamicBuffer hook, which is the one that matters for stereo.
    //
    // Independent of the vtable hooks above: this patches an instruction inside
    // the function that fills the buffer, so it cannot collide with a previous
    // build of this mod the way a second vtable write would. Resolved by
    // signature, so an update that moves the code still finds it.
    // -----------------------------------------------------------------------
    {
        // The image size must come from the PE headers, not from VirtualQuery.
        // VirtualQuery on the module base reports only the region containing
        // the base, which is the 64 KB header region — far short of the
        // 0xDE130 the resolver needs to reach its search window, so the scan
        // bailed out with "NOT FOUND" even though the site is there. Reading
        // SizeOfImage from the optional header is both correct and immune to
        // how the loader mapped the sections.
        uint8_t* img = (uint8_t*)g_hooks.image;
        size_t img_size = 0;
        if (img) {
            const IMAGE_NT_HEADERS64* nt =
                (const IMAGE_NT_HEADERS64*)(img + *(const uint32_t*)(img + 0x3C));
            if (nt->Signature == IMAGE_NT_SIGNATURE) {
                img_size = nt->OptionalHeader.SizeOfImage;
            }
            if (img_size < TDVR_SDB_SEED_RVA + TDVR_SDB_SEARCH_WINDOW) {
                vr_log("image size %zu is too small to search, skipping SDB",
                       img_size);
                img_size = 0;
            }
        }
        vr_log("image=%p SizeOfImage=%zu", (void*)img, img_size);

        // Trace beginRender's direct callees.
        //
        // DISABLED. Hooking callees by RVA is unsound here, and the two that
        // did install killed the game on its first frame. The reason is a bug in
        // how the target list was built: it came from the call-site addresses
        // rather than the function entry addresses, so three of the twenty
        // "functions" are actually the middle of someone else's code — 0x5BB2D0
        // begins "89 51 04 C3 CC CC", which is `mov [rcx+4],edx; ret` followed
        // by padding, and stealing ten bytes from there overwrites a real
        // instruction and the padding after a return.
        //
        // Fixing that means resolving each call target to its enclosing function
        // entry and then proving the prologue is relocatable, which needs a
        // real instruction decoder. That is worth doing properly, but not on the
        // same run as everything else, and not by patching bytes on a guess.
        vr_log("trace: disabled (callee RVAs were call sites, not function "
               "entries; see the comment above)");

        // Per-frame SceneDynamicBuffer bind.
        //
        // This is the real target, found after proving the frame hooks are not
        // it. 0x087F40 is the only function in the image that reads
        // renderer+0xB90, and it is reached from the game loop via
        // 0x070B20 <- 0x071CE0. The pattern is located by instruction encoding,
        // not by RVA, and the target is sanity-checked before patching.
        {
            // V53: BOTH BYTE-STEALING DETOURS ARE OFF.
            //
            // v52 froze roughly 10-20 s after injection, DURING a level, with no
            // memory reads, no scan and no Present search. The only code still
            // patched were the two detours below, which steal real bytes from
            // live code (5 for the SDB bind call, 15 for the upload consumer)
            // and redirect through a trampoline.
            //
            // Byte-stealing is a better suspect than the vtable hooks because it
            // rewrites the instruction stream of a function that is actively
            // running, whereas vtable entries point at real functions we merely
            // forward to. A mis-stolen boundary turns a prologue into a
            // different instruction sequence, and the failure shows up as a
            // freeze several frames later, which is the observed signature.
            //
            // This build keeps the vtable hooks and installs no detour at all.
            // If it survives, byte-stealing is the cause and the
            // SceneDynamicBuffer must be read some other way. If it still
            // freezes, the vtable hooks are the cause.
            vr_log("V53: byte-stealing detours disabled "
                   "(sdb bind + upload consumer)");
        }

        // The 0x9E130 hook is gone. Two live entry/call hooks proved it is
        // never invoked per frame (0 hits over 6600 and 11760 frames
        // respectively), and 0x087F40 turned out to be the per-frame pass that
        // actually reads renderer+0xB90. Hooking the wrong function is worse
        // than not hooking it: it produced a green build, a healthy 144 FPS
        // process, and no stereo at all, which is the hardest kind of bug to
        // notice. The resolver in sdb_resolve.h still works and is still
        // useful as a decoder, but it no longer installs anything.
        vr_log("0x9E130 hook: removed (not per frame; see "
               "docs/SDB_BREAKTHROUGH_RESOURCE_TYPE.md)");
    }

    return 1;
}

// ---------------------------------------------------------------------------
// Exported control API
// ---------------------------------------------------------------------------
__declspec(dllexport) void VR_Enable(int on)   { g_enabled = on; vr_log("VR_Enable(%d)", on); }
__declspec(dllexport) int  VR_IsEnabled(void)  { return g_enabled; }
__declspec(dllexport) void VR_SetIPD(float mm) { g_ipd_mm = mm; vr_log("IPD=%.2fmm", (double)mm); }
__declspec(dllexport) long VR_GetFrameCount(void) { return g_frame_count; }
__declspec(dllexport) void* VR_GetRenderer(void)  { return g_renderer; }
__declspec(dllexport) void* VR_GetSwapchain(void) { return g_swapchain; }
__declspec(dllexport) int  VR_IsXRReady(void)    { return g_xr_ready; }
__declspec(dllexport) void* VR_GetSceneBuffer(void) { return g_scene_buf; }

// ---------------------------------------------------------------------------
// DllMain
//
// Strictly minimal: it must not call anything that takes a loader lock, which
// rules out the whole CRT (fopen/fprintf/fclose) and anything that loads
// another module. It records the module and hands off to a thread, which is
// the first place the CRT is safe to touch.
// ---------------------------------------------------------------------------
static DWORD WINAPI init_thread(LPVOID unused) {
    (void)unused;
    // Build stamp first, so a log is never ambiguous about which DLL wrote it.
    // The "wine" wording below is a leftover from the Proton-era runs and reads
    // wrong on native Windows, so say what this actually is.
    vr_log("=== teardown_vr.dll build %s (%s) ===", TDVR_BUILD, sizeof(void *) == 8 ? "x64" : "x86");
    vr_log("=== init thread, pid %lu ===", GetCurrentProcessId());
    vr_log("log file: %s", g_log_path ? g_log_path : "NONE - could not open any");

    // OpenXR only in builds that ask for it. Loading a VR runtime inside the
    // game process kills it -- measured on both runtimes present here, SteamVR
    // and Meta, each dying inside LoadLibraryA with a 210-byte log and a Sentry
    // dialog. The bisect variants must not touch the runtime at all, or they
    // would crash for a reason that has nothing to do with the hooks.
    #if TDVR_HOOKS_ENABLED && !TDVR_NO_XR
    xr_try_load();
    #endif

    // The renderer's code exists as soon as the module is mapped, so hooking
    // can happen right away — no waiting for a device. (On the container test
    // the game never gets a device, so beginRender is never called, but the
    // hook still installs and the failure is the game's, not ours.)
    //
    // Wait for the injector to hand us the base.
    //
    // LoadLibraryA returns before the injector makes its VR_SetHostImage call,
    // and this thread starts during DLL_PROCESS_ATTACH — so it must not give
    // up before the injector has had its turn. 10 s covers a slow
    // CreateRemoteThread round trip; the wait is capped so a genuine failure
    // still falls through to the loader walk and reports why.
    vr_log("looking for the game image, up to 10 s ...");

#if TDVR_VEH
    // First thing on this thread, before any hook and before OpenXR is even
    // touched. A fault during the rest of init is exactly the case where a
    // Sentry report says "General crash" and nothing more, so the handler has to
    // already be in place. It only reads; it never takes over the exception.
    tdvr_veh_install();
#endif

#if TDVR_XRPROBE
    // Diagnostic mode: ONE bounded xrCreateInstance call on a worker thread, with
    // a 20 s deadline. This is the whole purpose of this build.
    //
    // xr_try_load() has already run above and it is the only thing that creates an
    // instance, so this must NOT sit before it. The loader has to be loaded and
    // g_xr_gipa resolved first: xrCreateInstance is a GLOBAL, and the loader is
    // the only thing allowed to hand it out before an instance exists. The probe
    // then returns from this init thread, so the normal instance path never runs
    // and the two cannot be confused with each other in the log.
    if (!g_openxr_module) {
        vr_log("[XRP] no OpenXR loader was loaded; nothing to probe");
        return 0;
    }
    if (!g_xr_gipa) {
        vr_log("[XRP] the loader's xrGetInstanceProcAddr is not resolved; "
               "cannot probe without it");
        return 0;
    }
    tdvr_xrprobe_run((void*)g_xr_gipa, "teardown_vr");
    return 0;
#endif

    int waited = 0;
    while (waited < 10000) {
        g_hooks.image = td_host_image();
        if (g_hooks.image) break;
        Sleep(100);
        waited += 100;
    }
    if (g_hooks.image)
        vr_log("host image %p after %d ms", (void *)g_hooks.image, waited);
    else
        vr_log("host image NOT found after %d ms — install_hooks will say why",
               waited);

#if TDVR_PRESENT_HOOK
    // The swapchain hook goes in FIRST, before anything else, because it is
    // create-time only and the window it needs to see has exactly one opening.
    //
    // Measured ordering from a real run, log line numbers as they appeared:
    //      40  [hit] hooked_begin_render call #1
    //      52  [SC] === swapchain capture hook ===
    // The game had already created its swapchain before the DXGI hook existed.
    // That is why the patch verified cleanly on both slots and then never fired:
    // there was no second creation to catch. install_hooks() below spends
    // seconds walking the game image, and the 10 s host-image search above can
    // take the full 10 s, so anything installed after them is installed late by
    // construction.
    //
    // Order below: read the flag, install the DXGI hook, then do the rest.
    tdvr_early_present_hook();
#endif

    if (install_hooks()) {
        g_enabled = 1;
        vr_log("ready");

#if TDVR_SLOT_CENSUS
        // Path B, and the instrument the project specified at bisect.h:212 and
        // never built. The class vtable above is a measured dead end -- 48
        // counting stubs, zero calls, while the game rendered -- so this counts
        // the DXGI layer underneath instead. Every stub forwards to the real
        // implementation, so the game keeps running and the numbers are
        // evidence rather than a change in behaviour.
        td_census_dxgi();
#endif

#if TDVR_DXGI_PROBE
        // Path B. The engine's own vtable is unreachable by name (measured: 48
        // counting stubs installed, zero calls), so measure the DXGI layer
        // underneath it instead. Everything here is read-only: create a factory,
        // prove which slots are really dxgi.dll thunks, enumerate the adapters.
        // No hook is installed by this, so it cannot destabilise the game, and it
        // settles the "D3D12CreateDevice is E_NOTIMPL everywhere" question that
        // has been blamed for xrCreateSession failing.
        tdvr_dxgi_probe();
#endif

#if TDVR_PRESENT_HOOK
        // NOTE: the swapchain hook is NOT installed here any more. It has to go
        // in before the 10 s host-image search and before install_hooks(), and
        // it is called as tdvr_early_present_hook() above. Installing it twice
        // would patch the vtable slot with the hook's own address as the
        // "original", so the second hook would call the first hook.
#endif
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_module = hModule;
        CreateThread(NULL, 0, init_thread, NULL, 0, NULL);
    } else if (reason == DLL_PROCESS_DETACH) {
        // Only reached at process exit, where the loader lock is gone and the
        // CRT is still valid. Safe to write the final tally.
        if (g_log) {
            vr_log("=== detached after %ld frames ===", g_frame_count);
            fclose(g_log);
            g_log = NULL;
        }
    }
    return TRUE;
}
