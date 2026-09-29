# Konwencja projekcji w silniku — potwierdzona z HLSL

> **AKTUALIZACJA 2026-09-27:** ten dokument opisywał `mubProjectionData.z` jako
> „przesunięcie w X, które nie ma ustalonej skali". Inline comment silnika przy
> `SceneDynamicBuffer` rozstrzyga to ostatecznie: `pd.z = m[8] / m[0]`, czyli
> silnik **już podzielił offset przez `xScale`**. Delta to światowy offset w
> metrach, bez skalowania. Pełne wyprowadzenie i lista moich błędów:
> `PROJECTION_DATA_PATH.md`. Reszta tego dokumentu (konwencja macierzy,
> `computePixelDir`, porównanie z ReShade) pozostaje aktualna.

Data: 2026-09-27. Odpowiedź na pytanie użytkownika, czy matematyka stereo jest
poprawna. Odpowiedź: tak, ale nie z powodu, dla którego napisałem pierwszy test.

## `computePixelDir` — definicja z obrazu (0x1C85E8E)

```hlsl
vec3 computePixelDir(float2 texCoord)
{
    vec2 screenCoord = texCoordToScreenCoord(texCoord).xy;
    return mubViewXDir.xyz * (screenCoord.x * mubProjectionData.r + mubProjectionData.z) +
           mubViewYDir.xyz * (screenCoord.y * mubProjectionData.g + mubProjectionData.w) -
           mubViewZDir.xyz;
}

vec3 computeWorldPos(vec3 pixelDir, float linearDepth)
{
    return pixelDir * linearDepth * mubNearFar.g + mubCameraPos.xyz;
}
```

## Co to zmienia

Silnik **nie dekoduje projekcji z macierzy VP**. Nie ma `clip = VP * p` i potem
dzielenia przez w. Zamiast tego trzyma osobno:

- `mubViewXDir` / `mubViewYDir` / `mubViewZDir` — baza kamery, 3 wektory;
- `mubProjectionData = (r, g, z, w)` — cztery skalary;
- `mubNearFar = (near, far, ?, 1/(far-near))`.

Promień kierunku jest składany ręcznie w shaderze:
`X * (x*r + z) + Y*(y*g+w) - Z`. Czyli **przesunięcie oka trafia w
`mubProjectionData.z` i `.w`**, a nie do macierzy 4×4. To jest dokładnie ten sam
off-axis co w mojej implementacji — przesunięcie wzdłuż osi X bazy kamery — tylko
wyrażone w innej reprezentacji.

To znaczy, że:
- `mubProjectionData.z` to przesunięcie w X (nie „near/far ratio", jak
  założyłem w pierwszej wersji testu);
- `mubProjectionData.w` to przesunięcie w Y;
- `mubProjectionData.r` i `.g` to `1/xScale` i `1/yScale`.

## Dwie rzeczy, których pierwsza wersja testu rozumiała źle

1. `computePixelDir` zwraca **kierunek** w world, nie world position. Promień
   pochodzi z bazy kamery, a głębokość dochodzi z depth bufferu, a nie z w.
   Offset oka więc nie może zmieniać głębokości — i nie zmienia, bo trafia w
   `.z`/`.w`, a nie w wiersz depth.
2. `computeWorldPos` mnoży przez `mubNearFar.g`, czyli `1/(far-near)`. Offset
   w world space traci sens — liczy się tylko w bazie kamery.

## Wniosek dla implementacji

Offset per-eye **nie musi dotykać `mubVpMatrix`** w ogóle. Wystarczy
zmodyfikować:

- `mubProjectionData.z` o `±ipd/2 * mubProjectionData.r` na czas dispatchu oka,
- `mubViewXDir` gdy głowa jest obrócona (kamera VR ma własny yaw).

To znacznie prostsze i bezpieczniejsze niż modyfikacja macierzy, bo nie rusza
wiersza depth, nie psuje TAA i nie wymaga zna��enia konwencji wierszy/kolumn.

**Do zrobienia:** potwierdzić, że `mubProjectionData` faktycznie leży w buforze
i jest przesyłany per-dispatch. `docs/BUFFER_LAYOUT.md` ma to na `+0x070`, ale
aktywna kopia nadal nie została znaleziona — patrz `docs/MEMORY_SCAN_RESULT.md`.

## ReShade VR dla porównania

Sprawdzone, jak robi to ReShade VR: przetwarza **kolorową teksturę oka jako
post-process** i przesuwa próbki o parallax zależny od głębokości. To nie
działa dla Teardown, bo:

- oświetlenie jest compute-shader raycasting, więc próbkowanie koloru po
  postprocessie nie daje poprawnej geometrii;
- TAA musi zgadzać się z historią per-eye;
- brak zgodności z depth bufferem.

Dlatego mod musi robić prawdziwe dwa dispatche z off-axis projekcją, a nie
przesuwać piksele. To potwierdza wybór ścieżki, ale nie zastępuje renderowania.
