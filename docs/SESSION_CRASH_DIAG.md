# Utrata okna gry — 2026-09-27

## Przyczyna: sesja RDP bez widocznych okien

```
NVIDIA GeForce RTX 4070  1920x1080  143 Hz   (adapter aktywny)
Meta Virtual Monitor      (brak rozdzielczości — sesja RDP)
desktop window 35258548   size=1024x768
explorer.exe pid=6976 sesja 1, Responding=True

total windows: 1   visible: 0        <-- CAŁY SYSTEM
teardown pid=15452 sesja=1 responding=True hwnd=0
```

**Cały system ma jedno okno i zero widocznych.** Desktop RDP ma 1024x768, adapter
NVIDIA jest aktywny, ale sesja nie ma aktywnego displayu. Gra utraciła
`MainWindowHandle` i przestała dostawać `Present`, więc DXGI nie ma gdzie
prezentować i `beginRender` przestał być wołany — stąd milczenie hooka.

Próba obudzenia (`ShowWindow(SW_SHOW)` + `RedrawWindow` na oknie pulpitu) nie
przywróciła renderowania: przed i po nadal `visible = 0`, klatki w miejscu.
Sesja RDP musi zostać odświeżona po stronie klienta, z tego procesu nie da się
jej obudzić.

## To nie był crash

- `responding=True`, 47 wątków, 952 handlowe
- zero crash dumpów, zero zdarzeń Application Error (id 1000)
- zero TDR (brak zdarzenia 4101, brak `nvlddmkm`/`d3d12` w logu System)
- CPU +0.05 s w 10 s — proces nie robi nic, ale nie umiera

## To nie był mój patch

`build/verify_site2.c` odczytał wszystkie call-site'y, które ktokolwiek z
dzisiejszych buildów mógł zpatchować, i bajty są identyczne z oryginałem:

| adres | bajty w procesie | oczekiwane | |
|---|---|---|---|
| 0x5BB91A | `FF 50 78 8B F8` | `FF 50 78` | IDENTICAL |
| 0x08368A | `E8 A1 89 01 00` | `E8 A1 89 01 00` | IDENTICAL |
| 0x5B3A8E | `E8 1D 74 00 00` | `E8 1D 74 00 00` | IDENTICAL |
| 0x5BAEB0 | `4C 89 4C 24 20` | `4C 89 4C 24 20` | IDENTICAL |
| 0x5BAD50 | `48 89 5C 24 18` | `48 89 5C 24 18` | IDENTICAL |

Uwaga o narzędziu: `verify_site.log` mówił „MODIFIED" przy identycznych bajtach.
Błąd w `hexeq()` — `sscanf(want + i*2, "%2x", &v)` nie zatrzymuje się po dwóch
znakach i czyta kolejne cyfry heksymalne. `hexval()` naprawia porównanie,
`%02X` w `fprintf` zawsze wypisywał poprawnie.

## Wniosek operacyjny

Testy muszą się odbywać przy **aktywnej sesji RDP z widocznym displayem**, inaczej
każdy pomiar po kilkunastu minutach bezużytecznościowy jest bezwartościowy —
proces zostaje, ale nie renderuje, a log modu milczy. To wyjaśnia też
wcześniejsze niejednoznaczne wyniki z nocy.

## Zalecenia na przyszłość

1. Nie zostawiać sesji RDP bez podłączonego klienta na długie postoje.
2. Hook powinien wykrywać `MainWindowHandle == 0` albo brak postępu klatek
   i zapisywać to w logu jako jednoznaczny stan, zamiast milczeć.
3. Do pomiarów używać krótkich sesji: start, pomiar, zamknięcie.

## Stan prac

Bez zmian.Znaleziony punkt podmiany swapchaina jest zachowany
(`docs/SWAPCHAIN_FOUND.md`, `0x5BB91A`, slot `0x78` = `IDXGIFactory4`).
Poprawiona wersja `scprobe.c` z 14-bajtowym skokiem i trampoliną jest napisana
i nieprzetestowana — czeka na świeżą, aktywną sesję.
