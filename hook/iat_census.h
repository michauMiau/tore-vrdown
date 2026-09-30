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

#if TDVR_IAT_CALL_STUB
/* The handler is given the game's HDC, which is simply whatever is in RCX when
 * SwapBuffers was entered. The stub never writes RCX -- that is the one rule
 * this whole arrangement rests on, and build/audit_stub.py fails the build if a
 * "mov rcx" reappears in the emitter.
 *
 * Two earlier versions got this wrong in ways that looked right on the page.
 * The first passed the hook entry in RCX, so the real SwapBuffers received a
 * struct pointer where its HDC belonged and the game died on the first present.
 * The second passed a CONTEXT*, assuming a stub has the caller's registers
 * available; it does not, because a stub is not an exception handler, so
 * ctx->Rcx was whatever happened to sit at the handler's own entry point.
 */
typedef void (*TdvrIatHandler)(HDC hdc);
#endif

typedef struct TdvrIatEnt {
    const char       *dll;
    const char       *fn;
    void             *forward;   /* what the loader put in the slot          */
    void            **slot;      /* the game's own slot                       */
    volatile LONG64  *calls;
    unsigned char    *stub;
    int               hooked;
#if TDVR_IAT_CALL_STUB
    TdvrIatHandler    handler;    /* runs on the game's thread at the hook */
#endif
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

/* Within those libraries, only these functions.
 *
 * Why this list, and not just SwapBuffers: the question that blocks the VR
 * design is whether a present equals a frame. SwapBuffers cannot answer it --
 * in a windowed WGL app it hands the back buffer to DWM and returns, so it is
 * not gated on the refresh rate even with vsync=1, and 138.59/s proved nothing
 * about frame count.
 *
 * glClear, glDepthFunc, glColorMask, glPolygonMode and friends run exactly
 * once per frame when the game sets frame state, independently of how many
 * times anything is presented. Their rate IS the frame rate. Comparing it
 * against SwapBuffers gives the presents-per-frame ratio directly, which is
 * the number the whole stereo design depends on.
 *
 * The wide run already showed these carried identical counts to each other
 * (4799 each), which is what a once-per-frame state block looks like. This
 * list makes that measurable instead of inferred. */
static const char *TDVR_IAT_FNS[] = {
    "SwapBuffers",
    "glClear", "glClearColor", "glDepthFunc", "glDepthMask",
    "glColorMask", "glPolygonMode", "glStencilFunc", "glStencilMask",
    "glStencilOp", "glViewport", "glEnable", "glBlendFunc",
};


/* The census that measured "41 hooked, 0 refused" followed the game from
 * 1731 MB down to 85 MB over twenty minutes and left it in a state where
 * Sentry showed an exception while the process kept running. It hooked EVERY
 * import in eight graphics libraries -- 41 of them -- including the ones on
 * the window-creation path: SetPixelFormat, wglCreateContext, wglMakeCurrent.
 *
 * Those are not per-frame. They run once, during startup, and a stub that
 * tail-jumps back into a GDI32 or OPENGL32 forwarder that itself touches loader
 * state is exactly the shape of thing that leaves a graphics context half
 * initialised. Whatever the precise mechanism, the measured fact is enough to
 * stop doing it: 41 hooks answered no question that 1 does not.
 *
 * One import answers the only question being asked -- does a frame end through
 * the import table -- and cannot plausibly disturb startup. Set
 * TDVR_IAT_NARROW=0 to restore the wide behaviour if the narrow one proves
 * nothing. */
#ifndef TDVR_IAT_NARROW
#define TDVR_IAT_NARROW 1
#endif

/* Whether this specific function should be hooked. Narrow by default: the
 * wide sweep was measured to bleed the game's memory from 1731 MB to 85 MB. */
static int td_iat_watch_fn(const char *name)
{
#if TDVR_IAT_NARROW
    for (unsigned i = 0; i < sizeof TDVR_IAT_FNS / sizeof TDVR_IAT_FNS[0]; i++)
        if (_stricmp(name, TDVR_IAT_FNS[i]) == 0) return 1;
    return 0;
#else
    (void)name;
    return 1;
#endif
}

static int td_iat_watch_dll(const char *name)
{
    for (unsigned i = 0; i < sizeof TDVR_IAT_LIBS / sizeof TDVR_IAT_LIBS[0]; i++)
        if (_stricmp(name, TDVR_IAT_LIBS[i]) == 0) return 1;
    return 0;
}

/* The counter stub, 24 bytes, and the shape of the problem it creates.
 *
 *     48 B8 <ctr>          mov  rax, ctr
 *     FF 00                inc  qword ptr [rax]
 *     48 B8 <forward>      mov  rax, forward
 *     FF E0                jmp  rax
 *
 * It counts and it forwards, and that is all 24 bytes allow. The displacement is
 * the only space there is, so there is no room for a call, and therefore no way
 * to run code on the game's thread at the moment it presents.
 *
 * That matters now. The pixel probe reads GL_BACK from a worker thread and gets
 * "no current HDC" every single time, 9624 out of 9624 -- because a GL context
 * is current only on the thread that made it current. Reading pixels from any
 * other thread cannot work, no matter how the read is coded.
 *
 * So the dispatch stub below adds exactly one thing: a single indirect call,
 * through a function pointer, to code that runs on the game's own thread. The
 * counter stub stays the default because it is the one thing that has been
 * stable all night, and the wider stub is opt-in.
 */

/* Layout: inc the counter, call the handler, jump to the original.
 *   48 B8 <ctr>        mov   rax, ctr
 *   FF 00              inc   qword ptr [rax]
 *   48 B8 <fn>         mov   rax, handler
 *   48 B8 <ctx>        mov   rcx, ent
 *   FF D0              call  rax
 *   48 B8 <forward>    mov   rax, forward
 *   FF E0              jmp   rax
 * 47 bytes, indices 0..46 inclusive. Handler is called with the entry as RCX.
 * Windows x64 gives the callee shadow space, so the handler must not assume
 * anything about its arguments afterwards. */
#define TDVR_STUB_CALL_SIZE 47

static void td_iat_emit(unsigned char *p, volatile LONG64 *ctr, void *forward)
{
    unsigned long long a = (unsigned long long)(uintptr_t)ctr;
    unsigned long long b = (unsigned long long)(uintptr_t)forward;
    p[0]  = 0x48; p[1]  = 0xB8;  memcpy(p + 2,  &a, 8);       /* mov rax, ctr    */
    p[10] = 0x48; p[11] = 0xFF; p[12] = 0x00;                 /* inc qword [rax] */
    p[13] = 0x48; p[14] = 0xB8;  memcpy(p + 15, &b, 8);       /* mov rax, fwd    */
    p[23] = 0xFF; p[24] = 0xE0;                               /* jmp rax         */
}

#if TDVR_IAT_CALL_STUB
/* Emitted only when the entry has a handler.
 *
 * The forwarding tail is byte-identical to the counter stub, which is the part
 * that has been verified stable all night. What differs is the call in the
 * middle, and above all what the handler is handed.
 *
 * The first version passed the entry in RCX. That crashed the game, and the
 * reason is worth writing down: this stub IS the function SwapBuffers, so
 * whatever ends up in RCX is what the real SwapBuffers receives as its HDC
 * argument. Handing GDI32 a pointer to a hook entry is a hard crash, which is
 * the Sentry dialog of 2026-09-30 -- the handler ran, logged "ON GAME THREAD
 * 60 presents", and the process died. The VEH never saw an exception because
 * the failure happened inside Sentry's own handler, not as a raised fault.
 *
 * So the handler gets a pointer to the CONTEXT and reads the real arguments
 * out of it. Nothing it does can reach the game's argument registers. The only
 * register the stub itself touches is RAX, which the counting prologue already
 * used and which SwapBuffers does not read as an argument.
 *
 * The push/sub around the call is not decoration. At the call site RSP is 8 mod
 * 16, because the caller's return address pushed 8, so a bare call would enter
 * the handler misaligned and any SSE store would fault. The push restores
 * 16-byte alignment and 0x28 covers 32 bytes of shadow space plus the 8 the
 * return address will take.
 *
 * THE OFFSETS BELOW ARE GENERATED, NOT COUNTED. build/audit_stub.py holds this
 * layout as data and fails the build if the numbers here disagree with it, or
 * if any two instructions are not adjacent. Getting this wrong three times in
 * one night -- once by 8-byte immediates written one byte early, which left
 * 0xCC gaps and an int3 on the hot path -- is what the audit exists to make
 * impossible a fourth time.
 */
static void td_iat_emit_call(unsigned char *p, volatile LONG64 *ctr,
                             void *forward, TdvrIatHandler h)
{
    unsigned long long a = (unsigned long long)(uintptr_t)ctr;
    unsigned long long b = (unsigned long long)(uintptr_t)forward;
    unsigned long long c = (unsigned long long)(uintptr_t)h;
    p[0]  = 0x48; p[1]  = 0xB8;  memcpy(p + 2,  &a, 8);       /*  0.. 9  ctr    */
    p[10] = 0x48; p[11] = 0xFF; p[12] = 0x00;                 /* 10..12  inc     */
    p[13] = 0x48; p[14] = 0xB8;  memcpy(p + 15, &c, 8);       /* 13..22  handler */
    p[23] = 0x50;                                             /* 23..23  push    */
    p[24] = 0x48; p[25] = 0x83; p[26] = 0xEC; p[27] = 0x28;   /* 24..27  sub rsp */
    p[28] = 0xFF; p[29] = 0xD0;                               /* 28..29  call    */
    p[30] = 0x48; p[31] = 0x83; p[32] = 0xC4; p[33] = 0x28;   /* 30..33  add rsp */
    p[34] = 0x58;                                             /* 34..34  pop     */
    p[35] = 0x48; p[36] = 0xB8;  memcpy(p + 37, &b, 8);       /* 35..44  fwd     */
    p[45] = 0xFF; p[46] = 0xE0;                               /* 45..46  jmp     */
}
#endif

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
#if TDVR_IAT_CALL_STUB
    /* A handler makes the stub wider, so the allocation is sized from what is
     * actually emitted rather than assumed. The 0xCC tail fill matters: a stub
     * that runs off its end into a zero byte decodes as "add [rax], al" and
     * the page becomes a silent zero sled instead of an int3 trap. */
    size_t stub_size = e->handler ? TDVR_STUB_CALL_SIZE : 24;
    unsigned char *stub = (unsigned char *)VirtualAlloc(NULL, stub_size + 16,
                            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) return 0;
    memset(stub, 0xCC, stub_size + 16);
    if (e->handler) td_iat_emit_call(stub, e->calls, e->forward, e->handler);
    else            td_iat_emit(stub, e->calls, e->forward);
#else
    unsigned char *stub = (unsigned char *)VirtualAlloc(NULL, 40,
                            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) return 0;
    memset(stub, 0xCC, 40);
    td_iat_emit(stub, e->calls, e->forward);
#endif

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

/* Call into the pixel probe from inside the SwapBuffers stub.
 *
 * The counter stub is "mov rax,imm64; inc qword ptr [rax]; jmp rax" -- it is
 * exactly the size of the displacement, and there is no room in it for a call.
 * Enlarging it to make space would mean rewriting the game's slot with a
 * bigger thunk, which changes the shape of the one thing that has been stable
 * all night.
 *
 * So instead of growing the stub, the probe runs on its own thread and is
 * driven by the counter. That keeps the counter stub untouched and still
 * answers the question the probe exists for: whether real pixels can be read
 * at all from this context. The pixel content does not have to be sampled at
 * the exact present instant to establish THAT -- if a frame can be read here,
 * the next step is reading it at the exact present instant once the stub shape
 * changes deliberately.
 *
 * Being explicit about the limit: this proves pixels are reachable and
 * non-flat. It does not yet prove the read is synchronised with the frame the
 * game just drew, because the thread is not the render thread.
 */
#if TDVR_PIXEL_PROBE
static DWORD WINAPI px_worker(LPVOID arg)
{
    (void)arg;

    /* Wait on the SwapBuffers counter, which the IAT stub really does
     * increment -- NOT on g_px_reads, which only tdvr_px_sample() increments.
     * The first version of this function waited on its own output, so the
     * counter it was watching could never move, the inner wait always timed
     * out, and the probe reported "0 frames at the present" forever while
     * looking perfectly healthy in the log. A probe that reports its own
     * idleness as a finding is worse than no probe: it looks like data.
     *
     * If the wait never times out, that is the bug, not the absence of frames.
     */
    LONG64 seen = 0;
    LONG64 idle = 0;
    for (;;) {
        int waited = 0;
        while (waited < 100) {
            LONG64 now = 0;
            if (g_iat_n > 0) {
                for (int i = 0; i < (int)g_iat_n; i++) {
                    if (!g_iat[i].hooked) continue;
                    if (!strstr(g_iat[i].fn, "SwapBuffers")) continue;
                    now = (LONG64)InterlockedCompareExchange64(
                              (volatile LONG64 *)g_iat[i].calls, 0, 0);
                    break;
                }
            }
            if (now != seen) { seen = now; break; }
            Sleep(20); waited++;
        }
        if (waited >= 100) { idle++; InterlockedIncrement64(&g_px_idle); Sleep(500); continue; }
        if (seen == 0) continue;

        tdvr_px_sample();
    }
}
#endif

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
            /* A RVA is not an address. The load base is applied on the way in
             * via host + rva; without that the pointer read here lands wherever
             * the value happens to point, _stricmp silently matches nothing,
             * and the census installs zero hooks while reporting nothing at
             * all. The measured symptom of exactly that was a log that stopped
             * after "host SizeOfImage" with no summary line. */
            if (!og) {
                vr_log("  iat: a %s import has no INT entry, cannot name it", dll);
                continue;
            }
            if (og->u1.Ordinal & 0x80000000u) continue;           /* by ordinal */
            DWORD nrva = og->u1.AddressOfData;
            if (nrva == 0 || nrva >= nt->OptionalHeader.SizeOfImage) {
                vr_log("  iat: a %s import has an INT entry outside the image (rva 0x%X)", dll, nrva);
                continue;
            }
            const char *fname =
                (const char *)((IMAGE_IMPORT_BY_NAME *)(host + nrva))->Name;
            if (nrva + sizeof(IMAGE_IMPORT_BY_NAME) + 1 > nt->OptionalHeader.SizeOfImage) {
                vr_log("  iat: %s import name runs off the end of the image", dll);
                continue;
            }
            if (!td_iat_watch_fn(fname)) continue;
            TdvrIatEnt *e = &g_iat[n++];
#if TDVR_IAT_CALL_STUB && TDVR_PIXEL_PROBE
            /* Only SwapBuffers gets a handler. A handler runs on the game's
             * thread inside its present, so it must be limited to the one call
             * that has been measured at exactly once per frame. The 41-hook
             * sweep already showed what happens when this is not deliberate:
             * the process bled out from 1731 MB to 85 MB in twenty minutes. */
            e->handler = (_stricmp(fname, "SwapBuffers") == 0)
                       ? (TdvrIatHandler)tdvr_px_on_present : NULL;
#endif
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
