# Renderer: obiekt, tabela uchwytów i obalone założenia

Data: 2026-09-27. Wszystko poniżej odczytane z żywego procesu PID 15452,
sesja 1, RTX 4070, D3D12.

## 1. W obrazie są dokładnie 2 obiekty z vtable `TRendererD3D12`

Skan pamięci za wskaźnikami równymi `image_base + 0xA81D80`:

```
  000001f2de737528      <- żywy, w stercie
  00007ffe7e9a7048      <- rekord metadanych, nie obiekt
```

Rekord metadanych sam opisuje mod i potwierdza RVAs:

```
  [00] 000000000204E000   SizeOfImage
  [08] 00007ff755071D80   vtable
  [10] 0000000000A81D80   RVA vtable
  [18] 00007ff754BA35A0   beginRender
  [20] 00007ff754BA6E50   endRender
  [38] 0000000000000001   licznik
```

Czyli `beginRender=0x5B35A0` i `endRender=0x5B6E50` potwierdzone niezależnie.
Ten rekord to prawdopodobnie tabela zbudowana przez `vtable_resolve.h`, a nie
obiekt silnika — nie należy go dalej analizować.

## 2. Poprawka o 8 bajtów

`vtobj` zapisał `found = q - 8`, gdzie `q` wskazywał vtable. Żywy obiekt ma
więc vtable **pod `0x1F2DE737530`**, czyli dokładnie tam, gdzie zaczyna się
wartość publikowana przez `VR_GetRenderer`.

Wniosek: `VR_GetRenderer()` zwraca prawidłowy początek obiektu. Każdy wcześniejszy
odczyt zrobiony z `renderer - 8` był przesunięty, ale odczyty z `renderer`
(beginRender, `+0xDA8`, `+0xEF8`) były poprawne.

## 3. Tabela 40 uchwytów, stride 0x18

Od `renderer+0x8D0` do `renderer+0xCF0`, dokładnie 40 wpisów co `0x18` bajtów.
W każdym **tylko pierwsze qword** jest niezerowe, reszta wpisu zerowa:

```
  +0x0900 = 00007ffe00000000
  +0x0918 = 00007ffe00000000
  ...
  +0x0B88 = 00007ffe00000000
  +0x0BA0 = 00007ffe00000000
  ...
  +0x0CF0 = 00007ffe00000000
  +0x0CF8 = 00007ffe00000002
```

Wszystkie mają tę samą wartość. `0x7FFE00000000` to zakres GPU virtual address,
nie adres CPU — `VirtualQuery` zwraca „not committed". **To nie są wskaźniki do
czytania z CPU.**

### Istotna niespójność

`+0xB90` **nie leży na tej siatce.** Najbliższe wpisy to `+0xB88` i `+0xBA0`.
Zapis `[rbx + 0xB90]` ze ścieżki create (`0x9C137`) wypada więc w środek wpisu,
co oznacza, że siatka `0x18` i offset `0xB90` dotyczą **różnych obiektów**
albo różnych baz. To wyklucza interpretację, że tabela uchwytów to lista
`SceneDynamicBuffer`.

## 4. Obalone założenie: `renderer+0x1130` to NIE uploader

To jest najważniejsza poprawka tego dnia.

Disassembly pokazuje w `beginRender`:

```
  0x005B37D5  lea  rcx, [rdi + 0x1130]
  0x005B37DC  mov  edx, dword ptr [rdi + 0x1128]
  0x005B37E2  call 0x5BB2D0
  0x005B3A76  lea  rcx, [rsi + 0x1130]
  0x005B3A85  mov  r8d, 0x40
  0x005B3A8B  mov  rdx, rdi
  0x005B3A8E  call 0x5BAEB0
```

oraz w `endRender`:

```
  0x005B6E7F  lea  rcx, [rbx + 0x1130]
  0x005B6F77  lea  rax, [rbx + 0x1180]
  0x005B7004  mov  qword ptr [rbx + 0x1170], r8
  0x005B723C  inc  dword ptr [rbx + 0x1128]
```

Pasek `+0x1100..+0x1170` jako little-endian bajty:

```
  +0x1100  88 83 73 de f2 01 00 00  ..s.....     wskaźnik
  +0x1108  60 86 73 de f2 01 00 00  `.s.....     wskaźnik
  +0x1110  2f 01 39 34 00 00 10 00  /.94....     "/194"
  +0x1118  00 00 00 00 2c 30 78 30  ....,0x0     ",0x0"
  +0x1120  06 00 00 00 07 00 00 00  ........
  +0x1128  60 08 2b 79 f3 01 00 00  `.+y....     wskaźnik
  +0x1130  db 61 02 00 32 30 30 33  .a..2003     "2003"
  +0x1138  01 30 78 30 db 61 02 00  .0x0.a..     "0x0"
  +0x1140  00 00 00 00 ff 01 00 00  ........
  +0x1148  a0 e8 cd 39 f4 01 00 00  ...9....     wskaźnik
  +0x1158  a0 9f 01 35 f4 01 00 00  ...5....     wskaźnik
```

`+0x1110`, `+0x1118`, `+0x1130`, `+0x1138` zawierają **tekst ASCII**. To nie jest
struktura uploadera, tylko tabela stringów (numeracja `0x261DB`, `0x261DB`
powtarza się na obu końcach, a `,0x0` i `0x0` wyglądają jak fragmenty formatowania
liczb). Silnik trzyma tam wewnętrzną tablicę nazw — co jest zgodne ze ścieżką
create, gdzie nazwa bufora `Renderer::SceneDynamicBuffer` idzie jako `lea r9, [rip+…]`.

Wniosek: `+0x1130` to początek pod-obiektu stringów/ID, a `0x5BAEB0` to metoda
 tego obiektu, nie uploadera. Wcześniejszy wniosek „uploader w `+0x1130`" jest
**błędny** i został obalony.

## 5. Co `0x5BAEB0` robi naprawdę

Pełna funkcja: 237 instrukcji, `0x5BAEB0..0x5BB1AD`. W środku:

```
  shr  eax, 8      and eax, 0x7FF
  shr  ecx, 0x1F   shr edx, 0x13   and edx, 0x7FF
  imul edx, ecx    imul eax, [r10] ... imul edx, ecx
  rep stosd        (zero-fill)
```

To **dekodowanie chunków DXBC** — sygnatury DXBC mają dokładnie taki układ bitów
(`Token[7:0]`, `Length[27:16]`). Odtworzona wartość jest porównywana z sumą
kontrolną z `[rbp+0x10]`, `[rbp+0x1C]`, `[rbp+0x20]`, czyli walidacja strumienia
shaderów. Poprzedni wniosek „HLSL bit-pack decoder" był **prawie** poprawny;
błędna była tylko przypisana rola („uploader") i właściciel (`+0x1130`).

`0x5BA9D0` (wołane 3× z wnętrza) to wektor `uint32` o layoutcie
`{uint32 cap; int count; uint32* data; uint32 inline[N]}` z reallokacją przez
`0x505680`. To kontener, nie pamięć GPU.

## 6. Wniosek: dlaczego skan pamięci nic nie znalazł

Potwierdzone dwa niezależne powody:

1. `renderer+0x900..+0xCF0` zawiera GPU virtual addresses, których nie da się
   odczytać przez `VirtualQuery` z CPU.
2. `renderer+0x1100..+0x1170`, w tym `+0x1130`, to tabela stringów, a nie
   pod-obiekt uploadera z podejrzeniami z poprzednich iteracji.

Nie ma dowodu, że istnieje jakikolwiek CPU-side blok 500 bajtów zgodny z layoutem
`SceneDynamicBuffer`. Layout z HLSL jest wiarygodny co do semantyki, ale jego
egzystencja jako jednego ciągłego obiektu w pamięci CPU **nie została potwierdzona
i jest obecnie negatywnie przesłaniana**.

## 7. Poprawiony plan

Skoro tabela uchwytów to GPU VA, a `+0x1130` to stringi, to prawidłowy cel to
**sloty vtable, nie pola obiektu**. W obrazie nie ma ani jednego wywołania
`UpdateSubresource` (0x38), `WriteToSubresource` (0x48) ani `CopyBufferRegion`
(0x50) — patrz `find_vtable_slots.py`. To znaczy, że `call [rax+0x120]` przy
`0x9C129` nie jest metodą D3D12, tylko własną klasą-wrappera silnika.

Kolejne kroki, w kolejności:

1. **Zidentyfikować klasę właściciela** funkcji `0x9C030` przez RTTI, tak jak
   zrobiono dla `TRendererD3D12`. Nazwy klas są w obrazie; trzeba znaleźć pełny
   łańcuch COL od `0x9C030` do jego klasy.
2. **Sprawdzić, co `rcx` przy `0x8368A` realnie zawiera** — jedyny caller
   `0x9C030`. Nie przez detour, tylko przez odczyt statyczny: co jest w `rdi` tuż
   przed call-site, i czy to ten sam obiekt co `VR_GetRenderer()`.
3. **Odstawić detours na `+0x1130`** — to ślepa ścieżka.
4. Rozważyć, że `mubVpMatrix` jest budowany na stosie i uploadowany w kawałkach,
   wtedy jedynym celem jest moment uploadu, a nie adres w pamięci.

## 8. Stan bezpieczeństwa

- `vpwrite.dll` **wycofany** — trampoliny wskazywały na załadowane (zpatchowane)
  entry. Naprawione w źródle `build/vpwrite.c`, binarka pozostaje niebezpieczna.
- Wszystkie buildy diagnostyczne z tej serii są **read-only**: żaden nie patchuje
  vtable ani kodu gry, więc nie kłócą się z hakiem własnego moda.
- Gra żyje od 23:0, sesja 1, ~561 MB, mod `teardown_vr3.dll` załadowany.
