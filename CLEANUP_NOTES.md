# CLEANUP_NOTES.md — what changed on 2026-09-27

A comment-accuracy and dead-code pass. **No runtime behaviour was changed**, and that
is verified rather than asserted (see "Proof" below).

---

## 1. Comments that stated refuted claims as fact

### 1a. `renderer+0x1130` is NOT a SceneDynamicBuffer (three files)

The old comment block claimed the 0x800-byte upload at `TD_OFF_CONST_UPLOAD` is a
constant buffer holding a SceneDynamicBuffer whose entries are 0x40-byte float4x4
matrices, and used that as the justification for a byte-by-byte scan.

**That is refuted by measurement.** A live 2048-byte dump (`logs/upload_v29.bin`) shows
**0 of 24 slots pass a matrix test**; the values are small integers and heap pointers.
Matching 0x1F4 against 0x800 was a size coincidence treated as an identification.

Rewritten in:

| file | what changed |
|---|---|
| `hook/teardown_vr.c` (`TD_OFF_CONST_UPLOAD`) | split into "what is measured" (offset, length, per-frame upload — all real) and "what was refuted". The wrong reasoning is **quoted and kept**, because the mistake is the lesson. |
| `hook/upload_scan.h` | file header no longer justifies the scan by an SDB it does not contain; now states plainly that the region is a diagnostic that has never reported a hit. |
| `hook/teardown_vr.c` (`tdvr_dump_const_upload`) | function **deleted** — it was uncalled and encoded the refuted claim in its own comment ("0x40 is exactly one float4x4"). Replaced by a note pointing at `tdvr_hex_classify`, which needs no payload assumption. |

The general trap is now written down where the offsets are: *an offset that is genuinely
on the frame path, read by a function that genuinely runs, can still carry a payload
that is not the payload you assumed.* That is exactly how this project was wrong at
`+0xE40` and at `0x9E130`.

### 1b. `renderer+0xE40` is not unconditionally "the swapchain" (four places)

True **only for the object `endRender` receives**. `beginRender` is handed a different
object; in the early frames where that object's `+0xE40` was read, the value there was
`0000023e5c019650`, whose vtable slot 9 read back as `0x234800000238c181` — a float4
exponent pattern, not an address. Same offset, wrong object.

Qualified in: `hook/teardown_vr.c` header offset table, `TD_OFF_SWAPCHAIN`, and
`hook/present.h` (`TD_SWAPCHAIN_OFFSET` and a new **"WHICH OBJECT"** section at the top
of the file). The invariant added: *a successful `td_read` proves the address is mapped,
not that the object is the right one; always run the result through `td_find_present`
before writing through it.*

### 1c. The Present double-dereference bug, now recorded at the validator

`present.h:td_find_present` already carried a short note. It now records the trap in
full, because the code looks correct at a glance and **the failure is silent**:

- **It cost four builds, v54 → v57.** The validator is the last line of defence; if it
  is wrong, everything is refused and the log reads like a wrong offset.
- The bug: dereferencing twice lands on vtable **slot 0**, which for a D3D12 vtable is
  interface data, not a function pointer. Hence the same `0x2348...` float-exponent
  pattern that a *wrong object* also produces.
- **Why it was expensive:** wrong offset, wrong object, and wrong read were all
  simultaneously compatible with the log. All three were live. The tie-break was the
  disassembly of `endRender`, which is unambiguous.
- The rule, stated so it cannot be "improved" into a bug: *for any C++ interface
  pointer, `obj[0]` **is** the vtable. Exactly one dereference.*

### 1d. Dangling reference fixed

`docs/PRESENT_CALLSITE.md` was cited from `present.h` and from
`docs/PRESENT_HOOK_REFUSES_FF25.md`. **That file does not exist.** The source reference
is corrected; the doc-side reference is left alone as out of scope (see §6).

---

## 2. Dead code removed

Everything below was confirmed unreachable by `-Wunused-function` on a clean build
(baseline: **43** dead-code warnings; now **20**).

| removed | size | why it was dead | what was kept |
|---|---|---|---|
| `upload_hook.h` — the entire file | 244 lines | `td_install_upload_hook()` is **never called**; the whole chain (`hooked_upload`, `td_capture_upload`, `g_orig_upload`, `g_tramp`, `TD_RVA_UPLOAD_CONSUMER`, `TD_DETOUR_LEN`, the `g_present_*` forward declarations) was reachable only from that one dead installer. | Replaced by a rationale-only file recording **why** byte-stealing is banned, the DETOUR_LEN 14-vs-15 bug, and the separate fact that the hook fired **once in two minutes** (it was never on the per-frame path). |
| the `if (0) { … }` in `hooked_upload` | 13 lines | disabled vtable scan over the uploader's 3 arguments. | The analysis above it (≈500 000 VirtualQuery calls/frame — the cost that killed the game) stays. |
| the `if (0) { … }` in `hooked_begin_render` | 3 lines | disabled `td_present_from_object(self)`. | Replaced by a note saying the call belongs in `hooked_end_render` and why. |
| 20 per-callee trace hooks + `record_call` + `dump_callrec` + `trace_hook_rva` | ~145 lines | `install_hooks` logs `"trace: disabled"` and never calls them; the callrec array was only fed by the hooks. | The **root cause**, which is the real lesson: the target list was built from **call-site** addresses, not function entries, so 0x5BB2D0 was the middle of someone else's code. Plus the byte-stealing ban. The RVA list was deliberately **not** kept — it would only invite reuse. |
| `tdvr_dump_const_upload` | ~55 lines | uncalled, and carried the refuted float4x4 claim. | see §1a. |
| `td_read_unsafe` | 4 lines | uncalled, and its body was `return len;` — it **claimed a successful full read without reading anything**, behind a name that implied the guard above had run. | A warning not to reintroduce it. Genuine landmine. |

`upload_scan.h` was **kept**, not removed: it is now an explicitly-labelled read-only
diagnostic with no detour and therefore none of the byte-stealing risk.

### 2a. Also condenses, no removals

The V-numbered build-by-build narrative in `hooked_begin_render` / `hooked_end_render`
was ~60 lines of `// V54: … // V55: … // V56: …` interleaved as if it were design. It is
now one account under **"WHY beginRender DOES NOT INSTALL THE PRESENT HOOK"**, keeping
every measured number (12 840 frames/min, the 0x2348… value, the successful
self/`+0xE40`/vtable/slot-9 quadruple, the 0000023e5c019650 failure case) and all three
competing diagnoses. Same for the two stale "counter-only build" notes, which described
a build configuration the current code no longer is.

---

## 3. Institutional memory explicitly preserved

| item | where it now lives |
|---|---|
| byte-stealing freeze analysis (v38–v52) | `docs/ROOT_CAUSE_BYTE_STEALING.md`, restated at `upload_hook.h`, at the trace-hook site, and at `install_hooks` |
| Present double-deref bug (v54–v57) | `docs/PRESENT_HOOK_DOUBLE_DEREF.md` + full trap at `td_find_present` |
| vtable-hook design rationale | `present.h` header, the "Install" block at `install_hooks` |
| DETOUR_LEN 14-vs-15 truncation | `upload_hook.h` (new), original at `DETOUR_LEN` in `teardown_vr.c` |
| "a guarded read proves the address is mapped, not the object" | `present.h` "WHICH OBJECT", `TD_OFF_SWAPCHAIN`, `tdvr_probe_swapchain_candidate` |
| the call-site-vs-function-entry bug | trace-hook site, `install_hooks` log line |

**One code change I made and reverted:** while removing the `if (0)` in `hooked_upload`
I also altered the forward call to `g_orig_upload(rcx, rdx, r9 ? r8d : 0, r9)`. That was
a behaviour change and was reverted immediately; the argument list is now byte-identical
to the original.

---

## 4. build/ consolidation

61 versioned DLLs → **10 live + 4 named milestones + 48 archived**. Nothing was
destroyed: every removed DLL still exists in `build/archive/` under its original name,
and all 48 were verified **byte-identical (md5) to a pre-move backup**.

### Deleted — three byte-identical duplicates only

| deleted | identical to | script repointed |
|---|---|---|
| `teardown_vr5.dll` | `teardown_vr4.dll` | `inject_any.ps1` |
| `teardown_vr8.dll` | `teardown_vr7.dll` | `inject_run.ps1`, `check8.ps1` |
| `teardown_vr37.dll` | `teardown_vr36.dll` | none referenced it |

### Moved to `build/archive/` (48 files, 4.44 MB)

`v9–v35`, `v38–v40`, `v42–v52`, `v54–v56`, `v58–v61` — every build with **zero references**
in any script, source, doc or log. All recoverable; see `build/INDEX.md` for the full
table with sizes, timestamps and hashes.

### Left in the top level (10)

`teardown_vr.dll` (current build; `build.sh` output, `injector.c` default) plus
`v2, v3, v4, v6, v7, v36, v41, v53, v57` — every one of which is still referenced by an
operational script or a doc that quotes it as the build that proved something.

### `build/keep/` — four named milestones

`milestone-v03-camera-haptics.dll`, `milestone-v41-ff25-refused.dll`,
`milestone-v53-first-stable.dll`, `milestone-v57-present-hook-live.dll` — copies, so the
original filenames keep working. v53 = first stable build, v57 = first working Present
hook, as named in the brief and confirmed in `docs/`.

**Full index: `build/INDEX.md`** (new).

---

## 5. Proof that runtime behaviour is unchanged

Claiming "no behaviour change" is easy; here is the evidence.

1. **Instruction-level diff of the whole `.text` section, pristine source vs cleaned
   source, both built unstripped with the same flags:**
   - instruction count: **17 384 → 17 384** (identical)
   - normalized mnemonic+operand sequence: **0 differences**
2. The `.text` section is the **same size** (69 424 bytes) and the DLL's 10-section table
   is identical.
3. Raw `.text` bytes differ in 49 instructions — **all of them rip-relative
   displacements**, retargeted because removing the dead `g_tramp[64]` and the upload
   counters shifted `.bss`/`.data`. Each one was checked individually: after normalizing
   the ASLR base and image addresses, all 53 are identical instructions. Symbol
   resolution confirms the new targets are the intended globals (`g_module`, `g_log`,
   `g_present_orig`, `g_orig_begin`, …).
4. No file in `build/` that a script loads was removed; the three deletions were
   byte-identical duplicates and the scripts were repointed.
5. `sh build.sh` exits 0.

**Tests — all five pass, before and after:**

| test | result |
|---|---|
| `test_present_patch` | ALL PASS (0 failures) |
| `test_memscan` | ALL CHECKS PASS (0 failures) |
| `test_detour` | ALL CHECKS PASS |
| `test_atomic` | ALL CHECKS PASS |
| `test_stealable` | ALL PASS (0 failures) |

Baseline for all five was captured before any edit and was identical.

---

## 6. Found but deliberately NOT changed (report, don't act)

The brief says to report code changes rather than make them. These are live:

1. **`docs/PRESENT_HOOK_REFUSES_FF25.md:45`** still cites the non-existent
   `docs/PRESENT_CALLSITE.md`. The source-side reference is fixed; I did not edit a
   project doc, since the brief scopes me to the source and warns that docs/ is being
   written concurrently.
2. **`docs/PRESENT_HOOK_REFUSES_FF25.md` is titled "v41: the crash"** but its content is
   the Present-install refusal. The title and the content do not match; worth a look.
3. **A third refuted claim still stands in source**, discovered while reading
   `docs/SDB_OFFSET_CORRECTION.md` (written by a concurrent task, 19:40): the claim that
   `renderer+0xB90` is the SceneDynamicBuffer is wrong — the disassembly shows `+0xB90`
   is a **flag byte** inside a copy-constructor byte array (`0xB74`–`0xB93`). Source
   still asserts the old claim in ~14 places, including `TDVR_SDB_FIELD` in
   `scene_bind_find.h:42`, `sdb_resolve.h:14` and `sdb_patch.h:79`. **I did not touch
   these** — they are outside the three claims named in the brief, and the correction
   arrived from another task while I was working. Flagging rather than acting.
4. **`docs/UPLOAD_OFFSET_CONFIRMED.md` (13:20, the newest 0x1130 doc)** still models
   `+0x1130` as 0x800 staging that yields the SDB. Newest ≠ correct here.
5. **20 dead-code warnings remain** (`stereo_apply`, `scan_frame_contexts`,
   `hooked_sdb_*`, `td_walk_*`, …). Left deliberately: they are the SDB-write path this
   project is still trying to get working, they cost nothing at runtime (never called),
   and removing them would discard the approach rather than the cruft. Say the word and
   I will strip them the same way.
6. **`TDVR_BUILD` is `"2026-09-26-dupdet"`**, which does not match any current build
   number. Cosmetic, but it is the string that makes a log unambiguous, which was the
   point of writing it.

---

## 7. Files changed

**Modified:** `hook/teardown_vr.c`, `hook/present.h`, `hook/upload_scan.h`,
`hook/upload_hook.h`, `build/inject_any.ps1`, `build/inject_run.ps1`, `build/check8.ps1`

**Created:** `build/INDEX.md`, this file

**Moved:** 48 DLLs → `build/archive/`

**Deleted:** `teardown_vr5.dll`, `teardown_vr8.dll`, `teardown_vr37.dll` (byte-identical
duplicates; recoverable from `archive/teardown_vr{4,7,36}.dll`)

**Not touched:** `injector/`, `docs/`, `logs/`, `build.sh`, any test source, the other
~130 files in `build/`
