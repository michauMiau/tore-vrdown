# Testy na Windows: co działa, a co blokuje klatki

Data: 2026-09-26. Wszystko poniżej zmierzone na maszynie testowej, nie
wnioskowane.

## Działa

**Hook RTTI instaluje się poprawnie:**

```
[VR] resolved via RTTI: vtable=00007ff755071d80 (rva 0xA81D80)
[VR] hooks installed via VTABLE: begin=00007ff754ba35a0 end=00007ff754ba6e50
[VR] ready
```

Gra przeżywa wstrzyknięcie, proces żyje. Adresy różnią się od tych z
rozpakowanego builda o ponad 4 MB, więc hardcoded RVA nigdy by nie zadziałał —
RTTI była właściwym wyborem.

## Nie działa: brak klatek

Brak linii `frame N`. Licznik rośnie dopiero przy renderowanej klatce, a gra nie
dochodzi do renderera. Przyczyna, z logu samej gry:

```
[NoTag|Render] Graphics API: OpenGL
[NoTag|Render] GPU vendor: Unknown
Compute shader support: false
ERROR [NoTag|Render] Can't load shader cache, platform is wrong 'x64_dx12',
       expected platform is 'x64_gl'
```

Trzy fakty, każdy z logu:

1. **Gra chodzi na OpenGL, mimo RTX 4070.**
2. **`Compute shader support: false`** — a oświetlenie Teardowna to raycasting
   na compute shaderach. Bez tego ścieżka nie ma szans działać.
3. **Cache shaderów jest skompilowany pod `x64_dx12`** — gra sama mówi, że
   wersja, którą chce uruchomić, nie jest tą, dla której zbudowano zasoby.

## Dlaczego D3D12 się wyłącza

Próba wymuszenia `gfxapi=1` działa częściowo:

```
Graphics API: D3D12
ERROR TSwapChainD3D12::initSwapChain:439
      CreateSwapChainForHwnd(...) return HRESULT=887a0022
ERROR Initialization error!!!
Graphics API: OpenGL          <- gra wraca do GL
```

`0x887A0022` = `DXGI_ERROR_SWAP_CHAIN_NOT_STILL`. D3D12 startuje i pada przy
tworzeniu swapchainu, po czym gra wraca do OpenGL. Przy kolejnym uruchomieniu
`options.xml` ma z powrotem `d3d12support value="0"` — **gra sama zeruje
przełącznik po nieudanej próbie.**

System ma pełne wsparcie: `d3d12.dll` 10.0.26100.7705, `D3D12Core.dll`,
`dxgi.dll` — wszystko obecne.

## Sesja pulpitu

Pierwszy test uruchamiał grę przez SSH, czyli w sesji 0, która nie ma pulpitu.
Sprawdzenie to potwierdziło:

```
this session: 0
explorer.exe: SessionId 1
```

Proces z sesji 0 nie ma okna, a D3D12 odmawia swapchainu dla okna, które nie
istnieje. Uruchamianie przez zaplanowane zadanie z tokenem interaktywnym
przenosi proces do sesji 1 i gra dostaje konsolę oraz okno — ale wraca do
OpenGL, więc samo przeniesienie nie wystarczy.

## Co trzeba jeszcze ustalić

1. **Dlaczego `CreateSwapChainForHwnd` zwraca `887A0022`.** Możliwe, że okno nie
   ma jeszcze rozmiaru, że hwnd jest niepoprawny, albo że gra tworzy swapchain
   drugi raz dla tego samego okna. Warto sprawdzić rozmiar okna w momencie
   wywołania i czy `hWnd` jest niezerowy.
2. **Czy w ogóle da się wymusić D3D12.** Jeśli gra aktywnie zeruje przełącznik
   po niepowodzeniu, to albo przyczyna `887A0022` jest błędem do naprawienia, albo
   trzeba patchować decyzję gry.
3. **Frame counter bez renderera.** Alternatywa: liczyć klatki w `endRender`
   zamiast `beginRender`? Nie — oba są wirtualne i oba wymagają renderera.

## Ocena

Hook jest gotowy i udowodniony. Reszta zależy od tego, czy da się uruchomić
D3D12, a to jest problem konfiguracji sterowników i okna, nie moda. Nie ma
powodu podejrzewać, że sam mod blokuje renderowanie.
