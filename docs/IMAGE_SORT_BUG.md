# KRYTYCZNE: sortowanie chunków obrazu było zepsute

Data: 2026-09-27. Znalezione po ~2 godzinach bezowocnych skanów.

## Objaw

Skrypty analityczne w `teardown-analysis/` składały obraz `teardown.exe` z
chunków `logs/image/img_*.bin` i konsekwentnie pokazywały **zera** w miejscach,
gdzie powinien być kod:

```
0x5B35A0: 00000000000000000000000000000000   <- beginRender
0x5B6E50: 00000000000000000000000000000000   <- endRender
```

`find_upload_slots.py` raportował dla `beginRender` **0 interfejsowych calli**,
co jest fizycznie niemożliwe dla 14 KB funkcji renderującej.

## Przyczyna

Nazwy chunków to **liczby w systemie szesnastkowym**, nie dziesiętnym:

```
img_000000.bin   ->  0x000000
img_200000.bin   ->  0x200000   (2 MB)
img_400000.bin   ->  0x400000   (4 MB)
img_E00000.bin   ->  0xE00000   (14 MB)
img_1000000.bin  ->  0x1000000  (16 MB)
```

Sortowanie `sorted(os.listdir(...))` jest **alfabetyczne**, więc:

```
img_000000.bin
img_1000000.bin   <- "1" < "2", idzie przed chunkiem 2 MB
img_1200000.bin
...
img_1E00000.bin
img_200000.bin    <- dopiero teraz
img_2000000.bin
img_400000.bin
```

Chunki 16–34 MB trafiały **przed** chunki 2–15 MB. Obraz był kawałkowy:
`.text` w zakresie `0x2..0xE` MB (gdzie siedzi `beginRender`) lądował pod
zastąpionymi zerami z niewłaściwego offsetu.

## Skala szkody

To nie był jeden skrypt. **30 plików** w `teardown-analysis/` miało ten sam
błąd. Każdy wynik z nich jest podejrzany.

Co jest bezpieczne (bo używa `blob[a:a+n]` po sklejeniu i trafiło w zakres
pokryty): większość analiz shaderów i stringów, bo stringi `.rdata` są nisko
i chunki niskie były dokładane poprawnie.

Co jest **zatrute** i wymaga powtórzenia:
- `find_upload_slots.py` — 0 calli w `beginRender` (bzdura)
- `find_sdb_writer.py`, `find_prodfill.py`, `find_projmatrix.py` — wyniki
- każdy wniosek o tym, czego **nie ma** w `.text` w zakresie 2–14 MB
- `find_all_sdb_uploads.py`: „5 trafień 0x1F4, 1 z UpdateSubresources" —
  po poprawieniu nadal 1, ale to teraz wynik wiarygodny

## Naprawa

Wszędzie:

```python
for f in sorted((f for f in os.listdir(d) if f.startswith("img_") and f.endswith(".bin")),
                key=lambda f: int(f[4:-4], 16)):
```

Weryfikacja po naprawie — `beginRender` musi mieć realny prolog:

```
0x5B35A0: 48895c241848896c2420565741564883
```

Zgadza się z prologiem zapamiętanym z żywego procesu
(`48 89 5C 24 18 48 89 6C 24 20 56 57 41 56 48 83 EC 30`).

## Lekcja

Test, którego nie da się zobaczyć zepsuć: **sprawdź znaną prawdę na obrazie
przed użyciem obrazu.** Jeden `blob[0x5B35A0:0x5B35B0]` i porównanie z
prologiem z pamięci procesu wykryłoby to w pierwszej minucie. Zamiast tego
przez długi czas interpretowałem „brak sygnału" jako „brak kodu w grze".
