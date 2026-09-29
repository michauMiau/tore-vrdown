// ---------------------------------------------------------------------------
// scene_bind_find.h - locate the per-frame SceneDynamicBuffer bind.
//
// Split from scene_bind.h so it can be compiled and tested on the analysis
// machine. It is pure byte matching: no Windows API, no globals, no side
// effects. scene_bind.h includes it and adds nothing.
//
// Why the hook goes here is the whole point, so it is worth stating plainly.
//
// 0x9E130 was hooked on the strength of a perfect signature match: it builds
// 500 bytes with the layout from the HLSL declaration and calls
// UpdateSubresources with size 0x1F4. Two live hooks then showed it is never
// called per frame — 0 hits over 11760 frames. Hooking the wrong function
// produced a clean build, a healthy 144 FPS process, and no stereo, which is
// the hardest kind of bug to see.
//
// 0x087F40 is the function that actually runs. It is 7725 bytes, 178 direct
// calls, reached from the game loop through 0x070B20 <- 0x071CE0, and it is
// the only function in the entire 33 MB image that reads renderer+0xB90. The
// three scene buffers become 24-byte descriptors there:
//
//     0x088795  mov rdx,[r15+0xB88] ; lea rcx,[rbp-0x80] ; call 0x5df000
//     0x0887A5  mov rdx,[r15+0xB90] ; lea rcx,[rbp-0x68] ; call 0x5df000
//     0x0887B5  mov rdx,[r15+0xB98] ; lea rcx,[rbp-0x50] ; call 0x5df000
//
// 0x5df000 is not a D3D12 call. It stores the resource pointer and reads a type
// tag from resource->vtable[0] (4 = texture, 8 = buffer), so the engine has its
// own resource abstraction and +0xB90 is not an ID3D12Resource*. That is why
// searching for ID3D12 field reads never found it, and why the upload cannot be
// found by looking for UpdateSubresources on that field.
//
// The pattern is matched on the field offset 0xB90, not just the instruction
// shape. The three siblings are byte-identical apart from the disp32 and sit
// 16 bytes apart, so a shape-only matcher would hook +0xB88 or +0xB98 and the
// SDB would silently go untouched.
// ---------------------------------------------------------------------------
#ifndef TDVR_SCENE_BIND_FIND_H
#define TDVR_SCENE_BIND_FIND_H

#include <stdint.h>

// REFUTED 2026-09-27: renderer+0xB90 is NOT the SceneDynamicBuffer.
//
// Disassembly says it is a single flag byte inside a contiguous flag block that
// the object's copy constructor moves one byte at a time:
//
//     00040324  movzx eax, byte ptr [rdi + 0xB90]
//     0004032B  mov    byte ptr [rbx + 0xB90], al
//
// running 0xB74..0xB93, then dwords at 0xB94/0xB98/0xB9C. The neighbouring
// 0xBA0 is a plain owned pointer inside a twelve-element array that
// 0x82350..0x8239D zeroes and 0x84580..0x845CB destroys. Neither is the SDB.
//
// The old belief came from the string "Renderer::SceneDynamicBuffer" at rva
// 0x99FA82 and one lea reference. That proves a constructor is called, not
// where the result is stored.
//
// The matcher below is kept because it is a correct, tested byte-pattern
// scanner, and it is still the right tool for whatever field is real. What is
// banned is the constant: 0xB90 is hard-coded to the value it was measured at,
// and that value is a flag byte. A different field needs its own measurement,
// not a different guess.
//
// See docs/SDB_OFFSET_CORRECTION.md and docs/UPLOAD_CHAIN_TRUTH.md.
#define TDVR_SDB_FIELD  0xB90u

// Locate `call 0x5df000` that builds the descriptor for renderer+0xB90.
// Returns the address of the E8 byte, or NULL. out_desc_ctor, if given,
// receives the constructor the call targets.
static uint8_t* tdvr_find_sdb_bind(uint8_t* image, size_t size,
                                   void** out_desc_ctor) {
    for (size_t i = 0; i + 16 < size; ++i) {
        // mov rdx, [r15+0xB90]  =  49 8B 97 90 0B 00 00
        //
        // The REX byte is 0x49 (W+R), not 0x4C: r15 needs the R bit because its
        // low three bits are all ones. Matching only 0x48/0x4C is what made the
        // first live attempt miss the one site that mattered.
        if ((image[i] & 0xF8) != 0x48) continue;       // REX present, 0100WRXB
        if (!(image[i] & 0x08)) continue;                // require REX.W (bit 3)
        if (image[i + 1] != 0x8B) continue;
        // mod=10, reg=rdx (2), rm=rdi (7) -> modrm = 10 010 111 = 0x97
        //
        // Compare modrm exactly. Masking with 0xC7 and comparing the masked value
        // against a value that still has the reg bits set can never be true —
        // that is the bug this test was written to catch, and the first live
        // attempt's "pattern not found" was the same class of mistake.
        if (image[i + 2] != 0x97) continue;
        int32_t disp = (int32_t)((uint32_t)image[i + 3] | ((uint32_t)image[i + 4] << 8)
                                 | ((uint32_t)image[i + 5] << 16) | ((uint32_t)image[i + 6] << 24));
        if (disp != (int32_t)TDVR_SDB_FIELD) continue;

        // lea rcx, [rbp+disp8]  =  48 8D 4D xx   — four bytes, REX + opcode +
        // modrm + disp8. The call is therefore at j+4, not j+3; reading j+3
        // picks up the displacement byte and never sees the E8.
        size_t j = i + 7;
        if ((image[j] & 0xF8) != 0x48) continue;
        if (image[j + 1] != 0x8D) continue;
        if (image[j + 2] != 0x4D) continue;              // mod=01, reg=rcx, base=rbp
        if (j + 4 >= size) continue;
        size_t c = j + 4;

        // call rel32
        if (image[c] != 0xE8) continue;
        int32_t rel = (int32_t)((uint32_t)image[c + 1] | ((uint32_t)image[c + 2] << 8)
                                | ((uint32_t)image[c + 3] << 16) | ((uint32_t)image[c + 4] << 24));
        uint8_t* target = image + c + 5 + rel;
        if (target < image || target + 3 > image + size) continue;

        // The target must look like the descriptor constructor:
        // `mov [rcx], rdx` is 48 89 11.
        if (target[0] != 0x48 || target[1] != 0x89 || target[2] != 0x11) continue;

        if (out_desc_ctor) *out_desc_ctor = target;
        return image + c;
    }
    return NULL;
}

#endif /* TDVR_SCENE_BIND_FIND_H */
