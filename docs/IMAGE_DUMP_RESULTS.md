# Sesja 2026-09-26 wieczór — wyniki

## Metoda: cały obraz

`VirtualQueryEx` odmawia (`ERROR_ACCESS_DENIED`, brak `PROCESS_QUERY_INFORMATION`).
Zamiast tego `SizeOfImage` z nagłówka PE: **0x204E000 = 33.9 MB, 17 chunków, 17/17
odczytanych**. Czytanie po 2 MB, nieudany chunk jest pomijany zamiast przerywać
całość.

## Kod jest rozpakowany — potwierdzone

`beginRender` pod `0x5B35A0` w obrazie z dumpu:

```
48 89 5C 24 18 48 89 6C 24 20 56 57 41 56 48 83
```

To dokładnie ten prolog, który znam z poprzedniego live dumpu. Zatem pełny obraz
jest analizowalny, a `.text` ma niską entropię.

Rozkład sekcji po zawartości:

| zakres | charakter |
|---|---|
| `0x000000`–`0xA00000` | kod (int3 1–9%, uniq 1.00) |
| `0xA00000`–`0xC00000` | przejściowe, 35% zer |
| `0xC00000`–`0x1E00000` | prawie puste, 85–99% zer |
| `0x1E00000`–`0x204E000` | kod/data, 6.5% zer |

## Nagłówek PE w pamięci jest zaśmiecony

Sekcje wypisują się jako `p¦¾`, `ð"` — nazwy są nieczytelne, `VirtualSize` rzędu
`0x280E020B` (671 MB, absurdalne). Ale `e_lfanew = 0x158` i `SizeOfImage` czytają
się poprawnie, więc nagłówek jest częściowo żywy — wystarczyło do odczytu obrazu.
**Nie polegać na nazwach sekcji z pamięci.**

## Skan SIMD: tylko 8 kandydatów w całym obrazie

Dla porównania: samo okno 512 KB dawało zero, cały obraz daje osiem. Gęstości:

```
rva 0x0040C160   math=167  span=196     <- najgęstszy
rva 0x0040B5CC   math=164  span=199
rva 0x0040BC30   math=129  span=141
rva 0x00404340   math=28   span=37
rva 0x0040339A   math=20   span=82
rva 0x0040A932   math=17   span=44
rva 0x00408790   math=16   span=47
rva 0x004053D1   math=11   span=14
```

Trzy najgęstsze (129–167 operacji) to jedna funkcja z tablicą skoków — analiza
pokazała, że to **parser właściwości materiałów**, nie matematyka kamerowa. False
positive.

## Co się nie udało

**`find_entry()` cofał się za daleko.** Dysasemblacja `0x5BB2B0` zaczęła się w
środku epilogu poprzedniej funkcji, przez co wyglądała jak `je / push rax / ret`.
Naprawiona wersja idzie do przodu od paddingu, ale nadal ląduje w funkcji
porównującej recte viewport zamiast w uploaderze.

**`SetMatrix` pod `0x5BB950` nie ma referencji.** Zero wywołań `E8`, zero
absolutnych 8-bajtowych, zero rip-relative w całym 512 KB oknie. To znaczy, że
albo adres jest błędny, albo funkcja jest wołana rzadko i spoza tego okna.

## Wniosek

Pełny obraz jest teraz lokalnie i jest w pełni analizowalny — to realny postęp,
bo eliminuje zgadywanie okien. Ale **bufora sceny wciąż nie znalazłem**, a
dotychczasowe tropy okazały się fałszywe.

Konkretna rzecz do zrobienia: skan SIMD na `0x40C160` wymaga poprawki kryterium
(obecnie liczy `movups`, które jest zbyt częste) oraz **rozszerzenia poza okno
kodu** — matematyka może siedzieć w `0x1E00000`–`0x204E000`, które wygląda jak
kompilowany kod z niską zawartością zer.
