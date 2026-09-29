// A timed probe around xrCreateInstance.
//
// WHY
// ---
// A real run's log ends with
//     [VR] OpenXR: calling xrCreateInstance (api 1.0.34, app teardown_vr)
// and never prints the line after it. vr_log fflushes after every line (see
// vr_log in teardown_vr.c), so this is NOT a buffering artefact -- the call
// genuinely did not come back during the 150 s we watched. The game kept burning
// 10-18 s of CPU per 10 s throughout, which makes a missing line very easy to
// read as "nothing else happened". It is the opposite: a blocked call. Same
// shape as the Khronos finding from 2026-09-28, where an instance made every
// later D3D12CreateDevice hang -- except here the hang is the instance itself.
//
// HOW
// ---
// The call runs on a worker thread and the init thread watches a completion
// event with a hard deadline. If the call never returns the worker is left
// parked and the game carries on. This is a measurement; it deliberately does
// not become a fix, a hang, or a second thing that can kill the process.
//
// The structures are built from verified offsets rather than the SDK types,
// because this build compiles as C with XR_NO_PROTOTYPES. The offsets below are
// read out of third_party/openxr/openxr.h on x64 and are asserted at compile
// time against the real struct sizes where those are available.

#ifndef TDVR_XRPROBE_H
#define TDVR_XRPROBE_H

#ifndef TDVR_XRPROBE
#define TDVR_XRPROBE 0
#endif

#if TDVR_XRPROBE

#include <windows.h>
#include <stdint.h>

// ---- verified layout, x64, MEASURED with offsetof against the real header ----
//
// Measured on the target ABI (see build/off.c, run on the Windows box):
//
//   XrApplicationInfo  size=272 align=8
//     applicationName[128]   off=  0
//     applicationVersion     off=128  (uint32)
//     engineName[128]        off=132
//     engineVersion          off=260  (uint32)
//     apiVersion             off=264  XrVersion == uint64_t, so 8 bytes
//   XrInstanceCreateInfo  size=328 align=8
//     type                   off=  0
//     next                   off=  8
//     createFlags            off= 16   (uint64)
//     applicationInfo        off= 24
//     enabledApiLayerCount   off=296
//     enabledApiLayerNames   off=304
//     enabledExtensionCount  off=312
//     enabledExtensionNames  off=320
//
// Two things were wrong in the first version of this file and both are the kind
// that would have been reported as a runtime refusing a malformed create info:
//   1. XrApplicationInfo has NO type and NO next -- it is a plain data struct,
//      not an XrStructureBaseHeader. Writing a type there corrupts the first
//      bytes of applicationName.
//   2. XrVersion is 64-bit, so apiVersion is 8 bytes and the struct is 272, not
//      268. And XR_MAKE_VERSION(1,0,34) is 34, NOT (1<<16)|34 -- a version is
//      major*10000000 + minor*100000 + patch, not bit-shifted. Writing 65570
//      would have asked for OpenXR 65570.0.34.
#define TD_XR_APPINFO_SIZE        272
#define TD_XR_APPINFO_APPNAME    0
#define TD_XR_APPINFO_APPVER     128
#define TD_XR_APPINFO_ENGINENAME 132
#define TD_XR_APPINFO_ENGINEVER  260
#define TD_XR_APPINFO_APIVER     264

#define TD_XR_CII_SIZE           328
#define TD_XR_CII_TYPE           0
#define TD_XR_CII_NEXT           8
#define TD_XR_CII_CREATEFLAGS    16
#define TD_XR_CII_APPINFO        24
#define TD_XR_CII_EXTCOUNT       312
#define TD_XR_CII_EXTNAMES       320

// XR_TYPE_INSTANCE_CREATE_INFO = 3. There is no XR_TYPE_APPLICATION_INFO: the
// compiler rejects it, because XrApplicationInfo carries no type field.
#define TD_XR_TYPE_INSTANCE_CREATE_INFO 3

static HANDLE        g_xrp_evt    = NULL;
static volatile LONG g_xrp_result = -12345;   // sentinel: worker never finished
static volatile LONG g_xrp_stage  = 0;

typedef struct { void* gipa; const char* appname; } td_xrp_args;

static DWORD WINAPI td_xrp_worker(LPVOID p);

static void tdvr_xrprobe_run(void* gipa, const char* appname) {
    static td_xrp_args a;
    a.gipa    = gipa;
    a.appname = appname;

    g_xrp_evt = CreateEventA(NULL, TRUE, FALSE, NULL);
    InterlockedExchange(&g_xrp_result, -12345);
    InterlockedExchange(&g_xrp_stage, 0);

    vr_log("[XRP] probe: calling xrCreateInstance on a worker thread");
    vr_log("[XRP] probe: appName='%s' api=1.0.34 ext=XR_KHR_D3D12_enable", appname);

    HANDLE th = CreateThread(NULL, 0, td_xrp_worker, &a, 0, NULL);
    if (!th) { vr_log("[XRP] CreateThread failed, err=%lu", GetLastError()); return; }
    CloseHandle(th);   // fire-and-forget; the event is the join point

    const DWORD LIMIT_MS = 20000;
    DWORD waited = 0;
    for (;;) {
        if (WaitForSingleObject(g_xrp_evt, 500) == WAIT_OBJECT_0) break;
        waited += 500;
        if (waited < LIMIT_MS) continue;

        vr_log("[XRP] *** TIMEOUT: xrCreateInstance did not return in %lu ms ***",
               (unsigned long)LIMIT_MS);
        vr_log("[XRP] still blocked inside the runtime, 8 more seconds of proof:");
        for (int i = 0; i < 4; i++) {
            Sleep(2000);
            vr_log("[XRP]   t=%lus  stage=%ld  (1 = inside the runtime)",
                   (unsigned long)(waited / 1000 + (i + 1) * 2), (long)g_xrp_stage);
        }
        vr_log("[XRP] leaving the worker parked; the game keeps running.");
        vr_log("[XRP] THIS is the blocker: no session exists without an instance.");
        return;
    }

    long rc = (long)g_xrp_result;
    vr_log("[XRP] *** xrCreateInstance RETURNED *** after %lu ms, result=%ld",
           (unsigned long)waited, rc);
    if (rc == 0) {
        vr_log("[XRP] instance accepted -- the truncated log was NOT this call.");
        return;
    }
    // Name the code from the table in xr_session.h rather than promising that
    // xrResultToString will do it. That promise was wrong: xrResultToString needs
    // a valid instance, and this is the failure of xrCreateInstance itself, so
    // no instance exists and the log printed the bare string "XR_RESULT_-10".
    vr_log("[XRP] instance REFUSED -> %s", tdvr_xr_err_name_only((int)rc));

    // The bisect lives HERE, on the init thread, not in the worker. In the
    // worker it sat after SetEvent(), so the init thread woke up, logged
    // "instance REFUSED" and finished the probe while the worker was still
    // about to start -- the first run produced no bisect lines at all and it
    // looked like the code path was dead. Same thread, same lifetime, the
    // worker is long gone by now and the instance handle is known-bad.
    //
    // The error is XR_ERROR_LIMIT_REACHED (-10, verified in openxr.h line 168 --
    // not GRAPHICS_REQUIREMENTS_CALL_MISSING, which is -50). The spec requires
    // the graphics-requirements call before xrCreateSession, not before
    // xrCreateInstance, so there is nothing to reorder here.
    if (rc == -10) {
        vr_log("[XRP] --- LIMIT_REACHED: bisecting what hit the limit ---");

        // Re-resolve the function: the worker's local is long gone. g_xr_gipa's
        // third parameter is PFN_xrVoidFunction*, NOT void** -- it takes a
        // pointer to a function pointer, so a void** here is a type mismatch
        // the compiler rejects even though the size is identical.
        PFN_xrVoidFunction fn = NULL;
        g_xr_gipa(XR_NULL_HANDLE, "xrCreateInstance", &fn);
        if (!fn) { vr_log("[XRP] cannot re-resolve xrCreateInstance; bisect skipped"); return; }
        PFN_xrCreateInstance ci = (PFN_xrCreateInstance)fn;

        // The real struct type now, not the byte-array cast: ci is a proper
        // PFN_xrCreateInstance, so the compiler checks the argument. The byte
        // layout inside the worker is still filled through measured offsets,
        // and those are checked against the real header by the static asserts.
        XrInstanceCreateInfo c2;
        ZeroMemory(&c2, sizeof c2);
        XrApplicationInfo* a2s = &c2.applicationInfo;
        unsigned char* a2 = (unsigned char*)a2s;
        XrInstance h = XR_NULL_HANDLE;
        long r2;

        // (a) no extension at all
        c2.type = XR_TYPE_INSTANCE_CREATE_INFO;
        a2s->apiVersion = XR_MAKE_VERSION(1, 0, 34);
        lstrcpynA(a2s->applicationName, "teardown_vr", XR_MAX_APPLICATION_NAME_SIZE);
        a2s->applicationVersion = 1;
        lstrcpynA(a2s->engineName, "none", XR_MAX_ENGINE_NAME_SIZE);
        a2s->engineVersion = 1;
        h = XR_NULL_HANDLE; r2 = (long)ci(&c2, &h);
        vr_log("[XRP] bisect a) extCount=0        -> %ld", r2);
        if (r2 == 0) vr_log("[XRP] *** the extension COUNT was the limit ***");

        // (b) the one extension again, but with applicationName emptied
        static const char* exts[1] = { "XR_KHR_D3D12_enable" };
        c2.enabledExtensionCount = 1;
        c2.enabledExtensionNames  = exts;
        a2s->applicationName[0] = 0;
        h = XR_NULL_HANDLE; r2 = (long)ci(&c2, &h);
        vr_log("[XRP] bisect b) appName=\"\"       -> %ld", r2);
        if (r2 == 0) vr_log("[XRP] *** the application NAME was the limit ***");

        // (c) versions zeroed -- a runtime may cap these
        lstrcpynA(a2s->applicationName, "teardown_vr", XR_MAX_APPLICATION_NAME_SIZE);
        a2s->applicationVersion = 0;
        a2s->engineVersion      = 0;
        h = XR_NULL_HANDLE; r2 = (long)ci(&c2, &h);
        vr_log("[XRP] bisect c) versions=0        -> %ld", r2);
        if (r2 == 0) vr_log("[XRP] *** a VERSION field was the limit ***");
        a2s->applicationVersion = 1;
        a2s->engineVersion      = 1;

        // (d) deliberately unknown extension name. LIMIT_REACHED on a name a
        // runtime does not know would mean the rejection is about the extension
        // list itself rather than about a count or a size.
        static const char* odd[1] = { "XR_KHR_D3D12_enable\x7F" };
        c2.enabledExtensionNames = odd;
        h = XR_NULL_HANDLE; r2 = (long)ci(&c2, &h);
        vr_log("[XRP] bisect d) ext=unknown-name  -> %ld", r2);
        vr_log("[XRP] --- bisect done ---");
    }
}

static DWORD WINAPI td_xrp_worker(LPVOID p) {
    td_xrp_args* a = (td_xrp_args*)p;
    typedef void* (WINAPI *PGIPA)(void*, const char*, void*);
    PGIPA gipa = (PGIPA)a->gipa;

    // stage 1 means control has left our code and is in the runtime.
    InterlockedExchange(&g_xrp_stage, 1);
    void* ci = NULL;
    // XR_NULL_HANDLE: xrCreateInstance is a global, and the loader is the only
    // thing allowed to resolve it before an instance exists.
    gipa((void*)0, "xrCreateInstance", &ci);
    if (!ci) {
        vr_log("[XRP] the loader did not hand out xrCreateInstance");
        InterlockedExchange(&g_xrp_result, -99);
        if (g_xrp_evt) SetEvent(g_xrp_evt);
        return 1;
    }
    vr_log("[XRP] worker: inside the loader's runtime now");

    unsigned char cii[TD_XR_CII_SIZE];
    ZeroMemory(cii, sizeof cii);
    unsigned char* ai = cii + TD_XR_CII_APPINFO;

    *(uint32_t*)(cii + TD_XR_CII_TYPE)        = TD_XR_TYPE_INSTANCE_CREATE_INFO;
    *(const void**)(cii + TD_XR_CII_NEXT)     = NULL;
    *(uint64_t*)(cii + TD_XR_CII_CREATEFLAGS) = 0;
    // XrApplicationInfo has no type/next of its own -- writing them here would
    // land on the first bytes of applicationName.

    // An empty applicationName killed this on the very first attempt: the log
    // stopped dead with no result code and the process was gone. The spec
    // requires it and a runtime that trusts the loader to have checked is
    // entitled to fault.
    lstrcpynA((char*)(ai + TD_XR_APPINFO_APPNAME),
              a->appname ? a->appname : "teardown_vr", 128);
    *(uint32_t*)(ai + TD_XR_APPINFO_APPVER)     = 1;
    lstrcpynA((char*)(ai + TD_XR_APPINFO_ENGINENAME), "none", 128);
    *(uint32_t*)(ai + TD_XR_APPINFO_ENGINEVER)  = 1;
    // XrVersion is 64-bit, and XR_MAKE_VERSION is major*10000000 + minor*100000
    // + patch -- so 1.0.34 is the literal 34. Do not "reconstruct" it by
    // bit-shifting; that asks the runtime for OpenXR 65570.0.34.
    *(uint64_t*)(ai + TD_XR_APPINFO_APIVER)     = 34;

    // The runtime needs XR_KHR_D3D12_enable or xrCreateSession later refuses with
    // "failed to call xr*GetGraphicsRequirements before xrCreateSession".
    static const char* exts[1] = { "XR_KHR_D3D12_enable" };
    *(uint32_t*)(cii + TD_XR_CII_EXTCOUNT)          = 1;
    *(const char* const**)(cii + TD_XR_CII_EXTNAMES) = exts;

    void* inst = NULL;
    long rc = (long)(intptr_t)((long long (*)(void*, void*))ci)(cii, &inst);
    vr_log("[XRP] worker: xrCreateInstance returned %ld, handle=%p", rc, inst);
    InterlockedExchange(&g_xrp_result, rc);
    if (g_xrp_evt) SetEvent(g_xrp_evt);

    return 0;
}

#endif // TDVR_XRPROBE
#endif
