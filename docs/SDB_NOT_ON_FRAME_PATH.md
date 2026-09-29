# Ścieżka per-frame do SDB jest poza frame-hookami

Data: 2026-09-27 wieczór. Wszystko poniżej zmierzone, nie zgadnięte.

## Co wykluczono pomiarem

| funkcja | rozmiar | `0x1F4`? | pola renderera | vtable |
|---|---|---|---|---|
| `beginFrame 0x5B35A0..0x5B387A` | 730 B | **brak** | `+0xCC +0xF70 +0xEF8 +0xE58` | `+0x40` |
| `endFrame 0x5B6E50..0x5B725C` | 1036 B | **brak** | `+0xE40 ×3 +0x1130 ×2 +0x1128 +0x1250 +0x1320` | `+0x48` Present, `+0x10 +0x50 +0x70` |
| 15 callee'ów `endRender` | 17–1043 B | **brak** | brak | `Present`, `ResizeBuffers`, `GetCounter` |
| `0x5BF610..0x5C0ADF` (największa, 5327 B) | 5327 B | **brak** | **zero** | brak — to nie render |

`0x5BF610` okazał się fałszywym tropem: 149 wywołań, zero odwołań do pól
renderera, brak slotów ID3D12, powtarzany wzorzec `5075A0 → 160730 → 507710`
to generowanie stringów. Największa funkcja w paśmie nie jest ścieżką renderu.

## Fałszywe tropy wśród 20 uploaderów

- `0x5B1B70` = `SetRenderTarget(slot, target, flags)` — zapisuje `[rax+0x10] = 2/3/4`
  (`D3D12_RT`), chodzi po `+0x24A*8`, czyta `+0xE40`. Render target, nie upload.
  Przy okazji niezależnie potwierdza `+0xE40` jako swapchain.
- `0x5BB820` = obsługa utraty urządzenia — `GetDebugLayerInterface` po łańcuchu
  DXGI, zapis `[rbx+0x68] = removed`. Nie jest na ścieżce klatki.
- `0x900B0` = setup zasobów przez `+0x9C0` (sloty `+0x30`, `+0x68`).

## Wniosek

`beginFrame` i `endFrame` oraz ich callee'owie nie wgrywają SDB. Uploader jest
wołany z pętli gry, nie z renderera — dlatego szukanie go w paśmie `0x5B0000`
było bezcelowe.

To zmienia strategię: zamiast pytać „który renderer hook wgrywa SDB" pytam
„kto woła którego z 20 uploaderów i który z tych callerów jest sam wołany
per frame". Skrypt `find_uploader_callers.py` skanuje wszystkie 35 710 funkcji
i raportuje liczbę wywołań każdego uploadera oraz ranking callerów.

## Nadal potwierdzone

- `0x9E130` (`0x9E130..0x9E4F8`, 968 B) buduje 500-bajtowy SDB na stosie
  (`r8 = rsp+0x30`) i wgrywa przez `call [rax+0x128]`. Layout zgodny z HLSL.
- `0x9C030` (`0x9C030..0x9C254`, 548 B) tworzy parę buforów przez
  `call [rax+0x120]` = `CreateStructuredBuffer`:
  `+0xB80 ← 0x150` (immutable), `+0xB90 ← 0x1F4` (dynamic, = SDB).
- `+0xB90` nie jest czytany przez `mov reg,[reg+0xB90]` w żadnej funkcji
  z `.pdata` — SDB idzie przez wskaźnik.
