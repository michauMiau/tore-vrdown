# DWA RÓŻNE BUILDY — cała analiza offline była robiona na niewłaściwym pliku

Data: 2026-09-28. Odkryte przy weryfikacji offsetu `0x1130`.

## Fakty

Dwa pliki `teardown.exe` w repo:

```text
steamless/Teardown/teardown.exe
  sha256 4de0201f3f40774164a2eaf89fa4c653...   30 333 440 B
  .text entropy 6.474                          NIEPAKOWANY

teardown-analysis/game_full/Teardown/teardown.exe
  sha256 c8228c8748fef01f55c30db33ebedd58...   30 555 208 B
  prolog endRender NIE znaleziony               SPAKOWANY
```

Na maszynie z grą, `D:\SteamLibrary\steamapps\common\Teardown\teardown.exe`:

```text
Length     30555208
SHA256     C8228C8748FEF01F55C30DB33EBEDD5853903D96C53025A7949EA6C337E5F7CA
```

To `game_full`. **Analiza offline szła po `steamless`.**

## Co przetrwało, a co nie

Sprawdzone bajt po bajcie:

| RVA | steamless | game_full |
|---|---|---|
| `0x5AF270` | prawdziwy prolog `48895c24 1855...` | śmieci |
| `0x5B35A0` | śmieci | śmieci (spakowany) |
| `0x5B6E50` | śmieci | śmieci (spakowany) |

`0x5B35A0` i `0x5B6E50` **nie pochodzą z żadnego pliku na dysku**. Pochodzą
z live dumpu procesu, gdzie `game_full` jest już rozpakowany w pamięci. Dlatego
hooki RTTI/vtable działają — adresy są liczone względem obrazu w pamięci.

Natomiast `0x1130` (bufor uploadu) został odczytany z **dyskowego**
`steamless`. To inny build. Offset nie przeniósł się.

## Dowód, że 0x1130 jest złe

v30 odczytał 2048 bajtów z `renderer+0x1130` i `logs/upload_v29.bin`
pokazuje:

```text
0020  3/4  1.2271314868793882309340168192e+25  -1.2271314868793882309340168192e+25  0.0000  0.0000
0040  4/4  5.5378232513920496524328960e+25   ...
0110  4/4  3.2201248768e+10  ...
```

Wartości rzędu `1e25` to 64-bitowe wskaźniki reinterpretowane jako FP32 —
`0x0000021c...` jako float daje ~1e25. **To lista wskaźników, nie macierze.**

Dodatkowo `0x1128` zawiera `0x00003F48` = **16200** jako część pary
(float, uint16), co wygląda na licznik, nie na offset bufora.

## Jak znaleźć prawdziwy offset

Nie zgadywać. W rozpakowanym obrazie `steamless` jest dokładnie jeden site
pasujący do sygnatury uploadu:

```text
rva 0x005AF29F   lea +0x1130   mov r8d, 0x800
```

Jeden na 10 MB `.text`. To potwierdza, że `0x1130` **był** poprawny dla
`steamless` — i że nie ma powodu zakładać, iż `game_full` ma to samo.

Następny krok: skoro `game_full` jest spakowany, trzeba zrobić **live dump
rozpakowanego `.text` z uruchomionego procesu** i przepuścić przez ten sam
skrypt. Wtedy offset jest zmierzony, nie założony.

## Drugi błąd, który to ujawniło

`memscan.h::td_matches_shader_formula` miał:

```c
if (fabsf((pd[0] - inv0) / inv0) > 1e-3f) return 0;
```

Przy `m0 == 0`: `1/m0 = inf`, `(pd.x - inf)/inf = NaN`, a `NaN > 1e-3` jest
**fałszem**, więc blok **przechodził**. Każda dziura wypełniona zerami w
obiekcie renderera wyglądała na poprawną projekcję — dwadzieścia fałszywych
kandydatów w jednym przebiegu.

Poprawione na `!(x <= tol)`, bo NaN spełnia pierwszą formę i nie spełnia
drugiej. Regresja pokryta w `build/test_formula.c`.

To nie było widoczne w runtime, bo tam bloki nie były zerowe — ale offline
analiza obiektu natychmiast to pokazała.

## Stan

- Matcher: `ALL CHECKS PASS`, `0/20000` fałszywych na szumie, odrzuca 10/10
  fałszywych z v27, akceptuje prawdziwe 16:9 i stereo.
- `td_read` przez `ReadProcessMemory`: stabilny, v26–v30 nie padają.
- Hooki RTTI/vtable: działają, `begin=0x5B35A0 end=0x5B6E50` potwierdzone
  z logu żywego procesu.
- Bufor `renderer+0x1130`: **odrzucony**, zawiera wskaźniki nie macierze.
