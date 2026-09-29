# Stan projektu — noc 2026-09-29/30

Ostatnia aktualizacja: 2026-09-30 00:56 CEST.

## Co jest potwierdzone pomiarem

**1. Gra odmawia startu z mojego zadania, ExitCode 53.**
```
Can't load shader cache, platform is wrong 'x64_dx12', expected platform is 'x64_gl'
gfxapi value="0"
```
Ręczny start przez Steam działa. Nie jest to crash ani brak shadera — `GLCache`
istnieje i zawiera `steamapp_merged_shader_cache.bin` (1 885 969 B), więc OpenGL
jest używany. Do rozstrzygnięcia pozostaje, dlaczego dokładnie start z zadania
nie wzniosie frontendu.

**2. Ścieżka prezentacji wciąż nieznana.**
```
DXGI CENSUS: 32 slots, hot slot 2 calls=1
CreateSwapChain (slot 10) calls=0
TRendererD3D12: 48 slots, 0 calls
```
`GLCache` + `x64_gl` przechyla szalę na OpenGL, ale to wciąż wniosek, nie
pomiar aktywnej ścieżki klatki. IAT census (41 importów) czeka na pomiar.

**3. `at+6+disp` potwierdzone na żywo.** Off-by-four naprawiony:
```
GDI32!SwapBuffers   thunk 00007ffea23d93e0 -> impl 00007ffea07f4a70
GDI32!SetPixelFormat                           -> impl 00007ffea0805050
```

**4. OpenXR działa w czystym procesie:**
```
xrCreateInstance=XR_SUCCESS  API=1.1.0  xrGetSystem=35068
tracking pos=1 orient=1  view config=2  blend mode 1
```

## Co naprawione tej nocy

| Błąd | Skutek |
|---|---|
| `frame_census.h` pisał 8 B do pola disp32 (6 B instrukcji) | skok do śmieci + 4 B na następny thunk; `TDVR_FRAME_PATCH` nigdy nie był zdefiniowany, więc bezobjawowy |
| ogony stubów nie wypełnione (`VirtualAlloc` → zero) | `00 00` = `add [rax],al` → zero sled → ciche zawiesienie bez dialogu, dokładnie jak hard lock z 23:47 |
| `frame_reporter()` nie istniało, choć przekazywane do `CreateThread` | build frame+slot nigdy by się nie zlinkował |
| IAT census bez wątku raportującego | 41 żywych hooków, zero linijek w logu |
| `else` po nawiasie zamykającym `if (g_hooks.via_rtti)` | **build z `TDVR_SLOT_CENSUS=1` nigdy się nie kompilował** |
| brak `<string.h>` w `census.h` | `memcpy` działał tylko dzięki include'owi z innego pliku |

CI kompiluje teraz wszystkie 8 kombinacji flag. Pojedynczy build nie widzi
brakującego symbolu w gałęzi, do której żaden build nie wchodzi.

## Straż nocna działa

`C:	dvr
ight.ps1` przez task sesji 1, czeka na świeżą grę bez `vr_*.dll`,
wstrzykuje `vr_k1` (438 831 B, md5 6cebc82808126da95623ea3fc76d4ff6), raportuje
30 × 10 s, niczego nie ubije.

## Najbliższy krok

Wynik IAT census. 41 instrumentowanych importów powie, czy którakolwiek
klatka przechodzi przez `SwapBuffers`/`wglSwapBuffers` IAT. To rozstrzyga
OpenGL vs DXGI bez zgadywania i bez pivotu na Zinka.

## Stan

- branch `main`, HEAD `e098d1b`, CI `36642032929` success (8/8 konfiguracji)
- pełny lifecycle XR nadal nieukończony
- `xroperator`/MCP niegotowy
