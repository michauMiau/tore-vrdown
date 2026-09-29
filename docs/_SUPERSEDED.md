# _SUPERSEDED.md — documents that must not be trusted

**Written 2026-09-29. Scope: all 98 documents in `docs/` plus the repo-root files.**

This is the canonical index of what is dead. `README.md:12-13` still tells readers
that "where they disagree, the later dated one wins". **That rule is unsafe and this
file replaces it.** It fails for three reasons, all of which occur below:

1. The docs are undated as a set — 25 of 98 carry no date at all.
2. `git log` shows every `docs/*.md` was imported in a single commit `07b2177`
   (2026-09-29 18:04), so file mtimes carry no authoring order.
3. Several later docs *retract* earlier docs inside their own text while the earlier
   doc stays in place, unedited. A reader following the date rule lands on the
   retracted claim.

**The rule that actually works: a doc is trustworthy only if this file does not list
it, and only if its claim is anchored to a file:line in the current tree.**

The measured state that decides every entry below is recorded in
`docs/STATE_OF_PLAY_2026-09-29.md`. The single most load-bearing facts are:
the DXGI path is a dead end (32 factory slots, 1 ever called, `CreateSwapChain`
slot 10 = 0 calls); the `TRendererD3D12` class took 48 counting stubs and was
never called once while the game demonstrably rendered; and the game imports
OpenGL, not D3D12.

---

## Group 1 — Do not trust: the DXGI / swapchain / Present path

The census (`hook/frame_census.h:1-20`) measured this path dead. These docs assert
it works, or plan work on it.

| Doc | Verdict | Why, and what replaced it |
|---|---|---|
| `DXGI_PRESENT_PIVOT_2026-09-29.md` | **SUPERSEDED** — the single most dangerous file in `docs/` | Not a plan that was falsified: a plan that was falsified and then kept going for five more UPDATEs after the falsifying data existed. `:144-148` tells the reader to hook `CreateSwapChainForHwnd` and Present "slot 8" to get the backbuffer OpenXR needs — `CreateSwapChain` was never called. `:298-299` says "the swapchain hook on `dxgi.dll`'s factory remains the live path". `:712-715` closes with "Strategy B … is not blocked by any of the above. The DXGI work stands on its own and is worth finishing regardless." All false. Replaced by `hook/frame_census.h:1-20` and `docs/STATE_OF_PLAY_2026-09-29.md`. Note `:186-191` correctly falsifies its own shared-vtable premise and then `:712` restates the strategy that depended on it. |
| `SWAPCHAIN_FOUND.md` | SUPERSEDED | "Swapchain found" — the object at `+0xE40` was never an `IDXGISwapChain`. Refuted at `hook/present.h:99-104`; see also `ROADMAP.md:17`, which keeps the claim only for `endRender`'s object. |
| `GRAPHICS_DEVICE_2026-09-28.md` | PARTIALLY FALSE | Built on `+0xE40` = swapchain (`:73`). Its central claim ("the real reason is a missing graphics device") is superseded: the runtime is present and the missing thing is a D3D12 device *because the game runs OpenGL*. |
| `GRAPHICS_BINDING_2026-09-28.md` | PARTIALLY FALSE | Same `+0xE40` premise (`:95`). Claims at `:3-4` to supersede `OPENXR_2026-09-28.md` but leaves its own text unretracted. |
| `PRESENT_HOOK_DOUBLE_DEREF.md` | PARTIALLY FALSE | `:24-25` and `:78` assert `+0xE40` is a swapchain and that slot 9 (`+0x48`) is Present. Both wrong: `hook/present.h:83` defines `TD_PRESENT_SLOT 8`; slot 9 is `GetBuffer`. |
| `PRESENT_HOOK_REFUSES_FF25.md` | PARTIALLY FALSE | Describes a v41 crash; see "still unexplained" below. Its slot-9 reasoning is stale. |
| `BIND_HOOK_ZERO_HITS.md` | UNVERIFIED | Honest about zero hits but cannot be reconciled with `CAMERA_WORKS.md` (below). |
| `OPENXR_FRAME_LOOP_2026-09-28.md` | PARTIALLY FALSE | `:97-99` says `D3D12CreateDevice` returns `E_NOTIMPL` on every adapter. The external probe measured `hr=0` in a normal process. |
| `OPENXR_2026-09-28.md` | SUPERSEDED | `:71` names error `-38` as `LIMIT_REACHED`; `:89` names `-18` as `FORM_FACTOR_UNSUPPORTED`. Both are wrong against `openxr.h` (`GRAPHICS_DEVICE_2026-09-28.md:22` has the right names). Marked superseded, text unretracted. |
| `NIGHT_LOG_2026-09-27.md:11-14, 62, 76-77` | PARTIALLY FALSE (whole file is otherwise good) | v54 Present hook "through the swapchain's vtable, slot 9" — wrong slot. `:76-77` plans intercepting `IDXGISwapChain` methods as "viable" — measured dead. Note this file *does* annotate its v55 step SUPERSEDED in place, so the pattern here is one plan step retracted, its sibling three lines down left standing. |
| `BISECT_2026-09-28.md:61-63, 101-104` | PARTIALLY FALSE | `:61-63` claims `+0xE40` is a swapchain and "slot 9 = real Present". `:101-102` claims `TDVR_NO_XR` is now on by default; `hook/bisect.h:74-76` has `TDVR_NO_XR 0`, i.e. XR is on. |
| `MEASURED_2026-09-28_FIRST_HOOK_RUN.md` | PARTIALLY FALSE | The 2026-09-28 crash (~8640 frames, no error line, nothing in the 65 KB log, `:55-84`) is unexplained and byte-stealing is correctly excluded by the doc itself (`:74-75`). Keep the measurements, do not trust any conclusion drawn from "the hook is installed". |

**Top-level files that inherit the same error:**

- `README.md:17` sources its "VERIFIED" status block to `DXGI_PRESENT_PIVOT_2026-09-29.md`.
  `README.md:41-42` claims a working Present hook on "vtable slot 9 (`+0x48`)" counting
  25 800 calls — wrong slot, and the counters came off a dead hook.
  `README.md:51-53` lists "xrEndFrame never returns" as the remaining blocker; that
  diagnosis is pre-fix.
- `ROADMAP.md:17` and `ROADMAP.md:41` keep `renderer+0xE40` = `IDXGISwapChain*` alive.
- `ROADMAP.md:107` says `docs/` holds 59 documents; it holds 99.
  `ROADMAP.md:121-123` says `RE_MATRIX_PATH.md` and `HAPTICS_DESIGN.md` "are not
  present" — both exist, and `RE_MATRIX_PATH.md:203-204` contains the answer that
  refutes `NO_VP_IN_IMAGE.md`.

## Group 2 — Do not trust: the renderer vtable as frame path

| Doc | Verdict | Why |
|---|---|---|
| `CAMERA_WORKS.md:8-11` | PARTIALLY FALSE, and the corpus's largest unreconciled contradiction | Reports 37 680 frames through vtable RVA `0xA81D80` with `begin=0x...35a0 end=0x...6e50`. The census instrumented that same class with 48 counting stubs and recorded **zero** calls while the game rendered. `hook/bisect.h:212-219` states the resolved `endRender` address `0x7FF754BA6E50` was held by *no* slot, and `beginRender` never resolved at all (`0x0`). Either the two runs hooked different objects or the counters mean different things; nothing in `docs/` records which. Every downstream doc reasons from results the census says came off a dead hook. |
| `STATE_2026-09-27.md:9-10` | PARTIALLY FALSE | "Hooks installed and stable. 144 FPS measured live, process responsive" — against `hook/bisect.h:217-219`, "Neither hook has ever fired … everything derived from 'the hook is installed' was measured against a dead DLL." Same doc `:21-23` claims `IDXGISwapChain4` is available because a `CreateSwapChainForHwnd` call site exists in disassembly; a call *site* is not a call. Keep `:36-47` (the rules) — those held. |
| `STATE_2026-09-28.md:15, 142` | PARTIALLY FALSE | "no OpenVR/SteamVR reference anywhere in `hook/`" — `grep -rniE "openvr\|steamvr" hook/` returns 20 matches. The narrower true claim is that no *pose* API is referenced (`TrackedDevicePose`/`GetPoseToHead` → 0 hits). Also `:17` cites `LUA_INPUT_FINDINGS.md`, **which does not exist**. |
| `STATUS.md:32-35` | PARTIALLY FALSE | Transfers a *container* limitation onto the Windows box by citing `PLATFORM_LIMIT.md:9-20` as support. The game does reach the renderer on Windows. Rest of the file is strong — keep `:20-27` (hardcoded RVAs wrong by >4 MB), `:109-118` (ReShade splash), `:120-122` (only `keybd_event` works). |
| `SCENE_DYNAMIC_BUFFER_FOUND.md`, `SCENE_HUNT_STATUS.md`, `BUFFER_HUNT.md`, `RING_FINDINGS.md`, `UPLOAD_RING.md`, `UPLOAD_CHAIN_TRUTH.md` | SUPERSEDED / PARTIALLY FALSE | The hunt chain that looked for a CPU-side scene buffer on the frame path. Superseded by `SDB_WRITER_WRONG.md` and the census. |

## Group 3 — Do not trust: haptics / Steam Input

| Doc | Verdict | Why |
|---|---|---|
| `HAPTICS_FOUND.md` | SUPERSEDED | `:128-132` "the haptic layer ends in a call to Steam Input" is explicitly REFUTED by `HAPTICS_DESIGN.md:566-584` (it ends in `call [rax+0x78]`). `:98`/`:113` parser addresses `0x1A7A90`/`0x1A7C00` are not function starts (correct: `0x1401A3EA0`). `:7`/`:45` "57 files / 57 embedded names" — 55 in the image, 57 on disk. |
| `CONTROL_SCHEME.md` | PARTIALLY FALSE | `:314` says the action-read loop is at `0x004DD30A`; that address is `lea rcx,[rip+…]` → the interface fetch. `STEAMINPUT_CALLSITE.md:39-41` already records the refutation; `CONTROL_SCHEME.md` propagates the older claim. `:318-320` propagates both refuted `HAPTICS_FOUND.md` addresses. |
| `HAPTICS_DESIGN.md` | PARTIALLY FALSE (best doc in this group — keep) | Self-inconsistent at `:200-201` vs `:316-317` on which table holds the `SteamInput006` fetcher (binary says `0xC483D8` = `{0x4DA920,0,0,0x4DA9A0}`, `0xC48408` = `{0x4DA940,0,0,0x4DA970}`). `:202-203` "the table has 0 rip-relative references" contradicts the confirmed reference at `0x4DCFD6`. `:323`, `:538` attribute hangs to byte-stealing with no address — that is a project anecdote, not a measurement. |
| `STEAMINPUT_VTABLE_VERIFIED.md` | PARTIALLY FALSE | Slot *mappings* are correct (48/48 verified against `vtable_steaminput006.json`), but the **counts** are wrong: `:35` says "45 thunks" (actually 48) and `:39-41` says gaps at `{0, 18, 22}` (actually `{1, 18, 22}`; slot 0 is directly thunked, as the same file shows at `:60-62`). |

**Keep in this group:** `STEAMINPUT_VTABLE.md` (all 48 slot numbers verified byte-exact),
`STEAMINPUT_CALLSITE.md`, `INPUT_LAYER_RESEARCH_2026-09-28.md`.

## Group 4 — Do not trust: reversed SDB / mod-loading chains

These form override chains. The surviving link is named; the earlier links are wrong.

| Chain | Docs in order | Survives |
|---|---|---|
| SceneDynamicBuffer | `SDB_WRITER_FOUND.md` → `SDB_WRITER_WRONG.md` | `SDB_WRITER_WRONG.md` |
| SDB offset | `SCENE_DYNAMIC_BUFFER_FOUND.md` → `SDB_OFFSET_CORRECTION.md` → `SDB_BREAKTHROUGH_RESOURCE_TYPE.md` | `SDB_WRITER_WRONG.md` + census |
| SDB bind site | `STATE_2026-09-27-night.md:43-49` (plans v17) → `BIND_HOOK_ZERO_HITS.md` | `BIND_HOOK_ZERO_HITS.md` (zero hits = dead end) |
| Mod loading | `MOD_LOADING_STATE.md` → `GAMELUA_INJECTION_WORKS.md` → `MOD_LOADING_NEGATIVE.md` | `MOD_LOADING_NEGATIVE.md` |
| Lua store | `RESEARCH_BRIEF_LUA_STORE.md` → `LUA_STORE_LOCATION.md` → `LUA_STORE_VERIFIED.md` | `LUA_STORE_VERIFIED.md` |
| Frame host | `FRAME_HOST_MENU_LUA.md` → `FRAME_HOST.md` | `FRAME_HOST.md` (the level, not `menu.lua`) |
| Lua input injection | `LUA_INJECTION_FOUND.md` → `LUA_INPUT_WRITE_FEASIBILITY.md` | `LUA_INPUT_WRITE_FEASIBILITY.md` |
| Stereo offset index | `STEREO_MATH.md` → `STEREO_MATH_VERIFIED.md` | `STEREO_MATH_VERIFIED.md` |
| Projection scaling | `PROJECTION_CONVENTION.md` → `PROJECTION_DATA_PATH.md` | `PROJECTION_DATA_PATH.md` |

Specifics on the three chains that are easy to get wrong:

- `LUA_INJECTION_FOUND.md` — `:48` "creates events from Lua. this is the route" is refuted by
  full binding disassembly at `LUA_INPUT_WRITE_FEASIBILITY.md:233-237`: the function *scans* a
  record vector for an already-matching id and returns a boolean, and never touches the 51-slot
  state array. `:100`'s "45 injectable actions" is the count in `input_settings.xml`, not the API's
  — the real table has 51 entries including `l_stick_x/y` (`LUA_INPUT_WRITE_FEASIBILITY.md:128-145`).
  Keep `:10-18` (the function exists) and `:30-37` (real in-game call sites).
- `STEREO_MATH.md` — the maths is right and reproducible (`hook/verify_stereo.py` → ALL CHECKS PASS,
  table rows at `:77-80` match exactly), but `:20-24` prescribes the offset into `m[3]` when under
  the engine's row-vector convention it belongs in `m[12]` (`STEREO_MATH_VERIFIED.md:13-19`,
  `hook/stereo.h:66-82`). Under `m[3]` the write lands in `w_clip` and does nothing to x — stereo
  renders two identical images with no parallax and no error. Note the test passes anyway because
  `hook/stereo.h:178` picks the index by runtime detection: **the green test cannot certify the
  shipped convention.**
- `PROJECTION_CONVENTION.md` — superseded by its own header. The HLSL at `:17-28` is verbatim in
  the binary and survives; `:66`'s recommendation (`mubProjectionData.z ± ipd/2 * r`) is the exact
  error `PROJECTION_DATA_PATH.md:30-35` retracts, documented in code at `hook/stereo.h:33-43` as
  "about 0.56x the correct shift at hfov 90".

`MOD_LOADING_NEGATIVE.md` and `FRAME_HOST.md` are themselves superseded **as status
claims** (no script mod in this install executes; the frame host is the level) but
must be kept as the record of a searched negative.

## Group 5 — Injection / detour mechanics: mixed

**Trustworthy, keep:** `WRONG_BINARY_FOUND.md` (every hash/size/entropy re-checked
exact), `PE_BASE_MISTAKE.md` (disassembly byte-exact), `DETOUR_FF25_BUG.md` (the test
reproduces it), `DETOUR_NOTES.md`'s mechanics, `DEFENDER.md`,
`LIVE_TEST_BLOCKER.md` (ReShade, not the mod system, blocked live frames).

**Do not trust:**

| Doc | Verdict | Why |
|---|---|---|
| `ROOT_CAUSE_BYTE_STEALING.md` | SUPERSEDED | Names the freeze cause correctly for v38-v52, but it reads as *the* root cause and is cited project-wide for hangs that it does not explain (e.g. `HAPTICS_DESIGN.md:323`). The 2026-09-29 lockup was a different mechanism (`STATE_OF_PLAY_2026-09-29.md` §błędy 1). |
| `BINARY_PROTECTION.md` | PARTIALLY FALSE | Presents a system-wide protection story; the specific blocker was narrower. |
| `MEMORY_SCAN_RESULT.md:63-65` | PARTIALLY FALSE | The prescription is unusable: `UpdateSubresource` is not an `ID3D12Device` virtual method, and slots `0x38`/`0x50`/`0xA0` are `GetDevice`/`Reset`/`IASetPrimitiveTopology`. |
| `UNPACKED_ANALYSIS.md:169-171` | PARTIALLY FALSE | Recommends a 14-byte `mov rax,imm64; jmp rax` — the exact shape that caused the FF25 bug and the 2026-09-29 thunk lockup. Delete. |
| `OWNERID_FAILED.md:56-58` | PARTIALLY FALSE | Says the image has no MSVC Complete Object Locator before the vtable. There is one at RVA `0xA81D78` (`pSelf == own RVA`, TypeDescriptor `.?AVTRendererD3D12@@`). This false negative is what produced the wrong slot guesses. |
| `CRASH_DUMP_NOTIFICATION.md:50` | SUPERSEDED | "892 million torn reads" — unreproducible; the test prints 323 748 446, and `tests/test_atomic.c:154-166` explicitly forbids quoting the count as a rate. |
| `BUILD.md` | PARTIALLY FALSE | Stale artifact sizes: says 223 KB DLL / 251 KB injector; actual 114 176 B / 49 664 B. `DEFENDER.md:115` (48 KB) is the correct figure — the two repo docs contradict each other. |
| `BUILD_AND_RUN.md:164-173` | PARTIALLY FALSE | Documents running `build/test_*` executables; the live runner is `tests/run_tests.sh`, which compiles into `build/tests/` and also runs `guid_check`. `:53-55`, `:100-105` describe env-var flags; the gate is now `tdvr_flags.txt`. `:142-155`'s "healthy log" shows `slot 9 @+48`, which is wrong. `:90-137` documents a run path superseded by `build/golive_inner.bat`. |
| `PLATFORM_LIMIT.md`, `PROTON_TEST.md`, `WINDOWS_TESTBOX.md`, `SESSION_CRASH_DIAG.md` | PARTIALLY FALSE / UNVERIFIED | Container/Proton/testbox material. `PLATFORM_LIMIT.md`'s OpenGL-fallback-after-`80004005` reading is about the **container**, and is the source of the bad transfer in `STATUS.md:32-35`. |
| `HOST_MODULE.md`, `OPTION1_VERDICT.md`, `HOT_COUNTER_RETIRED.md`, `CRASH_DUMP_NOTIFICATION.md` | SUPERSEDED / REDUNDANT | One-off experiment records; their conclusions are carried by better docs. |

## Group 6 — Wrong-binary contamination

`WRONG_BINARY_FOUND.md` and `PE_BASE_MISTAKE.md` are themselves *authoritative* — they
correct the record. But every numeric address in an offline-analysis doc is suspect if
it predates them: the two builds differ by >4 MB (`STATUS.md:20-27`; Steam
`0xA35A0` vs unpacked-Steamless `0x5AB9D0`), and the PE base was confused with a
renderer object. Treat any address in a pre-correction doc as belonging to an unnamed
base unless the doc says which image it used.

## What is NOT superseded — keep these

- `docs/STATE_OF_PLAY_2026-09-29.md` — the current authoritative summary.
- `OPENXR_ENDFRAME_THREAD_2026-09-29`/`_2026-09-28.md` — the EndFrame-on-a-dedicated-
  thread fix. This is the one documented, still-valid XR diagnosis.
- `STEREO_MATH_VERIFIED.md`, `PROJECTION_DATA_PATH.md`, `IMAGE_SORT_BUG.md`,
  `BUFFER_LAYOUT.md`, `LIVE_RENDERER_DUMP.md`, `RE_MATRIX_PATH.md`,
  `RENDERER_OBJECT_FINDINGS.md` — verified math and structure facts.
- `LUA_STORE_VERIFIED.md`, `LUA_INPUT_WRITE_FEASIBILITY.md`, `ENGINE_LUA_VM.md`,
  `SDB_WRITER_WRONG.md`, `SDB_NOT_ON_FRAME_PATH.md`, `SDB_CANDIDATES_REJECTED.md`.
- `STEAMINPUT_VTABLE.md`, `STEAMINPUT_CALLSITE.md`,
  `INPUT_LAYER_RESEARCH_2026-09-28.md`.
- `DETOUR_FF25_BUG.md`, `WRONG_BINARY_FOUND.md`, `PE_BASE_MISTAKE.md`, `DEFENDER.md`,
  `LIVE_TEST_BLOCKER.md`, `CLEANUP_NOTES.md`.
- All method/measurement-technique docs: `STATE_2026-09-27-evening.md`,
  `SESSION_2026-09-26_EVENING.md`, `HOT_COUNTER_RETIRED.md` (as technique).

## Still unexplained — do not let anyone close these by citing a cause

1. **The 37 680-frame / 48-zero-call contradiction** (`CAMERA_WORKS.md:11` vs
   `hook/frame_census.h:8-10`). The single largest open item in the corpus.
2. **Variant C crash at exactly 600 frames** (`BISECT_2026-09-28.md:32`) with all
   three hooks live and only the `+0xE40` read *removed* — so the DXGI dead-end
   finding does not explain it. Why 600 exactly, and why variant B held 6 minutes,
   is unrecorded.
3. **Variant E died at 57 360 frames** having logged "57360 survived the original
   call" (`BISECT_2026-09-28.md:33`), in a build where `endRender` was never hooked.
4. **v41 crash** (`PRESENT_HOOK_REFUSES_FF25.md:51-61`) — three candidate causes, all
   open, no minidump written.
5. **The 2026-09-28 crash at ~8640 beginRender calls** with no error line and nothing
   in the log (`MEASURED_2026-09-28_FIRST_HOOK_RUN.md:55-84`).
6. **`detached after 0 frames`** death of pid 8348 — its own remedy (pre-inject pid
   snapshot, exit-time report) was never implemented.
7. **The 2026-09-29 pid 17936 `STALLED`** — no reproduction since, deliberately or not.
8. **`SendInput` MOUSEEVENTF_MOVE crashed the game once**
   (`TEARDOWN_INPUT_INJECTION_2026-09-28.md:61`) — no faulting address, no module.
9. **The v38-v52 delayed freezes** as a class: 15 builds, no fault address, no dump.
   `ROOT_CAUSE_BYTE_STEALING.md` names byte-stealing as the cause but the
   2026-09-29 lockup was a *different* diagnosed mechanism, so the class remains
   partly unexplained.
10. **Why a no-HMD SteamVR loader kills at `LoadLibraryA` while the null driver does
    not** (`BISECT_2026-09-28.md:91-99`). Narrowed, not explained.

### A methodological caveat about the liveness detector

`BISECT_2026-09-28.md:83-84` uses CPU-delta to decide which variants were "real
crashes". That arbiter rests on `STATE_2026-09-28.md:122-130`, where a *dead* game was
observed advancing 0.39 s of CPU over 4 s, attributed to "the crash handler". That is an
inference from plausibility — no profiler or stack was taken. An unverified claim
underpins the arbiter that the bisect conclusions rest on.

### One silent conflict the measured facts do NOT resolve

`LUA_INPUT_WRITE_FEASIBILITY.md:184-256` is a searched negative: `UiSendInputScreenTouchAction`
is "the only function that *emits* an action" and it "cannot create input out of nothing".
`STATUS.md:60-97` says the same function "produces input events" and proposes a one-line
decisive test. Neither acknowledges the other, and no measured fact settles it. The test
`STATUS.md:88-95` specifies has never been recorded as run.


---

## Weryfikacja tego audytu — 2026-09-30 01:20

Trzy z czterech głównych zarzutów audytu nie wytrzymały sprawdzenia. Każdy
zostałem obalony **pomiarem**, nie argumentem.

**1. „`OWNERID_FAILED.md:56` jest fałszywy, bo istnieje valid COL" — fałsz.**
Nazwa `.?AVTRendererD3D12@@` rzeczywiście jest w obrazie (RVA 0x1C6C890) i
TypeDescriptor jest poprawnie uformowany (`pVFTable = 0x140ACC650`,
`spare = 0`). Ale **w całym obrazie nie ma ani jednego wskaźnika do tego
TypeDescriptor**. Bez niego nie ma `CompleteObjectLocator`, nie ma
`ClassHierarchyDescriptor`, nie ma vtable, która by go używała. Nazwa jest
ciągiem w `.rdata`, dokładnie tak, jak dokument twierdzi. Dokument miał rację.

**2. „`present.h:96-97` sprzeczne z `:83`" — fałsz.**
`:83` poprawnie definiuje `TD_PRESENT_SLOT 8`. Linie 96–97 są **cytatem ze
wcześniejszego błędu** wewnątrz komentarza, opisującym disasemblację zupełnie
innej ścieżki (D3D12, `renderer+0xE40` → `IDXGISwapChain::Present`). To zapis
historii błędu, nie samokontradykcja.

**3. „`verify_stereo.py` przechodzi, ale nie certyfikuje kodu" — fałsz.**
Tego pliku **nie ma** w `tests/`. Katalog zawiera `guid_check.c`,
`test_atomic.c`, `test_detour.c`, `test_memscan.c`, `test_present_patch.c`,
`test_stealable.c`. `STEREO_MATH.md` nie zawiera ani `m[3]`, ani `m[12]`.
Zarzut dotyczy testu, którego nie ma, i linii, których nie ma.

**4. „Wynik censusu nie jest udokumentowany" — fałsz.**
Jest: `STATE_OF_PLAY_2026-09-29.md:33` (`TRendererD3D12: 48 slots, 0 calls`),
`_SUPERSEDED.md:22`, oraz `OPTION1_VERDICT.md` i `PRESENT_HOOK_DOUBLE_DEREF.md`.
To informacja z 30 września, napisana kilka godzin po audycie.

**Metoda, którą to potwierdza:** zarzut przeciw dokumentacji wymaga tego samego,
co dokumentacja — pomiaru na pliku, nie wniosku z nazwy. Nazwa klasy w `.rdata`
wygląda jak dowód na COL, ale nim nie jest, i odwrotnie: brak nazwy wygląda jak
dowód na brak RTTI, dopóki nie sprawdzisz, czy cokolwiek wskazuje na ten
TypeDescriptor.

**Co z tego zostaje do zrobienia:** samo spisanie stanu i `_SUPERSEDED.md` są
wartościowe i pozostają w repo. Konkretne zarzuty należy traktować jako
odrzucone do czasu, aż ktoś pokaże pomiar.
