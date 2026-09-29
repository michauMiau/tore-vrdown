# BUFOR UPLOADU: offset potwierdzony, ale wypełnia go inna funkcja

Data: 2026-09-28. v31.

## Offset 0x1130 jest poprawny — moja poprzednia teza była błędna

`docs/WRONG_BINARY_FOUND.md` twierdziła, że `0x1130` pochodzi z innego
builda. To nieprawda. W rozpakowanym obrazie z żywego procesu
(`logs/text_dump.bin`, 9 941 420 B, `.text` rva 0x1000) jest dokładnie ten sam
site:

```text
005B6E7F  48 8D 8B 30 11 00 00   lea  rcx, [rbx+0x1130]
005B6E86  41 B8 00 08 00 00      mov  r8d, 0x800
005B6E8C  48 8B D0              mov  rax, rdx
005B6E8F  E8 1C 40 00 00         call 0x5BAEB0
```

Dokładnie jeden taki site w całych 10 MB `.text`, identyczny co w `steamless`.
Offset i rozmiar przeniosły się bez zmian.

## Prolog potwierdza rbx = self

```text
005B6E50  48 89 5C 24 18         mov  [rsp+0x18], rbx
005B6E55  55 56 57 41 56 41 57  push rbp,rsi,rdi,r14,r15
005B6E5C  48 83 EC 50            sub  rsp, 0x50
005B6E60  48 8B D9               mov  rbx, rcx        <-- self
005B6E63  C6 84 24 84 00 00 00 00  mov byte [rsp+0x84], 0
005B6E6B  48 81 C1 70 0E 00 00  add  rcx, 0xE70     <-- self+0xE70
005B6E72  E8 39 45 00 00         call 0x5BB3B0
005B6E77  4C 8B 8C 24 80 00 00 00  mov r9, [rsp+0x80]   <-- 4. argument
005B6E7F  48 8D 8B 30 11 00 00  lea  rcx, [rbx+0x1130]
005B6E86  41 B8 00 08 00 00     mov  r8d, 0x800
005B6E8C  48 8B D0              mov  rax, rdx        <-- 2. argument
005B6E8F  E8 1C 40 00 00         call 0x5BAEB0
005B6E94  48 8B 93 40 0E 00 00  mov  rbx, [rbx+0xE40]   <-- swapchain
005B6E9B  48 8D 8B 30 11 00 00  lea  rcx, [rbx+0x1130]
005B6EA2  E8 5A 44 00 00         call 0x5BB300
005B6EA7  48 8B 9B 40 0E 00 00  mov  rbx, [rbx+0xE40]
005B6EAE  48 8B 01               mov  rcx, [rax]
005B6EB1  FF 10                  call  [rax]          <-- Present
```

`rbx = rcx = self` jest ustalone na 0x5B6E60 i żyje do końca funkcji.
`+0xE40` to swapchain (zgodne z logiem: `swapchain=000001f251015430
(this+0xE40)`), `call [rax]` to Present w slocie 0.

## Co wypełnia 0x1130

`0x5B6E6B: add rcx, 0xE70` — wywołanie `0x5BB3B0` dostaje `self+0xE70`, nie
`self+0x1130`. **To jest rzeczywisty producent danych.** `lea [rbx+0x1130]`
przy 0x5B6E7F to dopiero **konsument**: wywołanie `0x5BAEB0` z rozmiarem 0x800
wysyła zawartość do GPU.

Rozkład jest więc taki:

```text
0x5BB3B0(self+0xE70, ...)      produkcja  -> zapewne do +0x1130
0x5BAEB0(self+0x1130, rdx, 0x800, r9)   konsumcja -> upload do GPU
```

`self+0xE70` i `self+0x1130` to 0x8C0 bajtów od siebie, co mieści się
w buforze 0x800. Najbardziej prawdopodobne: `0xE70` to licznik uploaderów albo
rekord, a `0x1130` to staging, do którego uploader zapisuje w trakcie.

## Co to zmienia

v31 czyta `renderer+0x1130` **po** `g_orig_end` i nadal dostaje wskaźniki
(`read=2048 shape=767 formula=0`). Dwie możliwości:

1. `+0x1130` to staging, który `0x5BAEB0` opróżnia po wysłaniu, więc po
   powrocie z `endRender` jest pusty. Wtedy trzeba czytać **w trakcie** —
   czyli zaczepić `0x5BAEB0`, nie czytać po nim.
2. `+0x1130` nie jest buforem SDB, tylko innym buforem uploadu, a SDB leży
   wskazywany przez `+0xE70`.

**Następny krok jest jednoznaczny:** zaczepić `0x5BAEB0`. Argumenty są w
rejestrach (`rcx` = `self+0x1130`, `rdx` = coś z `rax`, `r8d` = 0x800,
`r9` = wskaźnik ze stosu). W chwili wywołania bufor zawiera dokładnie to, co
idzie do GPU, i można go odczytać i zweryfikować formułą.

To jest jeden 5-bajtowy patch call-site, nie detour funkcji — ta sama technika
co `scene_bind`, która już działa. Różnica: zamiast liczyć wywołania, patch
podmienia `lea` na `lea` do własnej trampoliny, która robi odczyt i wraca.

## Teza do zweryfikowania

Jeśli `+0x1130` jest stagingiem opróżnianym po uploadzie, to odczyt w
trampolinie pokaże prawdziwy SDB. Jeśli nadal puste, to `+0xE70` wskazuje
gdzie indziej i trzeba śledzić `0x5BB3B0`.

## Narzędzia dodane

- `build/dump_text.ps1` — zrzut całego rozpakowanego `.text` z procesu
  (9.9 MB), parsuje nagłówki PE z pamięci, baza z logu moda.
- `teardown-analysis/find_upload_live.py` — wyszukiwanie sygnatury uploadu
  w zrzucie, plus mapa disp32 endRender.
- `teardown-analysis/dis_endrender.py` — dekoder (własny, przyzwoity, ale
  `mov [rsp+xx],rbx` i kilka REX-kombinacji mu nie wychodzi; bajty są
  czytelne ręcznie, więc na razie to nie przeszkadza).
- `teardown-analysis/which_matrix_convention.c` — potwierdza, że `m[0]` i
  `m[5]` są takie same w column-major i row-major.
