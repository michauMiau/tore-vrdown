# Dostęp do maszyny testowej z Windows

Status: **działa** (zweryfikowane 2026-09-26)

## Połączenie

Szczegóły połączenia są w konfiguracji Hermesa i w kluczu SSH, nie tutaj.
Nie kopiuj adresów ani kluczy do dokumentów — trzymaj je w `~/.ssh/`.

## Co trzeba wiedzieć, żeby nie tracić czasu

1. **Ping nie działa** — 100% strat, mimo że maszyna odpowiada.
   Nie sprawdzaj dostępności pingiem. Użyj SSH albo połączenia TCP.

2. **Zdalna powłoka to `cmd.exe`**, nie bash. Składnia jak w DOS:
   - `&` zamiast `&&`
   - `if exist "sciezka" (echo TAK) else (echo NIE)`
   - wyjście zawiera znaki `\r` — usuwaj je przy przetwarzaniu

3. **Klucz prywatny musi mieć `chmod 600`.** Przy `0644` ssh odmawia
   z komunikatem `UNPROTECTED PRIVATE KEY FILE`, który wygląda jak problem
   po stronie serwera, a jest po mojej.

4. **Dla kont administrators plik `~/.ssh/authorized_keys` jest ignorowany.**
   Klucz musi być w `C:\ProgramData\ssh\administrators_authorized_keys`
   z uprawnieniami `Administrators:F` i `SYSTEM:F`, potem `Restart-Service sshd`.
   Komunikat błędu jest identyczny dla złego pliku, złej grupy i złych
   uprawnień — dlatego sprawdzaj konfigurację, a nie zgaduj.

5. **Komunikat "Permission denied" nie mówi, czego brakuje.** Trzy zupełnie
   różne przyczyny dają ten sam tekst. Diagnozuj po stronie serwera.

## Maszyna

- VMware VM, hostname `vmware\vm`
- Windows 11, build 10.0.26100.7840
- Użytkownik `vm`, jest administratorem
- Gra: `D:\SteamLibrary\steamapps\common\Teardown\teardown.exe`

## Zasady korzystania

Kopiuję pliki moda do folderu gry, uruchamiam grę, wstrzykuję DLL, czytam
logi. Nic poza folderem moda nie zmieniam bez uprzedzenia.
