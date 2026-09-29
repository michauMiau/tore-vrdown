# SDB writer: funkcja 0x9E130 NIE jest w pętli renderu

Data: 2026-09-27. Obala poprzedni wniosek z `SDB_WRITER_FOUND.md`.

## Co ustalono wcześniej (poprawnie)

`0x9E130` rzeczywiście buduje 500-bajtowy `SceneDynamicBuffer` i wgrywa go przez
`UpdateSubresources` na `call [rax+0x128]`. Kod jest poprawny, layout potwierdzony,
`mubProjectionData` liczone zgadza się z HLSL.

## Co jest nie tak

**Ta funkcja nie jest wywoływana co klatkę.** Dowód — dwa niezależne haki
w `teardown_vr13`:

| hak | adres | trafienia po 6600 klatkach |
|---|---|---|
| wnętrze — `call [rax+0x128]` | `0x9E4DD` | **0** |
| wejście funkcji | `0x9E130` | **0** |

Oba haki zainstalowały się poprawnie, bajty się zgadzały
(`target bytes: 48 89 5C 24 08 48 89 7C`), proces żył i renderował ~6600 klatek.
Zero trafień w obu. To wyklucza obie możliwe przyczyny:
- nie „zły patch" — wejście funkcji również milczy;
- nie „wyjątkowo rzadki event" — gdyby funkcja była wołana choć raz
  przy starcie, `entry` byłoby ≥ 1.

## Łańcuch wywołań

Jedyny caller `0x9E130` to `0x8869E`, wewnątrz funkcji zaczynającej się
w `0x87F40`. Ta ma **jednego** callera: `0x71102`.

```asm
0x710D0  cmp  dword ptr [rcx], eax
0x710D2  je   0x710DD
0x710D4  call 0x4060b0          ; ← test warunku
0x710D9  test al, al
0x710DB  je   0x71130           ; ← wyjście, jeśli nie
0x710F0  mov  r9, r12
0x710F3  mov  r8, qword ptr [rsi + 0x50]
0x710F7  mov  rdx, qword ptr [rsi + 0x888]
0x710FE  mov  rcx, qword ptr [r13]
0x71102  call 0x87f40
```

Warunek zależy od porównania w `[rcx]` i wyniku `call 0x4060b0`. To wygląda
na inicjalizację zasobów przy zmianie czegoś — nie na klatkę.

## Wniosek

`0x9E130` to ścieżka **tworzenia/uaktualniania zasobu**, używana przy
inicjalizacji i przy zmianach stanu (rozdzielczość, urządzenie, poziom).
Per-frame trafia inną drogą, przez tę samą tablicę `renderer+0xB80..0xB90`,
ale z innego kodu.

## Co dalej — właściwy kierunek

Szukanie dalej w `0x9E130` jest bezcelowe. Trzeba znaleźć **inną** funkcję,
która wgrywa te same 500 bajtów co klatkę. Kandydaci do przeszukania:

1. `call [reg + 0x128]` w kodzie osiągalnym z `beginRender` (`0x5B35A0`).
   `UpdateSubresources` to slot 20 `ID3D12DeviceContext`, a `beginRender`
   na pewno uploaduje bufory co klatkę.
2. Zbierz **wszystkie** miejsca w .text, gdzie `mov r9d/r8d, 0x1F4` stoi
   obok `call [reg+0x128]` — dziś szukałem tego w promieniu jednej funkcji,
   teraz trzeba w całym obrazie i sprawdzić każde trafienie.
3. Sprawdzić, czy `renderer+0xB90` ma wskaźnik do CPU-mirror w innym obiekcie.

Punkt 2 jest najbardziej obiecujący i najtańszy: sygnatura
`0x1F4` + slot `0x128` jest bardzo specyficzna, a w całym obrazie
powinna dać kilka trafień, nie tysiące.

## Uwaga metodologiczna

Ten sam błąd co wcześniej: **znalazłem funkcję, która robi coś poprawnego,
i uznałem, że to szukana ścieżka.** Tym razem wychwyciło to dopiero
podwójne haki — hak wejścia i hak w środku tej samej funkcji. Wartość
tego tricku: dwa haki na tej samej funkcji odpowiadają na pytanie
„czy funkcja działa?" i „czy patch w jej wnętrzu trafia?" oddzielnie.
