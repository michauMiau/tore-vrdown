# Wykrywanie zamrożenia — i dlaczego Sentry nie służy

## Incydent

2026-09-30 około 00:37 gra przestała odpowiadać. Agent tego nie zauważył.
Użytkownik zgłosił to wprost.

Co pokazało śledztwo:

- proces `teardown.exe` **zniknął sam**, bez `Application` eventu,
- bez crash dumpu, bez raportu Sentry, bez błędu w logu gry,
- log gry urwał się przy ładowaniu modów Workshop (`00:03:21`),
- dwa zadania `tdvr_*` zostały w stanie `Running` z `result=0x800710E0`,
  czekając na grę, która już odeszła.

## Dlaczego Sentry jest bezużyteczny jako sygnał

Zmierzone 2026-09-28 i powtórzone tutaj: pliki `.sentry-native/__sentry-event`,
`session.json`, `settings.dat` i breadcrumbs mają **wszystkie** czas startu
procesu co do sekundy. Envelope czyta `level=fatal` **podczas gdy gra renderuje
pełną prędkością**. Okno Eksploratora „wyślij crash dump?" zostaje po śmierci
procesu i wisie godzinami.

Wniosek: ani istnienie okna Sentry, ani „zdarzenie nowsze niż start" nic nie
mówią o bieżącym przebiegu.

`build/game_state.ps1` miał Sentry jako główny sygnał i przez to **dwukrotnie
zgłosił CRASHED dla żywej gry**, co wygenerowało fałszywego winowajcę
(`endRender`). Wariant E z wyłączonym `endRender` i tak crashował, więc
wniosek był zły w dwóch miejscach naraz.

## Zasada

**Dwa niezależne sygnały, oba tanie, żaden nie Sentry:**

1. **Ruch CPU** w próbce czasu. Zamrożony render thread przestaje palić cykle.
2. **Czy log instrumentacji rośnie.**

Zdrowe wymaga **obu** naraz. Samo okno dialogowe z zamrożoną resztą gry to
`NOT_RESPONDING`, nie `FROZEN` — i to się odróżnia enumeracją okien.

## Narzędzie

`watchdog.ps1` — 20 s × 270 próbek (90 min), zapisuje `C:\tdvr\watchdog.log`.
Rozróżnia:

- `ALIVE` — CPU rośnie, log rośnie
- `FLAT` — dwa z rzędu bez ruchu CPU
- `NOT_RESPONDING` — `Responding = False`, okna wciąż żywe
- `FROZEN` — ani CPU, ani log
- `*** GAME PROCESS GONE ***` — znikł, z wzmianką że był

Każdy stan ogłaszany **raz**, nie co próbkę, żeby log dało się czytać.
Każda linia zawiera listę `vr_*.dll` w procesie — to różni „gra zamrożona" od
„gra zamrożona przeze mnie".

## Porządek po sobie

`unregister_all.ps1` zdejmuje wszystkie zadania z `\tdvr\` i weryfikuje, że
zostało zero. Nazwa celowo **nie** `cleanup.ps1`: taki plik już istniał i
nadpisanie go bez przeczytania to dokładnie ten błąd, którego narzędzie ma
zapobiegać.
