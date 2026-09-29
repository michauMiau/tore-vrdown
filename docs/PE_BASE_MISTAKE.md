# Przełom: baza PE to nie obiekt renderera

## Co było nie tak

Cały czas zakładałem, że adres z logu `host image 00007ff7545f0000` to obiekt
`TRendererD3D12`, i z niego czytałem pola. To **baza obrazu PE**.

Dowód, który powinienem zauważyć od razu — sweep 8192 bajtów zwrócił:

```
+0x0260  0x747865742E          <- ".text" jako ASCII
+0x0288  0x61746164722E        <- ".rdata"
+0x0300  0x637273722E          <- ".rsrc"
+0x0178  0x1696000             <- rozmiar .text
```

To są nagłówki sekcji PE, nie pola obiektu. Wszystkie wcześniejsze odczyty
`+0x1130`, `+0xDA8`, `+0xEF8` z tej bazy były bezużyteczne — i żaden nie rzucił
wyjątku, tylko zwracał sensownie wyglądające zera.

## Prawdziwy obiekt

Log hooka ma osobną linię, której nie czytałem:

```
[VR] renderer=0000027a41789f70 swapchain=0000027a852fe6f0 (this+0xE40)
```

`0x27A41789F70`. Po odczytaniu z niego:

```
+0x0000  0x7FF755071D80  image  <- vtable, zgadza się z RTTI
+0x0E40  0x27A852FE6F0  heap   <- swapchain, zgadza się z logiem
+0x0EF8  0x27A4178AE70  heap   <- tabela rekordów, NIE zerowa
```

Trzy niezależne zgodności. To jest właściwy obiekt.

## Tabela rekordów

`beginRender`:

```
movsxd rax, [rdi + 0xda8]     ; indeks
lea    rbx, [rax + rax*2]
shl    rbx, 4                  ; *48
add    rbx, [rdi + 0xef8]      ; tabela
```

Rekordy 48-bajtowe, indeks oscyluje 0/1 (podwójne buforowanie klatek). Zawartość
rekordu to wskaźniki, nie macierz:

```
rec[0] qwords: 0x27A85055570 0x689DB 0x27A417099A0 0x27A85373590 ...
rec[0] floats: 0 0 0 0  15.0375 0 0 0  0 0 0 0
rec[1] floats: 0 0 0 0  15.0372 0 0 0  0 0 0 NaN
```

Jedyny żywy float to `15.03x` — skala renderowania albo wartość czasowa, nie
ogniskowa.

## Przeszukanie rekordów: zero projekcji

`walk_records.ps1` idzie 3 poziomy w głąb z rekordów (27 regionów, 512 B każdy,
krok 4 B) i klasyfikuje każdy blok 64 B. Kryterium: `|m[2][3]| == 1 ± 0.01`,
ogniskowa 0.05–20, brak NaN i nieskończoności.

```
total candidates: 0   (searched 27 regions)
```

## Co to wszystko razem znaczy

Trzy niezależne fakty, które się wzajemnie potwierdzają:

1. Brak mnożenia macierzy 4×4 w 8.4 mln instrukcji obrazu
2. Brak projekcji perspektywicznej jako danych w całym obrazie
3. Brak projekcji w rekordach klatki i 27 regionach, do których prowadzą

Teardown liczy view-projection na GPU. `Compute shader support: true` to
potwierdza. W pamięci procesu macierzy nie ma, bo CPU jej nie tworzy.

To nie jest porażka analizy — to odpowiedź. Zmienia cel z „znajdź macierz" na
„znajdź parametry, z których shader ją liczy" oraz miejsce, w którym się do niej
dostarcza.

## Co trzeba zrobić inaczej

Stereo nie polega na podmianie jednej macierzy, tylko na **dwóch dispatchach
compute shadera z różnymi parametrami**. Dla każdego oka trzeba:

1. Ustawić pozycję kamery przesuniętą o IPD/2 wzdłuż jej osi X
2. Ustawić tę samą orientację
3. Ponownie wywołać dispatch, używając tego samego bufora wyjściowego

To oznacza, że hook musi złapać moment między dwoma dispatchami, a nie między
`beginRender` a `Present`. Prawdopodobnie najlepszym punktem jest samo wywołanie
dispatchu, którego argumenty widać w kodzie.

## Błędy, które kosztowały dziś czas

| błąd | skutek |
|---|---|
| `base` z logu = obiekt renderer | wszystkie odczyty pól odczytywały nagłówek PE |
| `v & 0xFFFFF0000000` jako test wskaźnika | odrzucał każdy adres sterty `0x27A...` |
| `md.disasm()` bez okien | gubił cały chunk po pierwszym złym bajcie |
| `TdScore` zwracał `-1` na odrzucenie | `-1 >= 0` w PowerShellu dawało fałszywe trafienia |
| `$REC` i `$rec` | ta sama zmienna — adres jako rozmiar bufora |
| `$Matches` po `Select-String` | pusty, `ToInt64` rzucało |
| `RI` alias PowerShell | kolidowało z `Remove-Item` |
| `0x2000000` limit `SizeOfImage` | obraz to 33.9 MB, o bajt za duży |
