# mubProjectionData: ścieżka znaleziona, poprawka skalowania, test na żywo

Data: 2026-09-27. Kontynuacja po `STEREO_MATH_VERIFIED.md`.

## Inline comment silnika rozstrzyga skalę

`DECLARE_CONSTANT_BUFFER(SceneDynamicBuffer, 1)`, obraz `0x1C76AF4`:

```hlsl
float4x4 mubVpMatrix;
float4 mubViewXDir;   // xyz - dir
float4 mubViewYDir;   // xyz - dir
float4 mubViewZDir;   // xyz - dir, w - near plane bound sphere radius
// x - 1.0f / projMatrix.m[0], y - 1.0f / projMatrix.m[5],
// z - projMatrix.m[8] / projMatrix.m[0], w - projMatrix.m[9] / projMatrix.m[5]
float4 mubProjectionData;
```

Więc `pd.z = m[8] / m[0]`. `m[8]` **jest** poziomym offsetem projekcji, a silnik
dzieli go przez `xScale` przy pakowaniu. Dla oka przesuniętego o `sx` metrów
chcemy clip offset `-sx*xScale`, więc:

```
pd.z = (-sx * xScale) / xScale = -sx
```

**Delta do `mubProjectionData.z` to światowy offset oka w metrach, bez skalowania.**

## To był mój błąd, nie drobnostka

Poprzednia wersja pisała `pd[2] += eye_shift * pd[0]`, rozumując: „z jest
offsetem, a obok niego leży `r = 1/xScale`, więc trzeba przeskalować". Nie
trzeba — silnik już to podzielił. Był to błąd, bo `pd.r` i `pd.z` są
sąsiadami w tym samym `float4`, co prowadzi na manewr.

**Uczciwe zastrzeżenie:** przy hfov 90 obie wersje dają identyczną liczbę
(`xScale = 1`, `pd.r = 1`), więc nie mogłem tego rozróżnić testem. Gra ma
dziś `xScale = 1.00039`, więc błąd wynosiłby 12 mikrometrów — prawie
niezauważalny. Argument za wersją bez skalowania jest **formułą silnika**, nie
wielkością błędu. Przy hfov 110 różnica to już 27%.

## Layout potwierdzony binarnie

Suma rozmiarów pól z deklaracji = **500 bajtów = 0x1F4**, dokładnie to, co
silnik podaje do `createStructuredBuffer` w rva `0x9C030`. Offsety nie są
zmyślką z HLSL:

```
0x000  float4x4 mubVpMatrix            64
0x040  float4   mubViewXDir            16
0x050  float4   mubViewYDir            16
0x060  float4   mubViewZDir            16
0x070  float4   mubProjectionData      16   <- offset oka
0x080  float4x4 mubViewMatrix          64
0x0C0  float4x4 mubOldViewMatrix       64
0x100  float4x4 mubStableVpMatrix      64
0x140  float4x4 mubOldStableVpMatrix   64   <- historia TAA
0x180  float4   mubCameraPos           16
0x190  float4   mubPlayerPos           16
0x1A0  float4   mubFrameParams         16
0x1B0  float4x4 mubTransformMatrix     64
0x1F0  uint     mubGenericShaderFlag     4
                                    razem 500
```

## Zmiany w kodzie

- `stereo.h`: `TDVR_SDB_*` offsety, `TDVR_PD_*`, `tdvr_detect_layout()`,
  poprawione `tdvr_eye_projection_data()` (bez skalowania), dodane
  `tdvr_projection_data_eye_offset()` do weryfikacji round-trip.
- `teardown_vr.c`: `stereo_apply()` pisze teraz do `mubProjectionData` **i** do
  macierzy; zapis jest idempotentny (czyta mono z każdej klatki, potem nakłada
  offset), więc powtarzane wywołania nie kumulują błędu. Walidacja
  plausibility `r`/`g` zamiast pisania w ciemno.
- `vtable_resolve.h`: `td_is_foreign_hook()` — wykrywa, że slot 2 zajmuje
  **cudzy** build tego moda, i nazywa go w komunikacie.

## Znaleziony problem organizacyjny

W trakcie sesji w procesie siedziało kilka buildów naraz. Wszystkie hookują te
same sloty vtable, więc pierwszy wygrywa, a każdy kolejny myśli, że gra się
zaktualizowała:

```
RTTI resolution FAILED (vtable slot 2 is hooked by teardown_vr5.dll,
an earlier build of this mod still loaded in this process)
```

To nie jest problem silnika, tylko sposób pracy. **Zasada: jeden build na
proces, restart przed każdym testem nowej wersji.**

## Stan po restarcie i teście vr8

```
proces        pid 14024, sesja 1, responding
hook          via RTTI, vtable RVA 0xA81D80
              begin 0x7ff754ba35a0  end 0x7ff754ba6e50
renderer      0x1ad6763d710
wydajność     144 FPS stabilnie
build         86 528 B, KERNEL32/msvcrt/USER32
```

`scene=0000000000000000` — skaner `SceneDynamicBuffer` wciąż nie znajduje
aktywnej kopii w pamięci CPU, więc `stereo_apply` nie ma gdzie pisać i
`ready=0`. To jest teraz **jedyny** bloker drogi do realnego stereo.

## Co dalej

1. Znaleźć aktywny `SceneDynamicBuffer` — albo naprawić skaner (celowany, nie
   brute-force po 2 GB), albo podejść od strony shadera: znaleźć, gdzie
   `mubProjectionData` jest wypełniane po stronie CPU, i zaczepić tam.
2. Podpiąć `stereo_apply` do dwóch dispatchy per-eye.
3. Dopiero potem TAA per-eye (`mubOldStableVpMatrix` przy `+0x140`).
