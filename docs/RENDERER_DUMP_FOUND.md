# PRZEŁOM: zrzut renderera działa, i obiekt jest prawdziwy

Data: 2026-09-28. v18. `renderer_dump.bin` = 12 320 B (32 B nagłówek +
12 288 B payload, 3 pełne strony).

## Obiekt jest tym, który myślałem

```
+0x000  0x00007FF755071D80   ← vtable, dokładnie ta z logu RTTI (0xA81D80 + base)
+0xE40  0x000001A5C8229EB0   ← swapchain, zgadza się z logiem
+0xEF8  0x000001A5C7FE8760   ← tablica, czytana przez beginFrame
+0x1128 0x0000000000003F48   ← liczba slotów ringu, czytana przez endRender
```

RTTI → vtable → obiekt: spójne. Hooki `beginFrame`/`endFrame` działają, zrzut
powstaje w `endRender`, czyli w chwili gdy klatka jest realnie renderowana.
To jest pomiar, którego projekt nie miał przez cały czas.

## Ale buforów sceny nie ma

Dziesięć pól, które **muszą** być ustawione w trakcie pracy, jest zerowych:

```
+0x09C0  null   pula zasobów, używana przez endRender
+0x0B80  null   SceneImmutable, zapisywany przez 0x9C030
+0x0B88  null   offset immutable
+0x0B90  null   SceneDYNAMIC, zapisywany przez 0x9C030   ← cel
+0x0B98  null   SceneUpdatable, zapisywany przez 0x9C030
+0x0DA8  null   licznik czytany przez beginFrame
+0x1250  null   wskaźnik slotu render targetu (0x5B1B70)
+0x1320  null   maska render targetów (0x5B1B70)
+0x0948  null   bufor bindowany przez 0x5df210 w 0x087F40
+0x0B08  null   bufor bindowany przez 0x5df210 w 0x087F40
```

Cały blok `+0xB60..+0xBB8` zerowy. 10 z 10.

To jest spójne z `bind=0` i `entry=0`. Nie są to trzy niezależne porażki —
to jedna prawda: **ścieżka `0x9C030` / `0x087F40` nigdy nie była wykonywana w tej
sesji.** Buforów sceny w tym obiekcie po prostu nie ma.

## Co to oznacza dla całej analizy

Wszystkie adresy buforów pochodzą z **statycznej analizy funkcji, które się nie
wykonują**. `0x9C030` „tworzy SDB pod +0xB90" — prawda w disassembly, fałsz w
runtime. `0x087F40` „czyta +0xB90" — prawda w disassembly, fałsz w runtime.

Nie ma błędu wOffsets — są błędy w założeniu, że te funkcje należą do
`TRendererD3D12`. `0x9C030`, `0x087F40`, `0x5df000` operują na innym obiekcie,
z innym układem pól, gdzie `+0xB90` to co innego. Funkcje z renderer band
`0x5B0000..0x5C0000` należą do `TRendererD3D12` i tego obiektu; te z `0x90000`,
`0x900000` — do czegoś innego.

## Co dalej — i dlaczego to jest wreszcie do zrobienia

Mam teraz działający pomiar, więc strategia się zmienia zasadniczo: szukać
adresów w disassembly nie ma sensu, dopóki nie wiadomo, która klasa jest
aktywna. Zamiast tego:

1. **Zrzucić więcej kontekstu.** Obecny dump to 12 KB `this`. `+0x9C0` i okolice
   zerowe sugerują, że prawdziwe bufory leżą dalej w obiekcie. Rozszerzyć
   zrzut, albo — lepiej — zrobić zrzut z `beginFrame`, gdzie `call 0x511FA0`
   alokuje i zapisuje pod `+0xF70`.
2. **Pobrać RTTI z vtable w runtime**, nie z logu, i wypisać pełną listę klas
   silnika. Wtedy wiadomo, do której klasy należy `0x9C030` — a przez to
   gdzie naprawdę mieszka SDB.
3. **Zliczyć, które funkcje z renderera są realnie per frame.** Zamiast
   zgadywać, zrobić licznik na kilku kandydatach (`0x511FA0`, `0x6997C0`,
   `0x5EB8C0`) i odczytać z logu, co faktycznie leci co klatkę. To jest
   lista, której projekt potrzebował od początku.

Punkt 3 jest najtańszy i daje odpowiedź na pytanie, które od kilku rund
dominuje: **co woła się co klatkę.**
