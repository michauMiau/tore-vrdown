# Sytuacja po skanie macierzy — 2026-09-26

## Trzy rzeczy wykluczone

**1. Mnożenie macierzy 4×4 nie występuje statycznie.**

`find_matmul_exact.py` szukał łańcucha `shufps/unpck*` → `mulps/mulss` → `addps`
przed czterema zapisami 16-bajtowymi, minimum dwa razy. W całym obrazie (33.9 MB,
8.4 mln instrukcji) wyszły **dwa** trafienia, oba `ZeroMemory`:

```asm
xorps  xmm0, xmm0
movups [rcx],      xmm0
movups [rcx + 0x10], xmm0
movups [rcx + 0x20], xmm0
movups [rcx + 0x30], xmm0
```

**2. Projekcji perspektywicznej nie ma w obrazie jako danych.**

`find_proj_const.py` + zaostrzony filtr: `m[2][3] == -1`, `m[3][3] == 0`,
`m[0][1] == m[1][0] == 0`, ogniskowa 0.2–8, FOV 20–150°.

```
tight perspective candidates: 0
```

Luźniejszy filtr dał jedno trafienie pod `0xA0E0E4`, ale to macierz transformacji,
nie projekcja:

```
[ 0.7071  0.7071 -0.7071 -0.7071]     0.7071 = 1/sqrt(2)
[-0.7071 -0.7071  0.7071  1.0000]     rotacja 45 / 135 stopni
[ 0.0000  0.0000 -1.0000 -1.0000]
[ 0.0000  0.0000  1.0000  0.8660]     m33=0.866 — projekcja nigdy nie ma takiej wartości
```

**3. Zwycięzcy gęstości SIMD to nie kamera.**

| rva | co to jest |
|---|---|
| `0x51E7B0` | fizyka, transformacja AABB, `rcpps` na 4 torach |
| `0x197288` | kopiowanie macierzy w pętli kolejki |
| `0x40B5C0` | parser właściwości materiałów |
| `0x81A70` | sześć zapisów, brak obliczeń |

## Co to oznacza

`Compute shader support: true` i brak jakiejkolwiek projekcji w obrazie razem
dają spójną odpowiedź: **view-projection liczy GPU, w compute shaderze, z
parametrów przekazanych jako struktura.** W obrazie procesu nie ma go dlatego, że
CPU go nigdy nie liczy.

To zmienia cel. Nie szukamy macierzy. Szukamy **wejścia**:

- pozycja i orientacja kamery — struktura, która trafia do stałej
- parametry perspektywy — FOV, near, far, aspect
- indeks oka — dla stereo będzie to `+0` i `+1`, czyli ten sam bufor dwa razy

Stereo nie wymaga wtedy dwóch różnych buforów. Wymaga **tego samego bufora
przepisywanego per-eye przed dispatch** compute shadera.

## Następny krok

`beginRender` ma `lea rcx, [rbx + 0xE0]` i `lea rdx, [rbp + 0x28]` przy
zapisywaniu slotów `0x40`. Te dwa adresy to kandydaci na wejście. Trzeba:

1. Znaleźć, co jest pod `[rbx + 0xE0]` — czyli co przekazuje `beginRender` do
   ringa, skąd `rbx` pochodzi.
2. Zobaczyć, czy któryś z tych wskaźników prowadzi do struktury z trzema
   floatami pozycji i czterema kwaterniona.

To jest czytanie pamięci, nie analiza kodu — a do tego mam już działający
`ReadProcessMemory` i pełny obraz lokalnie.

## Narzędzia (wszystkie offline, obraz lokalny w `logs/image/`)

- `simd_scan2.py` — dekodowanie oknami, 273 klastry SIMD
- `find_mat4_stores.py` — 20 zapisów float4x4
- `find_matmul_exact.py` — 2 trafienia, oba `ZeroMemory`
- `find_proj_const.py` — 0 projekcji
- `dis_fn.py` — wejście funkcji przez padding
- `find_caller_of.py` — wywołujący przez `E8 rel32`
