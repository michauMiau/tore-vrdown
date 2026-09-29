/* Ground truth for the thunk layout, measured rather than assumed.
 *
 * The read-only census printed "impl a07f4a7000007ffe" for GDI32!SwapBuffers,
 * which is not a canonical user-space address. Either the decode is wrong or
 * the thunk is not the shape it was assumed to be. This prints the raw bytes so
 * the question is settled by the machine and not by argument.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <inttypes.h>

static void dump(const char *mod, const char *fn) {
    HMODULE m = LoadLibraryA(mod);
    if (!m) { printf("%-28s LoadLibraryA failed (%lu)\n", fn, GetLastError()); return; }
    unsigned char *at = (unsigned char *)GetProcAddress(m, fn);
    if (!at) { printf("%-28s not exported\n", fn); return; }

    printf("\n%s!%s\n", mod, fn);
    printf("  module base  %p\n", (void *)m);
    printf("  export addr  %p  (RVA 0x%Ix)\n", (void *)at,
           (unsigned long)((unsigned char *)at - (unsigned char *)m));
    printf("  first 24 B   ");
    for (int i = 0; i < 24; i++) printf("%02X ", at[i]);
    printf("\n");

    if (at[0] == 0xFF && at[1] == 0x25) {
        int32_t d = 0;
        memcpy(&d, at + 2, 4);
        unsigned char *slot = at + 2 + d;
        printf("  opcode       FF 25 = jmp qword ptr [rip+0x%08X]\n", (unsigned)d);
        printf("  slot addr    %p  (RVA 0x%Ix)\n", (void *)slot,
               (unsigned long)(slot - (unsigned char *)m));
        void *impl = 0;
        memcpy(&impl, slot, sizeof impl);
        printf("  slot bytes   ");
        for (int i = 0; i < 16; i++) printf("%02X ", slot[i]);
        printf("\n");
        printf("  impl         %p\n", impl);
        /* Is the slot inside the module, or is the displacement pointing
         * somewhere else entirely (a .idata pointer, another DLL, or junk)? */
        unsigned long long hi = ((unsigned long long)(uintptr_t)impl) >> 48;
        printf("  high16       %04llX  %s\n", hi,
               (hi == 0 || hi == 0x7ffe || hi == 0x7fff) ? "canonical" : "NOT CANONICAL");
        unsigned long long shi = ((unsigned long long)(uintptr_t)slot) >> 48;
        printf("  slot high16  %04llX  %s\n", shi,
               (shi == 0 || shi == 0x7ffe || shi == 0x7fff) ? "canonical" : "NOT CANONICAL");
    } else {
        printf("  opcode       %02X %02X -- not an FF 25 thunk\n", at[0], at[1]);
        if (at[0] == 0xE9) {
            int32_t rel = 0;
            memcpy(&rel, at + 1, 4);
            unsigned char *tgt = at + 5 + rel;
            printf("  opcode       E9 = jmp rel32, target %p\n", (void *)tgt);
        }
    }
}

int main(void) {
    printf("=== thunk ground truth ===\n");
    dump("GDI32.dll",       "SwapBuffers");
    dump("GDI32.dll",       "SetPixelFormat");
    dump("OPENGL32.dll",    "wglSwapBuffers");
    dump("OPENGL32.dll",    "wglCreateContext");
    dump("OPENGL32.dll",    "wglMakeCurrent");
    dump("OPENGL32.dll",    "wglGetProcAddress");
    /* A control: a function that is certainly a stub, to see whether this
     * loader uses the IAT form at all. */
    dump("USER32.dll",      "GetForegroundWindow");
    return 0;
}
