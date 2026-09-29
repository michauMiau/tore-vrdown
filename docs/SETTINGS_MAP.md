# Ustawienia silnika jako mapa kodu

Silnik rejestruje opcje po nazwie. Nazwy są w obrazie, a kod który je cytuje
też — to daje gotową mapę: gdzie ustawienie jest rejestrowane, gdzie odczytywane,
gdzie zapisywane.

## `options.gfx.fov` @ `0x98B650` — 8 referencji

Rejestracja (typowa, powtarzana dla każdej opcji):

```asm
lea   rdx, [rip + ...]            ; "options.gfx.fov"
lea   rcx, [rbp - 0x60]           ; lokalny std::string
call  0x5075A0                    ; konstrukcja std::string
nop
mov   rcx, rbx                    ; globalny rejestr ustawień
lea   rdx, [rbp - 0x60]
call  0x461150                    ; rejestracja / odczyt
movzx ebx, al
lea   rcx, [rbp - 0x60]
call  0x507710                    ; destrukcja stringa
```

Zapis wartości:

```asm
mov   ebx, [rsi + 8]              ; wartość z kontekstu
lea   rdx, [rip + ...]
call  0x5075A0
mov   rdi, [rax + 0x100]          ; globalny rejestr
lea   rdx, [rbp - 0x60]
mov   rcx, rdi
call  0x462620                    ; zapis ustawienia
```

**Getter** pod `0x7B9A3`:

```asm
lea   rdx, [rip + ...]            ; "options.gfx.fov"
call  0x5075A0
mov   rcx, rbx                    ; rejestr ustawień
lea   rdx, [rbp - 0x20]
call  0x460600                    ; odczyt wartości
mov   [rdi + 8], eax              ; zapis do struktury wyjściowej
```

`0x460600` zwraca FOV jako `int` w `eax`. To jest punkt, w którym można
odczytać aktualne FOV bez zgadywania.

## `options.input.headbob` @ `0x98B888` — ta sama struktura

Potwierdza, że wzorzec `0x5075A0` / `0x507710` / `0x461150` / `0x462620` /
`0x460600` to warstwa opcji, wspólna dla wszystkich ustawień.

## Co to daje

FOV jest czytany przez `0x460600` z globalnego rejestru ustawień, nie
przechowywany w rendererze. To znaczy, że projekcja jest liczona z kilku źródeł:
FOV z opcji, pozycja i orientacja z kamery gracza.

**Dlatego nie ma jednej struktury „kamera”, którą można podmienić.** Trzeba
albo znaleźć moment, w którym wszystko jest scalone do `mubVpMatrix`, albo
podać off-axis na poziomie, gdzie silnik liczy projekcję.

## Ścieżka do VP z shaderów

`mubProjectionData` = `(1/m[0], 1/m[5], m[8]/m[0], m[9]/m[5])` — to jest
rzut odwrotności elementów projekcji. `mubViewXDir/YDir/ZDir` to baza ortonormalna
kamery. Zatem:

```
mubViewXDir = kolumna 0 macierzy widoku (bez translacji)
mubViewYDir = kolumna 1
mubViewZDir = kolumna 2
```

Off-axis w układzie świata sprowadza się do przesunięcia pozycji oka wzdłuż
`mubViewXDir` i poprawienia składowej przesunięcia w macierzy. Nie trzeba
dotykać `mubProjectionData`, jeśli kąt pozostaje ten sam — zmienia się tylko
przesunięcie.

## Stan

Layout buforów, nazwy i rozmiar: `docs/BUFFER_LAYOUT.md`.
Zero śladu stereo w shaderach: brak `eye`, `eyeIndex`, `monoscopic`.
Trzeba zbudować off-axis samemu.
