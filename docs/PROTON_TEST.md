# Proton test run — measured results

Date: 2026-09-26
Host: TrueNAS container, Debian 13, x86_64, **no GPU** (`/dev/dri` absent)
Display: Xvfb `:99`, 1280x720x24
Vulkan: lavapipe — `deviceType = PHYSICAL_DEVICE_TYPE_CPU`, `llvmpipe (LLVM 19.1.7)`

## Setup that worked

```bash
# x86_64 GE-Proton (the aarch64 build fails with Exec format error)
wget https://github.com/GloriousEggroll/proton-ge-custom/releases/download/\
GE-Proton11-7/GE-Proton11-7-x86_64.tar.gz
tar -xzf GE-Proton11-7-x86_64.tar.gz -C /opt/GE-Proton --strip-components=1

# 32-bit loader is mandatory or wine dies with
#   /lib/ld-linux.so.2: could not open
dpkg --add-architecture i386
apt-get install -y libc6:i386 lib32gcc-s1 libfreetype6 wine32:i386

# Steamless ships binaries only — data/ dlcs/ mods/ must be copied in
cp -r <full install>/{data,dlcs,mods} /root/steamless/Teardown/

# Run
DISPLAY=:99 \
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
STEAM_COMPAT_DATA_PATH=/root/steamless/compat \
STEAM_COMPAT_CLIENT_INSTALL_PATH=/root/steamless \
STEAM_COMPAT_APP_ID=1167630 \
/opt/GE-Proton/proton run teardown.exe
```

## What works

The game **runs and renders** under Proton + lavapipe. It sits on the Tuxedo
Labs warning splash ("Press any key to continue", Copyright 2020-2026 Tuxedo
Labs AB) indefinitely — verified alive for **370+ seconds** with no injection.

The intro path works: exe loading, asset mount, Lua layer, and the initial
render target are all fine.

## What does not work

Pressing a key to advance past the splash kills the process (`exit_code 1`,
~85 s uptime, no diagnostic in the log beyond ALSA noise). Confirmed this
happens **with no DLL injected at all** — so it is not the hook.

Diagnosis: advancing the splash triggers real 3D renderer init. Teardown
defaults to the D3D12 backend (`rendererd3d12` string in the exe), which goes
through `vkd3d-proton` onto a **CPU** Vulkan device. The deferred pipeline
with compute-shader lighting does not survive that translation.

This is a hardware ceiling, not a code problem. It needs a real GPU.

## The injection test — passes

```
[VR] === teardown_vr.dll attached ===
[VR] === init thread, pid 220 ===
[VR] no OpenXR loader — flat rendering
[VR] module base 00006ffffbb00000
[VR] hooks installed: begin=00006ffffc0ab9d0 end=00006ffffc0af270
[VR] ready
```

Both hooks land exactly on the statically-derived addresses:

| Expected (static) | Observed at runtime |
|---|---|
| `base + 0x5AB9D0` | `0x6ffffbb00000 + 0x5AB9D0` = `...c0ab9d0` ✅ |
| `base + 0x5AF270` | `...c0af270` ✅ |

Injected into a game already running for 370 s, the process stayed alive for
another **120 s** with the detours installed — comparable to the uninjected
baseline, so the hooks are not destabilising anything.

The frame counter stays at zero because the renderer never gets past splash,
so `beginRender` is never reached. That part still needs a real GPU.

## Injection mechanics (Wine/Proton)

- Pass the **Wine** PID, not the Linux one. `pgrep teardown` gave 11496 while
  `tasklist` inside wine reported 220.
- The DLL path must be a **Windows** path and the file must live inside the
  prefix's `drive_c/`. Using the `Z:\` unix mapping gave
  `OpenProcess failed: 87` (ERROR_INVALID_PARAMETER).
- Injecting *while the game is on the splash screen* works fine — no need to
  wait for the main menu. This is actually the better order: it avoids a race
  with the first rendered frame.

## Call convention note

`beginRender` / `endRender` are **virtual** — there are zero direct `E8` call
sites in `.text`. A static search for callers comes up empty by design. The
pointers at RVA `0xA831B0` / `0xA831B8` in `.rdata` are RTTI-style metadata
(adjacent slots hold values like `0x400000001`), not a plain vtable, and they
have no rip-relative xrefs. Don't waste time there — the runtime hook is the
right approach anyway.

## Next step

On the gaming PC (RTX 4070): copy `build/teardown_vr.dll` + `build/injector.exe`,
inject at the splash screen, press a key to reach the menu, and confirm the
frame counter climbs in the log. That is the first moment `beginRender` is
observed live, and the point from which the per-eye `mubVpMatrix` work starts.

## Injector v2 end-to-end (verified)

The rewritten injector ran the whole flow unattended:

```bash
/opt/GE-Proton/proton run .../injector.exe \
  --game "Z:\\root\\steamless\\Teardown\\teardown.exe" \
  --dll  "C:\\teardown_vr.dll" --timeout 90000 --noquit
```

It launched the game, polled until the window appeared, found PID 368,
injected, and exited 0 after 151 s while the game kept running with the hooks
installed. This run used a **different ASLR base** than the earlier manual
ones and still landed exactly on target:

```
[VR] module base 00006ffffa570000
[VR] hooks installed: begin=00006ffffab1b9d0 end=00006ffffab1f270
```

`0x6ffffa570000 + 0x5AB9D0 = 0x6ffffab1b9d0` — the offsets are base-relative
and hold across separate runs.

