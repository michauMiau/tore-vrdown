# What the upload chain actually is: a compute-shader dispatch queue

`renderer+0x1130` is not a constant buffer and `renderer+0xE70` is not an array
of CPU data. Both readings were wrong. Here is what the disassembly and a live
read actually show.

## The chain, top to bottom

```
endRender:
  005AF28B  add  rcx, 0xE70
  005AF292  call 0x5B3780                 ; array[count]
  005AF2AC  mov  rdx, rax                 ; -> upload helper
  005AF2AF  call 0x5B3280
  005AF2C2  call 0x5B2C80                 ; flush/record
  005AF2D1  call [rax+0x48]               ; Present

0x5B3780  array[count] getter
0x5B3280  queue a resource transition / dispatch
0x5B2C80  walk two arrays, record, reset counters
0x5B2F70  per-element dispatch, three dimensions
0x5B28E0  the encoder: D3D12_RESOURCE_STATE bit assembly
```

## 0x5B28E0 is a resource-state encoder, not a memcpy

The body is a long chain of `test` / `cmove` assembling a bitfield, done twice
for two buffers:

```
005B28FB  and  edx, 1
005B28FE  shl  edx, 2
005B2901  mov  r8d, edx
005B2904  or   r8d, 0x20           ; 0x20 = D3D12_RESOURCE_STATE_COPY_DEST
005B2908  test r10b, 2
005B290C  cmove r8d, edx
...
005B295B  or   r8d, 1              ; PRESENT
005B296B  or   ecx, 8              ; 0x08
005B2979  bts  eax, 9              ; 0x200
```

`0x20`, `0x40`, `0x08`, `0x200`, `0x800` are D3D12 resource states. It then
grows a ring with `lea edi,[rax*2+1]` and `shl rcx,5`, and stores 32 bytes per
slot:

```
005B2A90  shl  rax, 5
005B2A9A  movups xmm0, [rsp+0x20]
005B2AA4  movups [rax], xmm0
005B2AA7  movups [rax+0x10], xmm1
```

There is no copy into `renderer+0x1130` anywhere in this path. The 0x800 third
argument is a resource handle or a transition payload, not a byte count.

## 0x5B2F70 is a 3D dispatch

```
005B2FCD  mov  ebp, dword ptr [rdx + 0x20]
005B2FD0  imul ebp, dword ptr [rdx + 0x1C]
005B2FF0  imul ebp, dword ptr [rdx + 0x10]
005B2FF4  test ebp, ebp
```

Three dimensions multiplied into a thread count, then a loop over them. That is
`Dispatch(x, y, z)`, consistent with a deferred renderer whose lighting is
compute-shader raytracing.

## The live read agrees

`build/hunt_upload_source.ps1` read the `+0xE70` pair and followed the array:

```
+0xE70 object: array=0x000001DDD0BD01D0  count=1
elements readable: 1   matrix-shaped hits: 0
```

and the one element begins:

```
+0x008  D0 05 00 00        = 0x5D0  (1488)
+0x038  C0 46 C2 D0 DD 01 00 00
+0x088  02 5A 0F 1F 00 05 00 90
```

`0x1F0F5A02` and a width of 0x5D0 are a `D3D12_RESOURCE_DESC`. The element is
an `ID3D12Resource`, so the array is a list of GPU resources, and no amount of
matrix testing over it will ever find a `float4x4`.

Also worth noting: `count=1`, so this is a one-entry queue. And the same read of
the `+0xE40` pair gave `array=0, count=3497127760`, which is not a valid count
at all, confirming that `+0xE40` is a single `IDXGISwapChain*` and must not be
paired with the getter's `+0x48`/`+0x50` layout.

## Where the matrices actually are

They are not in the renderer and not in this array. A deferred renderer sets
per-eye data through a **root constant buffer view or a descriptor heap**, and
the `float4x4` that matters is written by whatever camera code the game calls
before `beginRender`. The engine strings mention `mubVpMatrix`,
`mubVpInvMatrix`, `mubCameraPos`; `mub` is the engine's "material uniform
buffer" naming. Those are GPU-side, bound through descriptors, which is why no
CPU read of this process has found one.

The practical consequence for stereo: the mod cannot read a matrix and edit it,
because there is no readable CPU copy in the renderer to find. The options are

1. Intercept the camera code on the CPU side before it reaches the descriptor, or
2. Write our own root constants and overwrite the viewports, or
3. Use a driver or proxy layer (ReShade-style swapchain wrapping) that
   re-renders with two viewports, which is the technique every existing VR mod
   for a D3D12 game actually uses.

Option 3 is the honest answer for the remaining time. The hooks this project has
built are real and they work, but they observe a pipeline that keeps its camera
matrices on the GPU.

## The pattern, seventh occurrence

For the seventh time in this project, an offset was written down from a comment
or a string reference and then treated as fact:

| claim | source | killed by |
|---|---|---|
| `+0xE40` is the swapchain | comment | wrong object in early frames |
| `+0x1130` is a matrix ring | disassembly comment | byte dump, then the encoder |
| slot 9 needs two derefs | C++ habit | D3D12 vtable slot 0 is data |
| process alive means running | `Get-Process` | frozen game returns `True` |
| all slots zero | over-tight log filter | own data, unread |
| SDB at `+0xB90` | string + one ref | `movzx` byte, flag block |
| `+0xE70` is a CPU data array | inferred from a getter | `D3D12_RESOURCE_DESC` |

Each one felt solid when written down. Each one died the first time real bytes
were compared against it.
