# Ring pod 0x1130 — co to faktycznie jest

Data: 2026-09-26, pomiar na żywym procesie.

## Wynik: to nie jest miejsce na macierz

Odczytałem zawartość ringu i wszystkich wskaźników, które z niego wychodzą.
Żadna macierz nie przeszła nawet zaostrzonego kryterium, a surowe wartości
pokazują dlaczego.

`renderer+0x1140` → `0x27BBF360080`:

```
[    0.00000     0.00000    -0.00000     0.00000]
[        nan     0.00000     0.00000     0.00000]
[    0.00000     0.00000     0.00000     0.00000]
[        nan     0.00000     0.00000     0.00000]
```

To struktura wskaźników z sentinelami, nie `float4x4`. Zera i NaN to
nieutworzony bufor.

`renderer+0x1120` → `0x27A3B7D8620` — same duże liczby, wyglądające jak pary
(counter, 1) w niskich bitach. Też nie macierz.

## Podsumowanie struktury

| offset | wartość | interpretacja |
|---|---|---|
| `+0x1120` | `0x27A3B7D8620` | kolejny obiekt, nie dane |
| `+0x1128` | rośnie z każdą klatką (`0x21250` → `0x2453C`) | licznik, nie slot count |
| `+0x1130` | `0x2453C00000001` | para (licznik, 1) |
| `+0x1138` | `0x1FFF00000000` | wartownik |
| `+0x1140` | `0x27BBF360080` | wskaźnik, wskazuje na nagłówek z zerami |

**`0x1128` to nie liczba slotów.** Rosnąca wartość to frame counter, nie rozmiar
ringu. Moje wcześniejsze stwierdzenie było zbyt pewne.

## Filtr wskaźników — dwa błędy, oba moje

Warto uczyć się na obu, bo kosztowały dwa przebiegi:

1. **Za niski sufit.** `v > 0x7FFFFFFF` odrzucał wszystko, bo sterta gry jest
   przy `0x27A...` / `0x27B...`, czyli powyżej 4 GB.
2. **Zła maska.** `v & 0xFFFFF0000000` dla `0x27A3B7D8620` daje
   `0x27A30000000`, co **nie jest zerem** — maska 48-bitowa odrzuca każdy
   prawdziwy adres. Poprawne jest sprawdzenie górnych 16 bitów:
   `(v >> 48) >= 0x1000` oznacza wartownika `0x1FFF________`.

Ta druga jest podstępna, bo filtr *wygląda* poprawnie i po prostu zwraca za
mało wyników, zamiast się wywalać.

## Dokąd teraz

`0x1130` jest ringiem **constant bufferów** — to potwierdzone z kodu, wzorzec
`lea [reg+0x1130]` + rozmiar + `call 0x5BB2B0` powtarza się kilkanaście razy.
Dane trafiają tam przez `0x5BB2B0` z adresu podanego w `rdx`.

Nie da się znaleźć macierzy skanując ring, bo ruch jest jednokierunkowy:
gra zapisuje do niego przez `0x5BB2B0` i zapomina. Trzeba podejść od drugiej
strony — z argumentów wywołania `0x5BB2B0`, czyli z tego, co gra podaje jako
`rdx` w chwili bindowania.

Alternatywnie i prościej: zaczepić `0x5BB2B0` i zalogować `rdx` w chwili, gdy
`r8d == 0x40`. To daje dokładnie te trzy macierze, o które chodzi.
