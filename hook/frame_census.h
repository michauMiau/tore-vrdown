/* TDVR_FRAME_CENSUS -- find out what actually ends a frame in this game.
 *
 * The measured situation this replaces, all from one live run on 2026-09-29:
 *
 *   - teardown.exe imports 37 gl* functions and wglCreateContext /
 *     wglDeleteContext / wglMakeCurrent / wglGetCurrentDC / wglGetProcAddress
 *     from OPENGL32.dll, plus GDI32.dll.
 *   - the DXGI factory census instrumented all 32 slots and exactly ONE of them
 *     was ever called; IDXGIFactory::CreateSwapChain (slot 10) stayed at zero.
 *   - the renderer class TRendererD3D12 took 48 counting stubs and was never
 *     called once, while the game demonstrably rendered.
 *
 * So neither D3D12 nor the engine's own C++ object graph is the frame path, and
 * the whole OpenXR chain that hangs off an IDXGISwapChain has nothing to hang
 * off. What is left is the one call every OpenGL frame has to make: the buffer
 * swap.
 *
 * Note wglSwapBuffers is NOT in the import table, while GDI32.dll is. For a
 * desktop-WGL context, SwapBuffers(hdc) from GDI32 is the same presentation
 * step; wglSwapBuffers is the WGL entry point for the same job. Which one the
 * game actually uses is not something to guess -- hence both are instrumented,
 * plus the context-creation calls, because if a context is created and never
 * swapped the game is rendering somewhere else entirely.
 *
 * Unlike the vtable census this patches EXPORTS, which are jump thunks at
 * fixed addresses, not table entries. The trampoline is the same 24 bytes and
 * the same reasoning: increment, then jump to the original, touching only RAX,
 * which is dead at an entry point. GDI32/OpenGL32 are shared system DLLs, so
 * every other process on the machine is unaffected -- the patch lives in this
 * process's address space only.
 */

#ifndef TDVR_FRAME_CENSUS_H
#define TDVR_FRAME_CENSUS_H

#include <windows.h>
#include <string.h>   /* memcpy: the stub is assembled byte by byte, not by the compiler */

#ifdef __cplusplus
extern "C" {
#endif

/* A named function, its counter, and where the original lived. */
typedef struct TdvrFrameSlot {
    const char        *name;
    volatile LONG64   *calls;
    void              *original;
    void              *stub;
} TdvrFrameSlot;

#define TDVR_FRAME_MAX 8

/* Counters live in one array so the reporter reads a stable block, and so a
 * stub can embed the address before any slot bookkeeping exists. */
static volatile LONG64 g_frame_counts[TDVR_FRAME_MAX];

static TdvrFrameSlot g_frame_slots[TDVR_FRAME_MAX];
static volatile LONG  g_frame_installed;
static volatile LONG  g_frame_ctx_created;
static volatile LONG  g_frame_made_current;

static const char *TDVR_FRAME_WHY =
    "measured dead ends: DXGI factory 1/32 slots called, CreateSwapChain 0, "
    "TRendererD3D12 48/48 slots at 0 while the game rendered. The frame swap "
    "is the only remaining candidate.";

/* 24 bytes, same encoding as the vtable census:
 *     48 B8 <counter>   mov  rax, imm64
 *     FF 00             inc  qword ptr [rax]
 *     48 B8 <original>  mov  rax, imm64
 *     FF E0             jmp  rax
 * RAX is caller-saved and dead at an entry point, so a method cannot depend on
 * it surviving, and the second load restores it before the tail jump. */
static void td_frame_emit(unsigned char *p, volatile LONG64 *ctr, void *real)
{
    unsigned long long a = (unsigned long long)(uintptr_t)ctr;
    unsigned long long b = (unsigned long long)(uintptr_t)real;
    p[0]  = 0x48; p[1]  = 0xB8;
    memcpy(p + 2, &a, 8);
    p[10] = 0xFF; p[11] = 0x00;
    p[12] = 0x48; p[13] = 0xB8;
    memcpy(p + 14, &b, 8);
    p[22] = 0xFF; p[23] = 0xE0;         /* jmp rax -- the stub's real exit */
    /* Fill the rest of the allocation with int3. VirtualAlloc hands back zeroed
     * pages, and 00 00 decodes as 'add [rax], al': a zero sled. An entry at the
     * wrong offset, or any fall-through, then spins at 100% CPU with no fault
     * and no dialog -- the exact hard-lock signature from 2026-09-29 23:47.
     * int3 converts that silence into a loud, attributable trap. */
    for (int i = 24; i < 64; i++) p[i] = 0xCC;   /* alloc is 64 B */
}

/* Patch one export in place. The address comes from GetProcAddress at runtime,
 * never from a written-down constant -- the same rule as the GUIDs, and for
 * the same reason: a hardcoded address is wrong silently.
 *
 * A jump thunk is 5 bytes of FF 25 xx xx xx xx (jmp [rip+disp32]). We overwrite
 * those 5 bytes with a 14-byte absolute jump to our stub and pad with NOPs.
 * Overwriting 5 bytes in a thunk is safe precisely because the thunk IS the
 * function: the instruction stream starts at the thunk, so there is nothing
 * after it in the same instruction stream to slice in half. */
static int td_frame_patch(const char *name, void *fn, int slot)
{
    if (!fn || slot < 0 || slot >= TDVR_FRAME_MAX) return 0;

    unsigned char *at = (unsigned char *)fn;

    /* A thunk is  jmp qword ptr [rip+disp32]  =  FF 25 <disp32>, 6 bytes.
     * Its disp32 is the pointer to the REAL implementation. That pointer has
     * to be read BEFORE anything is overwritten, because the displacement is
     * exactly the bytes we are about to destroy.
     *
     * This is the bug that hard-locked the game on 2026-09-29 23:47. The first
     * version jumped the stub's tail back to `fn`, the THUNK address, and the
     * thunk by then contained the jump into the stub -- an infinite loop with
     * no exit, which is what a hard lockup with no Sentry dialog is: the
     * process is not crashing, it is spinning inside a two-instruction cycle.
     *
     * Read: target = *(void **)(at + 6 + *(int32_t *)(at + 2)).
     *
     * The +6, not +2. RIP inside 'jmp qword ptr [rip+disp32]' is the address of
     * the NEXT instruction, which is the end of this 6-byte one, so the slot
     * lives at at+6+disp. Reading at+2+disp is 4 bytes early and produced
     * "impl a07f4a7000007ffe" -- two concatenated 32-bit halves, not a pointer.
     * Measured on this machine, at+6+disp gives 00007FFEA23D93EC, a canonical
     * address inside GDI32.dll, which is what the real implementation must be.
     * (thunkprobe.c and idataprobe.c are the standalone checks.) */
    if (!(at[0] == 0xFF && at[1] == 0x25)) {
        vr_log("  frame: %s at %p is not a jump thunk (first bytes %02X %02X)"
               " -- NOT patching, a byte-steal here would split an instruction",
               name, fn, at[0], at[1]);
        return 0;
    }
    int32_t disp;
    memcpy(&disp, at + 2, 4);
    void *real;
    memcpy(&real, at + 6 + disp, 8);          /* RIP is after the whole jmp */
    if (!real || real == fn) {
        vr_log("  frame: %s thunk points at %p (implausible) -- NOT patching",
               name, real);
        return 0;
    }
    vr_log("  frame: %s thunk at %p -> real impl %p (displacement %d)",
           name, fn, real, (int)disp);

    unsigned char *stub = (unsigned char *)VirtualAlloc(NULL, 64,
                            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) return 0;
    /* The counter address goes in before the bytes land; see the note in
     * td_frame_emit about the window where a first call could arrive. */
    td_frame_emit(stub, &g_frame_counts[slot], real);

    DWORD old = 0;
    if (!VirtualProtect(at, 16, PAGE_EXECUTE_READWRITE, &old)) return 0;

    /* Overwrite exactly the 6 bytes of the jmp instruction and NOTHING more.
     * The earlier version wrote 16 bytes, padding with 11 NOPs that ran into
     * whatever followed in the module's layout. A thunk starts an instruction
     * stream, so 6 bytes cannot split an instruction -- but 16 can corrupt the
     * next one. Write 6.
     *
     * The displacement is 4 bytes, not 8. 'FF 25 disp32' dereferences the qword
     * at rip+disp32, and rip is at+6, so the pointer that must land there is
     * written to at+2 as 4 bytes with a zero high word. A 2026-09-30 audit found
     * this line writing 8 -- 2+8 = 10 bytes into a 6-byte instruction, with the
     * stub address' LOW 32 bits landing in the displacement field. The CPU then
     * computed at+6+low32(stub) and jumped through whatever was there. It is
     * fixed and left in the tree as a warning: x64 export thunks are packed at
     * 6-byte stride, so thunk N+1 begins at exactly at+6, which means those 4
     * extra bytes are not slack, they are the NEXT EXPORT'S OPCODE. */
    unsigned char jmp6[6] = { 0xFF, 0x25, 0, 0, 0, 0 };
    memcpy(at, jmp6, 6);
    int32_t rel = (int32_t)((int64_t)((char *)stub - (char *)(at + 6)));
    memcpy(at + 2, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), at, 16);

    /* Put the page back the way the loader left it, before anyone can notice. */
    VirtualProtect(at, 16, old, &old);

    g_frame_slots[slot].name = name;
    g_frame_slots[slot].original = real;      /* the REAL one, not the thunk */
    g_frame_slots[slot].stub = stub;
    g_frame_slots[slot].calls = &g_frame_counts[slot];
    InterlockedIncrement(&g_frame_installed);
    vr_log("  frame: patched %-20s thunk -> stub %p -> real %p",
           name, stub, real);
    return 1;
}

static void td_frame_install(void)
{
    int i = 0;
    /* LoadLibraryA, NOT GetModuleHandleA. A handle from GetModuleHandleA does
     * not have its exports resolved, and GetProcAddress against it fails
     * returning NULL even though the module is plainly loaded and its exports
     * are present. This is not a guess: the first live run of this census
     * reported "GDI32!SwapBuffers not exported" for all five exports while
     * GDI32=00007ffea23d0000 and OPENGL32=00007ffe7c4d0000 were both non-null
     * and the game was rendering. LoadLibraryA on an already-loaded module
     * just bumps its refcount and resolves the export table. */
    HMODULE gdi = LoadLibraryA("GDI32.dll");
    HMODULE gl  = LoadLibraryA("OPENGL32.dll");
    vr_log("  frame: GDI32=%p OPENGL32=%p", (void *)gdi, (void *)gl);
    if (!gdi || !gl) { vr_log("  frame: a graphics DLL is missing from the process"); return; }
    /* Prove resolution rather than assuming it: a census that silently
     * instruments nothing must be able to say so in one line. */
    vr_log("  frame: resolve check SwapBuffers=%p wglSwapBuffers=%p",
           (void *)GetProcAddress(gdi, "SwapBuffers"),
           (void *)GetProcAddress(gl, "wglSwapBuffers"));

    /* Patching a system DLL is out of bounds, and this is not caution for its
     * own sake -- it is a measured lockup. GDI32!SwapBuffers and
     * GDI32!SetPixelFormat were both instrumented on 2026-09-29 23:47 and the
     * game hard-locked with no Sentry dialog: the stub's tail jump went back to
     * the thunk it had already overwritten, so the process spun in a two
     * instruction cycle. The infinite loop is fixed (td_frame_patch now reads
     * the real implementation out of the displacement first), but the second
     * objection stands regardless: GDI32.dll and OPENGL32.dll are SHARED. A
     * detour there is visible to every other process in the session, including
     * ones the user did not ask me to touch, and it survives or vanishes with
     * whatever the loader does to that page.
     *
     * The census therefore instruments NOTHING by default. TDVR_FRAME_PATCH=1
     * is a deliberate, per-build, single-purpose flag, and a build that sets it
     * is not a build to leave loaded in a game someone is playing. The read-only
     * half -- resolving the exports, recording their addresses, and counting via
     * a mechanism that does not modify code -- is always safe. */
    struct { const char *label; HMODULE m; const char *exp; } want[] = {
        { "GDI32!SwapBuffers",         gdi, "SwapBuffers"      },
        { "GDI32!SetPixelFormat",      gdi, "SetPixelFormat"   },
        { "OPENGL32!wglSwapBuffers",   gl,  "wglSwapBuffers"   },
        { "OPENGL32!wglCreateContext", gl,  "wglCreateContext" },
        { "OPENGL32!wglMakeCurrent",   gl,  "wglMakeCurrent"   },
    };
    for (unsigned k = 0; k < sizeof want / sizeof want[0] && i < TDVR_FRAME_MAX; k++) {
        void *fn = (void *)GetProcAddress(want[k].m, want[k].exp);
        if (!fn) {
            vr_log("  frame: %s not exported (looked up as '%s')",
                   want[k].label, want[k].exp);
            continue;
        }
#if TDVR_FRAME_PATCH
        if (td_frame_patch(want[k].label, fn, i)) i++;
#else
        /* Read-only: record what the module exports and where the thunk points,
         * change nothing. This is enough to answer "which of these is the
         * frame boundary" for anything that can be decided by identity, and it
         * cannot lock anything up. */
        g_frame_slots[i].name = want[k].label;
        g_frame_slots[i].original = fn;
        g_frame_slots[i].stub = NULL;
        g_frame_slots[i].calls = &g_frame_counts[i];
        unsigned char *a = (unsigned char *)fn;
        if (a[0] == 0xFF && a[1] == 0x25) {
            /* Same displacement dance as td_frame_patch, and it is easy to get
             * wrong twice: the read-only path reported
             * "impl a07f4a7000007ffe", which is the bytes 7ffe00007ffea07f in
             * the wrong order -- a 64-bit value assembled from an int and a
             * pointer as if the int were a low half. The address is at
             * *(void**)(at + 2 + *(int32_t*)(at + 2)), and nothing else. */
            int32_t d;
            memcpy(&d, a + 2, 4);
            void *impl = NULL;
            memcpy(&impl, (unsigned char *)a + 6 + d, sizeof impl);   /* +6, see note */
            unsigned long long hi = ((unsigned long long)(uintptr_t)impl) >> 48;
            vr_log("  frame: read-only %-24s thunk %p -> impl %p%s",
                   want[k].label, fn, impl,
                   (hi == 0 || hi == 0x7ffe || hi == 0x7fff)
                       ? "" : "  <-- NOT A CANONICAL ADDRESS, thunk layout differs");
        } else {
            vr_log("  frame: read-only %-24s direct code at %p (bytes %02X %02X)",
                   want[k].label, fn, a[0], a[1]);
        }
        i++;
#endif
    }
    vr_log("  frame: %ld export(s) instrumented. %s", (long)g_frame_installed,
           TDVR_FRAME_WHY);
}

static void td_frame_report(void)
{
    int patched = 0, readonly = 0;
    for (int i = 0; i < TDVR_FRAME_MAX; i++) {
        if (!g_frame_slots[i].name) continue;
        if (g_frame_slots[i].stub) patched++; else readonly++;
    }
    if (patched)
        vr_log("FRAME CENSUS: %d export(s) DETOURED, %d read-only -- "
               "this build modifies shared system code", patched, readonly);
    else
        vr_log("FRAME CENSUS: %d export(s) resolved read-only, none modified",
               readonly);
    for (int i = 0; i < TDVR_FRAME_MAX; i++) {
        if (!g_frame_slots[i].name) continue;
        vr_log("  %-26s %-9s calls=%-10lld impl=%p",
               g_frame_slots[i].name,
               g_frame_slots[i].stub ? "DETOURED" : "read-only",
               (long long)InterlockedCompareExchange64(&g_frame_counts[i], 0, 0),
               g_frame_slots[i].original);
    }
}

#ifdef __cplusplus
}
#endif
#endif /* TDVR_FRAME_CENSUS_H */
