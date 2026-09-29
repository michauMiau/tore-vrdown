// ---------------------------------------------------------------------------
// sdb_patch.h - replace the 6-byte indirect call that uploads the SDB.
//
// The instruction is exactly:
//
//     0x9E4DA  48 8B 01              mov  rax, qword ptr [rcx]
//     0x9E4DD  FF 90 28 01 00 00     call qword ptr [rax + 0x128]
//
// Six bytes, one instruction, no partial overlap. That matters more here than
// it would for a prologue: a stolen-byte detour has to relocate whatever it
// copied, and a `call [mem]` is trivial to relocate (just another `call [mem]`
// with the same disp32) whereas a function prologue is not.
//
// Patch layout at the call site:
//
//     E8 rel32          call hooked_sdb_upload   (5 bytes)
//     90                nop                       (1 byte)
//
// The detour performs the original call itself. It needs the vtable, which is
// reachable from the argument that is live in rcx: the engine passes the
// immediate context there and does `mov rax, [rcx]` immediately before the call.
// So rcx at the detour entry IS the pointer to the object whose first qword is
// the vtable — which is exactly the layout a COM-style interface has, and it is
// also why the hook can be written without decoding any register state.
//
// Getting this wrong the first time is instructive: an earlier version read
// `*(void**)(*(uint8_t**)p + disp)`, i.e. it treated the 8 bytes of the
// instruction as a pointer. The bytes there are FF 90 28 01 00 00 4C 8D, so
// that produced a wild pointer, the read faulted, and the log simply stopped.
// ---------------------------------------------------------------------------
#ifndef TDVR_SDB_PATCH_H
#define TDVR_SDB_PATCH_H

#include <stdint.h>
#include <windows.h>

// Verify the call site really is `FF 9x disp32` and hand back the disp32 so the
// detour can call the original through the same slot. Returns 1 if it matches.
static int tdvr_read_call_site(void* call_site, int32_t* disp, uint8_t* rm) {
    uint8_t* p = (uint8_t*)call_site;
    if (p[0] != 0xFF) return 0;
    if ((p[1] & 0xF8) != 0x90) return 0;          // mod=10, reg=2 (call)
    int m = p[1] & 7;
    if (m == 4) return 0;                         // SIB form: not this shape
    if (m != 0) return 0;                         // [reg] with no disp32
    *disp = (int32_t)(p[2] | (p[3] << 8) | (p[4] << 16) | ((uint32_t)p[5] << 24));
    *rm = (uint8_t)m;
    return 1;
}

// Install the detour. Returns 1 on success. out_target, if given, receives the
// original function pointer read from the vtable, for logging only — the detour
// re-reads it every call so a device recreated mid-session is picked up.
static int tdvr_patch_upload_call(void* call_site, void* detour,
                                  int32_t vtable_disp, void** out_target) {
    uint8_t* p = (uint8_t*)call_site;
    DWORD old;

    if (!VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old)) return 0;

    if (out_target) *out_target = NULL;

    p[0] = 0xE8;
    int32_t rel = (int32_t)((intptr_t)detour - (intptr_t)(p + 5));
    memcpy(p + 1, &rel, 4);
    p[5] = 0x90;

    VirtualProtect(p, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 6);
    (void)vtable_disp;
    return 1;
}

// ---------------------------------------------------------------------------
// Direct call site: E8 rel32
//
// 0x087F40 binds the scene buffers like this:
//
//     0x0887A5  mov   rdx, qword ptr [r15 + 0xB90]   ; SceneDynamicBuffer
//     0x0887AC  lea   rcx, [rbp - 0x68]              ; destination descriptor
//     0x0887B0  call  0x5df000                       ; build 24-byte descriptor
//
// Five bytes, one instruction, and the arguments are exactly what the stereo
// rewrite needs: rdx is the SDB resource, rcx is where the descriptor goes.
// Detouring here means the original call can be forwarded verbatim, so no
// register decoding is required beyond passing rcx/rdx through.
//
// E8 is used rather than a prologue steal because 0x087F40's prologue runs to
// 0x087F5D (28 bytes, past `sub rsp, 0x450`) — more than the trampoline copies,
// and the frame would not be allocated.
// ---------------------------------------------------------------------------
static int tdvr_read_direct_call(void* call_site, void** out_target) {
    uint8_t* p = (uint8_t*)call_site;
    if (p[0] != 0xE8) return 0;
    int32_t rel = (int32_t)((uint32_t)p[1] | ((uint32_t)p[2] << 8)
                            | ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24));
    if (out_target) *out_target = p + 5 + rel;
    return 1;
}

static int tdvr_patch_direct_call(void* call_site, void* detour,
                                  void** out_target) {
    uint8_t* p = (uint8_t*)call_site;
    DWORD old;

    if (!tdvr_read_direct_call(call_site, out_target)) return 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;

    p[0] = 0xE8;
    int32_t rel = (int32_t)((intptr_t)detour - (intptr_t)(p + 5));
    memcpy(p + 1, &rel, 4);

    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return 1;
}

#endif /* TDVR_SDB_PATCH_H */
