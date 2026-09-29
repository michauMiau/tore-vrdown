# Build artifacts

Compiled with mingw-w64 (`x86_64-w64-mingw32-gcc`, present on the TrueNAS container).

## Hook DLL

```bash
x86_64-w64-mingw32-gcc -shared -O2 -o build/teardown_vr.dll hook/teardown_vr.c \
    -lole32 -luser32 -static-libgcc -static-libstdc++
```

Output: `teardown_vr.dll` — PE32+ x86-64, ~223 KB, statically linked runtime so
no MSVC redistributable is needed on the target machine.

### Exports

```
VR_Enable(int)
VR_IsEnabled()
VR_SetIPD(float mm)
VR_HookSwapchain(IDXGISwapChain*)
VR_SetSceneBuffer(void*)
VR_GetFrameCount()
```

`VR_SetSceneBuffer` is the entry point that flips stereo on — until someone
resolves the CPU-side address of the game's `SceneDynamicBuffer` it stays
disabled and the game plays flat (fail-safe, never breaks the game).

**SUPERSEDED 2026-09-27.** `VR_HookSwapchain` and `VR_SetSceneBuffer` do not
exist in `hook/teardown_vr.c`. The actual exports are `VR_Enable`,
`VR_IsEnabled`, `VR_SetIPD`, `VR_GetFrameCount`, `VR_GetRenderer`,
`VR_GetSwapchain`, `VR_IsXRReady`, `VR_GetSceneBuffer`. There is no longer a
"flip stereo on" entry point, because stereo is never written. See
`../ARCHITECTURE.md`.

## Injector (v2)

```bash
x86_64-w64-mingw32-gcc -O2 -Wall -o build/injector.exe injector/injector.c \
    -static-libgcc -static-libstdc++
```

Output: `injector.exe` — PE32+ x86-64, ~251 KB. Uses `CreateRemoteThread` +
`LoadLibraryA`. No launcher dependency, no elevated privileges beyond what the
game process already has.

### Options

```
--game <path>    teardown.exe to launch (default: teardown.exe)
--dll <path>     teardown_vr.dll (default: resolved next to the injector)
--attach         inject into an already-running teardown.exe
--delay <ms>     extra settle time after the window appears (default: 1500)
--nowindow       inject as soon as the process exists (no window wait)
--timeout <ms>   how long to wait for the game (default: 60000)
--noquit         exit immediately instead of waiting for Enter
```

### What v2 fixed

- **Absolute DLL path.** The remote `LoadLibraryA` resolves relative paths
  against the *target's* cwd, not the injector's, so a bare `teardown_vr.dll`
  silently fails. The path is now resolved with `GetFullPathNameA`.
- **No more fixed-delay race.** Instead of `Sleep(delay)`, the injector polls
  for the process to register and then for its top-level window before
  injecting.
- **Launch is detached** (`CREATE_NEW_CONSOLE`) so closing the injector
  console does not take the game down with it.
- **LoadLibraryA return value is read back** — the module handle, or 0 on
  failure. The injector now reports "DLL not found / DllMain failed" instead
  of always claiming success.
- **Clean `-Wall` build**, no warnings.

## Running it

### Normal (one click, injector starts the game)

```batch
copy build\teardown_vr.dll "%~dp0"
build\injector.exe
```

The injector launches Teardown, waits for its window, injects, and exits.
The game keeps running with the mod loaded.

### Into an already-running game

```batch
build\injector.exe --attach
```

It finds `teardown.exe` by name, so there is no need to look up the PID in
Task Manager.

### Under Wine/Proton (how it was tested)

```bash
cp build/injector.exe build/teardown_vr.dll /root/steamless/compat/pfx/drive_c/

/opt/GE-Proton/proton run \
  /root/steamless/compat/pfx/drive_c/injector.exe \
  --game "Z:\\root\\steamless\\Teardown\\teardown.exe" \
  --dll  "C:\\teardown_vr.dll" \
  --timeout 90000 --noquit
```

Verified: the injector launched the game, found PID 368, waited for the
window, injected, and exited 0 while the game kept running.

## Expected log output

Written next to the game executable as `teardown_vr.log`.

```
[VR] === teardown_vr.dll attached ===
[VR] === init thread, pid 368 ===
[VR] no OpenXR loader — flat rendering
[VR] module base 00006ffffa570000
[VR] hooks installed: begin=00006ffffab1b9d0 end=00006ffffab1f270
[VR] ready
```

The two hook addresses must equal `module base + 0x5AB9D0` and
`+ 0x5AF270` for this build. If the base or the addresses differ, the game
was updated and the offsets need re-deriving (see `docs/UNPACKED_ANALYSIS.md`).

The frame counter stays at 0 until the renderer is actually initialised —
i.e. until the splash screen is dismissed and the main menu appears. That
requires a real GPU; under software Vulkan Teardown dies before it gets
there (see `docs/PROTON_TEST.md`).
