# Wynik: skan właściciela (ownerid) — metoda odrzucona

## Co zrobiłem

`build/ownerid.c` — skan committed read-write pamięci w poszukiwaniu obiektów
z prawdziwą vftable, z wymaganiem podwójnego potwierdzenia (obie 8-bajtowe
wartości muszą wskazywać na kod wewnątrz obrazu).

## Wynik

```
scanned 4286 rw regions, 542725 image-code pointers
candidate objects with a real vftable: 4096   (limit osiągnięty)
```

Próbka pierwszych kandydatów:

```
obj 000001f2d6bebc48  vt ...0e6298  +0xB90=0040004000000040
obj 000001f2d6bebc50  vt ...0e62c4  +0xB90=0083002002410021
obj 000001f2d6c46e08  vt ...039900  +0xB90=0073005c00730001
    slot 0 -> 0x4E11D0
    slot 1 -> 0x4E5E70
    slot 2 -> 0x4E5D90
```

## Dlaczego to śmieci

1. **`0x0040004000000040`, `0x0083002002410021`, `0x0073005c00730001`** to nie
   uchwyty D3D12. `0x40` to opcode `dcl_*` w bajtkodzie DXBC,
   `0x0073 0x005C 0x0073 0x0001` to tekst UTF-16. Skan trafia w **bytecode
   shaderów i stringi**, gdzie przypadkowe sąsiedztwa dwóch qwordów wyglądają
   jak vftable.

2. Limit 4096 został osiągnięty, więc lista jest ucięta i niekompletna — nie
   da się z niej wyciągnąć wniosku.

3. Poprawny wskaźnik vtable dla renderera (`0xA81D80`) w ogóle nie pojawił się
   w próbce, mimo że `vtobj` potwierdziło jego istnienie dwa obiekty wcześniej.
   To dowodzi, że heurystyka nie odfiltrowuje danych.

## Wniosek: metoda odrzucona

 heurystyka „dwa sąsiednie qwordi to vftable" jest za słaba na proces z ~1.5 GB
zmapowanej pamięci zawierającej skompilowane shadery. Odrzucam ten trop; kolejne
podejście nie może opierać się na skanowaniu pamięci w poszukiwaniu obiektów.

## Co zostaje wiadome

- `0x9C030` (create `SceneDynamicBuffer`, rozmiar `0x1F4`) ma **jednego** callera:
  `0x8368A`, `rcx = rdi`.
- Właściciel ma pola `+0xA0A`, `+0xA0C`, `+0xA10`, `+0xA14`, `+0xA18`,
  `+0x23F2`, `+0x3A58`, `+0x3A60..+0x3AB8`, więc jest co najmniej `0x3AC0` bajtów.
- Renderer z `VR_GetRenderer` to **inny, mniejszy** obiekt (jego czytelna tabela
  kończy się na `+0xCF0`).
- Obraz **nie zawiera** MSVC Complete Object Locator przed znaną vtable, więc
  klasy właściciela nie da się nazwać offline przez standardowy RTTI-walk.
  Nazwy `Renderer::*` w `.rdata` to 32-bajtowe literały używane jako etykiety
  przy tworzeniu buforów, nie TypeDescriptory.

## Zmiana kierunku

Polowanie na adres `mubVpMatrix` w pamięci kosztowało dziesiątki iteracji bez
rezultatu i daje coraz słabsze hipotezy. Tymczasem istnieje blocker, który jest
**konieczny niezależnie** od tego, gdzie leży macierz: OpenXR nie ma własnego
loadera, a swapchain gry powstaje z `CreateSwapChainForHwnd`, więc do stereo
potrzebny jest swapchain stworzony przez OpenXR i podmieniony w miejscu.

Kolejny krok: zamiana swapchaina, a nie dalsze szukanie macierzy.
