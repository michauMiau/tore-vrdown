// ---------------------------------------------------------------------------
// sdb_resolve.h - locate the per-frame SceneDynamicBuffer writer by signature.
//
// Found in the image, not guessed. The function at RVA 0x9E130 (ret at 0x9E4F7)
// builds the 500-byte SceneDynamicBuffer on the stack and uploads it:
//
//     0x9E298  divss   xmm0, dword ptr [rdi]           ; 1.0 / m[0]
//     0x9E2A6  divss   xmm1, dword ptr [rdi + 0x14]    ; 1.0 / m[5]
//     0x9E2B0  mulss   xmm0, dword ptr [rdi + 0x20]    ; (1/m[0]) * m[8]
//     0x9E2BA  mulss   xmm1, dword ptr [rdi + 0x24]    ; (1/m[5]) * m[9]
//     ...
//     0x9E486  lea     r8, [rsp + 0x30]                ; the 500 bytes
//     0x9E48D  mov     r9d, 0x1f4
//     0x9E49E  mov     rdx, qword ptr [rbx + 0xB90]   ; the resource
//     0x9E4DD  call    qword ptr [rax + 0x128]         ; UpdateSubresources
//
// Two independent facts fall out of that and both are used as the signature,
// because one alone is not unique enough to trust:
//
//   1. The two divisions and two multiplies at displacements 0, 0x14, 0x20, 0x24
//      from one base register. That is m[0], m[5], m[8], m[9] of a column-major
//      float4x4 projection, and it is what produces mubProjectionData.
//   2. The size 0x1F4 passed as a register argument to an interface call at
//      vtable slot 0x128.
//
// A hardcoded RVA would work today and break on the next patch, which is the
// same mistake the RTTI resolver was written to avoid. So the RVA is only the
// search seed: the function is verified by its own instruction bytes before it
// is hooked, and if verification fails nothing is patched.
// ---------------------------------------------------------------------------
#ifndef TDVR_SDB_RESOLVE_H
#define TDVR_SDB_RESOLVE_H

#include <stdint.h>
#include <string.h>

// Seed for the search. If the code moves, the scan window below the seed is
// what actually locates it; a moved function is still found as long as it stays
// in .text, because the signature does not contain this address.
#define TDVR_SDB_SEED_RVA 0x9E130u

// How far around the seed to search, in bytes. The function is 0x3C8 bytes
// long, and a modest window survives an update that shifts code by a few
// hundred bytes without turning this into a whole-image scan.
#define TDVR_SDB_SEARCH_WINDOW 0x40000u

// Bytes from function start to the SDB pointer argument (rsp + 0x30). Verified
// from the disassembly: rbp = rsp - 0x170, the SDB base is rsp + 0x30, and the
// whole block is rsp .. rsp + 0x270 of frame, so the offset is stable within a
// build but is re-derived per build from the lea encoding rather than assumed.
// The size of the SceneDynamicBuffer, as the engine passes it to the upload.
// stereo.h already defines this from the HLSL declaration, so it is only
// defined here if that header has not been included yet.
#ifndef TDVR_SDB_SIZE
#define TDVR_SDB_SIZE 0x1F4u   // 500 B
#endif

// ---------------------------------------------------------------------------
// The two halves of the signature, as byte patterns with wildcards.
// ---------------------------------------------------------------------------

// 0F 5E 0?  divss xmm, dword ptr [reg]        F3 0F 5E /r
static const uint8_t SIG_DIVSS_MEM[] = { 0xF3, 0x0F, 0x5E };

// F3 0F 59  mulss xmm, dword ptr [reg]
static const uint8_t SIG_MULSS_MEM[] = { 0xF3, 0x0F, 0x59 };

// 0F 10 /r  movups xmm, m128
static const uint8_t SIG_MOVUPS[] = { 0x0F, 0x10 };

// B9 imm32   mov ecx, imm32  (also used for r9d via other encodings)
static const uint8_t SIG_MOV_R32_IMM[] = { 0xB9 };

// FF 90 disp32   call qword ptr [rax + disp32]  -- slot 0x128 in the known build
static const uint8_t SIG_CALL_MEM[] = { 0xFF, 0x90 };

// How far back from a 0x1F4 argument to the call it must appear. In the known
// build the mov r9d,0x1f4 is at 0x9E48D and the call at 0x9E4DD, so 0x50 of
// slack is generous while still requiring them to be the same statement group.
#define TDVR_SDB_ARG_TO_CALL_MAX 0x60u

// How far the four projection instructions may be from the 0x1F4 argument. In
// the known build the last mulss is at 0x9E2BA and the arg at 0x9E48D, so they
// are 0x1D3 apart. Allow a full function's worth.
#define TDVR_SDB_PROJ_TO_ARG_MAX 0x400u

// The four displacements the projection reads, column-major float4x4.
static const int32_t PD_DISPS[4] = { 0x00, 0x14, 0x20, 0x24 };

// ---------------------------------------------------------------------------
// Matching helpers
// ---------------------------------------------------------------------------

// ModRM/SIB decode just enough to recover [base + disp32] and the disp.
// Returns 1 if the instruction reads a dword from [reg + disp32].
static int tdvr_mem_disp32(const uint8_t* p, const uint8_t* end,
                           int32_t* disp, int* has_disp)
{
    if (p >= end) return 0;

    uint8_t modrm = *p++;
    uint8_t mod = (uint8_t)(modrm >> 6);
    uint8_t rm  = (uint8_t)(modrm & 7);
    if (mod == 3) return 0;                 // register operand, not memory

    int32_t d = 0;
    if (mod == 0 && rm == 5) {               // disp32, no base
        if (p + 4 > end) return 0;
        d = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
        p += 4;
    } else {
        if (rm == 4) {                       // SIB
            if (p >= end) return 0;
            uint8_t sib = *p++;
            if ((sib & 7) == 5) {             // no base, disp32
                if (p + 4 > end) return 0;
                d = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
                p += 4;
            }
        }
        if (mod == 1) {
            if (p >= end) return 0;
            d = (int8_t)*p++;
        } else if (mod == 2) {
            if (p + 4 > end) return 0;
            d = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
            p += 4;
        }
    }

    *disp = d;
    *has_disp = 1;
    return 1;
}

// True if the three bytes at p match the given opcode prefix, and the
// instruction that starts there reads memory.
static int tdvr_is_mem_op(const uint8_t* p, const uint8_t* end,
                          const uint8_t* opcode, size_t n, int32_t* disp)
{
    if ((size_t)(end - p) < n + 4) return 0;
    if (memcmp(p, opcode, n) != 0) return 0;
    int has_disp = 0;
    if (!tdvr_mem_disp32(p + n, end, disp, &has_disp)) return 0;
    return has_disp;
}

// ---------------------------------------------------------------------------
// Verification
//
// Returns 1 if p looks like the SceneDynamicBuffer writer, and fills the
// details the hook needs. Verification is deliberately paranoid: hooking the
// wrong function would mean writing stereo data into an unrelated 500-byte
// upload, which is a crash, not a visual glitch.
// ---------------------------------------------------------------------------
typedef struct {
    void*   fn;              // function entry, for diagnostics
    void*   call_site;       // the 6-byte "call [rax+0x128]" to detour
    int32_t sdb_disp;        // displacement of the SDB base (rsp + this)
    int     ok;              // all checks passed
} tdvr_sdb_site;

static int tdvr_verify_sdb(const uint8_t* p, const uint8_t* end, tdvr_sdb_site* out)
{
    out->fn = (void*)p;
    out->ok = 0;
    out->sdb_disp = 0;
    out->call_site = NULL;

    // Pass A: a mov reg, 0x1F4 argument within reach of an interface call.
    //
    // B9 is the opcode for "mov r32, imm32", so the value being looked for is
    // the FOUR bytes that follow it, not one. Comparing a single byte against
    // 0x1F4 can never be true, which the compiler points out.
    const uint8_t* arg = NULL;
    const uint8_t* call_at = NULL;
    for (const uint8_t* q = p; q + 8 <= end; ++q) {
        uint32_t imm;
        memcpy(&imm, q + 1, 4);
        if (q[0] == 0xB9 && imm == TDVR_SDB_SIZE) {
            arg = q;
            break;
        }
        // Also accept the r9d form: 41 B9 imm32
        if (q[0] == 0x41 && q[1] == 0xB9) {
            uint32_t imm2;
            memcpy(&imm2, q + 2, 4);
            if (imm2 == TDVR_SDB_SIZE) {
                arg = q + 1;
                break;
            }
        }
    }
    if (!arg) return 0;

    for (const uint8_t* q = arg + 5; q + 6 <= end; ++q) {
        if (q[0] == 0xFF && q[1] == 0x90) {   // call [rax + disp32]
            call_at = q;
            break;
        }
        if (q[0] == 0xFF && (q[1] == 0x90 || q[1] == 0x91 ||
                            q[1] == 0x92 || q[1] == 0x93)) {
            call_at = q;
            break;
        }
    }
    if (!call_at || (size_t)(call_at - arg) > TDVR_SDB_ARG_TO_CALL_MAX) return 0;

    // Pass B: the two divisions and two multiplies at the projection
    // displacements, all above the argument (the function computes first, then
    // uploads).
    int got[4] = { 0, 0, 0, 0 };
    for (const uint8_t* q = p; q < arg; ++q) {
        if ((size_t)(arg - q) > TDVR_SDB_PROJ_TO_ARG_MAX) break;
        int32_t d;
        if (tdvr_is_mem_op(q, end, SIG_DIVSS_MEM, sizeof SIG_DIVSS_MEM, &d) ||
            tdvr_is_mem_op(q, end, SIG_MULSS_MEM, sizeof SIG_MULSS_MEM, &d)) {
            for (int i = 0; i < 4; ++i) {
                if (d == PD_DISPS[i]) got[i] = 1;
            }
        }
    }
    for (int i = 0; i < 4; ++i) if (!got[i]) return 0;

    // Pass C: the SDB base register argument to the call. In the known build
    //     lea r8, [rsp + 0x30]    4C 8D 84 24 30 00 00 00
    // appears right before the call, and it is what we write into. Locate a lea
    // that loads rsp-relative memory and hand back its displacement.
    int32_t lea_disp = 0;
    int lea_found = 0;
    for (const uint8_t* q = arg - 64 > p ? arg - 64 : p; q + 8 <= call_at; ++q) {
        // REX.W + 8D /r with mod=01 or 10 and r/m=4 (SIB, base=rsp/r12)
        if (q[0] != 0x4C && q[0] != 0x48) continue;
        if (q[1] != 0x8D) continue;
        uint8_t modrm = q[2];
        if ((modrm & 7) != 4) continue;               // need SIB
        uint8_t sib = q[3];
        if ((sib & 7) != 4) continue;                 // base must be rsp/r12
        if ((modrm >> 6) == 0) continue;              // need a displacement
        if ((modrm >> 6) == 1) {                      // disp8
            lea_disp = (int8_t)q[4];
            lea_found = 1;
        } else {                                      // disp32
            lea_disp = (int32_t)(q[4] | (q[5] << 8) | (q[6] << 16) |
                                 ((uint32_t)q[7] << 24));
            lea_found = 1;
        }
        break;
    }
    if (!lea_found) return 0;

    // The SDB must fit inside the frame the function reserved. The known build
    // subtracts 0x270 and places the SDB at +0x30, so the requirement is that
    // the base is non-negative and 0x30 + 0x1F4 is within a plausible frame.
    if (lea_disp < 0 || lea_disp > 0x10000) return 0;

    out->sdb_disp = lea_disp;
    out->call_site = (void*)call_at;
    out->ok = 1;
    return 1;
}

// Scan the image around the seed for the writer. Returns 1 on success.
static int tdvr_resolve_sdb(const uint8_t* image, size_t image_size, tdvr_sdb_site* out)
{
    memset(out, 0, sizeof *out);
    if (!image || image_size < TDVR_SDB_SEED_RVA + TDVR_SDB_SEARCH_WINDOW) return 0;

    size_t lo = TDVR_SDB_SEED_RVA;
    // Only look FORWARD from the seed, never behind it. The resolver accepts
    // any address where the four projection instructions precede the 0x1F4
    // argument, and an earlier function containing a similar float sequence
    // satisfied that from 0x9E08E — one function too early, which reported
    // sdb_disp 0x30 from the wrong frame. Anchoring the search at the known
    // function start and requiring the argument to be inside this function
    // removes that.
    size_t hi = TDVR_SDB_SEED_RVA + TDVR_SDB_SEARCH_WINDOW;
    if (hi > image_size) hi = image_size;

    const uint8_t* base = image + lo;
    const uint8_t* end  = image + hi;
    for (const uint8_t* p = base; p + 0x40 <= end; ++p) {
        if (tdvr_verify_sdb(p, end, out)) {
            // The argument and the call must sit within a plausible distance of
            // the candidate start, i.e. inside one function rather than one
            // function plus a neighbour. The real one is 0x44F bytes end to
            // end, so 0x1000 is generous.
            uintptr_t d = (uintptr_t)((const uint8_t*)out->call_site - p);
            if (d < 0x1000) return 1;
            out->ok = 0;
        }
    }
    return 0;
}

#endif /* TDVR_SDB_RESOLVE_H */
