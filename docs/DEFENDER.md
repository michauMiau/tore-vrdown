# Windows Defender blokuje injector.exe

## Co się dzieje

Defender zgłasza `injector.exe` jako zagrożenie. To **fałszywy alarm** i nie
da się go obejść bez zmiany charakteru programu — ale można go wyciszyć na
poziomie pliku.

## Dlaczego to robi (i dlaczego to nie jest zarzut)

`injector.exe` wykonuje dokładnie ten zestaw operacji, którego definicja
wstrzykiwania DLL w proces to inna:

```
OpenProcess  +  VirtualAllocEx  +  WriteProcessMemory  +  CreateRemoteThread
```

To **jedyny** sposób wstrzyknięcia modułu do cudzego procesu na Windows. Każdy
mod do gier, każdy profiler, każdy debugger tego używa. Nie da się zrobić tego
inaczej.

Jednocześnie plik jest czysty i można to sprawdzić:

- **importy:** tylko `KERNEL32.dll`, `msvcrt.dll`, `USER32.dll` — żadnej sieci
- **zero** API typowych dla malware: brak `Nt*`/`Zw*`, `IsDebuggerPresent`,
  szyfrowania, base64, sieci
- **zero** URL-i, domen, pobierania czegokolwiek
- **nie łączy się z internetem** — w ogóle nie ma warstwy sieciowej

## Najprościej: nie przenosić gotowych binarek

Jeśli Defender blokuje nawet sam ZIP, to **paczka ze źródłami** jest
rozwiązaniem. Nie zawiera żadnego pliku wykonywalnego, więc klasyfikator nie
ma czego oceniać. Kompilację robisz u siebie:

**Windows, przez MSYS2** (najprościej):

1. Zainstaluj MSYS2 ze strony msys2.org
2. Otwórz *MSYS2 UCRT64*
3. ```
   pacman -S mingw-w64-ucrt-x86_64-gcc
   ```
4. Rozpakuj paczkę ze źródłami i w terminalu UCRT64:
   ```
   cd do-katalogu
   ./build.sh
   ```
5. Gotowe — `build/teardown_vr.dll` i `build/injector.exe` są twoje

**Linux / WSL:**

```
sudo apt install mingw-w64
./build.sh
```

Defender nie ma szans zobaczyć czegokolwiek, bo niczego nie dostaje — kod
kompiluje się dopiero na twoim komputerze.

---

## Jeśli wolisz gotowe binarki

**Opcja 1 — wyklucz folder gry (zalecana)**

Najlepsza metoda, bo Defender w ogóle przestaje sprawdzać ten katalog:

1. Otwórz **Zabezpieczenia Windowsa** → **Ochrona przed wirusami i zagrożeniami**
2. **Ustawienia ochrony przed wirusami i zagrożeniami**
3. Przewiń do **Wykluczenia** → **Dodaj wykluczenie**
4. **Pobierz folder** → wskaż folder gry:
   `D:\SteamLibrary\steamapps\common\Teardown`
5. Potwierdź, że wykluczono

Od tej chwili nic w tym folderze nie będzie skanowane. Nie wpływa to na
bezpieczeństwo reszty systemu — wykluczasz jeden katalog z grą.

**Opcja 2 — przywróć plik z kwarantanny**

1. **Ochrona przed wirusami i zagrożeniami** → **Historia ochrony**
2. Znajdź `injector.exe` w sekcji *Kwarantanna*
3. Wybierz **Przywróć**

Przywrócenie nie jest trwałe — przy ponownym pobraniu pliku Defender znów go
zablokuje. Dlatego opcja 1 jest lepsza.

**Opcja 3 — wyłącz ochronę w czasie testu**

Skuteczne, ale **niezalecane**: wyłączasz ochronę na stałe i zapominasz ją
włączyć. Jeśli już, to na czas jednej sesji.

## Dlaczego ML to złapało, a SmartScreen nie

`Ulthar.A!ml` — znacznik `!ml` oznacza, że to **klasyfikator uczony maszynowo**,
nie reguły. Widzi parę: mały plik bez reputacji + wstrzykiwanie procesu +
pakowanie w ZIP. Ta kombinacja w jego zbiorze treningowym niemal zawsze
oznacza wstrzykiwacz.

To nie znaczy, że coś jest nie tak z kodem. Znaczy, że sytuacja wygląda
statystycznie jak złośliwe oprogramowanie, i na poziomie pojedynczego pliku
nie ma jak tego rozstrzygnąć. Kompilacja ze źródeł omija problem, bo w
archiwum nie ma nic do oceny.

---

## Czego NIE robić

- **nie dodawaj wykluczenia na cały dysk** ani na `D:\`
- **nie wyłącz Defendera na stałe**
- **nie pobieraj `injector.exe` z innego źródła** „na wszelki wypadek" —
  to tylko dodaje plik, któremu ufasz

## Jeśli chcesz to sprawdzić sam

`injector.exe` to 48 KB, niepodpisany, z 3 importowanymi DLL. W PowerShell:

```powershell
Get-AuthenticodeSignature .\injector.exe | Format-List
Get-FileHash .\injector.exe -Algorithm SHA256
```

Status podpisu to `NotSigned` — tak ma być, nie mam certyfikatu. Hash powinien
zgodzić się z tym, co podam.

## Kontekst

To jest mod do gry, napisany od zera, źródła są w `/home/truenas_admin/teardown-vr-mod`.
Nie ładuje się z sieci, nie modyfikuuje plików gry, wstrzykuje wyłącznie
własną DLL w proces `teardown.exe` i tylko jeśli ty o to poprosisz.
