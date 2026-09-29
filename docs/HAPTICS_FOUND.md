# Haptyka: gra ma ją natywnie, format jest prosty, punkt przechwycenia znaleziony

Data: 2026-09-27. Odpowiedź na prośbę o haptykę w VR.

## 1. Gra ma 57 plików haptiki

`D:\SteamLibrary\steamapps\common\Teardown\data\haptic\*.xml` — 57 plików.
Są też w modach: `mods\minigun\haptic`, `mods\tillaggaryd\haptic`, i w DLC
`dlcs\artvandals\haptic`.

Format jest w pełni deklaratywny:

```xml
<haptic_effect>
  <lifetime>0.1</lifetime>
  <keypoints type="motor" index="0">
    <point pos="0 1"/>
    <point pos="0.1 0.75"/>
    <point pos="0.2 0.5"/>
    <point pos="1 0.3"/>
  </keypoints>
  <advanced_vibration>
    <file src="advanced/tools/gun0.wav"/>
  </advanced_vibration>
</haptic_effect>
```

Triggery adaptacyjne (do 22 plików):

```xml
<adaptive_trigger type="weapon" index="1">
  <start_position>2</start_position>
  <end_position>5</end_position>
  <strength>4</strength>
</adaptive_trigger>
```

Census tagów we wszystkich 57 plikach:

| tag | ile | | atrybut | ile |
|---|---|---|---|---|
| `point` | 159 | | `pos` | 159 |
| `keypoints` | 85 | | `type` | 107 |
| `lifetime` | 66 | | `index` | 107 |
| `haptic_effect` | 57 | | `src` | 20 |
| `adaptive_trigger` | 22 | | | |
| `strength` | 21 | | | |
| `advanced_vibration` | 20 | | | |
| `file` | 20 | | | |
| `position` | 18 | | | |
| `frequency` | 1 | | | |
| `amplitude` | 1 | | | |

W engine'ie są też stringi `channel` i `feedback`, których nie ma w plikach —
to kanał `keypoints type="channel"`, czyli ten sam efekt na innym wejściu
(kontroler vs. drugi kontroler).

## 2. To się mapuje 1:1 na OpenXR

| plik gry | OpenXR |
|---|---|
| `keypoints type="motor"` + `point pos` | `XrHapticVibration { frequency, amplitude }` |
| `advanced_vibration` + WAV | audio haptyczne na headset, `xrCreateAudioDevice` / `XRT_HAPTIC_OUTPUT_AUDIO` |
| `adaptive_trigger` | `XrActionFeedback` |
| `lifetime` | `XrHapticVibrationHeader.duration` |

Mod **nie musi wymyślać haptyki** — wystarczy przechwycić efekty, które gra
i tak odtwarza, i wyemitać je przez sesję OpenXR. To najtańsza możliwa droga
i działa automatycznie dla całej zawartości gry oraz dla modsów.

## 3. Parser haptiki w kodzie: `0x1A7A90`

Zestaw stringów w obrazie, `0x9EAFE0`–`0x9EB700`:

```
  0x9EB618  passive
  0x9EB620  haptic/
  0x9EB628  _tool.xml
  0x9EB638  lifetime
  0x9EB648  keypoints
  0x9EB654  index
  0x9EB65C  motor
  0x9EB664  channel
  0x9EB670  adaptive_trigger
  0x9EB688  feedback
  0x9EB698  position
  0x9EB6A4  weapon
  0x9EB6B0  start_position
  0x9EB6C0  end_position
  0x9EB6D0  vibration
  0x9EB6E0  amplitude
  0x9EB6F0  frequency
```

Referencje RIP-relative z kodu: 28, zakres `0x1A7B10`–`0x1A8324`, funkcje
`0x1A7A90`, `0x1A7BFF`, `0x1A7C00`.

`0x1A7A90` to **initializer** — ładuje listę 57 nazw efektów i parsuje je
przy starcie:

```
  0x001A7AA1  lea  rbx, [rip + 0xA49348]     początek tablicy nazw
  0x001A7AA8  lea  rdi, [rip + 0xA494F9]     koniec tablicy
  0x001A7AB0  mov  rdx, [rbx]                nazwa efektu
  0x001A7AB8  call 0x5075A0                  string copy
  0x001A7AC9  call 0x1A7C00                  parse one effect
  0x001A7ADD  cmp  rbx, rdi
  0x001A7AE0  jl   0x1A7AB0                  pętla po 57 efektach
  0x001A7B10  lea  rdx, [rip + 0x843B09]     "haptic/"
  0x001A7B22  lea  r8,  [rip + 0x843AFF]     "_tool.xml"
```

`0x1A7C00` to parser pojedynczego efektu (`keypoints`, `index`, `motor`,
`adaptive_trigger`).

## 4. Brak natywnego OpenXR

```
  xrHaptic        0
  HapticVibration 0
  ISteamInput     0
  TriggerHaptic   0
  Vibration       0
  GetHaptic       0
  SteamInput      3   (0xA47AF0, 0xA495E9)
```

Silnik nie ma ani jednego symbolu OpenXR haptycznego. Dostęp do Steam Input idzie
przez eksporty `steam_api64.dll`, który jest w folderze gry, więc interfejsu nie
nazwuje. Wniosek: warstwa haptyczna silnika kończy się na wywołaniu do
Steam Input, i **jedno przechwycenie** pomiędzy parserem a tym wywołaniem
wystarczy dla całego systemu.

## 5. Plan haptyki

1. Hook na `0x1A7C00` (parse) — pozwala zobaczyć wypełnioną strukturę efektu
   w pamięci i potwierdzić layout, zanim powstanie cokolwiek nowego.
2. Znaleźć wywołanie odtwarzania (prawdopodobnie w kodzie, który konsumuje
   struktury z tablicy 57 efektów) i przechwycić strukturę efektu w chwili
   odtworzenia — to moment, w którym znamy intensywność i kanał.
3. W tym momencie wywołać `xrHapticVibration` na sesji OpenXR z
   `amplitude` znormalizowaną do 0..1 i `frequency` wyliczoną z kanału.
4. `advanced_vibration` zostaje na Steam Input, albo idzie osobnym kanałem audio.
5. Adaptive triggery przez `XrActionFeedback` na później, to osobny temat.

## 6. Stan sesji na Windows

Gra działa: pid 6120, sesja 1, ~320 MB, 46 wątków, `responding=True`,
CPU rośnie. Mod `teardown_vr3.dll` wstrzyknięty poprawnie
(`LoadLibraryA OK`, baza `0x7FF7545F0000`).

**Poprawka do mojej wczorajszej diagnozy:** `MainWindowHandle=0` odczytane
z sesji 0 **nie mówi nic o oknie gry** — sesje mają oddzielne stacje okien.
Liczenie okien z sesji 0 dawało 1, co było artefaktem, nie dowodem utraty okna.
Prawdziwym problemem był task `tdvr_go` z `New-ScheduledTask -TaskName`, który
nie istnieje jako parametr, przez co task nigdy się nie rejestrował.
