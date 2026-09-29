/* Why the FF 25 slot does not contain a pointer.
 *
 * Measured: GDI32!SwapBuffers is
 *   FF 25 7A DE 01 00   jmp qword ptr [rip+0x1DE7A]   -> slot RVA 0x2725C
 * and the 8 bytes at that slot are  FE 7F 00 00 EC 93 3D A2
 * whose high half is 0000 7FFE, i.e. NOT an address in this process.
 *
 * The 0x7FFE high half is the giveaway: that is a selector/segment value, which
 * means the slot is not a plain pointer but part of a structure. Compare with
 * the layout this same probe read for a second GDI32 export and look at the
 * NAME table: an FF 25 thunk normally points at an 8-byte pointer that lives in
 * the module's .idata, and the module's own base should appear in the low half.
 *
 * Rather than reason about it, print the surrounding .idata region with its
 * interpretation, and separately ask the loader itself: what does
 * GetProcAddress return, and does the module's export directory say the same
 * thing? If the slot is not a pointer, the only safe conclusion is that a
 * 5-or-6 byte detour at that address cannot forward to a "real" address read
 * from there, and the census must not attempt one.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

static void region(const char *mod, const char *fn, int before, int after) {
    HMODULE m = LoadLibraryA(mod);
    unsigned char *at = (unsigned char *)GetProcAddress(m, fn);
    int32_t d = 0;
    memcpy(&d, at + 2, 4);
    unsigned char *slot = at + 6 + d;   /* RIP = address AFTER the 6-byte jmp */
    unsigned char *base = (unsigned char *)m;
    printf("\n%s!%s  slot=%p  RVA 0x%Ix\n", mod, fn, (void *)slot,
           (unsigned long)(slot - base));
    for (int off = -before; off < after; off += 8) {
        unsigned char *q = slot + off;
        if (q < base || q > base + 0x400000) continue;
        uint64_t v = 0;
        memcpy(&v, q, 8);
        printf("  %+4d  %p  %016llX  %s\n", off, (void *)q,
               (unsigned long long)v,
               (v >> 48) == 0x7ffe ? "<- 0x7FFE high half" :
               (v >> 48) == 0x7fff ? "<- 0x7FFF high half" :
               (v && v < 0x100000000ull) ? "<- small (RVA or ordinal)" : "");
    }
    /* Ask the PE what the export directory says, independently of GetProcAddress. */
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + *(uint32_t *)(base + 0x3C));
    DWORD ed = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    if (ed) {
        IMAGE_EXPORT_DIRECTORY *e = (IMAGE_EXPORT_DIRECTORY *)(base + ed);
        printf("  export dir: %u names, base ordinal %u, name RVA 0x%X\n",
               e->NumberOfNames, e->Base, e->AddressOfNames);
    }
    /* And what does the module's OWN import thunks look like? If the 0x7FFE
     * pattern is normal here, it is a deliberate encoding, not corruption. */
    DWORD id = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (id) {
        IMAGE_IMPORT_DESCRIPTOR *d2 = (IMAGE_IMPORT_DESCRIPTOR *)(base + id);
        printf("  imports:");
        for (; d2->Name; d2++) printf(" %s", (char *)(base + d2->Name));
        printf("\n");
    }
}

int main(void) {
    printf("=== idata layout around the FF 25 slots ===\n");
    region("GDI32.dll", "SwapBuffers", 32, 64);
    region("GDI32.dll", "SetPixelFormat", 32, 64);
    return 0;
}
