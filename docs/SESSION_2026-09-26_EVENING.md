# Sesja 2026-09-26 wieczór — wyniki i wniosek

## Co zrobiłem

1. Odczytałem zawartość ringu `renderer+0x1130` i przeszedłem po wskaźnikach
   (dwie głębokości, ~10 wskaźników). **Żadna macierz.**
2. Zawartość pod `0x1140` to zera i NaN — nagłówek, nie dane.
3. Znalazłem `SetMatrix` pod `0x5BB950`: `movups` × 3 + `movsd` przesuwane o
   `0, 0x10, 0x20, 0x30`. To kopiowanie 64 bajtów, nie budowanie.
4. Zrzuciłem **512 KB** rozpakowanego kodu (rva `0x590000`–`0x610000`) i
   przeanalizowałem: 15 216 instrukcji.
5. Szukałem konstrukcji macierzy — zero.
6. Szukałem wywołań `SetMatrix` na trzy sposoby (bezpośrednie `E8`, absolutne
   8-bajtowe, rip-relative) — **zero referencji**.

## Wniosek: okno analizy było złe

`beginRender` i `endRender` siedzą pod `0x5B35A0` i `0x5B6E50`. Moje okno
`0x590000`–`0x610000` je zawiera, ale to kod **dobywania zasobów i ringów**, nie
kod matematyki. Macierze buduje się gdzie indziej w obrazie.

Dowód: w całych 15 216 instrukcjach nie ma ani jednego `mulps`, `addps` ani
`shufps` w klastrze dłuższym niż 8 operacji. Kod renderera w tym miejscu to
zarządzanie buforami, nie grafika.

## Dwa błędy, które kosztowały czas

**Filtr wskaźników — dwa razy zły.**

1. Sufit `0x7FFFFFFF` odrzucał wszystko, bo sterta gry jest przy `0x27A...`.
2. Maska `v & 0xFFFFF0000000` — dla `0x27A3B7D8620` daje `0x27A30000000`, co
   **nie jest zerem**, więc odrzucała każdy prawdziwy adres. Poprawnie: sprawdzić
   `(v >> 48) >= 0x1000`.

Ten drugi jest podstępny — filtr wygląda sensownie i po cichu zwraca za mało
wyników zamiast się wywalać.

**Mój `plausible_vp` był za słaby.** Akceptował `m[12..15] == (0,0,0,1)` z
zeroskami i NaN, czyli niezainicjalizowaną pamięć. Trzeba wymagać: wszystko
skończone, niezerowe elementy diagonalne, niezerowy wyraz perspektywy.

## Co dalej, konkretnie

Zamiast zgadywać okno, trzeba zrobić rzecz pewną: **przejść po obrazie w poszukiwaniu skupisk operacji SIMD**. Windows pozwala przeczytać cały `SizeOfImage`, a to ~30 MB. Wyszukanie `mulps`/`shufps` w klastrach po 16+ operacji da wszystkie funkcje, które liczą na xmm — w tym konstrukcję view-projection.

To następna iteracja i jest wykonalna bezpiecznie, czytaniem pamięci.

## Czego nie zrobiłem

Nie wstrzyknąłem hooka na `0x5BB2B0`. Pierwsza wersja (`build/bindwatch.c`)
używała gołego trampoliny w zręcznej asemblerze — nie zbudowałem jej
dokładnie ani nie przetestowałem. Wstrzykiwanie niezrozumiałego kodu do procesu,
w którym użytkownik pracuje, jest złym pomysłem. Plik zostaje jako notatka.

## Uczciwe podsumowanie

Hook działa stabilnie i przeżywa tysiące klatek. Metoda odczytu kodu z procesu
się opłaciła i odblokowała analizę. Ale **bufora sceny wciąż nie znalazłem**,
a obecna sesja nie przesunęła tego do przodu — tylko ogranicza złe hipotezy.
