# STATE OF PLAY — night of 2026-09-29/30

Last update: 2026-09-30 00:56 CEST. Branch `main`.
Companion document: **`docs/_SUPERSEDED.md`** — read it before trusting anything in `docs/`.

> **Provenance note.** Sections 1-6 are the night shift's *live-box measurements* and are
> preserved verbatim from the earlier revision of this file; they are not mine and have not
> been re-run. Sections 7-11 are the documentation audit (all 98 docs in `docs/` + 5 repo-root
> files) added 2026-09-29 late evening. Where the two touch, the live measurement wins.

**The one-line state:** Teardown on this box runs **OpenGL**, the DXGI/D3D12 presentation path
is **measured dead**, the game **refuses to launch from a scheduled task** (ExitCode 53), and
the OpenXR runtime itself works fine in a clean process. The frame-presentation path is still
unknown, and the IAT census that would settle it has never once been run on a live game.

---

## 1. MEASURED — what the night shift established

### 1.1 The game refuses to start from the task (ExitCode 53)
```
Can't load shader cache, platform is wrong 'x64_dx12', expected platform is 'x64_gl'
gfxapi value="0"
```
Manual launch via Steam works. This is **not** a crash and **not** a missing shader:
`GLCache` exists and holds `steamapp_merged_shader_cache.bin` (1 885 969 B), so OpenGL
is in use. Why a task-launched start will not bring up the frontend is still unresolved.

### 1.2 The presentation path is still unknown
```
DXGI CENSUS: 32 slots, hot slot 2 calls=1
CreateSwapChain (slot 10) calls=0
TRendererD3D12: 48 slots, 0 calls
```
`GLCache` + `x64_gl` tilts toward OpenGL, but that is still an **inference**, not a
measurement of the active frame path. The IAT census (41 imports) is built and waiting.

### 1.3 `at+6+disp` confirmed live
```
GDI32!SwapBuffers   thunk 00007ffea23d93e0 -> impl 00007ffea07f4a70
GDI32!SetPixelFormat                           -> impl 00007ffea0805050
```
Canonical addresses, inside GDI32. The off-by-four is fixed and verified.

### 1.4 OpenXR works in a clean process
```
xrCreateInstance=XR_SUCCESS  API=1.1.0  xrGetSystem=35068
tracking pos=1 orient=1  view config=2  blend mode 1
```
`xrGetSystem` = 35068 = 0x88fc. `D3D12CreateDevice` returns `hr=0`. So the runtime,
the loader and D3D12 device creation are all fine — the failures are in *this* process.

### 1.5 Five instrumentation defects found and fixed that night
| Defect | Consequence |
|---|---|
| `frame_census.h` wrote 8 B into a 6-byte instruction's `disp32` | jump to junk + 4 B into the next thunk; `TDVR_FRAME_PATCH` was never defined, so dormant |
| stub tails left unwritten (`VirtualAlloc` → zeros) | `00 00` = `add [rax],al` → zero sled → silent hang, exactly the 23:47 hard lock |
| `frame_reporter()` never existed though passed to `CreateThread` | the frame+slot build could never link |
| IAT census had no reporter thread | 41 live hooks, zero log lines |
| `else` after a brace that had already closed `if (g_hooks.via_rtti)` | **the `TDVR_SLOT_CENSUS=1` build never compiled** |
| missing `<string.h>` in `census.h` | `memcpy` worked only via another file's include |

CI now compiles **all 8 combinations** of the three census flags (`.github/workflows/build.yml:38-56`),
because a single build cannot see a missing symbol in a branch no build enters.

### 1.6 The 2026-09-29 23:47 hard lock — closed
Root cause: the export-thunk detour's tail jump pointed back at the **thunk** instead of
the real implementation, and the thunk by then contained a jump into the stub → an infinite
two-instruction loop. It spun, so there was no crash, no Sentry dialog, no event log.
Fix (`8b87b03`): read the implementation out of the displacement *before* overwriting the
bytes that hold it; record `real`, not `fn`, as the original. Same commit also fixed a
`memcpy` that wrote 16 B over a 6-byte instruction.

---

## 2. MEASURED — from the code audit (independent of the live box)

Verified directly in the tree, not inherited from any doc:

- **Wrong COM GUIDs, now guarded.** `IID_ID3D12CommandQueue` had tail `99,03` where `d3d12.h`
  says `16,ed` — a real queue answered `E_NOINTERFACE` and the code concluded "not a queue".
  `IID_IDXGIAdapter1` had `1b` where `dxgi.h` says `1a` (15 of 16 bytes matched, so it read as
  correct — in the file that warns against hand-written GUIDs). Guarded by `tests/guid_check.c`
  in CI. Commit `97ceb52`.
- **`TDVR_SLOT_CENSUS` was specified but never built.** `hook/census.h:1` opens: *"implemented,
  because it was specified in bisect.h:212 and never built."* That build additionally never
  compiled. Commits `c4ff69f`, `e098d1b`.
- **`environmentBlendMode` is no longer hardcoded.** `hook/xr_session.h:2004` sets it from
  `X->end_blend_mode`, remembered from `xrEnumerateEnvironmentBlendModes` (`:850-884`). The old
  hardcoded `0` was an illegal enumerant.
- **`TD_PRESENT_SLOT` is 8, not 9** (`hook/present.h:83`). Slot 9 is `GetBuffer`. Four docs
  and one comment inside `present.h` itself still say otherwise.
- **The game imports OpenGL.** 37 `gl*` functions plus `wglCreateContext`/`wglDeleteContext`/
  `wglMakeCurrent`/`wglGetCurrentDC`/`wglGetProcAddress` from `OPENGL32.dll`, and `GDI32.dll`.
  `wglSwapBuffers` is **not** imported — hence the interest in `GDI32!SwapBuffers`.
- **The renderer vtable is not the frame path.** `hook/bisect.h:212-219`: the resolved
  `endRender` address `0x7FF754BA6E50` was held by *no* slot of a ≥32-slot vtable, and
  `beginRender` never resolved at all (`0x0`). *"Neither hook has ever fired … everything derived
  from 'the hook is installed' was measured against a dead DLL."*
- **CI is sound; the docs above it are what drifted.** Verified `build.sh`, `tests/run_tests.sh`,
  the 7 injector flags against `injector/injector.c:350-356,379-386`, and the real
  `build/injector.exe` import table (`KERNEL32.dll msvcrt.dll USER32.dll`).

---

## 3. ASSUMED — believed, not measured

Marked here rather than repeated as fact anywhere in `docs/`:

1. **The game renders through desktop WGL.** The import table proves it *can* create a
   context, not that it presents frames that way. §1.2 says the same about `x64_gl`.
2. **`GDI32!SwapBuffers` is the frame boundary.** Reasonable (it is the same presentation step
   `wglSwapBuffers` performs) but **never observed to tick**. This is the load-bearing assumption
   of the entire remaining plan.
3. **The virtual GPU is why the game falls back to OpenGL.** `GPUID: 900`, `Computer=VMware`,
   RTX 4070 passthrough, `nvlddmkm` event 153. `DXGI_PRESENT_PIVOT_2026-09-29.md:670-673`
   built an adapter-enumeration theory on it and then **refuted itself** at `:693-696`
   ("my adapter-enumeration theory is wrong, and it was the most plausible thing left to try").
   Nobody has re-measured whether the game uses D3D12 on non-virtual hardware.
4. **`D3D12CreateDevice` hangs while an XR instance is alive** — measured as a symptom
   (`DXGI_PRESENT_PIVOT_2026-09-29.md:700-708`), mechanism still a labelled hypothesis.
5. **`x64_gl` needs a shader download** vs. **`options.xml` being simply wrong.** Unresolved.
6. **Lua can drive game input.** `LUA_INPUT_WRITE_FEASIBILITY.md:184-256` concludes
   `UiSendInputScreenTouchAction` "cannot create input out of nothing"; `STATUS.md:60-97` says the
   same function "produces input events". The decisive test `STATUS.md:88-95` specifies has never
   been recorded as run. **This is the one silent conflict the measured facts do not resolve.**

---

## 4. OPEN — crash/lockup causes nobody has explained

Do not let anyone close these by citing a cause; ten are enumerated in `_SUPERSEDED.md`.
The worst two:

1. **37 680 frames vs. 48 zero-call slots.** `CAMERA_WORKS.md:11` reports 37 680 frames through
   vtable RVA `0xA81D80`; the census instrumented that same class and counted **zero** calls while
   the game demonstrably rendered. Same vtable, same class. Either the two runs hooked different
   objects or the counters mean different things — nothing in `docs/` records which. Every
   downstream doc reasons from results the census says came off a dead hook.
2. **Variant C crashed at exactly 600 frames** with all three hooks live and only the `+0xE40`
   read *removed* (`BISECT_2026-09-28.md:32`). The DXGI dead-end finding cannot explain it,
   because the swapchain read was already gone. Why 600 exactly, and why variant B held six
   minutes, is unrecorded.

**A caveat about the arbiter used by the bisect work:** `BISECT_2026-09-28.md:83-84` decides which
variants were "real crashes" using CPU-delta. That rests on `STATE_2026-09-28.md:122-130`, where a
*dead* game was seen advancing 0.39 s of CPU over 4 s, attributed to "the crash handler" — an
inference from plausibility, with no profiler or stack taken. An unverified claim underpins the
bisect conclusions.

---

## 5. NEXT — in order

1. **Fix the launch.** Determine whether `x64_gl` exists in the shader cache or whether `gfxapi`
   must be reverted. Until the game starts from the task, **no in-process measurement is valid** —
   every number below depends on this.
2. **Run the IAT census** on a live game. 41 instrumented imports will say whether any frame
   passes through `SwapBuffers`/`wglSwapBuffers`. This settles OpenGL vs DXGI without guessing and
   without pivoting to Zink.
3. **Then, and only then, re-attempt the XR frame loop** with EndFrame on a dedicated thread and
   the render thread submitting a **copy** of the layer (`OPENXR_ENDFRAME_THREAD_2026-09-28.md`).
   `hook/xr_copy.h` exists.
4. **Reconcile or retire the 37 680-frame result** (`CAMERA_WORKS.md`) — either reproduce it with
   the reachability counter in `hook/teardown_vr.c:1553-1563` actually read, or mark it void.
5. **Retire the DXGI line of work in writing.** `DXGI_PRESENT_PIVOT_2026-09-29.md` still closes by
   saying "Strategy B … is worth finishing regardless" (`:712-715`). That is false. The census
   result is currently documented **nowhere** in `docs/` — it exists only in code comments and
   commit messages.

---

## 6. Housekeeping

- Night watch `C:\tdvr\night.ps1` runs as a session-1 task, waits for a fresh game with no `vr_*.dll`,
  injects `vr_k1` (438 831 B, md5 `6cebc82808126da95623ea3fc76d4ff6`), reports 30 × 10 s, kills nothing.
- Machine owner `vm@192.168.1.6`. Repo `michauMiau/tore-vrdown`, GPLv3.
- **Never patch shared system libraries** — a detour in `GDI32.dll` is visible to every process in
  the session. Gated behind `TDVR_FRAME_PATCH`, default 0.
- **Don't trust `LastTaskResult` or `task=Ready` as evidence anything ran.** The only proof is a
  report file with a creation timestamp. Two separate failures reported success while doing nothing:
  a scheduled task registered under principal `Mechau` when `vm` is the only active account, and
  `$ErrorActionPreference='SilentlyContinue'` turning the failure into silence.
- **Don't trust `GetProcAddress == NULL` as proof an export is missing** without an independent probe.
  Two different errors produced the same message.
- **Test with a clean game and no mod before concluding the mod killed it.**
- **Add `--stop-parsing` / syntax validation to every PowerShell script at startup.** `"$tag: ..."`
  makes PowerShell read `$tag:` as a variable; the file was dead with `LastTaskResult=1` and zero output.

---

## 7-11. Documentation audit (added 2026-09-29)

### 7. Scale
99 markdown files in `docs/` (98 project docs + this one), ~642 KB, plus 3 data files
(`isteaminput_sdk164.h`, `vtable_steaminput006.json`, `steaminput006_slots.json`).
**All were committed in a single import commit `07b2177` (2026-09-29 18:04)**, so file mtimes
carry no authoring order. ~25% carry no date at all.

### 8. The "later date wins" rule is unsafe
`README.md:12-13` still states it. It fails because (a) a quarter of the docs are undated,
(b) all were imported at once, and (c) later docs *retract* earlier ones inside their own text
while the earlier doc stays in place unedited. `docs/_SUPERSEDED.md` replaces the rule:
**a doc is trustworthy only if `_SUPERSEDED.md` does not list it and its claims are anchored
to a file:line in the current tree.**

### 9. Grouped by topic
| Group | Count | Status |
|---|---|---|
| XR runtime / presentation (DXGI, swapchain, Present, OpenXR) | 12 | largely dead — see `_SUPERSEDED.md` §1 |
| Scene buffer / renderer / upload / stereo math | 24 | mixed; RE facts good, hunt chains reversed |
| Lua mods / store / SceneDynamicBuffer | 21 | override chains identified, survivor named |
| Injection / detour / Windows environment | 21 | best-measured cluster; several wrong-binary artefacts |
| Steam Input / haptics / control scheme | 8 | slot numbers verified 48/48; haptics addresses refuted |
| State & session logs | 8 | primary source of the silent conflicts |
| Everything else | 4 | incl. the superseded `DXGI_PRESENT_PIVOT_2026-09-29.md` |

### 10. Verdict tally across the 98 audited docs
Counts are the verdict each doc received, computed from the per-doc audit blocks; they sum to 98.

| Verdict | Count |
|---|---|
| AUTHORITATIVE — keep | 29 |
| PARTIALLY FALSE — keep, but trust only the cited parts | 40 |
| SUPERSEDED — do not trust | 23 |
| UNVERIFIED — cannot be checked from the repo | 4 |
| REDUNDANT — carried by a better doc | 2 |

Separately, the five repo-root files: `CLEANUP_NOTES.md` AUTHORITATIVE;
`README.md`, `ROADMAP.md`, `ARCHITECTURE.md`, `BUILD_AND_RUN.md` PARTIALLY FALSE.

### 11. What the audit found that no doc had recorded
- **The census result is documented nowhere.** `grep` for the census phrasing across every
  `docs/*.md` and root `.md` returns only unrelated uses of "exactly one". The measurement that
  killed the DXGI path exists solely in `hook/frame_census.h:1-20` and commit messages. `git status`
  was clean — no doc was ever updated to match.
- **`DXGI_PRESENT_PIVOT_2026-09-29.md` is SUPERSEDED, not merely stale** — it kept issuing UPDATEs
  for five rounds *after* the data that falsified it existed, and `README.md:17` sources its
  "VERIFIED" status block to it.
- **A live self-contradiction inside one source file:** `hook/present.h:96-97` still labels
  `call [rax+0x48]` as Present in a disassembly excerpt while `hook/present.h:83` correctly defines
  slot 8.
- **`OWNERID_FAILED.md:56-58` is false and is the root of the dead end.** It says there is no MSVC
  Complete Object Locator before the vtable; there is one at RVA `0xA81D78` yielding
  `.?AVTRendererD3D12@@`. This false negative produced the wrong slot guesses.
- **`UNPACKED_ANALYSIS.md:169-171` recommends the exact detour shape that caused both the FF25 bug
  and the 23:47 lockup.** Delete it.
- **`BUILD.md` and `DEFENDER.md` contradict each other on artifact size** (223/251 KB vs 48 KB);
  actual is 114 176 B DLL / 49 664 B injector.
- **`STEAMINPUT_VTABLE.md`'s 48-slot table is byte-exact** (48/48 names, offsets = slot×8), but
  `STEAMINPUT_VTABLE_VERIFIED.md:35` says "45 thunks" and `:39-41` places the gaps at `{0,18,22}`
  instead of `{1,18,22}`. The mappings are right; the counts are wrong.
