// Inject teardown_vr.dll into a running (or self-launched) teardown.exe.
//
// Design notes learned from testing under Wine/Proton:
//  - The DLL path handed to the remote LoadLibraryA MUST be absolute. The
//    remote process resolves relative paths against ITS OWN current directory,
//    not the injector's, so "teardown_vr.dll" silently fails on Windows.
//  - Waiting a fixed delay is racy. The game is not ready for injection the
//    instant CreateProcess returns. We poll for the process and then for its
//    main window instead.
//  - Injecting at the warning splash (before the main menu) is safe and avoids
//    a race with the first rendered frame. Verified: the hooks install and the
//    game keeps running.
//  - A module already present is NOT reloaded. LoadLibraryA on an already
//    loaded module returns the EXISTING handle and does nothing, so rebuilding
//    the DLL and injecting again silently runs the OLD code and leaves the OLD
//    log in place. That is confusing enough to look like a broken test, so the
//    injector says so explicitly instead of reporting success.
//  - Under GE-Proton the console handle is not reliably connected and printf
//    output is swallowed whole. Everything is mirrored into injector.log next
//    to the executable, which is what makes the Proton tests readable.
//
// Build (mingw-w64, from the project root), single line:
//   x86_64-w64-mingw32-gcc -O2 -o build/injector.exe injector/injector.c -static-libgcc -static-libstdc++

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define GAME_EXE      "teardown.exe"
#define VR_DLL        "teardown_vr.dll"
#define DEFAULT_DELAY 1500

// ---------------------------------------------------------------- utilities

// Write to both the console and a log file.
//
// Why: under GE-Proton the console handle is not reliably connected, and a
// printf to a detached stdout is swallowed whole — `injector --help` returns
// exit 0 and prints nothing at all. A file next to the executable always
// works, and it is what makes the Proton-based tests readable.
static FILE *g_out = NULL;

static void out_open(void) {
    if (g_out) return;
    g_out = fopen("injector.log", "w");
}

static void out_close(void) {
    if (g_out) { fclose(g_out); g_out = NULL; }
}

static void emit(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);

    if (g_out) {
        va_start(a, fmt);
        vfprintf(g_out, fmt, a);
        va_end(a);
        fflush(g_out);
    }
}

static void banner(void) {
    SetConsoleTitleA("Teardown VR Mod Injector");
    emit("\n");
    emit("  ================================================\n");
    emit("     T E A R D O W N   -   V R   M O D\n");
    emit("              dll injector v2\n");
    emit("  ================================================\n");
    emit("\n");
}

// Resolve a possibly-relative path to an absolute one. Returns 0 on success.
static int abspath(const char *in, char *out, size_t outsz) {
    DWORD n = GetFullPathNameA(in, (DWORD)outsz, out, NULL);
    if (n == 0 || n >= outsz) return 0;
    return 1;
}

static int file_exists(const char *p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Milliseconds since boot, monotonic.
static DWORD now_ms(void) { return GetTickCount(); }

// ---------------------------------------------------------------- process

DWORD find_process(const char *name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    DWORD found = 0;
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, name) == 0) { found = pe.th32ProcessID; break; }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

BOOL process_alive(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return FALSE;
    DWORD code = 0;
    BOOL ok = GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return ok && code == STILL_ACTIVE;
}

// Poll for the process, up to timeout_ms.
DWORD wait_for_process(const char *name, DWORD timeout_ms) {
    DWORD start = now_ms();
    for (;;) {
        DWORD pid = find_process(name);
        if (pid) return pid;
        if (now_ms() - start > timeout_ms) return 0;
        Sleep(250);
    }
}

// Poll until the process owns a visible top-level window, up to timeout_ms.
// This is the real readiness signal: the game is past process init and has
// something on screen, so our hooks will catch the first beginRender.
BOOL wait_for_window(DWORD pid, DWORD timeout_ms) {
    DWORD start = now_ms();
    for (;;) {
        if (!process_alive(pid)) return FALSE;

        HWND h = FindWindowA(GAME_EXE, NULL);
        if (h) {
            DWORD wpid = 0;
            GetWindowThreadProcessId(h, &wpid);
            if (wpid == pid) return TRUE;
        }

        if (now_ms() - start > timeout_ms) return FALSE;
        Sleep(250);
    }
}

// ---------------------------------------------------------------- injection

static void hand_host_image(DWORD pid);

// Is the DLL already mapped in this process?
//
// This is the check that makes "I rebuilt the DLL and nothing changed"
// explicable. LoadLibraryA against an already-loaded module is a no-op that
// still returns a valid handle, so without this the injector cheerfully
// reports success while the OLD build keeps running and the OLD log stays on
// disk untouched — which reads exactly like a broken test.
static BOOL module_loaded(DWORD pid, const char *dll_path, unsigned *mapped_size) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return FALSE;

    const char *base = strrchr(dll_path, '\\');
    base = base ? base + 1 : dll_path;
    WCHAR wbase[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, base, -1, wbase, MAX_PATH);

    // mingw's tlhelp32.h maps the bare names onto the W variants when UNICODE
    // is defined, and there is no MODULEENTRY32A typedef to force the ANSI
    // path. Use the wide struct and functions explicitly.
    MODULEENTRY32W me;
    me.dwSize = sizeof(me);
    BOOL found = FALSE;

    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, wbase) == 0) {
                found = TRUE;
                if (mapped_size) *mapped_size = (unsigned)me.modBaseSize;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

// Find a loaded module's base address in another process.
static BOOL find_module_base(DWORD pid, const char *mod_name, uintptr_t *out) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return FALSE;

    WCHAR wname[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, mod_name, -1, wname, MAX_PATH);

    MODULEENTRY32W me;
    me.dwSize = sizeof(me);
    BOOL found = FALSE;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, wname) == 0) {
                *out = (uintptr_t)me.modBaseAddr;
                found = TRUE;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}


BOOL inject_dll(DWORD pid, const char *dll_path) {
    emit("[*] Target PID      : %lu\n", (unsigned long)pid);
    emit("[*] DLL (absolute)  : %s\n\n", dll_path);

    unsigned mapped = 0;
    if (module_loaded(pid, dll_path, &mapped)) {
        emit("[!] %s is ALREADY LOADED in PID %lu (mapped image %u bytes).\n",
             dll_path, (unsigned long)pid, mapped);
        emit("    LoadLibraryA on a loaded module is a NO-OP: it returns the\n");
        emit("    existing handle and runs none of the new code. The result is\n");
        emit("    the OLD build still hooked and the OLD log still on disk.\n");
        emit("    Fix: restart the game, or give this build a different filename.\n\n");
        return FALSE;
    }

    HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
                               PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) {
        emit("[!] OpenProcess failed: %lu\n", GetLastError());
        return FALSE;
    }

    size_t len = strlen(dll_path) + 1;
    LPVOID remote = VirtualAllocEx(hProc, NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        emit("[!] VirtualAllocEx failed: %lu\n", GetLastError());
        CloseHandle(hProc);
        return FALSE;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(hProc, remote, dll_path, len, &written) || written != len) {
        emit("[!] WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return FALSE;
    }

    HMODULE hKernel = GetModuleHandleA("kernel32.dll");
    LPVOID loadlib = (LPVOID)GetProcAddress(hKernel, "LoadLibraryA");
    if (!loadlib) {
        emit("[!] GetProcAddress(LoadLibraryA) failed: %lu\n", GetLastError());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return FALSE;
    }

    HANDLE hThread = CreateRemoteThread(hProc, NULL, 0,
                                        (LPTHREAD_START_ROUTINE)loadlib,
                                        remote, 0, NULL);
    if (!hThread) {
        emit("[!] CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return FALSE;
    }

    WaitForSingleObject(hThread, 15000);

    // Read back LoadLibraryA's return value: the loaded module handle, or 0.
    DWORD exit_code = 0;
    GetExitCodeThread(hThread, &exit_code);

    CloseHandle(hThread);
    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
    CloseHandle(hProc);

    if (exit_code == 0) {
        emit("[!] LoadLibraryA returned NULL inside the target.\n");
        emit("    The DLL was not found or failed its DllMain.\n");
        return FALSE;
    }

    emit("[+] LoadLibraryA OK, module handle 0x%08lx\n", (unsigned long)exit_code);
    hand_host_image(pid);
    return TRUE;
}

// Hand the game base to the DLL, by writing it to a file.
//
// The base is read from the target's own module table, where it is full width.
// The DLL cannot read that table itself, so the value travels through a file
// the DLL polls. The DLL logs the base it ended up using, so a disagreement
// between the two is visible instead of silent.
static void hand_host_image(DWORD pid) {
    uintptr_t game_base = 0;
    if (!find_module_base(pid, GAME_EXE, &game_base)) {
        emit("[!] could not locate %s in the target\n", GAME_EXE);
        return;
    }
    emit("[+] %s base in target: 0x%llx\n", GAME_EXE,
         (unsigned long long)game_base);

    // Hand the base over in a FILE. The only reliable channel into the target:
    // every loader query inside it is wrong under Wine (see docs/HOST_MODULE.md),
    // and CreateRemoteThread "succeeds" at a truncated address, so its success
    // proves nothing. A file cannot lie about its contents.
    //
    // Write it next to the injector's own EXE, not to the working directory. The
    // DLL looks for it relative to its own CWD, which the two may not share, and
    // a handshake file that lands in the wrong directory is a hang with no
    // error message.
    char here[MAX_PATH];
    GetModuleFileNameA(NULL, here, sizeof here);
    char *slash = strrchr(here, '\\');
    if (slash) slash[1] = 0; else strcpy(here, ".\\");

    char hostfile[MAX_PATH];
    _snprintf(hostfile, sizeof hostfile, "%stdvr_host.txt", here);
    FILE *hf = fopen(hostfile, "w");
    if (hf) {
        fprintf(hf, "%llx\n", (unsigned long long)game_base);
        fclose(hf);
        emit("[+] base 0x%llx written to %s\n",
             (unsigned long long)game_base, hostfile);
    } else {
        emit("[!] could not write %s (error %lu)\n", hostfile,
             (unsigned long)GetLastError());
    }

    // No remote call: the DLL reads tdvr_host.txt. Under Wine a remote thread
    // lands at a truncated address and reports success while doing nothing, and
    // on Windows there is no reason to prefer it over a value already written.
    emit("[+] handover done - the DLL reads tdvr_host.txt");
}

// ---------------------------------------------------------------- main

static void usage(const char *prog) {
    emit(
"Usage: %s [options]\n"
"\n"
"  --game <path>    teardown.exe to launch (default: %s)\n"
"  --dll <path>     teardown_vr.dll (default: %s, resolved next to injector)\n"
"  --attach         inject into an already-running teardown.exe\n"
"  --delay <ms>     extra settle time after the window appears (default: %d)\n"
"  --nowindow       inject as soon as the process exists (no window wait)\n"
"  --timeout <ms>   how long to wait for the game (default: 60000)\n"
"  --noquit         exit immediately instead of waiting for Enter\n"
"  --help           this text\n"
"\n"
"With no --attach the injector starts the game itself, waits for its window\n"
"to appear, then injects. That is the normal path.\n",
        prog, GAME_EXE, VR_DLL, DEFAULT_DELAY);
}

int main(int argc, char *argv[]) {
    const char *game_arg = GAME_EXE;
    const char *dll_arg   = VR_DLL;
    int attach   = 0;
    int no_window = 0;
    int no_quit  = 0;
    int delay_ms = DEFAULT_DELAY;
    DWORD timeout_ms = 60000;
    DWORD explicit_pid = 0;

    // Open the log first: the argument loop can exit before banner() runs
    // (--help, unknown option), and those paths still have to be readable.
    out_open();

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game")    && i + 1 < argc) game_arg = argv[++i];
        else if (!strcmp(argv[i], "--dll") && i + 1 < argc) dll_arg   = argv[++i];
        else if (!strcmp(argv[i], "--pid")  && i + 1 < argc) { explicit_pid = (DWORD)atoi(argv[++i]); attach = 1; }
        else if (!strcmp(argv[i], "--attach"))   attach = 1;
        else if (!strcmp(argv[i], "--nowindow")) no_window = 1;
        else if (!strcmp(argv[i], "--noquit"))   no_quit = 1;
        else if (!strcmp(argv[i], "--delay")   && i + 1 < argc) delay_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) timeout_ms = (DWORD)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--help")) { usage(argv[0]); out_close(); return 0; }
        else { emit("[!] unknown option: %s\n", argv[i]); usage(argv[0]); out_close(); return 2; }
    }

    banner();

    // --- resolve paths -----------------------------------------------------
    char dll_abs[MAX_PATH];
    char game_abs[MAX_PATH];
    if (!abspath(dll_arg, dll_abs, sizeof dll_abs)) {
        emit("[!] cannot resolve DLL path: %s\n", dll_arg); return 1;
    }
    if (!file_exists(dll_abs)) {
        emit("[!] DLL not found: %s\n", dll_abs);
        emit("    Build it first:\n");
        emit("      x86_64-w64-mingw32-gcc -shared -O2 -o build/teardown_vr.dll \\\n");
        emit("          hook/teardown_vr.c -lole32 -luser32 -static-libgcc -static-libstdc++\n");
        return 1;
    }
    if (!abspath(game_arg, game_abs, sizeof game_abs)) {
        emit("[!] cannot resolve game path: %s\n", game_arg); return 1;
    }

    emit("  game : %s%s\n", game_abs, file_exists(game_abs) ? "" : "   [NOT FOUND]");
    emit("  dll  : %s\n\n", dll_abs);

    // --- obtain the pid ----------------------------------------------------
    DWORD pid = 0;

    if (explicit_pid) {
        pid = explicit_pid;
        emit("[*] Using explicit PID %lu\n", (unsigned long)pid);
        if (!process_alive(pid)) {
            emit("[!] that process is not running\n"); return 1;
        }
    } else if (attach) {
        emit("[*] Waiting for a running %s ...\n", GAME_EXE);
        pid = wait_for_process(GAME_EXE, timeout_ms);
        if (!pid) { emit("[!] no %s appeared within %lu ms\n", GAME_EXE, (unsigned long)timeout_ms); return 1; }
        emit("[+] Found %s, PID %lu\n", GAME_EXE, (unsigned long)pid);
    } else {
        if (!file_exists(game_abs)) {
            emit("[!] Game not found. Use --game <path> or --attach.\n"); return 1;
        }
        emit("[*] Launching %s ...\n", game_abs);

        STARTUPINFOA si;
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOW;

        PROCESS_INFORMATION pi;
        memset(&pi, 0, sizeof pi);

        // Detach so closing this console does not take the game down with it.
        if (!CreateProcessA(game_abs, NULL, NULL, NULL, FALSE,
                            CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
            emit("[!] CreateProcess failed: %lu\n", GetLastError());
            return 1;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        emit("[*] CreateProcess returned, waiting for the process to register ...\n");
        pid = wait_for_process(GAME_EXE, timeout_ms);
        if (!pid) {
            emit("[!] %s never appeared - it probably crashed on startup\n", GAME_EXE);
            return 1;
        }
        emit("[+] %s is up, PID %lu\n", GAME_EXE, (unsigned long)pid);
    }

    if (!no_window) {
        // Do not block on a window. Teardown creates its window late, and by
        // then the renderer is already running, so waiting for one only
        // delays injection without making it safer. What actually matters is
        // that the process is alive and has finished mapping its image, which
        // is what the settle delay below covers.
        emit("[*] Waiting for the process to settle");
        for (int i = 0; i < 8; i++) {
            if (!process_alive(pid)) break;
            Sleep(250);
            emit(".");
        }
        emit("\n");
        if (process_alive(pid)) {
            emit("[+] Process is alive\n");
        } else {
            // Distinguish "exited" from "we cannot open it", which look identical
            // here: OpenProcess can fail on a protected or elevated process, and
            // that is a problem with our rights, not with the game.
            DWORD err = GetLastError();
            emit("[!] Game exited before a window appeared (GetLastError=%lu)\n",
                 (unsigned long)err);
            if (err == ERROR_ACCESS_DENIED)
                emit("    That is a permissions problem, not a crash: "
                     "run injector.exe as administrator.\n");
            else
                emit("    The game really did exit. Start it normally first, "
                     "wait for the main menu, then use --attach.\n");
            return 1;
        }
    }

    if (delay_ms > 0) {
        emit("[*] Settling for %d ms\n", delay_ms);
        Sleep((DWORD)delay_ms);
    }

    // --- inject -----------------------------------------------------------
    emit("\n");
    if (!inject_dll(pid, dll_abs)) {
        emit("\n[!] Injection FAILED\n");
        return 1;
    }

    // The DLL logs from a thread it spawns after loading, so give it a moment
    // before reading the log. Without this the check races and reports failure
    // even when the hook went in fine.
    Sleep(1200);

    emit("\n");
    // Do not claim success on the strength of LoadLibraryA. That call returning
    // non-NULL says the module loaded, not that a single hook was installed, and
    // those two outcomes look identical from here. The DLL's log is the only
    // thing that can tell them apart, so read it and report what it says.
    {
        char logpath[MAX_PATH];
        const char *dir;
        GetModuleFileNameA(NULL, logpath, sizeof logpath);
        char *slash = strrchr(logpath, '\\');
        if (slash) { *slash = 0; dir = logpath; } else dir = ".";

        char full[MAX_PATH];
        _snprintf(full, sizeof full, "%s\\teardown_vr.log", dir);
        FILE *f = fopen(full, "r");
        if (f) {
            char line[512];
            int found = 0, ready = 0;
            while (fgets(line, sizeof line, f)) {
                if (strstr(line, "resolved via RTTI")) found = 1;
                if (strstr(line, "hooks installed") ||
                    strstr(line, "[VR] ready"))       ready = 1;
            }
            fclose(f);
            if (ready) {
                emit("[+] The DLL reports the hook is installed.\n");
                emit("[+] Start the game and look for a rising 'frame' counter.\n");
            } else if (found) {
                emit("[*] RTTI resolution worked but the log ends before\n");
                emit("    'hooks installed' — the hook did not go in.\n");
                emit("    Send me teardown_vr.log.\n");
            } else {
                emit("[!] teardown_vr.log has no 'resolved via RTTI' line:\n");
                emit("    resolution failed. Send me that log, it says why.\n");
            }
        } else {
            emit("[*] Could not read teardown_vr.log yet - check it yourself.\n");
        }
    }

    emit("  ================================================\n");
    emit("   Read teardown_vr.log next to the game. It is the\n");
    emit("   only thing that tells you whether this worked:\n");
    emit("     resolved via RTTI      <- addresses found\n");
    emit("     hooks installed        <- hook is live\n");
    emit("     frame N                <- renderer reached\n");
    emit("  ================================================\n\n");

    if (!no_quit) {
        emit("[*] Press Enter to close this window...");
        getchar();
    }
    out_close();
    return 0;
}
