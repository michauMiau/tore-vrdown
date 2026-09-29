# beginRender to beginFrame — szukałem uploadu w złej funkcji

Data: 2026-09-27 wieczór. Przełom.

## Co zmierzyłem

Granice funkcji z `.pdata` (tabela RUNTIME_FUNCTION), nie skanowaniem wstecz:

| adres | funkcja | rozmiar |
|---|---|---|
| `0x5B35A0` | `0x5B35A0..0x5B387A` | **730 B** |
| `0x5B6E50` | `0x5B6E50..0x5B725C` | 1036 B |
| `0x9E130` | `0x9E130..0x9E4F8` | 968 B (potwierdzony uploader SDB) |
| `0x9C030` | `0x9C030..0x9C254` | 548 B (tworzenie buforów) |

`beginRender` ma **730 bajtów**. BFS przez `.pdata` do głębokości 6:
**31 osiągalnych funkcji, żadna nie woła `UpdateSubresources`.**

## Dlaczego to wszystko wyjaśnia

Wcześniejszy `analyze_beginrender.py` raportował „68 wywołań bezpośrednich
w beginRender". Nie może być prawdziwe: 730 bajtów mieści ~60–70 instrukcji,
nie 68 wywołań. Ten skrypt dekodował okno zaczynające się w `0x5B35A0` bez
końca funkcji, więc policzył wywołania z **następnych kilku funkcji** i
przypisał je `beginRender`.

To ten sam błąd klasy co tabela callee'ów z v14 (RVAs z adresów wywołań),
tylce w innej postaci: analiza okna zamiast analizy funkcji.

Więc `0x5B35A0` to `beginFrame` — robi `call 0x511fa0` (alokacja), zapisuje
`[rdi+0xCC] = 1`, `+0xF70`, `+0xEF8`, `+0xE58`. Nie zawiera uploadu i nie
powinien. Prawdziwy draw path jest gdzie indziej.

## Nowa lokalizacja

Pasmo renderera `0x5B0000..0x5C0000` ma **316 funkcji**. Największa:

    0x5BF610..0x5C0ADF   5327 bajtów

To 5,5× więcej niż potwierdzony uploader SDB i 7× więcej niż `beginFrame`.
Ścieżka rysowania z per-frame uploadem 500-bajtowego constant buffera musi być
tam, nie w `beginFrame`.

## Znaczenie dla strategii

Szukałem `+0xB90` jako pola renderera. Wynik `A ∩ B = 0` mówi, że pole **nigdy
nie jest czytane** przez `mov reg,[reg+0xB90]`. SDB idzie przez wskaźnik, więc
trzeba szukać uploaderów po 20 funkcjach wołających `UpdateSubresources`
(pełna lista w `UPLOAD_FUNCTIONS.md`), a nie po polu w rendererze.
