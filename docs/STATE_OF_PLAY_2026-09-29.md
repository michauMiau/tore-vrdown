# Stan projektu — 2026-09-29 00:20 (Europe/Warsaw)

Ostatnia korekta: 2026-09-30 00:20 CEST.
Właściciel maszyny: vm@192.168.1.6. Repozytorium: michauMiau/tore-vrdown, GPLv3.

## Jedno zdanie

Teardown na tej maszynie uruchamia się **w OpenGL** (`gfxapi=0`, `x64_gl`), a na dysku
leży tylko cache shaderów `x64_dx12` — dlatego gra wy exits z kodem 53 zanim cokolwiek
narysuje, i dlatego żaden pomiar wewnątrz procesu wykonany z mojego zadania nie był
ważny.

## Zmierzone, nie zgadnięte

| fakt | dowód |
|---|---|
| Gra probuje OpenGL | `C:\Users\vm\AppData\Local\Teardown\log.txt`: `expected platform is 'x64_gl'`, `gfxapi value="0"` |
| Wychodzi z kodem 53 | `Start-Process -PassThru` → `ExitCode = 53` (0x35) |
| Cache shaderów jest z niewłaściwej platformy | ten sam log: `platform is wrong 'x64_dx12'` |
| `cmd.exe /c start` działa, `Start-Process` nie | 1 proces po 6 s vs `ExitCode 53` po 2 s |
| Ścieżka DXGI jest martwa | census: 32 sloty factory, dokładnie 1 z wywołaniami; `CreateSwapChain` (slot 10) = 0 |
| Klasa `TRendererD3D12` nie renderuje | 48 slotów zainstrumentowanych, 0 wywołań przy działającej grze |
| Eksporty OPENGL32 nie są thunkami | `thunkprobe.c`: `wglSwapBuffers` = `E9 57`, `wglCreateContext` = `33 D2`, `wglMakeCurrent` = `40 55` |
| Eksporty GDI32 są thunkami `FF 25` | ten sam probe, slot pod `at+6+disp` |
| Hard lockup 23:47 to był mój błąd | patrz sekcja błędów |

## Czego jeszcze nie wiadomo

- Czy `x64_gl` wymaga pobrania shadera, czy problem jest w `options.xml` tylko.
- Czy po naprawieniu startu gra faktycznie renderuje przez WGL (importy tego dowodzą
  tylko tyle, że potrafi założyć kontekst, nie że nim renderuje).
- Czy istnieje w ogóle ścieżka prezentacji klatki dostępna do podpięcia. IAT census
  (`vr_i1.dll`) został zbudowany, ale nigdy nie zmierzył, bo gra nie wstawała.

## Błędy, które kosztowały najwięcej

1. **Skok w siebie** (`hook/frame_census.h`): stub wracał do adresu THUNKU zamiast
   implementacji. Thunk już prowadził do stuba → nieskończona pętla, brak crashu,
   brak dialogu Sentry. Poprawka: czytać implementację z dyspozycji **przed** zapisem.
2. **Off-by-four w tym samym pliku**: slot thunka jest pod `at+6+disp`, nie `at+2+disp`.
   RIP wskazuje na *następną* instrukcję. Czytanie z `at+2` dawało `a07f4a7000007ffe`.
   To trzeci raz z rzędu przesunięcie o cztery bajty (dwa wcześniejsze dotyczyły
   GUID-ów). Dlatego do repo trafiły `thunkprobe.c` i `idataprobe.c`.
3. **`memcpy` 16 B na instrukcję 6 B** → 11 NOP w najbliższym eksporcie.
4. **Detour w `GDI32.dll`** → zmiana widoczna dla każdego procesu w sesji. Wyłączone
   za flagą `TDVR_FRAME_PATCH` (domyślnie 0).
5. **Zadanie zgłosiło sukces, a nic nie zrobiło**: `Register-ScheduledTask` z principalem
   `Mechau`, gdy jedynym aktywnym kontem jest `vm`. Do tego `$ErrorActionPreference=
   'SilentlyContinue'` zamieniał błąd w ciszę.
6. **Skrypt nie parsował się**: `"$tag: ..."` → PowerShell czyta `$tag:` jako zmienną.
   Cały plik martwy, `LastTaskResult=1`, zero outputu. Dodana walidacja składni na
   starcie `auto_run.ps1`.
7. **AppID zgadnięte z pamięci**: `appmanifest_1245620.acf` nie istnieje; Teardown to
   `appmanifest_1167630.acf`. Wniosek „Steam nie zna gry" był fałszywy.

## Reguły, które wynikają z powyższego

- Nie ufać `LastTaskResult` ani `task=Ready` jako dowodowi, że coś się wykonało.
  Jedynym dowodem jest plik raportu z naniesionym czasem powstania.
- Nie ufać `GetProcAddress == NULL` jako dowodowi braku eksportu, bez niezależnego
  probe'u. Dwa moje błędy dały ten sam komunikat.
- Adres implementacji czytać z thunka **przed** zapisem, i weryfikować kanoniczność.
- Nigdy nie patchować wspólnych bibliotek systemowych.
- Test z czystą grą **bez moda** przed wnioskiem, że mod coś zabił.

## Następny krok

Naprawić start: sprawdzić, czy istnieje `x64_gl` w cache, i cofnąć `gfxapi`, jeśli
shaderów nie ma. Potem IAT census na działającym procesie.
