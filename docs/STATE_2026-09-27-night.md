# Stan 2026-09-27 wieczór: hook SDB zweryfikowany offline, czeka na test

## Matcher przetestowany na prawdziwym obrazie

`build/test_bind.c` czyta te same chunki co mod i sprawdza matcher na bajtach,
nie na atrapie. Wynik:

```
site = image+0x0887B0
ctor = image+0x5DF000          (48 89 11 = mov [rcx],rdx)
read[0] 0x088795  field 0xB88
read[1] 0x0887A5  field 0xB90   <- SDB, zmatched
read[2] 0x0887B5  field 0xB98
ALL CHECKS PASS
```

Negatyw jest tu tyle, co pozytywów: `0xB88` i `0xB98` mają identyczny kształt
instrukcji i różnią się tylko `disp32`. Matcher kluczuje na offset pola, nie
kształt — inaczej podmieniłby immutable albo updatable zamiast dynamic.

## Trzy błędy, wszystkie moje, wszystkie złapane offline zamiast na żywo

1. **REX `0x49`, nie `0x4C`.** `r15` w REX.R bo jego dolne 3 bity to same
   jedynki. Pierwsza wersja dopuszczała tylko `48`/`4C` → `pattern not found`.
2. **REX.W to bit `0x08`, nie `0x04`.** Sprawdzałem bit R.
3. **Off-by-one na `lea`.** `48 8D 4D xx` to 4 bajty, więc `call` jest w `j+4`.
   Czytałem `disp8` i nigdy nie widziałem `E8`.

Plus jeden logiczny, który编译器 sam wskazał: `(b & 0xC7) != 0x97` nigdy nie jest
prawdziwe, bo `0x97 & 0xC7 = 0x87`. Teraz modrm porównywane dokładnie.

Wniosek o procesie: każdy z tych błędów kosztowałby uruchomienie gry. Dwa
ostatnie złapał `-Wtautological-compare` i test offline. Test warto był napisać
wcześniej — `test_resolve.c` był wzorem, którego nie zastosowałem.

## Podpięte, ale nietestowane na żywo

`teardown_vr16` działa stabilnie, ale zawiera jeszcze starą wersję matchera
(błędy 1–3 powyżej), więc loguje `SDB bind: pattern not found`. Poprawiony
matcher jest w `hook/scene_bind_find.h`, dzielony z `scene_bind.h` żeby dał się
kompilować bez `windows.h`.

Następny build (`v17`) użyje `tdvr_install_sdb_bind`. Test wymaga zamknięcia
gry — użytkownik zaznaczył, że obecnie gra działa.

## Co potwierdzone (nie zmieniło się)

- `0x087F40..0x089D6D` (7725 B, 178 calli) — jedyna funkcja w 33 MB obrazu
  czytająca `+0xB90`; wołana z pętli przez `0x070B20` ← `0x071CE0`.
- `0x5df000` to konstruktor 24-bajtowego deskryptora silnika, **nie** wywołanie
  D3D12. Zwykły typ zasobu: `+0xB90` to nie `ID3D12Resource*`.
- `0x9E130` buduje i wgrywa 500-bajtowy SDB, ale nie per frame (2 haki, 0 trafień).
- `beginFrame` 730 B, `endFrame` 1036 B, żaden nie zawiera `0x1F4`.
