# Znaleziony bufor sceny — z żywego procesu

Data: 2026-09-26. Przełomowa zmiana w sposobie pracy.

## Nowa metoda: czytanie kodu z pamięci procesu

Plik `teardown.exe` jest spakowany (`.text` entropy 8.000), więc analiza
statyczna niczego nie daje. Ale **w pamięci działającego procesu jest
rozpakowany kod**. Wystarczy `ReadProcessMemory` z uprawnieniami do odczytu.

Realizacja: `build/dump_code.ps1` otwiera proces z `PROCESS_VM_READ` i zapisuje
32 KB wokół `beginRender` i `endRender`. Adresy bierze **z logu**, nie z
notatki — wczesniejsza wersja skryptu miała błędne RVAs po cichu zrzucała
złe bajty.

Wynik: entropy **6.168** dla `beginRender` i **6.075** dla `endRender`, czyli
zwykły kod maszynowy.

Prolog `beginRender` odczytany z procesu:

```
48 89 5c 24 18 48 89 6c 24 20 56 57 41 56 48 83 ec 30 48 8b
```

Dokładnie ten, którego hook szukał. Nowy build ma:

| funkcja | stary rozpakowany build | żywa gra (RVA) |
|---|---|---|
| `beginRender` | `0x5AB9D0` | `0x5B35A0` |
| `endRender` | `0x5AF270` | `0x5B6E50` |

## Potwierdzony layout TRendererD3D12

Pierwsze instrukcje `endRender` (rva `0x5B6E50`):

```
0x5B7250  mov  [rsp+0x18], rbx
0x5B7255  push rbp
...
0x5B7260  mov  rbx, rcx              ; this
0x5B726B  add  rcx, 0xE70            ; jakiś zasób
0x5B7272  call 0x5BB7B0
0x5B727F  lea  rcx, [rbx + 0x1130]   ; <-- bufor uploadu
0x5B7286  mov  r8d, 0x800            ; 2048 bajtów
0x5B728C  mov  rdx, rax
0x5B728F  call 0x5BB2B0              ; upload
0x5B7294  mov  rdx, [rbx + 0xE40]    ; IDXGISwapChain*
0x5B729B  lea  rcx, [rbx + 0x1130]
0x5B72A2  call 0x5BACB0
0x5B72A7  mov  rcx, [rcx]
0x5B72AE  mov  rax, [rcx]
0x5B72B1  call [rax + 0x48]          ; IDXGISwapChain::Present, slot 9
```

To potwierdza offsety odziedziczone z rozpakowanego Steamlessa:

- **`this + 0x1130`** — bufor uploadu, rozmiar **0x800** = 2048 bajtów
- **`this + 0xE40`** — `IDXGISwapChain*`
- **`call [rax+0x48]`** — `Present` w vtable, slot 9

Rozmiar 0x800 jest istotny: `SceneDynamicBuffer` ma 2048 bajtów, więc cały
mieści się w jednym uploadzie. To ten bufor trzeba modyfikować per-eye.

## Co dalej

Mam teraz dwie rzeczy, których wcześniej nie było:

1. **Rozpakowany kod renderera** — można szukać miejsca, które wypełnia
   `SceneDynamicBuffer`, zamiast zgadywać offset.
2. **Potwierdzony bufor `this+0x1130` i jego rozmiar.**

Krok następny: disassemblować funkcję, która wypełnia strukturę pod `0x1130`,
i znaleźć zapis `mubVpMatrix`. Dopiero wtedy per-eye.
