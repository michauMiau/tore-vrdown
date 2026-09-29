# Swapchain: znaleziony punkt podmiany

Data: 2026-09-27. Przełomowa zmiana kierunku.

## Dlaczego zmiana kierunku

Dziesiąt iteracji polowania na adres `mubVpMatrix` zakończyło się negatywnie.
Każda hipoteza była obalana przez następny pomiar, a koszt rosnął. Tymczasem
istnieje blocker, który **musi** zostać rozwiązany niezależnie od tego, gdzie
leży macierz: OpenXR nie ma własnego loadera, a swapchain musi należeć do OpenXR,
żeby stereo w ogóle mogło się wyświetlić.

## Jak znaleziono

Ścieżki źródłowe silnika są w obrazie jako stringi `assert`:

```
  0xA84A82  ...\td_mp\src\tlib\gfx\d3d12\tswapchaind3d12.cpp.....mParams.fa
  0xA84AA0  mParams.factory->CreateSwapChainForHwnd( mParams.commandQueue, h
```

Te stringi **nie** są argumentami `lea` — są argumentami `assert`, więc nie ma
do nich zwykłych referencji RIP-relative. Pierwsze dwa skanowania (`.text` tylko,
potem cały obraz z filtrem na `modrm & 0xC7`) zwróciły zero i były błędne.

Poprawna metoda: zbudować **odwróconą mapę** wszystkich `LEA/MOV` RIP-relative
w obrazie (47653 instrukcji, 18928 unikalnych celów), a potem szukać celów.
Format stringu z `assert` jest ładowany dopiero w ścieżce błędu, więc trzeba
było wziąć `0xA84AA0` (początek tekstu `mParams.factory->…`), a nie `0xA84A92`.

## Wynik

```
  lea at 0x5BB92B   ->  0xA84AA0  "mParams.factory->CreateSwapChainForHwnd(..."
```

Funkcja zaczyna się pod `0x5BB820` (rozpoznana po `c3 cc cc cc 40 53`, czyli
`ret`, padding int3, `push rbx`):

```
  0x005BB820  push  rbp
  0x005BB822  push  rbx
  0x005BB823  push  rsi
  0x005BB824  push  rdi
  0x005BB825  push  r14
  0x005BB827  mov   rbp, rsp
  0x005BB82A  sub   rsp, 0x70
  0x005BB82E  mov   rbx, rcx                ; this = obiekt swapchaina
  0x005BB831  mov   rcx, [rcx + 0x18]       ; this->mParams.factory
  ...
  0x005BB8A6  mov   ecx, [rbx + 0x28]       ; mParams.width
  0x005BB8AC  mov   ecx, [rbx + 0x2C]       ; mParams.height
  0x005BB8B2  mov   ecx, [rbx + 0x30]       ; mParams.refreshRate.numerator
  0x005BB8CB  mov   ecx, [rbx + 0x34]       ; mParams.refreshRate.denominator
  0x005BB8DD  mov   eax, 0x42               ; DXGI_USAGE?
  0x005BB8E2  mov   ecx, 0x842
  0x005BB8E7  cmp   byte ptr [rbx + 0x68], 0
  0x005BB8EB  cmovne eax, ecx                ; wybór flagi
  ...
  0x005BB8F5  mov   rcx, [rbx + 0x18]       ; factory
  0x005BB8F9  mov   rax, [rcx]               ; vtable factory
  0x005BB8FC  lea   rdx, [rbp + 0x40]        ; &ppSwapChain (out)
  0x005BB900  mov   [rsp + 0x30], rdx
  0x005BB905  mov   [rsp + 0x28], r14        ; pFullscreenOutput = NULL
  0x005BB90A  mov   [rsp + 0x20], r14        ; pAllowTearing = NULL
  0x005BB90F  lea   r9, [rbp - 0x30]         ; &swapChainDesc
  0x005BB913  mov   r8, rsi                  ; pCommandQueue
  0x005BB916  mov   rdx, [rbx + 0x10]        ; hWnd
  0x005BB91A  call  [rax + 0x78]             ; <<< CreateSwapChainForHwnd
  0x005BB91D  mov   edi, eax
  0x005BB91F  test  eax, eax
  0x005BB921  jns   0x5BBA01                  ; sukces -> pomija assert
  0x005BB927  mov   [rsp + 0x28], eax
  0x005BB92B  lea   rax, [rip + ...]         ; "mParams.factory->CreateSwap..."
  0x005BB932  mov   [rsp + 0x20], rax
  0x005BB937  mov   r9d, 0x1B7               ; linia 439
  0x005BB93D  lea   r8, [rip + ...]          ; __FILE__
  0x005BB944  lea   rdx, [rip + ...]         ; assert text
  0x005BB94B  mov   ecx, 3
  0x005BB950  call  0x6997C0                 ; logAssert
  0x005BB955  cmp   edi, 0x8007000E           ; DXGI_ERROR_DEVICE_REMOVED
```

## Wniosek operacyjny

- `this` obiektu swapchaina ma pola: `+0x10` hWnd, `+0x18` factory,
  `+0x20` commandQueue (po `call 0x4F8D40`), `+0x28` width, `+0x2C` height,
  `+0x30/+0x34` refreshRate, `+0x68` flaga wyboru DXGI_USAGE.
- `CreateSwapChainForHwnd` to **slot 0x78** w vtable `IDXGIFactory`. To jest
  `IDXGIFactory4::CreateSwapChainForHwnd` — najnowsza wersja, więc dostępny jest
  `IDXGISwapChain4` z `SetFrameLatencyWaitableObject`, który OpenXR wymaga do
  synchronizacji klatek.
- Hook na `0x5BB91A` (call-site) pozwala zobaczyć realne parametry:
  hWnd, swapChainDesc z rozdzielczością i formatem, oraz wywołanie
  `xrCreateSwapchain` zamiast tego calla.
- `logAssert` to `0x6997C0`, linia `0x1B7` = **439** w `swapchaind3d12.cpp`.

## Stan poprzedniego wniosku (do korekty)

`docs/SDB_OWNER.md` i `docs/SCENE_DYNAMIC_BUFFER_FOUND.md` zawierają twierdzenie,
że `renderer+0x900..0xC80` to tabela uchwytów buforów. `handles.log` pokazał, że
wszystkie 40 wpisów ma identyczną wartość `0x00007FFE00000000` i `+0xB90` nie
leży na tej siatce — **twierdzenie jest obalone**. Patrz
`docs/RENDERER_OBJECT_FINDINGS.md`.
