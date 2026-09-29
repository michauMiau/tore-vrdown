# Stan na 2026-09-26: hook działa

## Wynik testu na prawdziwej grze

```
[VR] === teardown_vr.dll build 2026-09-26-paths (x64) ===
[VR] host base 7ff7545f0000 handed over via tdvr_host.txt
[VR] resolved via RTTI: vtable=00007ff755071d80 (rva 0xA81D80)
[VR] hooks installed via VTABLE: begin=00007ff754ba35a0 end=00007ff754ba6e50
[VR] ready
```

RTTI znalazło klasę, vtable została podmieniona, oba hooki zainstalowane,
proces przeżył.

## Dlaczego to było warte zachodu

Porównanie obu buildów:

| | rozpakowany Steamless | Twoja gra Steam |
|---|---|---|
| vtable RVA | `0xA859A0` | `0xA81D80` |
| beginRender RVA | `0x5AB9D0` | `0xA35A0` |
| różnica beginRender | — | **ponad 4 MB** |

Zakodowany adres byłby zły o ponad 4 megabajty. Żadna aktualizacja
hardcoded RVA nie nadążyłaby za tym tempem. Klasy i sloty vtable — przeciwnie,
przesunęły się o kilkaset bajtów.

## Co jeszcze nie działa

Brak linii `frame N`. Licznik klatek rośnie dopiero, gdy renderer wykonuje
klatkę, a na tej maszynie gra nie dochodzi do renderera — to samo ograniczenie,
które udokumentowałem w `PLATFORM_LIMIT.md` dla kontenera, a teraz
potwierdzone też na Windows z RTX 4070.

Do zrobienia pozostaje znalezienie `SceneDynamicBuffer` i wstrzyknięcie
macierzy per-eye. To wymaga działającego renderera, żeby zobaczyć ramkę.

## Zmiany techniczne w tej iteracji

1. **Injector pisze `tdvr_host.txt` obok własnego EXE**, nie do CWD. Wcześniej
   DLL i injector miały różne katalogi robocze i handshake szukał pliku
   tam, gdzie go nie było.
2. **Oba logi lądują w folderze gry.** Wcześniej DLL zapisywała do `C:\`,
   bo stała ścieżka systemowa była wcześniejsza w kolejności niż ścieżka
   względna.
3. **Loguje się pierwszy nieudany odczyt pliku** — jeden raz, żeby zawieszenie
   nie było bezgwłośne.
4. **Injector czyta `teardown_vr.log` i mówi, czy hook się zainstalował.**
   Wcześniej pisał "injected successfully" tylko na podstawie `LoadLibraryA`,
   co jest prawdą o module, a nie o działającym hooku.

---

## 2026-09-27 późny wieczór — wejście: Lua wystarczy, natywny shim zbędny

Znalezione i udokumentowane w `LUA_INJECTION_FOUND.md`:

`UiSendInputScreenTouchAction(actionId, touchId, value)` —
`data/script_defs.luau:11180`.

Przyjmuje **nazwę akcji gry** (`"jump"`, `"crouch"`, `"usetool"`, `"grab"`,
`"tool_group_next"`, `"extra0"`/`"extra1"`), nie nazwę przycisku, i **tworzy
zdarzenie wejścia**. Nie jest to API menu: `creativemode.lua` używa go do
sterowania grą ze swojego HUD-u w trakcie rozgrywki (linie 2678, 2692, 2709,
2711, 2851, 2867), a `spawn.lua:1630` robi to samo z `"interact"`.

**Konsekwencja:** zweryfikowane sloty vtable 17 i 21 pozostają poprawne, ale
wypadają ze ścieżki krytycznej. Shim natywny nie jest potrzebny, dopóki test
`touchId` nie wykaże inaczej. Planowanie go było przedwczesne — projekt może
być czysto-Lua.

To nie koliduje z ustaleniem o remapowaniu. Remap
(`options.input.keymap.<action>`) mówi, który fizyczny klawisz steruje akcją;
ta funkcja **produkuje** zdarzenia. Pierwsze nigdy nie potrafiło wstrzyknąć,
drugie jest właśnie prymitywem wstrzykiwania.

### Otwarte pytanie, które rozstrzyga wszystko

Czy `touchId` musi odpowiadać prawdziwemu, aktywnemu dotyku, czy jest tylko
etykietą? Przykład w dokumentacji używa prawdziwego id, ale
`creativemode.lua:2851` woła funkcję z gałęzi
`elseif UiWasScreenTouchCompletedWithoutLeavingCircle(...)`, gdzie
`eraseTouchId` wynosi `0` — gra sama przekazuje **zerowe** id funkcji
udokumentowanej jako wymagająca go. Czytanie tego nie rozstrzyga.

Test rozstrzygający, jedna linia, bez kontrolera:

```lua
function tick()
    UiSendInputScreenTouchAction("jump", 1, 1)
    UiSendInputScreenTouchAction("jump", 1, 0)
end
```

Skok = shim natywny zbędny, mod jest czysto-Lua.

### Stan warstwy Lua — bez zmian

`vrinput.lua` teraz **odczytuje i publikuje, ale nic nie wstrzykuje**. To jest
luka, którą zamyka powyższe ustalenie. Niezależnie od niego mod nadal nie
został potwierdzony jako ładujący się w grze: canary rysujący magentowy tekst
nie pokazał się na ekranie mimo poprawnego `main.lua`, poprawnego katalogu
`mods/teardown_vr/` i wpisu `builtin-teardown_vr shown="true"` w
`AppData\Local\Teardown\mods.xml`. Ten problem jest starszy niż dzisiejsze
ustalenia i pozostaje otwarty.

### 2026-09-27 — blokada testów live rozwiązana: winna był ReShade, nie system modów

Trzy różne mody, trzy restarty gry, trzy **identyczne co do bajtu** zrzuty
(67 562 B). Okno nie było renderem, tylko statyczną powierzchnią — pomiar
czterech próbek co 4 s dał **0 z 1 457 376 pikseli różnych** przy rosnącym
CPU. Po dismiss splashu ten sam pomiar daje 3 614 001.

Odczytany tesseractem ekran: **ReShade 6.8.0, „Naciśnij dowolny klawisz,
kontynuować"**. Gra nigdy nie doszła do systemu modów. Wszystkie wcześniejsze
wnioski „mod się nie ładuje" były błędne.

Na tym VM do gry dociera **wyłącznie `keybd_event`** — `SendKeys` rzuca
`ArgumentException`, `mouse_event` i `SendInput` nie działają. Focus trzeba
ukraść przez `AttachThreadInput`.

**Mod nadal nie jest potwierdzony jako działający.** Do zrobienia: Opcje →
Mody (pozycja „Opcje" zmierzona: frac 0.7616, wiersz tekstu y=134..144).
