# SceneDynamicBuffer: znaleziony prawdziwy uchwyt

Data: 2026-09-27. Rozwiązuje blokera z `PROJECTION_DATA_PATH.md` (`scene=0`).

## Problem

Skan pamięci procesu (~2 GB) nie znalazł aktywnej kopii `SceneDynamicBuffer`.
`renderer+0xB90` trzyma wpis w tablicy zasobów, nie wskaźnik do danych.

## Rozwiązanie: `0x9E130` to per-frame update

Funkcja `0x9E130` (ret `0x9E4F7`) buduje 500-bajtowy SDB **na stosie** i wgrywa
go do GPU co klatkę. Koniec funkcji:

```asm
0x09E47F  mov      rcx, qword ptr [rbx + 0x9c0]   ; ID3D12Device
0x09E486  lea      r8, [rsp + 0x30]                ; ← 500 B SDB na stosie
0x09E48B  xor      edx, edx                         ; subresource 0
0x09E48D  mov      r9d, 0x1f4                       ; ← 500 bajtów
0x09E49E  mov      rdx, qword ptr [rbx + 0xb90]    ; ← docelowy zasób
0x09E4DD  call     qword ptr [rax + 0x128]         ; UpdateSubresources
```

**`UpdateSubresources(pResource, subresource, r0, r1, pData, ...)`** —
`rax+0x128` to slot 20 `ID3D12DeviceContext`, a sygnatura jest jednoznaczna.

### Wypełnienie `mubProjectionData` — potwierdzone binarnie

```asm
0x09E295  movaps   xmm0, xmm1
0x09E298  divss    xmm0, dword ptr [rdi]           ; 1.0 / m[0]
0x09E2A6  divss    xmm1, dword ptr [rdi + 0x14]    ; 1.0 / m[5]
0x09E2B0  mulss    xmm0, dword ptr [rdi + 0x20]    ; (1/m[0]) * m[8]  = m[8]/m[0]
0x09E2BA  mulss    xmm1, dword ptr [rdi + 0x24]    ; (1/m[5]) * m[9]  = m[9]/m[5]
0x09E2AB  movss    dword ptr [rbp - 0x60], xmm0     ; pd.x
0x09E2B5  movss    dword ptr [rbp - 0x5c], xmm1     ; pd.y
0x09E2BF  movss    dword ptr [rbp - 0x58], xmm0     ; pd.z
0x09E2CB  movss    dword ptr [rbp - 0x54], xmm1     ; pd.w
```

Silnik liczy `m[8] * (1/m[0])` zamiast `m[8] / m[0]` — **matematycznie tożsame**,
ale to potwierdza HLSL co do bajtu. Offsety `0x00`/`0x14`/`0x20`/`0x24` to
`m[0]`, `m[5]`, `m[8]`, `m[9]` w macierzy column-major.

`rdi = rbx + 0x2218` — **macierz projekcji jest na CPU pod `renderer+0x2218`**.

## Layout potwierdzony niezależnie

`0x83A70` (default init tego samego bufora) stawia `1.0f` na offsetach
`0x00, 0x14, 0x28, 0x3C, 0x80, 0x94, 0xA8, 0xC0, 0xD4, 0xE8, 0x100, 0x114, 0x128,
0x140, 0x154, 0x168, 0x17C, 0x1B0, 0x1C4, 0x1D8`.

Rekonstrukcja daje **pięć identity `float4x4`** na `0x00 / 0x80 / 0xC0 / 0x100 /
0x140` plus szóstą na `0x1B0`, i zera na `0x40/0x50/0x60/0x70/0x180/0x190/0x1A0`.

Zgodne z deklaracją HLSL, ale teraz potwierdzone **z instrukcji CPU**, nie
tylko z shaderu. `mubProjectionData` = `+0x70`. Suma pól = `0x1F4` = 500.

## Co to zmienia w implementacji

**Nie skanujemy pamięci.** Hookujemy `0x9E130`, a w środku, zanim `ret`
wywołuje `UpdateSubresources`, czytamy 500 B z `rsp+0x30` i:
- zapamiętujemy mono `mubProjectionData` (offset `+0x70`)
- zapisujemy per-eye

Kolejność per-eye: `0x9E130` musi zostać wywołane **dwa razy na klatkę**,
raz z każdym offsetem oka. To daje dwa dispatche per-eye, których brakowało.

Hook jest na funkcji, nie na vtable — więc nie koliduje z `teardown_vr3/v5`.
Rezolucja przez skan obrazu w poszukiwaniu sygnatury `divss [rdi]` +
`divss [rdi+0x14]` + `mulss [rdi+0x20]` + `mulss [rdi+0x24]` + `call [rax+0x128]`
obok `mov r9d, 0x1f4`. Ta sekwencja jest jednoznaczna.
