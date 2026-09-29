// Read this before changing anything in here.
//
// WHY NOT HOOK SENTRY
// -------------------
// Sentry's native SDK (sentry.dll, sentry.native 0.15.2 / crashpad) installs its
// own exception handling and swallows the fault. What it then writes is
// systematically lossy, and this project has been misled by every field:
//
//   error.yaml  message : "General crash"          <- no code, no address
//   callstack.txt top    : strncpy / _C_specific_handler / _chkstk
//                          RtlLocateExtendedFeature / KiUserExceptionDispatcher
//                          0x00000000004ffcc8        <- a stack value, not a fault
//   one run              : "GPU hung/removed/reset, HRESULT=887a0005"
//
// So the two runs that looked like different bugs reported the SAME callstack
// shape, and neither named the module that faulted. Every conclusion drawn from
// those files was guesswork.
//
// A vectored exception handler sits BELOW the structured handler chain: it runs
// when the exception is first raised, before any SEH handler -- including
// Sentry's -- gets to see it, and before the game's own __try/__except. That is
// the only place where EXCEPTION_RECORD is still intact. From there we get:
//
//   ExceptionCode    the real fault (0xC0000005 access violation, etc.)
//   ExceptionAddress the real faulting instruction
//   NumberParameters / ExceptionInformation  the operands, e.g. the exact
//                    address that was read or written
//
// All of which is exactly what a Sentry report does NOT tell us, and all of it
// resolves to a module by subtracting that module's base from the RVA -- the one
// question that decides whether a crash is ours.
//
// SAFETY
// ------
// AddVectoredExceptionHandler is called once at init and never removed. The
// handler MUST return EXCEPTION_CONTINUE_SEARCH: taking over the exception would
// mean reimplementing what the game and Sentry do, and getting it wrong turns a
// recoverable fault into a silent hang. This handler only reads and writes.
//
// The game is a 64-bit process, so CONTEXT is the 64-bit layout. Reading
// Rip/Rsp/Rbp out of it is the only part of CONTEXT that is version-sensitive;
// those three are stable across every x64 Windows build.

#ifndef TDVR_VEH_H
#define TDVR_VEH_H

#ifndef TDVR_VEH
#define TDVR_VEH 1
#endif

#if TDVR_VEH

#include <windows.h>
// MODULEINFO + GetModuleInformation come from psapi; the export is resolved by
// name at runtime so no import-table entry is added to this DLL.
#include <psapi.h>

// Refuse to log from inside the handler itself: a fault inside the logging path
// (heap corruption, a bad format argument) would recurse until the stack runs
// out, and the resulting crash would be blamed on whatever the game was doing.
static volatile LONG g_veh_busy = 0;
static volatile LONG g_veh_count = 0;

// Ring buffer of the last N faults. Dumped to the log on the next frame hook
// call, because writing to a file from inside an exception handler is itself
// unsafe -- the loader lock or the CRT may be held by the faulting thread.
#define TD_VEH_KEEP 16
typedef struct {
    DWORD    code;
    uint64_t address;
    uint64_t p0, p1, p2;
} td_veh_fault;

static td_veh_fault g_veh_faults[TD_VEH_KEEP];
static volatile LONG g_veh_head = 0;

static const char* td_veh_code_name(DWORD c) {
    switch (c) {
    case 0xC0000005L: return "ACCESS_VIOLATION";
    case 0xC00000FDL: return "STACK_OVERFLOW";
    case 0xC0000006L: return "IN_PAGE_ERROR";
    case 0xC000001DL: return "ILLEGAL_INSTRUCTION";
    case 0xC000008CL: return "STATUS_ARRAY_BOUNDS_EXCEEDED";
    case 0xC0000409L: return "STACK_BUFFER_OVERRUN_fastfail";
    case 0xC0000374L: return "HEAP_CORRUPTION";
    case 0xE06D7363L: return "CPP_EXCEPTION";
    case 0x80000003L: return "BREAKPOINT";
    case 0x80000004L: return "SINGLE_STEP";
    default:         return "other";
    }
}

// "%llX" IS NOT A WINDOWS CRT CONVERSION.
//
// The first version of this file printed every address with wsprintfA and
// "%llX". The Windows CRT does not implement the `ll` length modifier, so it
// printed the literal characters "lX" and the log came out as
//     at  KERNELBASE.dll+0xlX
//     info[0]=0xlX  info[1]=0xlX  info[2]=0xlX
// i.e. the faulting module+offset and all three operands were lost -- the one
// thing this handler exists to report. The compiler cannot catch it: the format
// string is not checked against the arguments in C.
//
// So hex is written by hand below. 64-bit and 32-bit get separate functions
// because the `ll` type does not exist in the MSVCRT printf family at all, and
// casting down to 32 bits would lose exactly the upper half we need.
static char* td_hex64(char* p, uint64_t v, int width) {
    static const char* d = "0123456789ABCDEF";
    char tmp[16];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = d[v & 0xF]; v >>= 4; }
    while (n < width) tmp[n++] = '0';
    while (n) *p++ = tmp[--n];
    *p = 0;
    return p;
}

static char* td_str(char* p, const char* s) {
    while (*s) *p++ = *s++;
    *p = 0;
    return p;
}

static char* td_dec(char* p, long v) {
    char tmp[24];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-(v + 1)) + 1UL : (unsigned long)v;
    while (u) { tmp[n++] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) tmp[n++] = '-';
    while (n) *p++ = tmp[--n];
    *p = 0;
    return p;
}

// Which module does this address belong to? Walks what is loaded right now, so a
// fault in a freshly mapped DLL is still attributed instead of being reported as
// a bare RVA. Falls back to "unknown" rather than guessing.
static void td_veh_locate(uint64_t addr, char* out, size_t outsz) {
    HMODULE mods[512];
    DWORD need = 0;
    // EnumProcessModules lives in psapi but is exported from kernel32 as
    // K32EnumProcessModules; resolve by name so no import entry is added.
    typedef DWORD (WINAPI *PME)(HANDLE, HMODULE*, DWORD, DWORD*);
    static PME pEnum = NULL;
    if (!pEnum) {
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        if (k) pEnum = (PME)(void*)GetProcAddress(k, "K32EnumProcessModules");
        if (!pEnum && k) pEnum = (PME)(void*)GetProcAddress(k, "EnumProcessModules");
    }
    if (pEnum && pEnum(GetCurrentProcess(), mods, sizeof mods, &need)) {
        DWORD n = need / sizeof(HMODULE);
        if (n > 512) n = 512;
        for (DWORD i = 0; i < n; i++) {
            if (!mods[i]) continue;
            MODULEINFO mi;
            if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi))
                continue;
            uint64_t lo = (uint64_t)(uintptr_t)mi.lpBaseOfDll;
            uint64_t hi = lo + mi.SizeOfImage;
            if (addr < lo || addr >= hi) continue;

            // MODULEINFO carries only base/size/entry -- no name -- so the name
            // comes from GetModuleFileName, which works for any module in this
            // process without touching the disk.
            char* p = out;
            char full[MAX_PATH];
            const char* nm = "module";
            if (GetModuleFileNameA(mods[i], full, MAX_PATH)) {
                const char* b = strrchr(full, '\\');
                if (b) nm = b + 1;
            }
            p = td_str(p, nm);
            p = td_str(p, "+0x");
            if ((size_t)(p - out) < outsz) p = td_hex64(p, addr - lo, 1);
            return;
        }
    }
    td_str(out, "unknown");
}

// Write a small preformatted line straight to the handle the rest of the mod
// already uses. vr_log is not used on purpose: it formats into a caller buffer
// and appends to a file, and both are unsafe with a foreign exception in flight.
static HANDLE g_veh_file = NULL;

static void td_veh_emit(const char* line) {
    if (!g_veh_file) {
        char path[MAX_PATH];
        // GetModuleFileNameA(NULL) is this process's own image, which needs no
        // symbol like __ImageBase (that is a linker-provided MSVC thing and is
        // not declared under MinGW).
        if (!GetModuleFileNameA(NULL, path, MAX_PATH)) return;
        char* slash = strrchr(path, '\\');
        if (slash) slash[1] = 0;
        // The log name is derived from THIS module, not a fixed "veh.log".
        //
        // Two builds were resident in the same process at once and both wrote
        // veh.log, so reading it answered a question about whichever handler
        // happened to be installed by the oldest DLL: the new code appeared to
        // do nothing because an older copy was speaking over it. One file per
        // DLL makes that impossible -- each handler's output is separable, and
        // the module name says which build it came from.
        char stem[MAX_PATH];
        const char* b = strrchr(path, '\\');
        b = b ? b + 1 : path;
        size_t n = 0;
        while (b[n] && b[n] != '.' && n < sizeof stem - 5) { stem[n] = b[n]; n++; }
        stem[n] = 0;
        lstrcatA(path, stem);
        lstrcatA(path, "_veh.log");
        g_veh_file = CreateFileA(path, FILE_APPEND_DATA,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_veh_file == INVALID_HANDLE_VALUE) g_veh_file = NULL;
    }
    if (!g_veh_file) return;
    DWORD w = 0;
    WriteFile(g_veh_file, line, (DWORD)strlen(line), &w, NULL);
}

static LONG CALLBACK td_veh_handler(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;

    DWORD code = ep->ExceptionRecord->ExceptionCode;

    // Debug output, thread naming and the C++ throw are NOT faults -- they are
    // normal control flow that passes through this handler constantly. The
    // 0x40010006 flood in the first run was OutputDebugString: a live game
    // emitted several per second, they all arrived here, and they buried the
    // one line that would have mattered. Filter them by code, because the
    // addresses are in KERNELBASE for every one of them and say nothing.
    if (code == 0x80000003L   ||   // breakpoint
        code == 0x80000004L   ||   // single step
        code == 0xE06D7363L   ||   // C++ throw
        code == 0x40010006L   ||   // DBG_PRINTEXCEPTION_C -- OutputDebugString
        code == 0x406D1388L   ||   // thread name / debugger notification
        code == 0x406D1389L)       // thread name
        return EXCEPTION_CONTINUE_SEARCH;

    if (InterlockedCompareExchange(&g_veh_busy, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    uint64_t addr = (uint64_t)(uintptr_t)r->ExceptionAddress;
    uint64_t p0 = (r->NumberParameters > 0) ? r->ExceptionInformation[0] : 0;
    uint64_t p1 = (r->NumberParameters > 1) ? r->ExceptionInformation[1] : 0;
    uint64_t p2 = (r->NumberParameters > 2) ? r->ExceptionInformation[2] : 0;

    char line[512];
    char where[160];
    td_veh_locate(addr, where, sizeof where);

    // Hand-formatted for the same reason as the rest of the file: no printf
    // conversion in MSVCRT prints a 64-bit value, and the arguments below are
    // 64-bit. See the note above td_hex64().
    char* p = line;
    p = td_str(p, "[VEH] *** FAULT #");
    p = td_dec(p, (long)InterlockedIncrement(&g_veh_count));
    p = td_str(p, " *** code=0x");
    p = td_hex64(p, (uint64_t)code, 8);
    p = td_str(p, " (");
    p = td_str(p, td_veh_code_name(code));
    p = td_str(p, ")\r\n[VEH] at  ");
    p = td_str(p, where);
    p = td_str(p, "\r\n[VEH] info[0]=0x");
    p = td_hex64(p, p0, 16);
    p = td_str(p, "  info[1]=0x");
    p = td_hex64(p, p1, 16);
    p = td_str(p, "  info[2]=0x");
    p = td_hex64(p, p2, 16);
    p = td_str(p, "\r\n[VEH] wrote veh.log -- Sentry will not record this\r\n");
    *p = 0;

    td_veh_emit(line);

    // Keep the last few in memory so a caller can print them even if the log
    // file could not be opened.
    LONG idx = InterlockedIncrement(&g_veh_head) - 1;
    if (idx < 0) idx = 0;
    g_veh_faults[idx % TD_VEH_KEEP].code    = code;
    g_veh_faults[idx % TD_VEH_KEEP].address = addr;
    g_veh_faults[idx % TD_VEH_KEEP].p0      = p0;
    g_veh_faults[idx % TD_VEH_KEEP].p1      = p1;
    g_veh_faults[idx % TD_VEH_KEEP].p2      = p2;

    InterlockedExchange(&g_veh_busy, 0);
    return EXCEPTION_CONTINUE_SEARCH;   // never take over
}

static int tdvr_veh_install(void) {
    AddVectoredExceptionHandler(1, td_veh_handler);
    vr_log("[VEH] vectored exception handler installed (first-chance logging)");
    return 1;
}

#endif // TDVR_VEH
#endif
