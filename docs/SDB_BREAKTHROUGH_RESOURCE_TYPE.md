# PRZEŁOM: SDB nie jest ID3D12Resource — silnik ma własny typ zasobu

Data: 2026-09-27 wieczór.

## Dowód

`0x087F40` czyta `+0xB90` raz i `+0xB98` dwa razy, i to jedyne miejsce w całym
obrazie (33 MB, 35 710 funkcji z `.pdata`) gdzie te pola są czytane. Każde
odczytanie idzie do `call 0x5df000`:

```asm
0x088795  mov rdx,[r15+0xB88] ; lea rcx,[rbp-0x80] ; call 0x5df000
0x0887A5  mov rdx,[r15+0xB90] ; lea rcx,[rbp-0x68] ; call 0x5df000   ; SDB
0x0887B5  mov rdx,[r15+0xB98] ; lea rcx,[rbp-0x50] ; call 0x5df000
```

Cele są 24 bajty od siebie. `0x5df000` to:

```asm
0x5DF000  mov   [rcx], rdx                 ; +0x00 resource*
0x5DF003  movzx eax, byte ptr [rdx]        ; type tag = resource->vtable[0]
0x5DF006  cmp   al, 4                      ; TEXTURE
0x5DF00A  mov   [rcx+0x14], 4
0x5DF011  mov   [rcx+0x8], 0
0x5DF018  mov   eax, [rdx+4]
0x5DF01B  mov   [rcx+0xC], eax
0x5DF022  cmp   al, 8                      ; BUFFER
0x5DF026  mov   [rcx+0x14], 0x21
0x5DF037  shr   eax, 2
0x5DF03A  mov   [rcx+0xC], eax
```

24 = `resource*` + `offset` + `size` + `type`. Silnik używa **własnego
abstrakcyjnego typu zasobu**, nie `ID3D12Resource*`. Sąsiednie warianty
(`+0x21`, `+0x11`, `+0x9` jako base type) potwierdzają, że to rodzina
konstruktorów descriptorów.

## Dlaczego to wyjaśnia porażkę szukania

`+0xB90` trzyma `Resource*` (własny typ), nie `ID3D12Resource*`. więc:

- `0x9E130` (`mov rdx,[rbx+0xB90]` → `call [rax+0x128]`) to uploader
  **wewnętrznej warstwy**, który wewnętrznie robi `GetResource` i dopiero
  potem `UpdateSubresources`. Nie szukałem go, bo szukałem pola z ID3D12.
- `mov rdx,[reg+0xB90]` nigdy nie wygląda jak `mov rdx,[rbx+B90]` przy
  porównywaniu tekstu, bo to jest offset **własnego obiektu zasobu**, nie pola
  w ID3D12Resource.
- `+0xB80`/`+0xB90`/`+0xB98` to potrójne: immutable / dynamic / updatable,
  dokładnie jak wcześniej zgadywałem z `0x9C030` — teraz potwierdzone.

## Zamknięty łańcuch wywołań

```
0x071CE0..0x074927   11 KB, sam jest UpdateSubresources callerem
  0x072F66 / 0x07305D  call 0x070B20
    0x071102           call 0x087F40        7725 B, 178 calli
      0x08869E         call 0x9E130         buduje + wgrywa 500 B SDB
      0x088788         call [rax+0x128]     UpdateSubresources, +0xB98, r9d=0x30
      0x0887A5         call 0x5df000         bind SDB do slotu renderu
```

`0x087F40` jest per-frame (wołany z pętli, 5 uploaderów, 178 wywołań). W nim
są **oba** miejsca: wgranie (`0x088788`) i bind (`0x0887A5`). To jest funkcja,
którą trzeba hookować — nie `beginFrame`, nie `endFrame`.

## Co dalej

`0x087F40` ma własny prolog: sprawdzić, czy `0x5DF000`-podobne wejście
przekracza stealable prologue, i zhookować **jego** wejście zamiast
`beginFrame`. Alternatywnie — w samym `0x087F40` podmienić adres docelowy
`call 0x5df000` przy `+0xB90`, co daje 5-bajtowy detour bez trampoliny.
