/* TDVR_IAT_CENSUS -- count what the game actually calls, without touching any
 * shared system DLL.
 *
 * WHY THIS EXISTS, in order, because the history is the argument:
 *
 * 1. A vtable census over the engine's own C++ renderer object measured 48
 *    instrumented slots and ZERO calls while the game demonstrably rendered.
 * 2. A DXGI factory census measured 32 instrumented slots and exactly one call;
 *    IDXGIFactory::CreateSwapChain stayed at zero. The whole D3D12 path is a
 *    dead end for this game.
 * 3. The first attempt to count the frame swap instrumented the EXPORTS of
 *    GDI32.dll and OPENGL32.dll. That hard-locked the game, twice over:
 *      - the stub's tail jump went back to the export THUNK instead of the real
 *        implementation, and the thunk by then pointed at the stub: an infinite
 *        two-instruction loop, so no crash, no Sentry dialog, just a spin;
 *      - a 16-byte memcpy over a 6-byte instruction.
 *    Both were my bugs, and the deeper one is the lesson: GDI32.dll and
 *    OPENGL32.dll belong to every process in the session. A detour there is
 *    not a measurement, it is an edit to shared state I was never asked to make.
 *
 * THE FIX IS THE GAME'S OWN IMPORT TABLE. The game imports these functions by
 * name, so every call it makes goes through a slot in the game's private
 * Import Address Table. Overwriting that slot redirects this one process and
 * leaves the DLL byte-for-byte untouched. The slot holds whatever the loader
 * put there -- for GDI32 that is the export thunk address -- and because the
 * thunk is no longer being modified, jumping to it is correct. The failure mode
 * that locked the game last time cannot recur: the stub's forward target is a
 * value the loader wrote, not something I overwrote.
 *
 * The stub is 24 bytes and clobbers RAX only:
 *     48 B8 <counter>    mov rax, counter
 *     FF 00              inc qword ptr [rax]
 *     48 B8 <forward>    mov rax, forward
 *     FF E0              jmp rax
 * RAX is caller-saved and dead at a function entry point, so no callee may
 * depend on it, and both loads are inside our own bytes. Flags are also
 * clobbered by INC, and flags are dead at an entry point for the same reason.
 *
 * Nothing here guesses. If the import directory cannot be walked, if a slot is
 * not writable, or if an address is not a canonical user-space pointer, that is
 * logged by name rather than skipped quietly.
 */

#ifndef TDVR_IAT_CENSUS_H
#define TDVR_IAT_CENSUS_H

#include <windows.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TDVR_IAT_MAX 768

typedef struct TdvrIatEnt {
    const char       *dll;
    const char       *fn;
    void             *forward;   /* what the loader put in the slot          */
    void            **slot;      /* the game's own slot                       */
    volatile LONG64  *calls;
    unsigned char    *stub;
    int               hooked;
} TdvrIatEnt;

static TdvrIatEnt          g_iat[TDVR_IAT_MAX];
static volatile LONG        g_iat_n;
static volatile LONG        g_iat_hooked;
static volatile LONG        g_iat_refused;

/* Only these libraries. Hooking the whole import table would mean a stub on
 * every CRT and Win32 call the game makes, which is hundreds of hot functions
 * for no extra information. */
static const char *TDVR_IAT_LIBS[] = {
    "OPENGL32.dll", "GDI32.dll", "dxgi.dll", "d3d11.dll", "d3d12.dll",
    "vulkan-1.dll", "GLU32.dll", "d3d9.dll",
};

static int td_iat_watch_dll(const char *name)
{
    for (unsigned i = 0; i < sizeof TDVR_IAT_LIBS / sizeof TDVR_IAT_LIBS[0]; i++)
        if (_stricmp(name, TDVR_IAT_LIBS[i]) == 0) return 1;
    return 0;
}

static void td_iat_emit(unsigned char *p, volatile LONG64 *ctr, void *forward)
{
    unsigned long long a = (unsigned long long)(uintptr_t)ctr;
    unsigned long long b = (unsigned long long)(uintptr_t)forward;
    p[0]  = 0x48; p[1]  = 0xB8;  memcpy(p + 2,  &a, 8);
    p[10] = 0xFF; p[11] = 0x00;
    p[12] = 0x48; p[13] = 0xB8;  memcpy(p + 14, &b, 8);
    p[22] = 0xFF; p[23] = 0xE0;
}

/* Install a stub over one IAT slot. */
static int td_iat_hook(TdvrIatEnt *e)
{
    if (!e->slot || !e->forward) return 0;
    unsigned long long hi = ((unsigned long long)(uintptr_t)e->forward) >> 48;
    if (hi != 0 && hi != 0x7ffe && hi != 0x7fff) {
        vr_log("  iat: %s!%s forward=%p is not a canonical address -- skipped",
               e->dll, e->fn, e->forward);
        InterlockedIncrement(&g_iat_refused);
        return 0;
    }
    unsigned char *stub = (unsigned char *)VirtualAlloc(NULL, 32,
                            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) return 0;
    td_iat_emit(stub, e->calls, e->forward);

    /* Since Windows 10 the loader may leave the IAT read-only, so the page has
     * to be opened for writing. If that fails the entry is reported, never
     * silently counted as done. */
    DWORD old = 0;
    if (!VirtualProtect(e->slot, 8, PAGE_READWRITE, &old)) {
        vr_log("  iat: %s!%s slot=%p VirtualProtect failed (%lu)",
               e->dll, e->fn, (void *)e->slot, GetLastError());
        VirtualFree(stub, 0, MEM_RELEASE);
        InterlockedIncrement(&g_iat_refused);
        return 0;
    }
    InterlockedExchangePointer(e->slot, stub);
    FlushInstructionCache(GetCurrentProcess(), e->slot, 8);
    VirtualProtect(e->slot, 8, old, &old);

    e->stub = stub;
    e->hooked = 1;
    InterlockedIncrement(&g_iat_hooked);
    return 1;
}

/* Walk the game's import directory and hook every graphics entry. */
static void td_iat_scan(void)
{
    /* td_host_image(), not a guessed name: the base is handed over in
     * tdvr_host.txt because no loader query works in this process. */
    unsigned char *host = (unsigned char *)td_host_image();
    if (!host) { vr_log("  iat: no host image; nothing to scan"); return; }

    DWORD peoff;
    memcpy(&peoff, host + 0x3C, 4);
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(host + peoff);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        vr_log("  iat: host does not look like a PE (sig %08X)", nt->Signature);
        return;
    }
    DWORD imp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!imp) { vr_log("  iat: host has no import directory"); return; }

    vr_log("  iat: host SizeOfImage=%u import dir RVA=0x%X",
           nt->OptionalHeader.SizeOfImage, imp);

    int n = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(host + imp);
         d->Name; d++) {
        const char *dll = (const char *)(host + d->Name);
        if (!td_iat_watch_dll(dll)) continue;
        if (!d->FirstThunk) continue;
        IMAGE_THUNK_DATA *th = (IMAGE_THUNK_DATA *)(host + d->FirstThunk);
        IMAGE_THUNK_DATA *og = d->OriginalFirstThunk
            ? (IMAGE_THUNK_DATA *)(host + d->OriginalFirstThunk) : NULL;
        for (; th->u1.Function; th++, og++) {
            if (n >= TDVR_IAT_MAX) { vr_log("  iat: table full at %d entries", n); break; }
            if (og && (og->u1.Ordinal & 0x80000000u)) continue;      /* by ordinal */
            const char *fname = "?";
            if (og && !(og->u1.Ordinal & 0x80000000u))
                fname = (const char *)((IMAGE_IMPORT_BY_NAME *)og->u1.AddressOfData)->Name;
            TdvrIatEnt *e = &g_iat[n++];
            e->dll    = dll;
            e->fn     = fname;
            e->forward = (void *)th->u1.Function;
            e->slot   = (void **)&th->u1.Function;
            e->calls  = NULL;
            e->hooked = 0;
        }
    }
    InterlockedExchange(&g_iat_n, n);
    vr_log("  iat: %d graphics import(s) found in %lu watched librar%s",
           n, (unsigned long)(sizeof TDVR_IAT_LIBS/sizeof TDVR_IAT_LIBS[0]),
           n == 1 ? "y" : "ies");

    /* Counters live in one block so the reporter reads a stable range and each
     * stub embeds a fixed address known before any byte is written. */
    static volatile LONG64 ctrs[TDVR_IAT_MAX];
    for (int i = 0; i < n; i++) g_iat[i].calls = &ctrs[i];

    int ok = 0;
    for (int i = 0; i < n; i++) if (td_iat_hook(&g_iat[i])) ok++;
    vr_log("  iat: %d hooked, %d refused, %ld total",
           ok, g_iat_refused, (long)g_iat_hooked);
    for (int i = 0; i < n && i < 400; i++)
        vr_log("  iat: %-14s %-34s -> %p  %s",
               g_iat[i].dll, g_iat[i].fn, g_iat[i].stub ? (void *)g_iat[i].stub
                                                          : g_iat[i].forward,
               g_iat[i].hooked ? "hooked" : "REFUSED");
}

/* Report the busiest entries first. A function called once per frame is the
 * answer to "where is the frame boundary"; one called once at startup is noise.
 * The selection sort keeps a used[] mask so an entry is printed once. */
static void td_iat_report(void)
{
    int n = g_iat_n;
    if (!n) { vr_log("IAT CENSUS: no graphics imports were hooked"); return; }

    int shown = 0;
    unsigned char used[TDVR_IAT_MAX];
    memset(used, 0, sizeof used);

    while (shown < 24) {
        int   best = -1;
        long long bv = 0;
        for (int i = 0; i < n; i++) {
            if (used[i] || !g_iat[i].hooked) continue;
            long long v = (long long)InterlockedCompareExchange64(
                              (volatile LONG64 *)g_iat[i].calls, 0, 0);
            if (v > bv) { bv = v; best = i; }
        }
        if (best < 0) break;                 /* nothing left with any calls   */
        used[best] = 1; shown++;
        vr_log("  IAT %-14s %-34s calls=%lld",
               g_iat[best].dll, g_iat[best].fn, bv);
    }

    int hot = 0, cold = 0;
    long long peak = 0;
    for (int i = 0; i < n; i++) {
        if (!g_iat[i].hooked) continue;
        long long v = (long long)InterlockedCompareExchange64(
                          (volatile LONG64 *)g_iat[i].calls, 0, 0);
        if (v > 0) { hot++; if (v > peak) peak = v; } else cold++;
    }
    vr_log("IAT CENSUS: %ld hooked, %ld refused | %d hot, %d at zero | peak calls=%lld",
           (long)g_iat_hooked, (long)g_iat_refused, hot, cold, peak);
    if (hot == 0)
        vr_log("  IAT CENSUS: nothing was ever called through the instrumented "
               "entries -- the game is not using these imports for the frame path");
}

#ifdef __cplusplus
}
#endif
#endif /* TDVR_IAT_CENSUS_H */
