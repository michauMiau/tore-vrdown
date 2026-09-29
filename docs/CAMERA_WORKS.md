# Kamera działa: 37 680 klatek

Data: 2026-09-26, pomiar na maszynie testowej w sesji 1 z D3D12.

## Wynik

```
[VR] resolved via RTTI: vtable=00007ff755071d80 (rva 0xA81D80)
[VR] hooks installed via VTABLE: begin=00007ff754ba35a0 end=00007ff754ba6e50
[VR] ready
[VR] frame 37680 idx=0 ctx=000001677fc4c2f0 stereo=1 ready=0 ipd=64.0 scene=0000000000000000
```

Proces żyje: 592 s CPU, 6.2 GB RAM, wstrzyknięcie niewidoczne dla gracza.
`idx` przełącza się między 0 i 1, czyli tablica kontekstów klatek działa.

## Przełomowe ustalenie: gra potrafi D3D12

Wcześniejsze pomiary pokazywały `Graphics API: OpenGL` i
`Compute shader support: false`. To było prawdziwe, ale wynikało z tego, że gra
nie dostawała prawidłowego kontekstu. Uruchomiona w sesji 1 z DX12:

```
[NoTag|Render] Graphics API: D3D12
[NoTag|Render] Compute shader support: true
```

**Wniosek: `PLATFORM_LIMIT.md` dotyczył kontenera, nie tej maszyny.** Limit
sprzętowy w kontenerze (lavapipe, brak GPU) był prawdziwy i pozostaje prawdziwy,
ale nie wolno go generalizować na maszynę z RTX 4070.

## Czego jeszcze nie znaleziono

`scene=0000000000000000` — bufor `SceneDynamicBuffer` nie został zlokalizowany.
Skaner szuka w kontekstach klatek, przechodząc wskaźniki i testując 256 bajtów
pod każdym, w krokach 16 bajtów, na 4 pierwszych klatkach.

Poprawka w tej iteracji: limit 4 → 600 klatek oraz zrzut całego kontekstu
(każdy wskaźnik z adresem i informacją o wyrównaniu), żeby przy braku trafienia
widać było, **gdzie** szukać, zamiast dostawać ciszę.

## Wykrywanie duplikatu

Druga kopia DLL w tym samym procesie nie może się podpiąć, bo sloty vtable
zajmuje już trampoline pierwszej. Zamiast zgadywać, nowy check porównuje wpisy
z bazą własnego modułu przez `VirtualQuery` i mówi wprost:

```
vtable slots already hold this mod's hooks - another copy of the DLL is loaded
in this process; restart the game
```

Wcześniej ten sam przypadek wyglądał jak awaria resolwera i prowadził do
niepotrzebnego fallbacku na hardcoded RVA.

## Uwaga o dewelopmencie

Windows nie pozwala nadpisać DLL zmapowanej w działającym procesie. Nowe
buildy wchodzą więc pod nową nazwą (`teardown_vr2.dll`, `teardown_vr3.dll`),
a nazwa logu jest teraz wyprowadzana z nazwy modułu, żeby każdy build pisał do
swojego pliku.

To obejście na czas developmentu. Dla użytkownika końcowego pozostaje jeden
plik i restart gry.
