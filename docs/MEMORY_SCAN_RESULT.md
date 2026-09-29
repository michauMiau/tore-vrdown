# Skan pamięci — wynik negatywny, 2026-09-27

## Co przeskanowano

Trzy wersje skanera, wszystkie **read-only** (zero patchowania kodu gry):

| build | zakres | wynik |
|---|---|---|
| `sdb_live` | 787 MB, 2778 regiony | 158158 luźnych macierzy, 0 ścisłych |
| `sdb_probe2` | 1.97 GB, 4224 regiony | 555144 kandydatów |
| `sdb_probe4` | 1.99 GB, 4248 regiony | 591443 kandydatów |

## Testy relacji i ich liczebność

```
A  proj.r == 1/m[0]              6645
B  proj.g == 1/m[5]             10217
C  oba A i B                     1335
D  0x40 to wektor jednostkowy    16797
E  0x40/0x50/0x60 ortonormalne       8   <- jedyny dyskryminator
```

**E daje 8 trafień na 555144.** Ortogonalna baza kamery jest tym, co odróżnia
bufor sceny od wszystkiego innego. Ale wszystkie 8 to śmieci:

```
0x1F2DEF2E6E0  proj=13.3971 -27.1664 8.84323 1   xScale=1.00039 yScale=1.77778
0x1F379BE8E60  proj=0 0 0 1                               xScale=1  yScale=1
0x1F3C1BD8CA0  proj=0 0 0 1                               xScale=0.003125
0x7FF7551DF540 proj=0 0 1 -1                              xScale=1.73205
0x7FF7551DF870 proj=0 0 1 -1                              xScale=1.73205
0x7FF7551DFA50 proj=0 0 1 -1                              xScale=1.97231
0x7FF755230040 proj=0 0 1 -1                              xScale=1.41421
0x7FF7552303A0 proj=0 0 1 -1                              xScale=1.41421
```

Sześć z nich ma `proj = 0 0 1 -1` i `xScale` = √2 lub √3 — to **statyczne dane
w .rdata obrazu gry** (adresy `0x7FF7551D…` są wewnątrz modułu teardown.exe).
Dwa pozostałe mają `proj=0 0 0 1`, co nie spełnia relacji `A`.

Żaden nie ma `mubProjectionData` zgodnego z `1/m[0]`. Ten jeden z
`xScale=1.00039` ma `proj.r = 13.3971`, a `1/1.00039 = 0.99961` — rozbieżność
o rząd wielkości, czyli to przypadek.

## Wniosek

`SceneDynamicBuffer` **nie istnieje jako 500 bajtów ciągłych w pamięci
odwzorowanej na CPU**. Dwa możliwe powody:

1. Jest w `MEM_MAPPED` z innym `PAGE_*` niż `PAGE_READWRITE` — skaner celowo
   odfiltrowywał tylko `PAGE_READWRITE`, a `MapViewOfFile` z D3D12 upload heap
   daje zwykle `PAGE_READWRITE`, więc to raczej nie.
2. **Engine zapisuje bezpośrednio do GPU-visible pamięci przez `WriteToSubresource`**
   albo przez mapped ring buffer, a `mubVpMatrix` nigdy nie istnieje jako
   kompletny blok w jednym miejscu przez dłuższy czas — jest budowany i
   uploadowany w kawałkach.

## Wniosek operacyjny

Nie da się znaleźć bufora skanem. Trzeba **zaczepić moment uploadu**. Kolejne
podejście musi:

- znaleźć wywołanie `ID3D12Resource::UpdateSubresource` lub `CopyBufferRegion`
  na zasobie `SceneDynamicBuffer` (vtable sloty `0x38`/`0x50`/`0xA0`),
- albo znaleźć funkcję, która wypełnia `float4x4` przed `call [rax+0x120]`.

Wszystkie dotychczasowe detours (`ownerwatch` na `0x8368A`, `vpwrite` na
`0x5BAEB0`/`0x5BAD50`, `vpprobe` na `0x5B3A8E`) nie znalazły macierzy:

- `0x8368A` — init buforów, działa tylko przy ładowaniu poziomu
- `0x5BAEB0` / `0x5BAD50` — dekodery bitów HLSL, `shr eax,8` / `and eax,0x7ff`
- `0x5B3A8E` — `r8d=0x40`, ale call-site nie jest osiągany w tej scenie

## Koszt

`vpwrite` zainstalował dwa patche na nieużywanych funkcjach, z trampolinami
wskazującymi na **załadowaną** (czyli zpatchowaną) adres — latentna
nieskończona rekurencja, gdyby ktokolwiek je zawołał. Naprawione w źródle,
ale `vpwrite.dll` był załadowany w procesie PID 1336, który padł.

**Wniosek operacyjny: nie patchować kodu gry funkcjami, które nie zostały
potwierdzone jako wywoływane.** Do diagnostyki używać skanowania read-only.
