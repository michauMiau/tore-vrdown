# Layout bufora uploadu — co naprawdę jest pod 0x1130

Data: 2026-09-26, z disassemblera żywego procesu.

## Korekta: 0x1130 to nie jest bufor sceny

Pierwsza hipoteza brzmiała: `this+0x1130` to `SceneDynamicBuffer` i wystarczy
przepisać w nim macierz. **To było błędne.** Odczyt pamięci pokazał, że pod
tym offsetem jest struktura wskaźników, a nie dane:

```
renderer + 0x1130 -> 0x27A4178B0A0
  +0x008 -> 0x1FFF00000000     <- deskryptor, nie wskaźnik
  +0x010 -> 0x27BBF360080
  +0x020 -> 0x27A26C9A010
  +0x110 -> 0x27A84EAE71F
```

Wszystkie „macierze" znalezione skanem okazały się zerami z NaN w ostatnim
elemencie — czyli niezainicjalizowaną pamięcią, która przechodziła luźny test
`(0,0,0,1)`. Zaostrzenie kryterium (wymóg niezerowych skali i wyrazu
perspektywy) odsiewa takie śmieci.

## Co 0x1130 robi naprawdę

Wzorzec powtarza się w całym `beginRender`:

```
lea  rcx, [reg + 0x1130]     ; bufor ring
mov  r8d, 0x40               ; rozmiar slota
mov  rdx, źródło
call 0x5BB2B0                ; zapisz slot
```

To **bind constant buffer ringu**, nie jeden bufor sceny. Każde wywołanie
zapisuje jeden slot o podanej wielkości.

Znalezione sloty w `beginRender`:

| adres | rozmiar | źródło |
|---|---|---|
| `0x5B3E76` | `0x40` | `rsp+0xe0` |
| `0x5B49AF` | `0x20` | `rsi+0x28` |
| `0x5B49C5` | `0x40` | `rbp+0x28` |
| `0x5B4F00` | `0x40` | `rsi` (mapy światła) |
| `0x5B4F24` | z obiektu | |

`0x40` = 64 bajty = dokładnie jedna `float4x4`.

W `beginRender` jest też:

```
lea  rcx, [rdi + 0x1130]
mov  edx, [rdi + 0x1128]     ; liczba slotów
call 0x5BB6D0                ; przebuduj ring
```

`0x1128` trzyma rozmiar/count ringu. Odkryte, nie zgadnięte.

## Odrzucona ścieżka

`0x5BB6D0` i pętla od `0x5BB610` to przetwarzanie listy zasobów, nie bufor
sceny: pętla po `ebx < [rdi+0x34]`, alokacja `0x88` bajtów, wywołania
`[rax+0x48]`. Nie prowadzi do macierzy.

## Co dalej

`0x40` w trzech miejscach oznacza trzy niezależne macierze przekazywane
do GPU. Trzeba odczytać ich zawartość i rozpoznać, która jest
view-projection — po wartościach, nie po zgadywaniu z kodu.
