# Znaleziony SceneDynamicBuffer — 2026-09-26

## Skąd wziąć nazwę bufora

Silnik nazywa swoje własne bufory, a te nazwy są zwykłymi stringami w obrazie:

```
0x0099BEE8  Renderer::SceneImmutableBuffer
0x0099BF08  Renderer::SceneDynamicBuffer
0x0099BF28  Renderer::SceneUpdatableBuffer
```

Każda ma dokładnie **jedną** referencję `lea reg, [rip+disp]` w kodzie. Wszystkie
trzy prowadzą do tego samego wywołania `[rax + 0x120]` z rozmiarem w `[rsp+0x24]`.

## Kod tworzenia

```asm
; --- SceneImmutableBuffer ---
mov      dword ptr [rsp + 0x24], 0x150        ; 336 B
mov      rcx, [rbx + 0x9c0]
mov      rax, [rcx]
lea      r9,  [rip + ...]                     ; "Renderer::SceneImmutableBuffer"
lea      r8,  [rbp - 0x40]
lea      rdx, [rsp + 0x20]
call     [rax + 0x120]                        ; createStructuredBuffer
mov      [rbx + 0x9C0], rax                   ; uchwyt

; --- SceneDynamicBuffer ---
mov      dword ptr [rsp + 0x24], 0x1F4        ; 500 B
lea      r9,  [rip + ...]                     ; "Renderer::SceneDynamicBuffer"
lea      r8,  [rbp + 0x110]
lea      rdx, [rsp + 0x20]
call     [rax + 0x120]
mov      [rbx + 0xB90], rax                   ; uchwyt

; --- SceneUpdatableBuffer ---
lea      rsp, [rip + ...]                     ; "Renderer::SceneUpdatableBuffer"
```

Funkcja: `rva 0x9C030`, 547 bajtów, pierwszy `ret` pod `0x9C253`.

## Rozmiar i lokalizacja

| bufor | rozmiar | slot w rendererze |
|---|---|---|
| `SceneImmutableBuffer` | `0x150` = 336 B | `+0x9C0` |
| `SceneDynamicBuffer` | `0x1F4` = 500 B | `+0xB90` |
| `SceneUpdatableBuffer` | `0x1F4` = 500 B | `+0xBA0` |

**`SceneDynamicBuffer` to 500 bajtów pod `renderer + 0xB90`.** Dokładnie ten bufor,
którego szukałem od początku — nazwa wskazywana przez własny kod silnika, nie
zgadywana z heurystyki.

## Kto to wywołuje

```
0x0008368A  call 0x9C030        ; create all three buffers
```

W kontekście:

```asm
0x0008367F  mov  rcx, rdi
0x00083682  call 0x93800
0x00083687  mov  rcx, rdi
0x0008368A  call 0x9C030        ; <- bufory
0x0008368F  mov  rcx, rdi
0x00083692  call 0x9C600
```

`rcx = rdi` w trzech kolejnych wywołaniach inicjalizacyjnych, więc `rdi` to ten sam
obiekt co `this` w `beginRender`. Bufory powstają raz przy starcie i od tego czasu
są niezmienne jako uchwyty.

## Co jest w buforze

Teksty shaderów HLSL są w obrazie jako stringi i wymieniają pola:

```
mubVpMatrix
mubVpInvMatrix
mubCameraPos
mubTransformMatrix
mubEditorParams.r = near, .g = far
mubDofParams.r = focusMin, .g = focusMax, .b = focusScale, .a = far
mubSceneBrightness
mubExposureParams.rg / .ba
mubPixelSize
mubVolMatrix, mubVolInvMatrix, mubIsoForward
mubDiffuseLights[]
mubVoxelSize, mubVoxUIntData
mubMaxRadiusRndPixelSize
mubSceneBrightness
```

`mub` = **multi-unstructured-buffer**, czyli nazwa struktury HLSL dla
`StructuredBuffer`. Każde pole to element struktury o tym rozmiarze.

## Stan odczytu

Uchwyt pod `renderer+0xB90` w tym momencie **zero**. To nie jest błąd — kod
tworzący bufory działa przy inicjalizacji, a odczyt szedzie w `beginRender`.
Trzeba sprawdzić, która klasa trzyma te uchwyty: `rdi` w `0x8368A` to obiekt
innego typu niż `this` z `beginRender`, bo wątek renderujący dostaje inny
obiekt.

**To jest następny krok i jest konkretny:** znaleźć, czym jest obiekt przekazywany
do `0x9C030`, i odczytać pola `+0x9C0`, `+0xB90`, `+0xBA0` z niego.

## Korekta wcześniejszych ustaleń

Wszystkie wcześniejsze skanowania pamięci były bezcelowe, bo szukały
`SceneDynamicBuffer` zgadywanką w obiekcie renderera. Nazwa istniała w obrazie
cały czas — trzeba było szukać stringów, a nie struktur.
