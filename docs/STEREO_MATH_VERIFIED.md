# Stereo: weryfikacja matematyki i prawdziwa konwencja silnika

Data: 2026-09-27. Odpowiedź na pytanie użytkownika: czy obliczenia stereo są
poprawne i jak robi to inne oprogramowanie.

## Wynik: matematyka była błędna w jednym konkretnym miejscu

`hook/stereo.h` pisał offset oka do `m[3]`, z komentarzem że to „czwarty
element pierwszego wiersza". To prawda tylko dla jednej z dwóch konwencji.

**Konwencja row-vector** (`v = M*p`, flat `m[r*4+c]`):

```
out[0] = p0*m[0] + p1*m[4] + p2*m[8] + p3*m[12]
out[3] = p0*m[3] + p1*m[7] + p2*m[11] + p3*m[15]
```

offset X trafia do `m[12]`, a `m[3]` jest wierszem w — zapis w `m[3]` nie robi
nic z x i psuje w_clip. Stereo renderowałoby dwa identyczne obrazy bez
paralaksy i bez żadnego błędu.

**Konwencja column-vector** (`out = M*v`):

```
out[0] = m[0]*x + m[1]*y + m[2]*z + m[3]*w
```

offset X trafia do `m[3]` — czyli to, co było w kodzie.

**Nie da się tego rozstrzygnąć statycznie.** `computePixelDir` w ogóle nie
używa `mubVpMatrix`, więc żaden shader w obrazie nie wymusza jego układu.
Rozstrzygnięte w kodzie: pozycja jedynego `-1` w macierzy identyfikuje
konwencję (`m[11]` = row-vector, `m[14]`/`m[2]` = column-vector), bo w
macierzy perspektywy nic innego nie równa się -1.

## Co znalazłem w shaderach — i to zmienia plan

`computePixelDir`, obraz `0x1C85E8E`:

```hlsl
vec3 computePixelDir(float2 texCoord)
{
    vec2 screenCoord = texCoordToScreenCoord(texCoord).xy;
    return mubViewXDir.xyz * (screenCoord.x * mubProjectionData.r + mubProjectionData.z) +
           mubViewYDir.xyz * (screenCoord.y * mubProjectionData.g + mubProjectionData.w) -
           mubViewZDir.xyz;
}
```

`texCoordToScreenCoord` to dosłownie `texCoord * 2 - 1`, więc `screenCoord`
jest NDC w `[-1,1]`.

Wnioski:
- **offset oka ma już swoje miejsce**: `mubProjectionData.z` (poziomo) i `.w`
  (pionowo). Nie trzeba w ogóle dotykać macierzy 4×4.
- `.r` i `.g` to `1/xScale` i `1/yScale`.
- **głębokość bierze się z depth bufferu, nie z w_clip** — `computeWorldPos`
  to `pixelDir * linearDepth * mubNearFar.g + mubCameraPos`.

To jest lepsza ścieżka niż modyfikacja macierzy: nie rusza wiersza głębokości,
nie kłóci się z TAA i nie wymaga rozstrzygnięcia konwencji. Do zrobienia:
potwierdzić, że `mubProjectionData` jest przesyłany per-dispatch.

## Jak robi to ReShade VR

Sprawdzone: ReShade VR i każdy depth3d shader przetwarzają **kolorową
teksturę oka jako post-process** i przesuwają próbki o parallax zależny od
głębokości. To nie działa dla Teardown, bo oświetlenie jest compute-shader
raycasting — geometria musi być prawdziwa, żeby TAA i depth buffer się zgadzały.
Potwierdza wybór ścieżki, ale nie zastępuje renderowania.

## Pliki

- `teardown-analysis/verify_stereo_independent.py` — 100+ asercji, obie
  konwencje, plus ścieżka silnika. `ALL INDEPENDENT CHECKS PASS`.
- `hook/stereo.h` — `tdvr_matrix_layout`, offset w `m[3]` lub `m[12]`,
  nowa funkcja `tdvr_eye_projection_data()` dla ścieżki silnika.
- `hook/teardown_vr.c` — auto-detekcja konwencji po pozycji `-1`.

## Cztery błędy w moim własnym teście, warte zapisania

Każdy dawał przekonujący, ale beztreściowy FAIL:

1. Offset wpisywany do `m[3]` w **obu** konwencjach — w row-vector trafiał w
   wiersz w i nic nie robił, więc żadna gałąź nie mogła przejść.
2. Mnożenie `M*v` przy opisie `v = M*p` — translation lądował w komponencie 0
   pod `v*M`, a pod `M*v` był czytany jako wiersz 0 i gubiony.
3. `screenCoord` to NDC, nie metry — porównywanie separacji w clip-space z
   ipd w metrach dawało błąd 5×.
4. `mubProjectionData` miało near/far ratio; faktycznie `.z`/`.w` to offsety,
   co dawało błąd 1.2 m przy rekonstrukcji.

## Stan po wstrzyknięciu v5

```
teardown.exe base   0x7ff7545f0000
vtable RVA          0xA81D80
beginRender         0x7ff754ba35a0
endRender           0x7ff754ba6e50
renderer            0x18ff8dcf3a0
swapchain           0x18ff9330f20 (this+0xE40)
```

Hook żyje. Skanowanie `SceneDynamicBuffer` wciąż szuka — patrz
`docs/MEMORY_SCAN_RESULT.md`.
