# Stan prac nad buforem sceny — 2026-09-26, po sesji z sondą

## Co działa

- Hook RTTI/vtable na żywej grze, stabilnie, przeżywa tysiące klatek
- Kodek d3d12 działa w sesji 1
- Odczyt rozpakowanego kodu z procesu — metoda, która odblokowała wszystko
- RTTI przeżywa aktualizacje gry

## Co zostało ustalone o buforach

`renderer+0x1130` to ring constant bufferów. Potwierdzone ze wzorca powtarzającego
się kilkanaście razy w `beginRender`:

```
lea  rcx, [reg + 0x1130]     ; ring
mov  r8d, N                  ; rozmiar slota
mov  rdx, źródło
call 0x5BB2B0                ; zapisz slot
```

Sloty `0x40` = jedna `float4x4`. Jest ich trzy.

## Dlaczego skan pamięci nie zadziałał

Ruch jest jednokierunkowy. Gra zapisuje do ringu przez `0x5BB2B0` i nie
przechowuje kopii — do odczytu czasu slot jest już zużyty. Zawartość pod
`0x1140` to zera i NaN, czyli nagłówek, nie dane.

To nie jest do obejścia lepszym skanem. To zmiana metody.

## Odkryte struktury

Podfunkcja od `0x5B3DE0`:

```
0x5B3DF6  mov  rbp, [rdx + 0x38]      ; obiekt z argumentu
0x5B3E21  mov  rdi, [r8 + rax*8 + 0x38] ; z tablicy przekazanej przez wywołującego
0x5B3E3E  mov  eax, [r8]              ; dwa DWORD-y, nie macierz
0x5B3E45  mov  eax, [r8 + 4]
0x5B3E49  mov  r8d, 0x20
0x5B3E69  call 0x5BB2B0
```

Te sloty `0x40` to **nie** macierze widoku. `r8` wskazuje na parę DWORD-ów
(id, licznik). To są parametry draw calli, nie kamery.

Trzy osobne miejsca z `r8d = 0x40`, które wcześniej uznałem za trzy macierze,
okazały się innymi rzeczami. Moje rozpoznanie po samym rozmiarze slota było
przedwczesne — rozmiar mówi, ile bajtów, nie co to jest.

## Wniosek: gdzie szukać view-projection

`mubVpMatrix` nie jest w ringu constant bufferów jako osobna struktura. Silnik
przekazuje go prawdopodobnie przez inny mechanizm — globalną tablicę
uniformów albo osobny bufor powiązany z kamerą. Szukanie go w ringu to
przeszukiwanie złego miejsca.

Najbliższa pewna droga: znaleźć w kodzie **konstrukcję** macierzy
view-projection, a nie miejsce jej przechowywania. Wystarczy złapać instrukcję
przedziału, która wykonuje `mul` na dwóch macierzach 4×4 — iloczyn
view × projection. Wtedy znamy i adres, i moment, i obie składowe.

## Czego nie zrobiłem

Nie napisałem jeszcze hooka na `0x5BB2B0`. Pierwsza wersja (`build/bindwatch.c`)
używała gołego trampoliny w zręcznej asemblerze i jej nie zbudowałem ani nie
przetestowałem — nie chcę wstrzykiwać kodu, którego nie rozumiem, do
procesu, w którym użytkownik pracuje. Kod zostaje jako notatka, nie jako
artefakt.

## Następny krok

Analiza statyczna zrzuconego kodu w poszukiwaniu `mul` na macierzach 4×4.
Mam 64 KB rozpakowanego kodu renderera, to wystarczy. Zero ryzyka dla gry.
