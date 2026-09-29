# Sesja 2026-09-26, ciąg dalszy — po pełnym obrazie

## Metody, które dały wynik

**`find_mat4_stores.py`** — szuka czterech kolejnych zapisów 16-bajtowych pod tę
samą bazą z przesunięciami 0/0x10/0x20/0x30. W całym obrazie: **20 trafień**.
To realne zawężenie z 33 MB do dwudziestu miejsc.

**`find_matmul_exact.py`** — wymaga łańcucha broadcast (`shufps`/`unpck*`) →
`mulps`/`mulss` → `addps` przed zapisem, co najmniej dwa razy. W całym obrazie:
**2 trafienia**.

## Oba trafienia to fałszywe

`0x2262B0` i `0x2262D0`:

```asm
xorps   xmm0, xmm0
mov     rax, rcx
movups  [rcx],      xmm0
movups  [rcx + 0x10], xmm0
movups  [rcx + 0x20], xmm0
movups  [rcx + 0x30], xmm0
ret
```

To `ZeroMemory`. Żadnego mnożenia. Moje kryterium „łańcuch przed zapisem" znalazło
`shufps` z dekodowania sąsiedniej instrukcji — czyli błąd pozycjonowania w
oknach `0x1000` z nakładką `0x40`, gdzie instrukcja przekracza granicę okna.

**Wniosek: `movups` cztery razy pod rząd to w tym obrazie prawie zawsze zerowanie
albo kopiowanie, nie wynik mnożenia.**

## Poprzednie kandydaty odrzucone

| rva | co to było |
|---|---|
| `0x51E7B0` | fizyka, transformacja AABB, `rcpps` na 4 torach |
| `0x197288` | kopiowanie macierzy w pętli kolejki, `ctx_ops=15` |
| `0x81A70` | sześć zapisów, `ctx_ops` 0–1 |
| `0x40B5C0` | parser właściwości materiałów z tablicą skoków |

## Wniosek po sesji

**Mnożenie macierzy 4×4 w tym obrazie nie występuje w formie, którą widać
statycznie.**

To jest realna informacja, nie porażka. Te trzy hipotezy:

1. **Kompilator inline'uje mnożenie i używa `mulss` + `addss` zamiast SIMD**, więc
   brak `mulps` nic nie mówi. Teardown ma włączony AVX2 (w obrazie są prefiksy
   `C5 FA`), więc to realne.
2. **Macierze przychodzą z pliku zasobu** i nigdy nie są mnożone w kodzie — silnik
   może budować VP w shaderze albo liczyć na CPU w kodzie, którego nie szukam
   (render thread, inny moduł).
3. **Mnożenie jest w kodzie shaderów** — pliki `.tde` trzymają je jako zasoby,
   a `Compute shader support: true` potwierdza, że rendering idzie przez compute
   shadery. Wtedy VP liczy GPU i w pamięci procesu go nie ma w postaci, której
   szukam.

Hipoteza 3 jest najbardziej prawdopodobna i zmienia strategię: **zamiast szukać
mnożenia, trzeba znaleźć dane wejściowe** — pozycję kamery i parametry
perspektywy, czyli `view` i `proj` osobno. Te są zwykłymi strukturami, które da się
rozpoznać bez analizy kodu.

## Stan narzędzi

Wszystko offline, cały obraz lokalny w `logs/image/`:

- `simd_scan2.py` — dekodowanie oknami, 273 klastry
- `find_mat4_stores.py` — 20 zapisów float4x4
- `find_matmul_exact.py` — 2 fałszywe trafienia
- `dis_fn.py` — wejście funkcji przez padding
- `find_caller_of.py` — wywołujący przez skan `E8 rel32`

Następny krok: szukać osobno struktury `view` i `proj` w pamięci procesu, z
charakterystyczną wartością `proj[0][0]` zależną od FOV, zamiast czekać na
iloczyn.
