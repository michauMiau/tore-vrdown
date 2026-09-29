# xrEndFrame musi działać poza render threadem

Data: 2026-09-28
Status: rozwiązane, potwierdzone pomiarem

## Objaw

Po dodaniu warstwy projekcji gra **zamrażała się na pierwszej klatce**:

```
[ok] teardown pid=1376
[+] LoadLibraryA OK
[VR] OpenXR: >> xrWaitFrame
[VR] OpenXR: << xrWaitFrame -> XR_SUCCESS (shouldRender=1)
[VR] OpenXR: >> xrBeginFrame
[VR] OpenXR: << xrBeginFrame -> XR_SUCCESS
[VR] OpenXR: >> xrLocateSpace
[VR] OpenXR: << xrLocateSpace OK flags=0xf
[VR] OpenXR: >> xrAcquireSwapchainImage(eye 0) -> XR_SUCCESS idx=0
[VR] OpenXR: >> xrAcquireSwapchainImage(eye 1) -> XR_SUCCESS idx=0
[VR] OpenXR: >> xrLocateViews returned XR_SUCCESS got=2
[VR] OpenXR: >> xrEndFrame (displayTime=184151407272311 layer=1)
=== alive? ===
  pid=1376 ws=598MB cpu=5,9s
```

Proces żyje, CPU stoi, wszystkie wątki w `UserRequest`, `endRender=4`, pamięć 598 MB.
`<< xrEndFrame` nigdy nie pojawia się — **blokada, nie błąd**.

## Diagnoza

Logowanie `>>` / `<<` wokół każdego wywołania OpenXR zawęziło zawieszenie
do `xrEndFrame`. Wszystko przed nim wraca `XR_SUCCESS`:

| Krok | Wynik |
|---|---|
| `xrWaitFrame` | XR_SUCCESS, shouldRender=1 |
| `xrBeginFrame` | XR_SUCCESS |
| `xrLocateSpace` | OK, flags=0xf (pozycja i orientacja valid) |
| `xrAcquireSwapchainImage` ×2 | XR_SUCCESS, idx=0 |
| `xrLocateViews` | XR_SUCCESS, got=2 |
| `xrEndFrame` | **nie wraca** |

Moja pierwsza hipoteza (deadlock w `xrWaitSwapchainImage`) była **błędna** —
usunięcie tego wywołania niczego nie zmieniło, zawieszenie przesunęło się
o krok dalej.

## Przyczyna

`xrEndFrame` wywołany z render threada wstrzymuje ten sam wątek, którego
urządzenie ma klatkę w locie. Runtime komponuje warstwę na tym urządzeniu
i czeka na zakończenie pracy GPU, która nie może się wykonać, bo wątek
renderujący nie zostaje zwolniony. Wzajemne oczekiwanie.

## Rozwiązanie

`xrEndFrame` działa na **dedykowanym wątku**. Render thread oddaje pełny
snapshot warstwy i idzie dalej.

```
render thread                    XR thread
─────────────                    ──────────
xrWaitFrame
xrBeginFrame
xrLocateSpace
xrLocateViews
acquire L/R
kopia warstwy ──► InterlockedCompareExchange(&busy, 1, 0) == 0
                    │              └─► xrEndFrame  (blokuje spokojnie)
                    │                  release L/R
                    │                  busy = 0
render thread ◄──────┘ następna klatka
```

Trzy szczegóły, które musiały być zrobione poprawnie:

1. **Kopia warstwy, nie wskaźnik.** Przekazanie `X->proj` jest wyścigiem danych:
   render thread nadpisuje pozycje, gdy runtime jeszcze komponuje → migotanie
   między dwiema pozycjami głowy. Slot ma własne `proj` i `pv[2]`.
2. **Po memcpy `proj.views` wskazuje na oryginały** — trzeba przestawić na
   `s->pv`, inaczej runtime czyta pozycje, które zaraz zostaną nadpisane.
3. **Reguła specyfikacji:** `xrBeginFrame` nie wolno przed powrotem poprzedniego
   `xrEndFrame`. Jeśli wątek jeszcze pracuje, klatka jest **pomijana**
   (`frame_skipped`), nie blokowana. Pominięta klatka jest legalna, zamrożona
   gra nie.

Wyniki, jeden slot, brak kolejki: to nie kolejka, tylko przekazanie. Runtime
jest jedynym konsumentem, a specyfikacja i tak zabrania nakładania klatek.

## Pomiar po poprawce

```
[VR] OpenXR: frame-end thread started (xrEndFrame cannot run on the render thread)
[VR] OpenXR: loop waits=16 ends=15 ready=1 err=0 tracked=1
[VR] OpenXR: projection submissions=16 of 5 ends (swapchains L=1 R=1 1280x720 fmt=29)

=== simulator metrics ===
  lastLayerCount = 1
  projectionSubmissions = 16
  lastReason = NONE

=== process ===
  pid=10928 ws=612MB cpu_grow_4s=0,19s -> ALIVE
```

`lastLayerCount = 1` i rosnące `projectionSubmissions` to potwierdzenie
z **runtime'u**, nie z naszego logu — symulator przyjmuje warstwę.
To zamyka etap „waiting for stereo projection".

Pamięć stabilna ~612 MB, wcześniej rosła do 1,7 GB na tej samej pętli.

## Wniosek na przyszłość

`xrWaitFrame` blokuje do woli — to normalne, runtime decyduje o tempie.
`xrEndFrame` **nie wolno** wołać z wątku, który ma urządzenie GPU w locie.
Jeśli w logu widać `>>` bez odpowiadającego `<<`, zawieszenie jest właśnie
w tym wywołaniu — i nie da się go obejść czekaniem, trzeba zmienić wątek.
