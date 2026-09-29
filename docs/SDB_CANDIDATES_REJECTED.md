# SDB NIE ZNALEZIONY — te bloki to nie jest, i dlaczego

Data: 2026-09-28. v27. Obalone przez własny test formułu.

## Co znalazł skan

`teardown_vr27.log`, 10 trafień, wszystkie z tej samej dwóch obiektów
(`...BDD920` i `...DF930`):

```text
SDB ...BDD980  pd=(0.07193 0.07202 0.07214 0.07216)  maxVP=0.07
SDB ...DFBC0  pd=(0.09537 0.09537 0.09545 0.09555)  maxVP=0.09
```

Na pierwszy rzut oka wygląda to jak projekcja. Nie jest.

## Test, który to obalił

Deklaracja shaderowa jest autortatywna i nie podlega dyskusji:

```hlsl
pd.x = 1.0f / projMatrix.m[0]
pd.y = 1.0f / projMatrix.m[5]
pd.z = projMatrix.m[8] / projMatrix.m[0]
pd.w = projMatrix.m[9] / projMatrix.m[5]
```

Trzy niezależne sprawdzenia (`teardown-analysis/verify_sdb_formula.py`):

**1. `pd.x / pd.y` powinno dać aspect.** Teardown renderuje 16:9, więc
`m[0] = 1/(aspect·tan(fov/2))` i `m[5] = 1/tan(fov/2)`, a ich iloraz to
aspect ≈ 1.78. Zmierzone wartości: **0.9613 … 1.0052**. To nie jest macierz
projekcji — to liczby, które rosną razem.

**2. `maxVP` musi śledzić `1/pd.x`.** Z definicji `m[0] = 1/pd.x`, a `maxVP`
to największy element tej macierzy, więc korelacja musi być dodatnia i monotoniczna.
Zmierzone: **2 z 9 kroków rosnących**. Losowo.

**3. `pd.z` to metry, `pd.x` to 1/m.** Stosunek `pd.z/pd.x ≈ 1.0` oznaczałby
shear ~0.07 m. Żadna gra tego nie robi. Dla IPD 64 mm `pd.z` powinno być
±0.032, czyli być **mniejsze** niż `pd.x`, nie równe.

## Co to znaczy dla matchera

`td_test_candidate` przepuścił te bloki, bo sprawdza kształt, nie tożsamość.
„Liczby w rozsądnym zakresie" + „stabilna para podobna do siebie" to za mało,
żeby odróżnić projekcję od gradientu po heapie.

## Konwencja macierzy — rozstrzygnięta (teardown-analysis/which_matrix_convention.c)

Dla kanonicznej projekcji perspektywnej 16:9 z 90° FOV pionowym:

```text
1/(aspect*tan) = 0.562500
1/tan          = 1.000000
m[0] = 0.5625    m[5] = 1.0
```

**`m[0]` i `m[5]` są te same w konwencji column-major i row-major** — te dwa
elementy leżą na przekątnej, więc transpozycja ich nie rusza. Matcher nie
musi zgadywać konwencji dla tych pól. (Ma to znaczenie dla `m[8]`/`m[9]`, które
transpozycja przestawia, ale nie dla bramki formuły.)

Wniosek o wartościach, których szukamy:

```hlsl
pd.x = 1.0f / projMatrix.m[0]   ->  1.77778
pd.y = 1.0f / projMatrix.m[5]   ->  1.00000
```

Czyli prawdziwy SDB powinien mieć `pd ≈ (1.78, 1.00, ...)` — a nie obie wartości
w okolicy 0.07, jak miały wszystkie kandydaty z v25 i v27. **Żaden kandydat,
który do tej pory trafił, nie miał szansy być prawdziwym SDB.**

To zmienia sposób myślenia o skanie: szukamy `pd.x` w okolicy 1.0–3.0 i
`pd.y` w okolicy 0.5–1.5, a nie obu w okolicy 0.07.

## Stan po v28

v28 z autorytatywną bramką formuły: `TOTAL_SDB_HITS=0`, proces stabilny.
To poprawna odpowiedź, nie porażka — weryfikacja offline
(`build/test_formula.c`) potwierdza, że bramka odrzuca wszystkie 10 fałszywych
kandydatów z v27 i przepuszcza prawdziwe projekcje 16:9 i stereo.

Skoro prawdziwego SDB nie ma w dwóch skokach wskaźnikowych od renderera, to
najbardziej prawdopodobne wyjaśnienia to:
- bufor żyje głębiej niż dwa poziory (`pass2` jest przycięty do 242 obiektów),
- albo nie jest osiągalny wskaźnikiem w ogóle — jest kopiowany do GPU przez
  uploader, którego bufor źródłowy leży gdzie indziej.

**Następny krok, który eliminuje obie możliwości naraz:** nie skanować pamięci
w poszukiwaniu SDB, tylko zaczepić moment przesłania go do GPU. Każdy SDB
musi trafić do stałego bufora przez jakieś wywołanie, a w chwili tego wywołania
adres jego buforu źródłowego jest znany i czytelny z CPU. Zaczepienie
`CopyBufferRegion` albo `UpdateSubresource` na liście poleceń daje wskaźnik
bez zgadywania struktury obiektu.

## Stan niezależny od tego

`ReadProcessMemory` w `td_read` zamknął wyścig, który zabijał v24 i v25:
v26, v27 i v28 przeżywają setki przebiegów skanu bez detachu. v25 pokazał, że
`memcpy` po `VirtualQuery` pada po ~15 000 klatek — to był TOCTOU na stronie
renderującej, a nie zły wskaźnik.

`docs/HOT_COUNTER_RETIRED.md` opisuje wycofanie hot-path countera.
`docs/RENDERER_DUMP_FOUND.md` opisuje zrzut renderera.
