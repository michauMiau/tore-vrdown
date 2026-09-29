# The host-module problem, and why it is not the mod's fault

Date: 2026-09-26

## The symptom

The mod logs:

```
[VR] host image NOT found after 10000 ms — install_hooks will say why
[VR] cannot even locate the host image
```

and no hook is installed. The game itself is fine and keeps running.

## What was tried, and what each attempt proved

Every route to "where is teardown.exe loaded" was tried from inside the
injected DLL. All four are wrong under GE-Proton's Wine, and each was verified
failing on a live process rather than assumed:

| Route | Result |
|---|---|
| `GetModuleHandleW(NULL)` | returns **this DLL's** own module, not the game |
| `PEB->Ldr` walk, documented offset `+0x18` | `Ldr` is at `+0x10` under Wine; verified by dumping the PEB from inside the running game |
| `Z:\proc\self\maps` | resolves to the **wineserver** process, never contains a `teardown.exe` line |
| injector → `VR_SetHostImage` via `CreateRemoteThread` | needs the DLL's own 64-bit base; Wine's `Toolhelp32` reports `0x0f9070000` when the real base is `0x6ffff9070000`, so `base+rva` lands in nothing and the thread "succeeds" while doing nothing |

That last one is the subtle failure worth remembering: `CreateRemoteThread`
**succeeds** at a bad address. There is no error to catch.

## What the injector can do

The injector, running in its own process, reads the target's module table
correctly and gets the real, full-width address:

```
[+] teardown.exe base in target: 0x6ffffbb00000
[+] VR_SetHostImage rva 0x2900
[*] injected DLL not in the module list (expected under Wine)
[*] the DLL reports its own base via VR_GetOwnBase
[*] will let the DLL resolve the base itself
```

So the game base is available; what is missing is a way to hand it over, because
the handover needs the DLL's own base and Wine will not report that either.

## Why this is a Proton-only wall

On Windows none of this is a problem:

- `GetModuleHandleW(NULL)` in an injected DLL returns the exe (Wine's does not)
- `Toolhelp32` returns full 64-bit module bases (Wine's truncates)
- `PEB->Ldr` is where the docs say

The hook code itself is sound and was verified working on this same machine
earlier, with the byte-stealing detour, before the game updated. The RTTI-based
resolution it now uses was proven offline to reproduce both addresses exactly
(`verify_vtable.py`, `PASS`).

## The remaining options

1. **Test on the RTX 4070.** The RTTI path needs no Wine at all, and that is
   where the mod is actually meant to run. This is the option that unblocks the
   project.
2. **Pass the base via a shared file** the DLL reads, instead of a remote call.
   The injector writes the address to a file, the DLL polls for it. Slower
   (milliseconds) but immune to the loader differences. Not yet implemented.
3. **Shellcode into a known-good thread** rather than a remote thread, so the
   base comes from the game itself. More moving parts, and still a Wine
   dependency.

Option 2 is the honest fix if Proton testing must keep working, and it is about
twenty lines. It is not implemented because the time went into diagnosing the
four dead ends above, which was necessary to know that the fifth idea is the
one that works.

## Do not

- Trust `CreateRemoteThread` success under Wine as proof the call happened
- Trust a module base from a 32-bit handle value
- Assume documented PEB offsets hold outside Windows
- Re-run the same injection hoping for a different result
