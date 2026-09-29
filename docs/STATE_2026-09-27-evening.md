# Stan po 2026-09-27 wieczór: dwa błędy, które sam wprowadziłem

## 1. Sortowanie chunków obrazu (szczegóły w `IMAGE_SORT_BUG.md`)

Nazwy `img_*.bin` to **hex**, sortowałem tekstowo. `img_1000000.bin` (16 MB)
lądowało przed `img_200000.bin` (2 MB), więc `.text` w 2–14 MB — gdzie jest
`beginRender` i `endRender` — czytałem jako zera. **30 skryptów** dotkniętych.

Weryfikacja po naprawie: `0x5B35A0` = `48895c241848896c2420565741564883`,
zgodne z prologiem zapamiętanym z żywego procesu. Nowy `dis.py` ma to jako test.

Skutek: `find_upload_slots.py` mówił „0 interfejsowych calli w beginRender".
Naprawiony: **31 calli, 17 różnych slotów, 27 różnych celów.**

## 2. RVAs callee'ów to adresy wywołań, nie wejść funkcji

Próba zhookowania 27 helperów `beginRender` zabiła grę na 1. klatce.
Tabela była zbudowana z `ins.op_str` (adres wywołania), nie z wejścia funkcji.
Trzy z dwudziestu „funkcji" to środek cudzego kodu:

```
0x5BB2D0: 89 51 04 C3 CC CC ...   mov [rcx+4],edx ; ret ; padding
0x5BB3B0: 48 63 51 50 ...         movsxd rdx,[rcx+0x50]
0x90B83D: FF 25 25 21 07 00       jmp qword ptr [rip+0x72125]
```

Kradzież 10 bajtów z środka funkcji nadpisuje realne instrukcje.
Trace wyłączony w `teardown_vr15`; do poprawnego trace potrzebny jest
prawdziwy dekoder instrukcji, nie zgadywanie bajtów.

## Co jest pewne

| fakt | status |
|---|---|
| `0x9E130` buduje i wgrywa 500-bajtowy SDB | potwierdzone, disassembly |
| `0x9E130` **nie** jest wołane per frame | potwierdzone: dwa haki, 0 trafień |
| layout SDB (5 identity float4x4, `pd` na `+0x70`) | potwierdzone z CPU |
| `pd.z` = surowy world offset w metrach | potwierdzone z HLSL i CPU |
| resolver sygnatury działa offline i live | potwierdzone, `teardown_vr10` |
| obraz teraz czytany poprawnie | potwierdzone, `dis.py verify` |
| `beginRender` ma 27 callee'ów, 31 call interfejsowy | potwierdzone |

## Co pozostaje

Ścieżka per-frame do SDB. `0x9E130` to init. Potrzebna inna funkcja, która
wgrywa te same 500 bajtów co klatkę. Najlepsza hipoteza: render przechodzi
przez wskaźniki, bo graf osiągalności z `beginRender` ma tylko 57 adresów,
a `beginRender` ma 68 wywołań bezpośrednich — to sprzeczność, która mówi,
że coś jest poza grafem.

Następny krok wymaga prawdziwego dekodera x86-64 w C (nie Capstone, bo
build celuje w Windows/x64 i nie ma tam Pythona). Alternatywnie: zrzut całego
`.text` z żywego procesu i analiza offline w Pythonie, gdzie Capstone jest
dostępny — to bezpieczniejsze niż debugowanie na żywo.
