// upload_hook.h - RETIRED. The upload-consumer detour is no longer built.
//
// WHAT THIS USED TO BE
//
// A detour on the constant-upload consumer at RVA 0x5BAEB0, so the 0x800-byte
// staging buffer could be read at the moment it was handed to the GPU. It was
// the most promising route to the SceneDynamicBuffer this project ever found,
// and it is gone for a reason worth keeping.
//
// WHY IT IS RETIRED
//
// Byte-stealing. This hook does not patch a vtable entry; it overwrites the
// first bytes of a live function with a jump and replays the stolen bytes from
// a trampoline. v52 froze the game roughly 10-20 s after injection, during a
// level, with every memory read, scan and Present search disabled - the only
// code still patched were the two byte-stealing detours (this one and the SDB
// bind). v53 disabled both and the game kept running, which is what identified
// the mechanism. Full account in docs/ROOT_CAUSE_BYTE_STEALING.md.
//
// The reason byte-stealing is uniquely dangerous here, and why the vtable
// approach replaced it: a vtable entry is a pointer to a real function, so
// replacing it forwards to the original and the game's instruction bytes are
// never touched. A stolen prologue is not. A mis-stolen boundary does not
// produce an error, it silently turns one instruction sequence into a
// different one, and the symptom appears several frames later as a freeze -
// which is what happened, every time, from v38 to v52.
//
// Note what is NOT the reason. The target and its prologue were fine. The
// 15-byte prologue at 0x5BAEB0 is:
//
//     4C 89 4C 24 20   mov  [rsp+0x20], r9
//     53 55 56 57      push rbx / rbp / rsi / rdi
//     41 54            push r12
//     48 83 EC 20      sub  rsp, 0x20
//
// The mechanism is sound and the install-time checks in the deleted function
// were correct (prologue compare, then a check that the stolen length fits the
// patch site). The failure was never "this detour was written wrong". It was
// "patching the instruction stream of a function that is actively running is
// the risk this project cannot afford", because a correct boundary is
// indistinguishable from a wrong one until it faults, and the fault looks like
// a game bug rather than a mod bug.
//
// One real bug did live here, and DETOUR_LEN in teardown_vr.c was fixed because
// of it: the patch site was 14 bytes against a 15-byte prologue, so the detour
// truncated `sub rsp, 0x20` and the render thread took an access violation on
// its first call, logged only as "detached after 1 frames". DETOUR_LEN is 16
// now and the explanation lives at its definition in teardown_vr.c. See also
// docs/DETOUR_FF25_BUG.md.
//
// A SECOND, SEPARATE REASON THIS NEVER PAID OFF
//
// Even while it was installed, the hook was reached exactly ONCE in two minutes
// of a running game. It was never the per-frame uploader, so nothing it
// captured was the frame's constants. The offset was not the problem and the
// detour was not the problem: the function simply was not on the per-frame
// path, which is the same mistake as the 0x9E130 hook documented in
// docs/SDB_BREAKTHROUGH_RESOURCE_TYPE.md. The function it was intercepting
// turned out to sit inside endRender, not in a per-frame upload loop.
//
// WHAT SURVIVES
//
// The upload region is still scanned, by upload_scan.h - a read-only diagnostic
// with no detour and therefore none of the risk above. The constants it uses
// (TD_OFF_CONST_UPLOAD, TD_CONST_UPLOAD_SZ) are defined in teardown_vr.c, and
// what those offsets were MEASURED to contain - which is not a
// SceneDynamicBuffer - is documented at their definition.
//
// The Present hook that replaced this as the live hook is in present.h. It
// patches a vtable slot, so the game's code bytes are never modified.
//
// To bring this detour back, do not simply re-enable the code that was deleted
// from here. Re-derive it against a measured build, and keep the install-time
// checks described above.

#include "upload_scan.h"
