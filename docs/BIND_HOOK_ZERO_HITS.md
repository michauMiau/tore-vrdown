# 0x087F40 nie działa w sesji gry — ale to nie jest błąd hooka

Data: 2026-09-28. v17.

## Wynik testu

```
[VR] SDB bind: call at 00007ff7546787b0 (desc ctor 00007ff754bcf000)
[VR] SDB bind hook installed (5 bytes)
[VR] renderer=000001656bcba320 swapchain=000001656c0f2f20 (this+0xE40)

frame 13200  ...  sdb:bind=0 res=0000000000000000 desc=0000000000000000
```

Hook zainstalowany pod **dokładnie** tym adresem co offline test przewidział
(RVA `0x887B0`), proces stabilny, 13 200 klatek, zero trafień.

## Dowód, że to nie jest problem patchowania

`is_bind_conditional.py` rekonstruuje bloki podstawowe wstecz od bindu:

- blok zaczyna się pod `0x088662`
- **zero rozgałęzień** między `0x088662` a `0x0887B0`
- żadna gałąź w oknie `[0x088600, 0x0887B0]` nie omija tego miejsca

A w tym samym prostoliniowym bloku, osiem linii wyżej:

```asm
0x08869B  mov  rcx, r15
0x08869E  call 0x9E130        ← buduje + wgrywa 500-bajtowy SDB
0x0886A3  mov  eax, [r15+0xA18]
   ...   dzielenia, tworzenie floatów na [rsp+0x40..0x6C]
0x088788  mov  rdx,[r15+0xB98] ; call [rax+0x128]   (r9d=0x30)
0x0887A5  mov  rdx,[r15+0xB90] ; lea rcx,[rbp-0x68]
0x0887B0  call 0x5DF000        ← bind SDB
```

Bind i `0x9E130` są w jednym bloku bez rozgałęzień. Jeśli bind nie odpala,
`0x9E130` też nie — a to jest **niezależne potwierdzenie** starego wyniku
`entry=0` z v13. Dwa niezależne hooki, w tej samej gałęzi sterowania, oba zero.

To zamyka wniosek: `0x087F40` nie jest wołane w sesji gry. Hooki są poprawne,
offline test przechodzi, bajty się zgadzają. Po prostu ta funkcja nie jest na
ścieżce, którą przechodzi normalna rozgrywka.

## Co to znaczy dla strategii

Cała gałąź analizy oparta na „0x087F40 to per-frame" jest błędna. Była
oparta na trzech przesłankach, z których każda okazała się niesprawdzalna:

1. „wołana z pętli" — statyczny caller chain, nie pomiar
2. „czyta +0xB90" — prawda, ale nie dowodzi, że funkcja działa
3. „5 uploaderów wewnątrz" — prawda statycznie, zero trafień dynamicznie

Do rozstrzygnięcia potrzebny jest pomiar, nie analiza. Kolejny krok: zamiast
kolejnego hoka po adresie, **licznik wywołań na `0x070B20`** i `0x071CE0` oraz
sprawdzenie, czy w ogóle wchodzą. Jeśli `0x087F40` nie działa, to albo
`0x070B20` też nie, i wtedy cały ten podgraf to kod inicjalizacji sceny
wywoływany przy wejściu do poziomu — i cała ścieżka per-frame jest gdzie
indziej.

Alternatywnie i prawdopodobnie taniej: `strace`-podobne podejście przez
`Present`. `endRender` jest na pewno per frame i znamy go z vtable. Licznik na
jego wywołaniu plus zrzut `this` pozwoli przeszukać pamięć renderera w
poszukiwaniu 500-bajtowego bufora **w chwili, gdy klatka jest renderowana** —
czyli wtedy, gdy istnieje. Wcześniej szukałem w momentach, gdy go nie było.
