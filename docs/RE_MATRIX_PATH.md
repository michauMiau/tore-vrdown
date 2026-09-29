# RE_MATRIX_PATH — where the view/projection matrix is built and how it reaches the GPU

Date: 2026-09-27. Binary: `/root/steamless/Teardown/teardown.exe`, DECRYPTED/UNPACKED.
Image base `0x140000000`. All addresses below are **RVAs**; add the base at runtime.
`.text` = rva `0x1000..0x98176C` (35,675 functions from `.pdata`).

Method note: every claim here is read out of instruction bytes with capstone
(`tdre.py` / `tdidx2.py` / `tddisp.py` in `/home/truenas_admin/teardown-analysis`).
Function boundaries come from the x64 unwind table, not from linear sweep, so no
instruction in this report is decoded from a misaligned address. Inferences are
labelled **[inference]** and each says what would settle it.

---

## 0. The path in one table

| Step | RVA | What |
|---|---|---|
| 1 | `0x8774A` | VP matrix written to `self+0x22AC` (4×`movups`, 64 B) |
| 2 | `0x87A2E` | `call 0x9D430` with `rcx = self` |
| 3 | `0x9D4A6`–`0x9D7CD` | 500-byte `SceneDynamicBuffer` built **on the stack** at `rsp+0x30` |
| 4 | `0x9D7DD` | `call [rax+0x128]` = `UpdateSubresources`, 500 B → GPU |
| — | `0x9B429` | (creation, not per-frame) `CreateStructuredBuffer` size `0x1F4` → stored at `self+0xB90` |

**The stereo injection point is `0x9D4A6`–`0x9D4CB`** (four instructions that copy
`self+0x22AC..0x22DC` into `SDB+0x000`), or equivalently `0x8774A` one level up.
See §5 for why the first is better.

---

## 1. The SDB writer — RVA `0x9D430`, 964 bytes, `0x9D430..0x9D7F8`

This is the function that fills the `SceneDynamicBuffer` and uploads it.
It has **exactly one caller**: `0x87A2E`.

### 1.1 Prologue and frame layout

```asm
0009D430  mov  [rsp+8], rbx
0009D435  mov  [rsp+0x10], rdi
0009D43A  push rbp
0009D43B  lea  rbp, [rsp-0x170]
0009D443  sub  rsp, 0x270
0009D451  mov  rbx, rcx              ; rbx = self (renderer)
```

Frame arithmetic: after `push rbp` (`rsp-=8`) and `sub rsp,0x270`,
`rsp = rbp - 0x100`, so **`SDB base = rsp+0x30 = rbp-0xD0`**.
The buffer is 500 (`0x1F4`) bytes and is passed as `r8` to the upload.

### 1.2 The upload — the copy to the GPU

```asm
0009D77F  mov  rcx, [rbx+0x9C0]      ; ID3D12Device
0009D786  lea  r8, [rsp+0x30]         ; <-- SOURCE: 500 B on the stack
0009D78B  xor  edx, edx              ; subresource 0
0009D78D  mov  r9d, 0x1F4            ; <-- 500 bytes
0009D793  mov  [rsp+0x28], edx
0009D79A  mov  [rsp+0x20], edx
0009D79E  mov  rdx, [rbx+0xB90]      ; <-- DESTINATION RESOURCE (the SDB)
0009D7DA  mov  rax, [rcx]            ; vtable
0009D7DD  call [rax+0x128]           ; ID3D12DeviceContext::UpdateSubresources
0009D7E3  ...  ret
```

`[rax+0x128]` is slot 20 of `ID3D12DeviceContext`, which is
`UpdateSubresources`. This is the `UpdateSubresources` the earlier notes
described — but **it lives at `0x9D7DD`, not `0x9E4DD`**. See §6.

### 1.3 Where the VP matrix is copied in

```asm
0009D47F  movups xmm0, [rbx+0x22AC]  ; <-- LOAD  self+0x22AC  (VP row 0)
0009D486  lea    rdi, [rbx+0x2218]   ;     rdi = &projMatrix
0009D495  movups xmm1, [rbx+0x22BC]  ; <-- LOAD  self+0x22BC  (VP row 1)
0009D49C  mov  rcx, rdi
0009D4A6  movaps [rsp+0x30], xmm0    ; <-- STORE SDB+0x000  == mubVpMatrix row 0
0009D4B2  movaps [rsp+0x40], xmm1    ; <-- STORE SDB+0x010  == mubVpMatrix row 1
0009D4AB  movups xmm0, [rbx+0x22CC]
0009D4BE  movaps [rsp+0x50], xmm0    ; <-- STORE SDB+0x020  row 2
0009D4B7  movups xmm1, [rbx+0x22DC]
0009D4CB  movaps [rsp+0x60], xmm1    ; <-- STORE SDB+0x030  row 3
```

`SDB+0x000` is `mubVpMatrix` per the HLSL declaration (§4), so **`SDB+0x000` is
the view-projection matrix, and it is copied from `self+0x22AC` (64 bytes).**

### 1.4 Full store map of the 500-byte buffer

Produced mechanically by `sdbmap.py` (symbolic tracking of `rsp`/`rbp` from the
prologue, every store resolved to an SDB offset). 52 stores, offsets `0x000`..`0x1F0`:

| SDB offset | Source (`self+`) | Site | HLSL field **[inference from §4 + size math]** |
|---|---|---|---|
| `0x000` | `0x22AC`,`0x22BC`,`0x22CC`,`0x22DC` | `0x9D4A6`,`B2`,`BE`,`CB` | `mubVpMatrix` (0x40) |
| `0x040` | `0x2258` | `0x9D4D8` | `mubViewXDir.xyz` |
| `0x044` | `0x2268` | `0x9D4E6` | `mubViewYDir.xyz` |
| `0x048` | `0x2278` | `0x9D4F4` | `mubViewZDir.xyz` |
| `0x04C` | imm `0` | `0x9D48D` | (padding) |
| `0x050` | `0x225C` | `0x9D502` | `mubViewXDir.w` |
| `0x054` | `0x226C` | `0x9D50F` | `mubViewYDir.w` |
| `0x058` | `0x227C` | `0x9D514` | `mubViewZDir.w` |
| `0x05C` | imm `0` | `0x9D49F` | (padding) |
| `0x060` | `0x2260` | `0x9D549` | `mubProjectionData` (0x10, 4 float4) |
| `0x064` | `0x2270` | `0x9D55A` | ″ |
| `0x068` | `0x2280` | `0x9D55F` | ″ |
| `0x06C` | `sqrt(…)` | `0x9D590` | ″ (viewZDir.w = near bound radius) |
| `0x070` | `1.0/m[0]` | `0x9D5AB` | ″ |
| `0x074` | `1.0/m[5]` | `0x9D5B5` | ″ |
| `0x078` | `m[8]/m[0]` | `0x9D5BF` | ″ |
| `0x07C` | `m[9]/m[5]` | `0x9D5CB` | ″ |
| `0x080`..`0x0C0` | `0x2258`,`0x2268`,`0x2278`,`0x2288` | `0x9D5D7`–`0x9D603` | `mubViewMatrix` |
| `0x0E0`..`0x110` | `0x236C`,`0x237C`,`0x238C`,`0x239C` | `0x9D619`,`62F`,`3A` | (4th matrix) |
| `0x120`..`0x170` | `0x22EC`..`0x235C` | `0x9D62F`–`0x9D674` | (5th matrix) |
| `0x180`,`0x184`,`0x188` | `0x22A0`,`0x22A4`,`0x22A8` | `0x9D683`,`AA`,`93` | `mubCameraPos` |
| `0x18C` | imm `0` | `0x9D59C` | (padding) |
| `0x190`,`0x194`,`0x198` | `g+0x558`,`0x55C`,`0x560` | `0x9D6DC`,`E4`,`EC` | `mubPlayerPos` |
| `0x19C` | imm `0` | `0x9D6F4` | (padding) |
| `0x1A0`,`0x1A4` | computed | `0x9D733`,`5F` | `mubFrameParams.xy` |
| `0x1A8` | g+0x210 | `0x9D745` | ″ |
| `0x1AC` | `cvtdq2ps` | `0x9D772` | ″ |
| `0x1B0`..`0x1E0` | copy of `rax` (4×`movups`) | `0x9D7A5`–`0x9D7D3` | `mubTransformMatrix` |
| `0x1F0` | `byte [self+0x9A3]` zero-extended | `0x9D7CD` | `mubGenericShaderFlag` (uint) |

Last written offset `0x1F0` + 4 = `0x1F4`. The buffer is filled **completely** —
no gaps, no reads of uninitialised stack.

### 1.5 `mubProjectionData` — exact math

```asm
0009D486  lea    rdi, [rbx+0x2218]       ; rdi = &projMatrix (16 floats)
0009D519  call  0x5E3F90                 ; returns xmm0 = [rcx+0x80] = projMatrix[0x80]
0009D581  movss xmm1, [rip+0x8E967B]     ; = 1.0f   (verified: rva 0x986C04 holds 0000803F)
0009D595  movaps xmm0, xmm1
0009D598  divss xmm0, [rdi]              ; 1.0 / m[0]
0009D5A6  divss xmm1, [rdi+0x14]         ; 1.0 / m[5]
0009D5AB  movss [rbp-0x60], xmm0         ; SDB+0x070  = 1/m[0]
0009D5B0  mulss xmm0, [rdi+0x20]         ; m[8]/m[0]
0009D5B5  movss [rbp-0x5C], xmm1         ; SDB+0x074  = 1/m[5]
0009D5BA  mulss xmm1, [rdi+0x24]         ; m[9]/m[5]
0009D5BF  movss [rbp-0x58], xmm0         ; SDB+0x078  = m[8]/m[0]
0009D5CB  movss [rbp-0x54], xmm1         ; SDB+0x07C  = m[9]/m[5]
```

This matches the HLSL comment **exactly**:
`x = 1/m[0]`, `y = 1/m[5]`, `z = m[8]/m[0]`, `w = m[9]/m[5]`.
Note the engine computes `(1/m[0]) * m[8]`, not a single division — same value.

**Stereo consequence [inference]:** `SDB+0x078` (`mubProjectionData.z`) is the
projection's horizontal shear in clip units, and `SDB+0x070` is `1/xScale`.
To shift an eye by `sx` metres, the cleanest single edit is
`SDB[0x78] += sx * SDB[0x70]` (equivalently, set `m[8] = sx` in the projection).
*Settles it:* write both and compare a screenshot — if the shift is horizontal
and correct, the formula is right. This is also why editing `m[8]` directly in
the projection is equivalent, and simpler, than recomputing a 4×4.

`projMatrix` lives at **`self+0x2218`** (64 B), used here as `m[0]`,`m[5]`
(diagonal, so transposition-invariant) and `m[8]`,`m[9]`.

---

## 2. The VP matrix producer — RVA `0x8774A`

The 64 bytes at `self+0x22AC` are written in **exactly one place in the whole
image**. Verified by enumerating every instruction in every `.pdata` function
with a memory displacement of `0x22AC` — 3 hits total, only one a 16-byte store:

```
000826E5  mov qword ptr [rdi+0x22AC], 0x3f800000   fn 0x81FF0  (init: = 1.0f)
0008774A  movups [r15+0x22AC], xmm0                 fn 0x872D0  <-- THE ONLY PRODUCER
0009D47F  movups xmm0, [rbx+0x22AC]                fn 0x9D430  (the SDB copy, §1.3)
```

Context:

```asm
00087738  mov  r8, rbx
0008773B  lea  rdx, [rbp+0x70]
0008773F  mov  rcx, r12
00087742  call 0x1405E7AD0            ; r12 = proj*view  (builds identity at
                                     ;   [rdx] then calls 0x5E7070 = matrix mul)
00087747  movups xmm0, [rax]
0008774A  movups [r15+0x22AC], xmm0   ; <-- row 0
00087756  movups [r15+0x22BC], xmm1   ;     row 1
00087762  movups [r15+0x22CC], xmm0   ;     row 2
0008776E  movups [r15+0x22DC], xmm1   ;     row 3
```

`r15 = rcx = self`, established at the top of the containing function:

```asm
00087309  mov  r15, rcx
```
(function `0x872D0..0x890FD`, 7,725 bytes).

So the identity is:

```
call 0x5E7AD0(proj, view, out)  ->  self+0x22AC = proj * view
call 0x9D430(self)              ->  SDB+0x000 = self+0x22AC = proj * view
```

**The view-projection matrix in GPU memory is at `SDB+0x000`, and the single
CPU-side computation of it is at `0x87742`.**

---

## 3. The caller chain

```
0x87309  fn 0x872D0: r15 = rcx (= self)
  ├─ 0x87742  call 0x5E7AD0   ->  self+0x22AC = proj*view     (§2)
  ├─ 0x87A2B  mov  rcx, r15
  │   0x87A2E  call 0x9D430   ->  SDB built on stack, UpdateSubresources  (§1)
  └─ 0x87B35  mov  rdx, [r15+0xB90]   ; bind the SDB
          0x87B3C  lea  rcx, [rbp-0x68]
          0x87B40  call 0x5D7690       ; 24-byte descriptor constructor
```

`0x87B18` also does `mov rdx,[r15+0xB98]; mov r9d,0x30; call [rax+0x128]`
— the sibling buffer, size `0x30`.

### 3.1 `0x872D0` is NOT the per-frame path — caveat that matters

A caller-trace of `0x872D0` found a single direct caller, `0x70652`, and reached
it through an init/screenshot chain
(`0x6D0C08` → `0xA7020` → `0xA5650` → `0x71230` → `0x70070` → `0x872D0`) with
nearby strings `game.screenshot`, `game.screenrecord`, `hud.disable`,
`fullscreen.bin`, and no backward jump (no loop) anywhere in that chain.

**[inference]** The 500-byte `UpdateSubresources` at `0x9D7DD` is therefore
probably *not* the per-frame upload; it is likely the screenshot/fullscreen
render path. Two things would settle it: (a) a breakpoint on `0x9D7DD` during
normal gameplay — if it hits 144/s, the inference is wrong; (b) finding the
per-frame `UpdateSubresources` by searching for `mov r9d/r8d, 0x1F4` across the
image. I ran that search: `mov r8d,0x1F4` has **0** hits; `mov r9d,0x1F4` has
**2** (`0x9D78D` and `0x265350`); `mov eax,0x1F4` has **2**
(`0x4553D8`, `0x639D8E`). So `0x9D78D` is the only SDB-sized upload in `.text`
that I could find — which *weakens* the "screenshot-only" inference and makes
`0x9D7DD` the per-frame upload more likely. This needs a live breakpoint to
resolve; I did not have the process.

**Do not treat the "+0xB90 is a flag byte" reading as settled — see §6.**

---

## 4. HLSL declaration, from the binary

`DECLARE_CONSTANT_BUFFER(SceneDynamicBuffer, 1)` is at rva `0xC2B95C`
(ASCII in `.rdata`; found via `strings`, then used only as documentation, not as
proof of any address):

```hlsl
float4x4 mubVpMatrix;              // 0x00
float4   mubViewXDir;              // 0x40
float4   mubViewYDir;              // 0x50
float4   mubViewZDir;              // 0x60
float4   mubProjectionData;        // 0x70
float4x4 mubViewMatrix;            // 0x80
float4x4 mubOldViewMatrix;         // 0xC0
float4x4 mubStableVpMatrix;        // 0x100
float4x4 mubOldStableVpMatrix;     // 0x140
float4   mubCameraPos;             // 0x180
float4   mubPlayerPos;             // 0x190
float4   mubFrameParams;           // 0x1A0
float4x4 mubTransformMatrix;       // 0x1B0
uint     mubGenericShaderFlag;     // 0x1F0
```

Sum = `0x1F4` = 500. The `mubVpMatrix` @ `0x00` and `mubProjectionData` @ `0x70`
placements are confirmed by the CPU store map in §1.4 (the `0x70` block is
provably the `1/m[0], 1/m[5], m[8]/m[0], m[9]/m[5]` quad). The labels for the
interior matrices are **[inference]** from size/shape only.

---

## 5. Recommended injection point

**Patch site: `0x9D4A6` (and the three `movaps` at `0x9D4B2`, `0x9D4BE`, `0x9D4CB`).**

At that moment `xmm0/xmm1` hold `self+0x22AC..0x22DC` and `rsp+0x30` is the
500-byte buffer that is about to be handed to `UpdateSubresources`. A detour
there can:

1. read the 64 B of mono VP at `[rsp+0x30]`,
2. write a per-eye VP into the same 64 B,
3. adjust `SDB+0x070..0x07C` (`mubProjectionData`) consistently,
4. return, and the engine uploads exactly what we wrote.

Why this site rather than `0x8774A`:
* It is the **last** point where the buffer is plain CPU memory, immediately
  before the GPU copy — no window where the engine re-derives it.
* It is a 5-byte `movaps` store; a call-site patch is possible but the project
  has a hard rule against byte-stealing detours (see
  `docs/ROOT_CAUSE_BYTE_STEALING.md`).
* The alternative that avoids code patching entirely: write `self+0x22AC` before
  the engine's own `0x9D47F` read. That is 4 stores of 16 B into a **stable
  renderer field** — the cheapest correct option, and it is idempotent because
  the engine rewrites it every frame at `0x8774A`.

The one thing that must be settled before shipping: **is `0x9D430` per-frame?**
See §3.1. One breakpoint, one run, and the answer is known.

---

## 6. Corrections to prior notes (with the evidence that settles them)

These matter because the notes in this repo have repeatedly been wrong, and two
of my own inputs in this session were wrong in the same way.

**a) `0x9E130` is not an instruction boundary.** It decodes to
`test [rbx],ebx / cli / ...` — garbage. It is in the middle of the function
`0x9D990..0x9E4B4`. Every disassembly excerpt quoted at `0x9E130`, `0x9E2A0`,
`0x9E295..0x9E2CB`, `0x9E47F..0x9E4DD` in `SDB_WRITER_FOUND.md` and
`SDB_BREAKTHROUGH_RESOURCE_TYPE.md` is misaligned and describes nothing real.
The real writer is `0x9D430`, found by searching the memory displacement
`0xB90` against unwind-table function boundaries.

**b) The "0 hits for 0x9E130" measurement was consistent, not contradictory.**
`SDB_WRITER_WRONG.md` concluded the function was never called because two hooks
got 0 hits. That conclusion was correct — the *address* was never a function
entry, so hooking it hit nothing by construction. The feature is fine; the
address was wrong.

**c) `renderer+0xB90` — the byte access at `0x40324` does not refute it.**
`0x40324`/`0x4032B` are real `movzx`/`mov byte` on `+0xB90`, inside function
`0x3E000..0x403F8`. But that function is a **byte-serialisation copy of a
0x121-byte blob** (`mov edx,0x121` at `0x3E017`, `movzx`/`mov byte` loop at
`0x3E030`), operating on a settings/state object — a *different type* that
happens to also have a field at `+0xB90`. It is not the renderer.

The renderer's `+0xB90` is a **qword**, proven by its own constructor:

```asm
0009B404  mov  [rsp+0x24], 0x1F4          ; size = 500
0009B40C  mov  rcx, [rbx+0x9C0]           ; ID3D12Device
0009B413  mov  rax, [rcx]
0009B416  lea  r9, [rip+0x905A5B]         ; -> "Renderer::SceneDynamicBuffer"
0009B41D  lea  r8, [rbp+0x110]
0009B424  lea  rdx, [rsp+0x20]
0009B429  call [rax+0x120]                ; CreateStructuredBuffer
0009B430  mov  rcx, [rbx+0xB90]           ; load old
0009B437  mov  [rbx+0xB90], rax           ; <-- 8-BYTE STORE of the new resource
0009B43E  test rcx, rcx
0009B443  call 0x5C1DD0                   ; release old
```

Immediately above it, the same shape with `0x150` and a neighbouring name
lands at `+0xB88` — two sibling scene buffers created back to back. And the
name string `"Renderer::SceneDynamicBuffer"` (rva `0x9A0E78`) has **exactly one**
rip-relative reference in the image, at `0x9B416` — the `lea r9` above. That
closes the loop: the name, the size `0x1F4`, and the store are all in one
function.

Full inventory of `+0xB90` references image-wide (15 total) is consistent with a
resource handle on the renderer: an init-zero at `0x8235E`, the create at
`0x9B437`, a destroy at `0x845BF`, and `mov rdx,[reg+0xB90]` reads at
`0x87B35`, `0x8E6B9`, `0x8FA2A`, `0x9C704`, `0x9D79E`.

**d) The `Renderer::SceneDynamicBuffer` string is at rva `0x9A0E78`, not
`0x99BF08`.** `0x99BF08` contains non-ASCII bytes. The single-`lea` structure is
real; the recorded address was not.

**e) `renderer+0x1130` is not a constant-buffer ring — confirmed.** A
differentially-encoded pass over the 12 call sites of `0x5B3280` shows `r8`
takes values `0x1,0x4,0x8,0x20,0x40,0x100,0x200,0x800` — a per-call flag
bitfield, not a byte count (`0x800` appears at exactly one of the 12 sites,
`0x5AF2AF`, and every other site has a different value). The function's body
keeps its argument in `rbp` and writes through `rbp+0x10/0x18/0x1C/0x20`, i.e.
into the *passed-in struct*, and performs a 3-deep nested loop over a
`dword` array at `[rbp+0x70]`. **[inference]** it is a sparse 3D
region/descriptor dirty-marking routine, not a byte copier. It does not
`memcpy` its `rdx` argument into its `rcx` argument.
*Settles it:* a breakpoint in it and inspect the buffers it touches.

---

## 7. Reproducing

```
cd /home/truenas_admin/teardown-analysis
python3 -c "import tdre; print(tdre.dis(0x9D430, 0x3C8))"   # the SDB writer
python3 sdbmap.py                                             # the 500-byte store map
python3 -c "import tddisp; tddisp.refs_by_functions(0xB90)"   # resource-handle refs
python3 -c "import tdpat; tdpat.report('x', bytes([0x41,0xB9,0xF4,1,0,0]))"  # 0x1F4 sites
```

`tdre.py` — PE parse + capstone. `tdidx2.py` — cached linear index
(2,818,120 insns). `tddisp.py` — displacement search over unwind-table
functions (alignment-proof). `sdbmap.py` — symbolic stack-frame store mapper.
