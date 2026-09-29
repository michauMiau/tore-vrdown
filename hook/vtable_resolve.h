// vtable_resolve.h — hook TRendererD3D12 through its RTTI, not through a
// hardcoded address.
//
// THE PROBLEM THIS SOLVES
// A hardcoded RVA for beginRender works until the game patches, then the
// prologue check fires and the mod silently does nothing:
//
//   [VR] ABORT: beginRender prologue mismatch at 00007ff754b9b9d0
//     expected: 48 89 5C 24 18 48 89 6C 24 20 56
//     actual  : 41 56 41 57 48 8D 6C 24 B9 48 81
//
// That is not a failure to work around, it is the design pointing the wrong
// way. RTTI lives in .rdata, which is NOT encrypted even in the protected
// build (measured entropy 5.399 against 8.000 for .text), and a virtual
// function's SLOT INDEX is a property of the class definition, not of where
// the compiler placed the code. So the address can be re-derived on any
// build from data that survives patching.
//
// Measured on the unpacked build (see docs/VTABLE_SLOTS.md):
//   TypeDescriptor  ".?AVTRendererD3D12@@"  rva 0x1C6C880
//   Complete Object Locator                rva 0xAF22B8   (vtable offset 0)
//   primary vtable                         rva 0xA859A0
//   beginRender = vtable[2]
//   endRender   = vtable[3]
//
// WHY THE VTABLE IS PATCHED INSTEAD OF THE CODE
// Swapping the vtable pointer needs no instruction-length decoder, no stolen
// bytes and no trampoline. The prologue above is a good illustration: a
// byte-stealing detour would have to decode "48 8D 6C 24 B9" (lea rbp,
// [rsp-0x47]) to avoid slicing an instruction in half, and the next patch
// could introduce an opcode the decoder has never seen. A vtable entry is
// just a pointer; there is nothing to decode and nothing to get wrong.

#ifndef TDVR_VTABLE_H
#define TDVR_VTABLE_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// Slot indices, measured on the unpacked Steamless build.
#define TD_SLOT_BEGIN_RENDER 2
#define TD_SLOT_END_RENDER   3

#define TD_CLASS_NAME ".?AVTRendererD3D12@@"

// Fallback RVAs from the build we originally analysed. Only used if RTTI
// resolution fails, and logged loudly when that happens, because it is the
// path that breaks on the next patch.
#define TD_RVA_BEGIN_RENDER 0x005AB9D0ull
#define TD_RVA_END_RENDER   0x005AF270ull

typedef struct {
    uint8_t  *image;        // module base
    size_t    image_size;   // SizeOfImage
    void    **vtable;       // resolved primary vtable
    uint32_t  vtable_rva;   // for logging
    void     *begin;        // vtable[TD_SLOT_BEGIN_RENDER]
    void     *end;          // vtable[TD_SLOT_END_RENDER]
    void     *begin_orig;   // saved, so we can still call through
    void     *end_orig;
    int       via_rtti;     // 1 = RTTI, 0 = hardcoded fallback
} td_hooks;

// Map an RVA to its address in a LOADED image.
//
// PointerToRawData (the +20 field) is the offset inside the file on disk. In
// memory that offset is meaningless: the loader places each section at its
// VirtualAddress, so a loaded image maps an RVA to image+rva and nothing else.
// Using raw here happens to work on an image where the file and memory layouts
// coincide, which is why it survived testing against an unpacked build, and
// then reads garbage in a real game.
static int td_rva_to_ptr(const uint8_t *image, uint32_t rva, const uint8_t **out) {
    DWORD e_lfanew = *(const DWORD *)(image + 0x3C);
    const uint8_t *coff = image + e_lfanew + 4;
    WORD nsec  = *(const WORD *)(coff + 2);
    WORD optsz = *(const WORD *)(coff + 16);
    const uint8_t *secoff = coff + 20 + optsz;

    for (WORD i = 0; i < nsec; i++) {
        const uint8_t *s = secoff + i * 40;
        DWORD vsz = *(const DWORD *)(s + 8);
        DWORD va  = *(const DWORD *)(s + 12);
        if (rva >= va && rva < va + vsz) {
            *out = image + rva;
            return 1;
        }
    }
    return 0;
}

static int td_readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
    const uint8_t *s = (const uint8_t *)p;
    return (s + n) <= (const uint8_t *)mbi.BaseAddress + mbi.RegionSize;
}

// Does this pointer look like a compiled function in the main image?
static int td_looks_like_code(const td_hooks *h, const void *p) {
    if (!p) return 0;
    if ((const uint8_t *)p < h->image) return 0;
    if ((const uint8_t *)p >= h->image + h->image_size) return 0;
    if (!td_readable(p, 16)) return 0;
    return 1;
}

// --- finding the host module ------------------------------------------------
//
// GetModuleHandleW(NULL) inside an injected DLL returns the DLL's OWN HMODULE,
// not the game's: the loader resolves "the main module" relative to whoever
// called. Scanning our own 230 KB image for RTTI finds nothing, which is
// exactly the "Complete Object Locator not found" the first live run produced.
//
// So walk the loader's module list instead and pick the game. PEB->Ldr is
// reachable from the TEB, which is per-thread and needs no export:
// The loader structures are declared here rather than included: windows.h with
// WIN32_LEAN_AND_MEAN does not expose PEB or UNICODE_STRING, and pulling in
// winternl.h costs a set of structs we only need two fields of. Only the layout
// matters here, and that layout is fixed by the loader.
typedef struct td_LDR_DATA_TABLE_ENTRY {
    LIST_ENTRY InLoadOrderLinks;
    LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks;
    PVOID      DllBase;
    PVOID      EntryPoint;
    ULONG      SizeOfImage;              // <-- the field we select on
} td_LDR_DATA_TABLE_ENTRY;

typedef struct td_PEB_LDR_DATA {
    ULONG      Length;
    ULONG      Initialized;
    PVOID      SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
    LIST_ENTRY InMemoryOrderModuleList;
    LIST_ENTRY InInitializationOrderModuleList;
} td_PEB_LDR_DATA;

// x64 TEB is at gs:[0x60], the 32-bit one at fs:[0x30].
//
// __readgsqword lowers to a built-in that GCC's -Warray-bounds misreads as an
// out-of-bounds subscript on a zero-length array. Inline the assembly instead,
// which is also one instruction shorter.
static void *td_get_peb(void) {
#if defined(_M_X64) || defined(__x86_64__)
    void *out;
    __asm__ __volatile__("movq %%gs:0x60, %0" : "=r"(out) : : "memory");
    return out;
#else
    void *out;
    __asm__ __volatile__("movl %%fs:0x30, %0" : "=r"(out) : : "memory");
    return out;
#endif
}

// PEB->Ldr, at the offset this loader actually uses.
//
// Windows documents +0x18 on x64. GE-Proton's Wine puts it at +0x10 — verified
// by dumping the PEB from inside the running game: +0x10 held 6ffffbb00000,
// which is teardown.exe's base, while +0x18 held an unrelated pointer. Trusting
// the documented offset produced "cannot even locate the host image" and no
// hook at all.
//
// The offsets are tried in order and the first one that yields a plausible
// module list wins, so the same DLL works on both.
static td_PEB_LDR_DATA *td_get_ldr(void) {
    uint8_t *peb = (uint8_t *)td_get_peb();
    if (!peb) return NULL;

    static const size_t offsets[] = {
        0x18,   // documented x64
        0x10,   // GE-Proton / Wine
        0x0C,   // documented x86
    };
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        void *cand = *(void **)(peb + offsets[i]);
        if (!cand) continue;
        // Cheap plausibility: a LDR_DATA_TABLE begins with three LIST_ENTRYs,
        // so Flink and Blink must both look like heap pointers.
        LIST_ENTRY *le = (LIST_ENTRY *)cand;
        uintptr_t fl = (uintptr_t)le->Flink, bl = (uintptr_t)le->Blink;
        if (fl > 0x10000 && bl > 0x10000)
            return (td_PEB_LDR_DATA *)cand;
    }
    return NULL;
}

// Pick the biggest module that is not us. The game is ~30 MB, our DLL 230 KB,
// system DLLs well under that, so size separates them without matching names.
static uint8_t *td_find_host_image(void) {
    // The game module is the one whose SizeOfImage dwarfs everything else, and
    // it is never our DLL. Compare against our own base obtained by address.
    HMODULE self = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             (LPCWSTR)(const void *)&td_find_host_image, &self))
        return NULL;

    uint8_t *peb = (uint8_t *)td_get_peb();
    if (!peb) return NULL;
    td_PEB_LDR_DATA *ldr = td_get_ldr();
    if (!ldr) return NULL;
    LIST_ENTRY *head = &ldr->InMemoryOrderModuleList;
    uint8_t *best = NULL;
    ULONG best_size = 0;

    for (LIST_ENTRY *link = head->Flink; link && link != head; link = link->Flink) {
        // InMemoryOrderLinks sits one pointer into the entry (after the two
        // InLoadOrder links), so back up to recover the struct start.
        td_LDR_DATA_TABLE_ENTRY *e =
            (td_LDR_DATA_TABLE_ENTRY *)((uint8_t *)link -
                                        offsetof(td_LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks));
        if (!e->DllBase || !e->SizeOfImage) continue;
        if ((HMODULE)e->DllBase == self) continue;
        if (e->SizeOfImage > best_size) { best_size = e->SizeOfImage; best = (uint8_t *)e->DllBase; }
    }
    return best;
}

static uint8_t *td_host_image_override = NULL;

// The base, handed over in a file.
//
// This is the only handover that works under GE-Proton's Wine, because it
// depends on no loader query at all. The injector runs in its own process where
// Toolhelp32 gives the full-width address (0x6ffffbb00000), writes it to a file
// next to the game, and the DLL reads it. Everything else needed a loader
// answer the DLL cannot get — see docs/HOST_MODULE.md.
static uint8_t *td_host_from_file(void) {
    // The bare name is relative to this process's working directory, which is
    // where the injector put the file. The absolute paths are fallbacks for the
    // case where the working directory is somewhere else entirely.
    static const char *cands[5] = {
        "tdvr_host.txt",                 // relative to the game folder
        "D:\\SteamLibrary\\steamapps\\common\\Teardown\\tdvr_host.txt",
        "C:\\tdvr_host.txt",
        "Z:\\root\\steamless\\Teardown\\tdvr_host.txt",   // Proton-era layout
        NULL
    };
    for (int i = 0; cands[i]; i++) {
        FILE *f = fopen(cands[i], "r");
        if (!f) {
            // Log the miss once, on the first poll only. This function runs on
            // every retry, and a path that never resolves would otherwise fill
            // the log with the same line for ten seconds. A silent miss here is
            // what produced a hang with no explanation.
            static int reported = 0;
            if (!reported) { reported = 1; vr_log("  no host file at: %s", cands[i]); }
            continue;
        }
        unsigned long long v = 0;
        int n = fscanf(f, "%llx", &v);
        fclose(f);
        if (n == 1 && v) {
            vr_log("host base %llx handed over via %s", v, cands[i]);
            return (uint8_t *)(uintptr_t)v;
        }
    }
    return NULL;
}

static uint8_t *td_host_image(void) {
    if (td_host_image_override) return td_host_image_override;
    uint8_t *from_file = td_host_from_file();
    if (from_file) {
        td_host_image_override = from_file;   // cache, so we read once
        return from_file;
    }
    return td_find_host_image();
}

static void td_set_host_image(void *base) {
    td_host_image_override = (uint8_t *)base;
}

// Copy a reason string without tripping -Wdiscarded-qualifiers.
static void td_set_why(char *dst, size_t n, const char *src) {
    if (!dst || n == 0) return;
    size_t i = 0;
    for (; i + 1 < n && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

// Resolve the class vtable by RTTI. Returns 1 on success; on failure returns 0
// and writes a human-readable reason into `why` for the log.
// True when `p` points inside any of this DLL's own sections. A vtable entry
// that resolves to our code means we already hooked it.
static int td_is_our_hook(const void *p, HMODULE self) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void *)p, &mbi, sizeof mbi)) return 0;
    if (mbi.AllocationBase != (PVOID)self) return 0;
    return 1;
}

// True when `p` is a hook installed by some OTHER build of this mod.
//
// Several builds can be resident at once during development — each one has a
// unique DLL name and its own log, because a mapped DLL cannot be overwritten.
// They all hook the same vtable slots, so the first one to load wins and every
// later build finds slot 2 pointing into a DLL rather than into the game image.
// The RTTI resolver then concludes the class moved and falls back to stale
// hardcoded RVAs, which aborts.
//
// This is not really resolvable from inside the new build: the existing hook
// is already in the call path and the new build cannot take it over. What it
// CAN do is stop pretending the game updated, and name the module that already
// owns the slot, so the log says what is actually true.
static int td_is_foreign_hook(const void *p, char *who, size_t whosz) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void *)p, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;

    HMODULE mod = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)p, &mod))
        return 0;
    if (!mod) return 0;

    char path[MAX_PATH];
    if (!GetModuleFileNameA(mod, path, sizeof path)) return 0;

    // A vtable entry normally points into teardown.exe. Anything else is a
    // hook, and the only thing that hooks this game is a previous build of this
    // mod, so naming the module is enough to identify it.
    {
        const char *base = strrchr(path, '\\');
        const char *leaf = base ? base + 1 : path;
        if (_stricmp(leaf, "teardown.exe") == 0) return 0;
        if (_stricmp(leaf, "ntdll.dll") == 0) return 0;
        if (whosz) {
            strncpy(who, leaf, whosz - 1);
            who[whosz - 1] = 0;
        }
        return 1;
    }
}

static int td_resolve_by_rtti(td_hooks *h, char *why, size_t whysz) {
    h->image = td_host_image();
    if (!h->image) { td_set_why(why, whysz, "host module not found in loader list"); return 0; }

    DWORD e_lfanew = *(DWORD *)(h->image + 0x3C);
    const uint8_t *opt = h->image + e_lfanew + 4 + 20;
    h->image_size = *(DWORD *)(opt + 0x38);          // PE32+ SizeOfImage

    // --- 1. TypeDescriptor: locate the mangled name, back up 16 bytes -----
    // TypeDescriptor is { u32 pVFTable; u32 spare; char name[]; } and the
    // 8-byte prefix is fixed, so name-16 is the struct start.
    size_t nlen = strlen(TD_CLASS_NAME) + 1;
    uint32_t first8 = *(const uint32_t *)TD_CLASS_NAME;
    const uint8_t *scan_end = h->image + h->image_size;
    const uint8_t *td = NULL;

    for (const uint8_t *q = h->image; q + 24 < scan_end; q += 4) {
        if (*(const uint32_t *)q != first8) continue;     // cheap reject
        if (memcmp(q, TD_CLASS_NAME, nlen) == 0) { td = q - 16; break; }
    }
    if (!td) {
        // Distinguish "the class does not exist here" from "the name exists but
        // the scan cannot see it", because those call for different next steps:
        // the first means the layout moved and RTTI is the wrong tool, the
        // second means the scan has a bug.
        //
        // A partial match is the useful middle case. If a mangled name for
        // TRendererD3D12 exists but not this exact one, the class was renamed,
        // re-namespaced or the class moved to another backend.
        size_t bare = strlen("TRendererD3D12");
        int partials = 0;
        for (const uint8_t *q = h->image; q + bare + 8 < scan_end; q++) {
            if (memcmp(q, "TRendererD3D12", bare) != 0) continue;
            // Show the surrounding mangled name: a renamed or re-namespaced
            // class still carries this substring, and the prefix is what
            // distinguishes one from another.
            if (partials < 3 && q >= h->image + 16) {
                const char *s = (const char *)q;
                if (s[-1] == 'V' || s[-1] == 'U' || s[-1] == 'W') {
                    const char *start = s - 2;          // ".?A" / ".?U" / ".?W"
                    char frag[64];
                    int i = 0;
                    while (i < 63) {
                        unsigned char ch = (unsigned char)start[i];
                        if (ch <= 0x20 || ch >= 0x7F) break;
                        frag[i++] = (char)ch;
                    }
                    frag[i] = 0;
                    vr_log("  nearby RTTI name: %s", frag);
                    partials++;    // only count the ones worth showing
                }
            }
        }
        // Count every hit, not just the ones printed, so the number in the log
        // is the true occurrence count.
        int all = 0;
        for (const uint8_t *q = h->image; q + bare + 8 < scan_end; q++)
            if (memcmp(q, "TRendererD3D12", bare) == 0) all++;
        td_set_why(why, whysz, all ? "class name differs, or RTTI is absent"
                                   : "no RTTI name for this class anywhere");
        vr_log("  TypeDescriptor scan: no '.?AVTRendererD3D12@@' in %u bytes",
               (unsigned)h->image_size);
        vr_log("  substring 'TRendererD3D12' occurrences: %d", all);
        return 0;
    }
    uint32_t td_rva = (uint32_t)(td - h->image);

    // --- 2. Complete Object Locator that points at that TypeDescriptor ----
    // x64 COL layout, 24 bytes:
    //   +0  signature (1)      +4  vtable offset   +8  cdOffset
    //   +12 pTypeDescriptor    +16 pClassDescriptor +20 pSelf
    // pTypeDescriptor and pSelf are RVAs, which is why this survives ASLR.
    uint32_t col_rva = 0;
    // Diagnostics for the step that has failed before: how many places even
    // mention this TypeDescriptor, and what the candidate COLs look like when
    // they are rejected. The rejection tests below each have their own counter,
    // because "found nothing" and "found the wrong thing" need different fixes.
    int seen = 0, rej_sig = 0, rej_ptd = 0, rej_self = 0, rej_unmap = 0;
    uint32_t first_cand = 0;
    for (const uint8_t *q = h->image; q + 24 < scan_end; q += 4) {
        if (*(const uint32_t *)q != td_rva) continue;
        seen++;
        uint32_t cand = (uint32_t)(q - h->image) - 12;     // COL starts 12 earlier
        const uint8_t *col = NULL;
        if (!td_rva_to_ptr(h->image, cand, &col)) { rej_unmap++; continue; }
        uint32_t sig   = *(const uint32_t *)(col + 0);
        uint32_t ptd   = *(const uint32_t *)(col + 12);
        uint32_t pself = *(const uint32_t *)(col + 20);
        if (!first_cand) first_cand = cand;
        // pSelf must point back at this very COL; that check rejects the
        // coincidental matches a raw 4-byte search turns up.
        if (sig != 1)      { rej_sig++; continue; }
        if (ptd != td_rva) { rej_ptd++; continue; }
        if (pself != cand) { rej_self++; continue; }
        col_rva = cand;
        break;
    }
    if (!col_rva) {
        vr_log("  COL search: %d references to this TypeDescriptor", seen);
        if (first_cand)
            vr_log("    first candidate at RVA 0x%X: sig=%u ptd=0x%X pself=0x%X"
                   "  (want sig=1 ptd=0x%X pself=0x%X)",
                   first_cand,
                   *(const uint32_t *)(h->image + first_cand + 0),
                   *(const uint32_t *)(h->image + first_cand + 12),
                   *(const uint32_t *)(h->image + first_cand + 20),
                   td_rva, first_cand);
        vr_log("    rejected: %d unmapped, %d wrong signature, %d wrong "
               "pTypeDescriptor, %d wrong pSelf", rej_unmap, rej_sig, rej_ptd,
               rej_self);
        td_set_why(why, whysz, seen ? "TypeDescriptor found, but its COL failed "
                                     "validation" : "TypeDescriptor has no COL");
        return 0;
    }

    // --- 3. The vtable sits 8 bytes after the pointer to the COL ----------
    // Layout:  [vtable-8] = &COL , vtable[0..] = function pointers
    uint64_t col_va = (uint64_t)h->image + col_rva;
    for (const uint8_t *q = h->image; q + 16 < scan_end; q += 8) {
        if (*(const uint64_t *)q != col_va) continue;
        h->vtable = (void **)(q + 8);
        h->vtable_rva = (uint32_t)(q + 8 - h->image);
        break;
    }
    if (!h->vtable) { td_set_why(why, whysz, "vtable pointer not found"); return 0; }

    // How many vtables point at this COL, and do they agree on slot 2/3?
    //
    // The loop above stops at the first hit, which is the whole reason this
    // matters. A class with a base subobject has one Complete Object Locator
    // but its virtual methods can be reached through more than one table (the
    // primary one and one per derived level), and a game that instantiates a
    // class through a pointer to a base will call through whichever table that
    // object actually carries. Patching only the first table installs a hook
    // that is never called -- which is exactly what the log looks like:
    // "hooks installed", every slot verified as ours, and then silence.
    //
    // So: count them, and log whether the slots agree. If there is more than
    // one, the first is not automatically the right one, and pretending
    // otherwise is a hypothesis dressed as a result.
    {
        int nvt = 0, agree = 0;
        uint32_t first_rva = 0;
        for (const uint8_t *q = h->image; q + 16 < scan_end; q += 8) {
            if (*(const uint64_t *)q != col_va) continue;
            void **vt = (void **)(q + 8);
            uint32_t rva = (uint32_t)(q + 8 - h->image);
            nvt++;
            if (!first_rva) first_rva = rva;
            if (vt[TD_SLOT_BEGIN_RENDER] == h->vtable[TD_SLOT_BEGIN_RENDER] &&
                vt[TD_SLOT_END_RENDER]   == h->vtable[TD_SLOT_END_RENDER])
                agree++;
            if (nvt <= 6)
                vr_log("  vtable candidate rva=0x%X slot2=%p slot3=%p", rva,
                       vt[TD_SLOT_BEGIN_RENDER], vt[TD_SLOT_END_RENDER]);
        }
        vr_log("  vtables for this COL: %d, agreeing on slot2/3: %d (using "
               "rva=0x%X)", nvt, agree, h->vtable_rva);
        if (nvt > 1)
            vr_log("  NOTE: more than one vtable exists for this class; only "
                   "rva=0x%X is hooked. If endRender never fires, the object "
                   "being rendered through a different table.",
                   h->vtable_rva);
    }

    h->begin = h->vtable[TD_SLOT_BEGIN_RENDER];
    h->end   = h->vtable[TD_SLOT_END_RENDER];

    // A slot may already hold one of our hooks from an earlier load in this same
    // process. That is not a failure to resolve, it is a second copy: the entry
    // points at a module, not at the game, and re-hooking it would replace the
    // first copy's trampoline and lose its frame counter. Say so plainly instead
    // of reporting a resolution failure and falling back to hardcoded RVAs.
    if (td_is_our_hook(h->begin, g_module) || td_is_our_hook(h->end, g_module)) {
        h->vtable = NULL;
        td_set_why(why, whysz,
                   "vtable slots already hold this mod's hooks - another copy "
                   "of the DLL is loaded in this process; restart the game");
        return 0;
    }

    // A different build of this mod is already resident. The name is in the
    // message because the fix is not obvious: during development several
    // uniquely-named builds sit in the process at once (a mapped DLL cannot be
    // overwritten), and they all hook the same slots. Saying "the game updated"
    // here sends you hunting for a nonexistent update.
    {
        char who[64];
        char msg[256];
        who[0] = 0;
        if (td_is_foreign_hook(h->begin, who, sizeof who) ||
            td_is_foreign_hook(h->end, who, sizeof who)) {
            h->vtable = NULL;
            // td_set_why takes a literal, not a format, so build the message
            // here rather than passing the module name through a variadic it
            // does not have.
            strncpy(msg, "vtable slot 2 is hooked by ", sizeof msg - 1);
            msg[sizeof msg - 1] = 0;
            strncat(msg, who[0] ? who : "another module", sizeof msg - strlen(msg) - 1);
            strncat(msg, ", an earlier build of this mod still loaded in this "
                        "process; restart the game to test a new one",
                    sizeof msg - strlen(msg) - 1);
            td_set_why(why, whysz, msg);
            return 0;
        }
    }

    if (!td_looks_like_code(h, h->begin) || !td_looks_like_code(h, h->end)) {
        h->vtable = NULL;
        td_set_why(why, whysz, "vtable slot does not point into the image");
        return 0;
    }
    return 1;
}

// Swap vtable[slot] for `hook`, keeping the original so calls pass through.
//
// It used to return 1 as soon as VirtualProtect succeeded, without ever reading
// the slot back. The "hooks installed via VTABLE" log line was therefore a claim
// about intent, not a measurement, and it was wrong for four builds in a row:
// a run at pid 9440 logged the install, then produced no per-frame output at
// all -- no beginRender hit, no endRender hit, nothing -- while a direct read of
// the process memory showed both slots still holding addresses inside
// teardown.exe rather than inside this DLL. So the patch silently did not take,
// every conclusion drawn from "the hook is installed" was unfounded, and the
// real XR chain could never even start because the device binding hangs off
// endRender.
//
// The read-back is the whole point. A patch that cannot be proven is not a patch.
static int td_patch_vtable_slot(void **vtable, int slot, void *hook) {
    if (!vtable || slot < 0) return 0;
    void *orig = vtable[slot];
    if (!orig) return 0;
    DWORD old = 0;
    if (!VirtualProtect(vtable, sizeof(void *) * (size_t)(slot + 1),
                        PAGE_EXECUTE_READWRITE, &old))
        return 0;
    vtable[slot] = hook;
    VirtualProtect(vtable, sizeof(void *) * (size_t)(slot + 1), old, &old);
    FlushInstructionCache(GetCurrentProcess(), vtable, sizeof(void *));

    // Prove it. Compiler and CPU may reorder the store past the call, so read
    // the slot again through a volatile path rather than trusting the write.
    void *now = *(void *volatile *)((unsigned char *)vtable + sizeof(void *) * (size_t)slot);
    if (now != hook) {
        vr_log("vtable slot %d: WRITE FAILED, still 0x%llX (wanted 0x%llX)",
               slot, (unsigned long long)(uintptr_t)now,
               (unsigned long long)(uintptr_t)hook);
        return 0;
    }
    return 1;
}


// Called by the injector once it has the game's base, because inside injected
// code GetModuleHandleW(NULL) hands back the DLL's own module and the PEB walk
// is loader-dependent. Takes a wait because the injector runs this right after
// LoadLibraryA returns, while our init thread may already be mid-attempt.
// Two entry points for the same value, because they fail differently.
//
// VR_SetHostImage is the normal path: the injector calls it in the target with
// CreateRemoteThread. On Windows that is reliable. Under Wine it is not —
// Toolhelp32's modBaseAddr is truncated, so the injector may compute a bad
// target address and the thread "succeeds" while doing nothing.
//
// VR_SetHostImageAt writes through an address the injector supplies instead of
// relying on any loader lookup, so it works even where the base the injector
// knows (read from /proc/<pid>/maps) disagrees with what Wine reports in
// process. Returns the address actually written, non-zero, so the caller can
// tell a completed write from a thread that never ran.
__declspec(dllexport) void *VR_SetHostImage(void *base) {
    td_set_host_image(base);
    return base;
}

__declspec(dllexport) void *VR_SetHostImageAt(void *variable, void *base) {
    if (!variable) return 0;
    *(uint8_t **)variable = (uint8_t *)base;
    return base;
}

// This DLL's own base, for the injector's benefit.
//
// GetModuleHandleExW(FROM_ADDRESS) is the one loader query that is correct from
// inside an injected DLL: it attributes an address to whichever module contains
// it, and this function lives in this module. Everything else that could name
// "the current module" is either ambiguous (GetModuleHandleW(NULL) under Wine
// returns the exe) or unavailable (the exe is not what we want anyway).
__declspec(dllexport) void *VR_GetOwnBase(void) {
    HMODULE h = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             (LPCWSTR)(const void *)&VR_GetOwnBase, &h))
        return 0;
    return (void *)h;
}

// Stamped into every log so a stale DLL is obvious at a glance.
#define TDVR_BUILD "2026-09-26-dupdet"

#endif /* TDVR_VTABLE_H */
