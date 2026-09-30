#if TDVR_PIXEL_PROBE
/* An off-screen target of our own, so the game never has to be read.
 *
 * WHY NOT KEEP READING THE GAME'S BACK BUFFER
 * glReadPixels on the game's back buffer was measured at 239138 bytes of
 * private memory per call, twice, by two independent windows. At one read per
 * 60 presents that is roughly 240 MB per minute, which is what turned a
 * 4.7 GB process into a 357 MB one. The read was never wrong -- it returned
 * real frames, 97 of every 101 -- it was just too expensive to be a mechanism.
 *
 * THE PBUFFER PATH WAS BUILT AND MEASURED, AND IT IS DEAD ON THIS MACHINE
 * A pbuffer is the textbook answer: a WGL surface with no window, a context of
 * our own, glReadPixels on our own buffer. It was built, injected and measured
 * on 2026-09-30, and it does not exist on an NVIDIA driver:
 *
 *   wglGetProcAddress("wglCreatePbufferARB")   -> a valid, callable address
 *   glGetString(GL_EXTENSIONS)                 -> does not list WGL_ARB_pbuffer
 *   wglCreatePbufferARB(win, 512, 512, 12, a)   -> NULL
 *
 * The driver exports the entry point and does not implement it. WGL_ARB_pbuffer
 * is inherited from GLX and NVIDIA's WGL does not provide it. Official sample
 * code checks the extension string before calling, so on this machine it would
 * never have reached the call at all -- and the string is a poor gate anyway,
 * because a function pointer from wglGetProcAddress is the thing that actually
 * answers the question. That is why this file calls first and reports second.
 *
 * WHAT REPLACES IT: A FRAMEBUFFER OBJECT
 * An FBO is the same idea in pure GL: a texture we allocate, a framebuffer we
 * bind, and rendering that lands in the texture instead of in the window. It
 * needs nothing from WGL, so the driver above cannot affect it, and GL 4.6 has
 * it in core.
 *
 * WHY IT RUNS ON ITS OWN THREAD
 * A context is current to one thread. Creating it on the presenting thread would
 * mean making it current over the game's own context and handing it back -- a
 * swap that can land between the game's draw and its present, and the game's
 * next frame would render into our target. A dedicated thread that creates and
 * owns the context has no such window.
 *
 * The cost is that a context can only be current on the thread that made it
 * current, which is precisely what killed the worker-thread probe: it asked
 * wglGetCurrentDC and got NULL 9624 times. Here it is not asked, because the
 * thread makes its own context current itself. */

typedef HGLRC  (WINAPI *PFN_wglCreateContext)(HDC);
typedef BOOL   (WINAPI *PFN_wglDeleteContext)(HGLRC);
typedef HGLRC  (WINAPI *PFN_wglGetCurrentContext)(void);
typedef HDC    (WINAPI *PFN_wglGetCurrentDC)(void);
typedef BOOL   (WINAPI *PFN_wglMakeCurrent)(HDC, HGLRC);
typedef PROC   (WINAPI *PFN_wglGetProcAddress)(LPCSTR);

static PFN_wglCreateContext     g_wglCreateContext;
static PFN_wglDeleteContext     g_wglDeleteContext;
static PFN_wglGetCurrentContext g_wglGetCurrentContext;
static PFN_wglGetCurrentDC      g_wglGetCurrentDC;
static PFN_wglMakeCurrent       g_wglMakeCurrent;
static PFN_wglGetProcAddress    g_wglGetProcAddress;

/* Core GL 3.0 FBO entry points, typed by hand rather than pulled from glext.h,
 * so this file does not depend on which GL headers a given mingw ships -- the
 * same reason pixel_probe.h declares its own function pointer types. */
typedef void     (WINAPI *PFN_glGenFramebuffers)(int, unsigned *);
typedef void     (WINAPI *PFN_glDeleteFramebuffers)(int, const unsigned *);
typedef void     (WINAPI *PFN_glBindFramebuffer)(unsigned, unsigned);
typedef unsigned (WINAPI *PFN_glCheckFramebufferStatus)(unsigned);
typedef void     (WINAPI *PFN_glGenTextures)(int, unsigned *);
typedef void     (WINAPI *PFN_glDeleteTextures)(int, const unsigned *);
typedef void     (WINAPI *PFN_glBindTexture)(unsigned, unsigned);
typedef void     (WINAPI *PFN_glTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
typedef void     (WINAPI *PFN_glTexParameteri)(unsigned, unsigned, int);
typedef void     (WINAPI *PFN_glFramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int);
typedef void     (WINAPI *PFN_glDrawBuffer)(unsigned);
typedef void     (WINAPI *PFN_glReadBuffer)(unsigned);
typedef void     (WINAPI *PFN_glGenVertexArrays)(int, unsigned *);
typedef void     (WINAPI *PFN_glBindVertexArray)(unsigned);
typedef void     (WINAPI *PFN_glDeleteVertexArrays)(int, const unsigned *);

static PFN_glGenFramebuffers        g_glGenFramebuffers;
static PFN_glDeleteFramebuffers     g_glDeleteFramebuffers;
static PFN_glBindFramebuffer        g_glBindFramebuffer;
static PFN_glCheckFramebufferStatus g_glCheckFramebufferStatus;
static PFN_glGenTextures            g_glGenTextures;
static PFN_glDeleteTextures         g_glDeleteTextures;
static PFN_glBindTexture            g_glBindTexture;
static PFN_glTexImage2D             g_glTexImage2D;
static PFN_glTexParameteri          g_glTexParameteri;
static PFN_glFramebufferTexture2D   g_glFramebufferTexture2D;
static PFN_glDrawBuffer             g_glDrawBuffer;
static PFN_glReadBuffer             g_glReadBuffer;
static PFN_glGenVertexArrays        g_glGenVertexArrays;
static PFN_glBindVertexArray        g_glBindVertexArray;
static PFN_glDeleteVertexArrays     g_glDeleteVertexArrays;

/* Counters, each with exactly one owner -- the same rule the audit enforces for
 * the probe. A counter that two threads touch is a number in the log that means
 * two things, and that is how "VARIED 692" came to sit beside "130 reads". */
static volatile LONG64 g_fb_attempts;
static volatile LONG64 g_fb_context;
static volatile LONG64 g_fb_objects;
static volatile LONG64 g_fb_complete;
static volatile LONG64 g_fb_draws;
static volatile LONG64 g_fb_reads;
static volatile LONG64 g_fb_uniform;
static volatile LONG64 g_fb_varying;
static volatile LONG64 g_fb_black;

static void tdvr_fb_report(void)
{
    LONG64 a = g_fb_attempts, c = g_fb_context, o = g_fb_objects;
    LONG64 cm = g_fb_complete, d = g_fb_draws, r = g_fb_reads;
    LONG64 un = g_fb_uniform, va = g_fb_varying, bl = g_fb_black;
    vr_log("FBO: attempts %lld, context %lld, objects %lld, complete %lld",
           a, c, o, cm);
    if (!a) { vr_log("  -> thread never ran"); return; }
    if (!c) { vr_log("  -> no context of our own, so nothing could own a target"); return; }
    if (!o) { vr_log("  -> no objects: the FBO entry points were absent on a "
                     "GL 4.6 context, which should not happen"); return; }
    if (!cm) { vr_log("  -> objects exist but the framebuffer is incomplete"); return; }
    vr_log("  draws %lld, reads %lld", d, r);
    vr_log("  uniform %lld, VARIED %lld, pure black %lld", un, va, bl);
    if (!r) { vr_log("  -> complete but never read; a complete framebuffer is "
                     "not yet a usable one"); return; }
    if (va) vr_log("  -> our own target holds a real image. The read is ours, the "
                   "game's frame was never touched, and the path XR needs is now "
                   "proven end to end.");
    else   vr_log("  -> uniform, so the draw did not land. A readback of an "
                   "unwritten texture is the same black buffer the back-buffer "
                   "path produced, and it is not evidence about the game.");
}

static DWORD WINAPI tdvr_fb_thread(LPVOID unused)
{
    InterlockedIncrement64(&g_fb_attempts);

    HMODULE gl = GetModuleHandleA("OPENGL32.dll");
    if (!gl) gl = LoadLibraryA("OPENGL32.dll");
    HMODULE user = GetModuleHandleA("USER32.dll");
    if (!user) user = LoadLibraryA("USER32.dll");
    HMODULE gdi = GetModuleHandleA("GDI32.dll");
    if (!gdi) gdi = LoadLibraryA("GDI32.dll");
    if (!gl || !user || !gdi) { vr_log("FBO: no gl/user32/gdi32"); return 0; }

    union { FARPROC p; PFN_wglGetProcAddress f; } ua;
    ua.p = GetProcAddress(gl, "wglGetProcAddress");
    g_wglGetProcAddress = ua.f;
    if (!g_wglGetProcAddress) { vr_log("FBO: no wglGetProcAddress"); return 0; }

    /* Resolve the WGL calls before using any of them. The pbuffer version of
     * this file declared wglCreateContext and wglMakeCurrent and never assigned
     * them, then called through the null pointer: the thread vanished between
     * two log lines without a fault, which is the least diagnosable outcome
     * there is. Each is checked, and a missing one is named. */
    #define TDVR_FB_WGL(nm, type, var)                                    \
        do { union { FARPROC p; type f; } u;                              \
             u.p = GetProcAddress(gl, nm); var = u.f;                     \
             if (!var) { vr_log("FBO: %s ABSENT from opengl32", nm);     \
                         return 0; } } while (0)
    TDVR_FB_WGL("wglCreateContext", PFN_wglCreateContext, g_wglCreateContext);
    TDVR_FB_WGL("wglMakeCurrent",   PFN_wglMakeCurrent,   g_wglMakeCurrent);
    #undef TDVR_FB_WGL
    union { FARPROC p; PFN_wglDeleteContext     f; } ud;
    union { FARPROC p; PFN_wglGetCurrentContext f; } uc;
    union { FARPROC p; PFN_wglGetCurrentDC      f; } uD;
    ud.p = GetProcAddress(gl, "wglDeleteContext");
    uc.p = GetProcAddress(gl, "wglGetCurrentContext");
    uD.p = GetProcAddress(gl, "wglGetCurrentDC");
    g_wglDeleteContext     = ud.f;
    g_wglGetCurrentContext = uc.f;
    g_wglGetCurrentDC      = uD.f;

    /* A window is only a surface a driver can be asked for a format on. It is
     * never shown, never made current, and destroyed on the way out. */
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "tdvr_fb_probe";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "tdvr_fb_probe", "fb", WS_POPUP,
                                0, 0, 16, 16, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { vr_log("FBO: no window"); return 0; }
    HDC win = GetDC(hwnd);
    if (!win) { vr_log("FBO: no DC"); DestroyWindow(hwnd); return 0; }

    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize    = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags  = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32; pfd.cRedBits = 8; pfd.cGreenBits = 8; pfd.cBlueBits = 8;
    pfd.cAlphaBits = 8; pfd.cDepthBits = 24; pfd.cStencilBits = 8;
    int pf = ChoosePixelFormat(win, &pfd);
    if (!pf || !SetPixelFormat(win, pf, &pfd)) {
        vr_log("FBO: ChoosePixelFormat %d failed", pf);
        ReleaseDC(hwnd, win); DestroyWindow(hwnd); return 0;
    }

    HGLRC rc = g_wglCreateContext(win);
    if (!rc) { vr_log("FBO: wglCreateContext failed"); ReleaseDC(hwnd, win);
               DestroyWindow(hwnd); return 0; }
    if (!g_wglMakeCurrent(win, rc)) { vr_log("FBO: window ctx not current");
        if (g_wglDeleteContext) g_wglDeleteContext(rc);
        ReleaseDC(hwnd, win); DestroyWindow(hwnd); return 0; }
    InterlockedIncrement64(&g_fb_context);
    vr_log("FBO: our own context is current on OUR thread, format %d", pf);

    vr_log("  GL_VENDOR   %s", g_glGetString ? (const char *)g_glGetString(0x1F00) : "?");
    vr_log("  GL_RENDERER %s", g_glGetString ? (const char *)g_glGetString(0x1F01) : "?");
    vr_log("  GL_VERSION  %s", g_glGetString ? (const char *)g_glGetString(0x1F02) : "?");
    vr_log("  wglGetCurrentContext = %p (ours), wglGetCurrentDC = %p",
           g_wglGetCurrentContext ? (void *)g_wglGetCurrentContext() : NULL,
           g_wglGetCurrentDC ? (void *)g_wglGetCurrentDC() : NULL);

    #define TDVR_FB_GET(nm, type, var)                                    \
        do { union { FARPROC p; type f; } u;                              \
             u.p = g_wglGetProcAddress(nm); if (!u.p) u.p = GetProcAddress(gl, nm); \
             var = u.f; } while (0)
    TDVR_FB_GET("glGenFramebuffers",        PFN_glGenFramebuffers,        g_glGenFramebuffers);
    TDVR_FB_GET("glDeleteFramebuffers",     PFN_glDeleteFramebuffers,     g_glDeleteFramebuffers);
    TDVR_FB_GET("glBindFramebuffer",        PFN_glBindFramebuffer,        g_glBindFramebuffer);
    TDVR_FB_GET("glCheckFramebufferStatus", PFN_glCheckFramebufferStatus, g_glCheckFramebufferStatus);
    TDVR_FB_GET("glGenTextures",            PFN_glGenTextures,            g_glGenTextures);
    TDVR_FB_GET("glDeleteTextures",         PFN_glDeleteTextures,         g_glDeleteTextures);
    TDVR_FB_GET("glBindTexture",            PFN_glBindTexture,            g_glBindTexture);
    TDVR_FB_GET("glTexImage2D",             PFN_glTexImage2D,             g_glTexImage2D);
    TDVR_FB_GET("glTexParameteri",          PFN_glTexParameteri,          g_glTexParameteri);
    TDVR_FB_GET("glFramebufferTexture2D",   PFN_glFramebufferTexture2D,   g_glFramebufferTexture2D);
    TDVR_FB_GET("glDrawBuffer",             PFN_glDrawBuffer,             g_glDrawBuffer);
    TDVR_FB_GET("glReadBuffer",             PFN_glReadBuffer,             g_glReadBuffer);
    TDVR_FB_GET("glGenVertexArrays",        PFN_glGenVertexArrays,        g_glGenVertexArrays);
    TDVR_FB_GET("glBindVertexArray",        PFN_glBindVertexArray,        g_glBindVertexArray);
    TDVR_FB_GET("glDeleteVertexArrays",     PFN_glDeleteVertexArrays,     g_glDeleteVertexArrays);
    #undef TDVR_FB_GET

    #define TDVR_FB_NEED(cond, what)                                       \
        do { if (!(cond)) { vr_log("FBO: %s unavailable", what);           \
             g_wglMakeCurrent(NULL, NULL);                                \
             if (g_wglDeleteContext) g_wglDeleteContext(rc);               \
             ReleaseDC(hwnd, win); DestroyWindow(hwnd); return 0; } } while (0)
    TDVR_FB_NEED(g_glGenFramebuffers,        "glGenFramebuffers");
    TDVR_FB_NEED(g_glBindFramebuffer,        "glBindFramebuffer");
    TDVR_FB_NEED(g_glCheckFramebufferStatus, "glCheckFramebufferStatus");
    TDVR_FB_NEED(g_glGenTextures,            "glGenTextures");
    TDVR_FB_NEED(g_glBindTexture,            "glBindTexture");
    TDVR_FB_NEED(g_glTexImage2D,             "glTexImage2D");
    TDVR_FB_NEED(g_glTexParameteri,          "glTexParameteri");
    TDVR_FB_NEED(g_glFramebufferTexture2D,   "glFramebufferTexture2D");
    TDVR_FB_NEED(g_glDeleteFramebuffers,     "glDeleteFramebuffers");
    TDVR_FB_NEED(g_glDeleteTextures,         "glDeleteTextures");
    #undef TDVR_FB_NEED

    vr_log("  every FBO entry point resolved; GL 4.6 has them in core, so the "
           "missing WGL extension above does not matter here");

    if (!tdvr_px_resolve()) {
        vr_log("FBO: basic gl functions unavailable");
        g_wglMakeCurrent(NULL, NULL);
        if (g_wglDeleteContext) g_wglDeleteContext(rc);
        ReleaseDC(hwnd, win); DestroyWindow(hwnd); return 0;
    }
    if (!g_glViewport || !g_glClearColor || !g_glClear || !g_glDrawArrays ||
        !g_glFinish || !g_glReadPixels || !g_glGetError) {
        vr_log("FBO: one of the basic draw/read entry points is missing");
        g_wglMakeCurrent(NULL, NULL);
        if (g_wglDeleteContext) g_wglDeleteContext(rc);
        ReleaseDC(hwnd, win); DestroyWindow(hwnd); return 0;
    }

    const int W = 512, H = 512;
    unsigned fbo = 0, tex = 0;
    g_glGenTextures(1, &tex);
    g_glBindTexture(0x0DE1 /*GL_TEXTURE_2D*/, tex);
    g_glTexImage2D(0x0DE1, 0, 0x8058 /*GL_RGBA*/, W, H, 0,
                   0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, NULL);
    g_glTexParameteri(0x0DE1, 0x2801 /*GL_TEXTURE_MIN_FILTER*/, 0x2600 /*GL_LINEAR*/);
    g_glTexParameteri(0x0DE1, 0x2800 /*GL_TEXTURE_MAG_FILTER*/, 0x2600 /*GL_LINEAR*/);
    g_glTexParameteri(0x0DE1, 0x2802 /*GL_TEXTURE_WRAP_S*/, 0x812F /*GL_CLAMP_TO_EDGE*/);
    g_glTexParameteri(0x0DE1, 0x2803 /*GL_TEXTURE_WRAP_T*/, 0x812F /*GL_CLAMP_TO_EDGE*/);

    g_glGenFramebuffers(1, &fbo);
    g_glBindFramebuffer(0x8D40 /*GL_FRAMEBUFFER*/, fbo);
    g_glFramebufferTexture2D(0x8D40, 0x8CE0 /*GL_COLOR_ATTACHMENT0*/,
                             0x0DE1, tex, 0);
    InterlockedIncrement64(&g_fb_objects);

    unsigned st = g_glCheckFramebufferStatus(0x8D40);
    if (st != 0x8CD5 /*GL_FRAMEBUFFER_COMPLETE*/) {
        vr_log("FBO: framebuffer incomplete, status 0x%X", st);
        g_glBindFramebuffer(0x8D40, 0);
        g_wglMakeCurrent(NULL, NULL);
        if (g_wglDeleteContext) g_wglDeleteContext(rc);
        ReleaseDC(hwnd, win); DestroyWindow(hwnd); return 0;
    }
    InterlockedIncrement64(&g_fb_complete);
    vr_log("FBO: %dx%d RGBA texture attached, framebuffer COMPLETE", W, H);

    g_glDrawBuffer(0x8CE0 /*GL_COLOR_ATTACHMENT0*/);
    g_glReadBuffer(0x8CE0 /*GL_COLOR_ATTACHMENT0*/);
    g_glViewport(0, 0, W, H);

    /* A mid-green clear, not black. A black readback cannot be told apart from
     * a readback of a surface nothing was ever drawn to, which is the failure
     * the back-buffer path produced and which cost an hour of measuring a leak
     * that was not a leak. The colour makes the difference visible. */
    g_glClearColor(0.10f, 0.35f, 0.20f, 1.0f);
    g_glClear(0x00004000 /*GL_COLOR_BUFFER_BIT*/);
    g_glDrawArrays(0x0004 /*GL_TRIANGLES*/, 0, 3);
    if (g_glFinish) g_glFinish();
    InterlockedIncrement64(&g_fb_draws);

    const unsigned char *e0 = g_glGetError();
    if (e0 && *e0) vr_log("  glGetError after the draw: 0x%X", (unsigned)*e0);

    g_glReadPixels(0, 0, 64, 4, 0x1907 /*GL_RGB*/, 0x1401, g_px);
    const unsigned char *e1 = g_glGetError();
    InterlockedIncrement64(&g_fb_reads);
    if (e1 && *e1) {
        vr_log("  glReadPixels error 0x%X", (unsigned)*e1);
    } else {
        int mn = 255, mx = 0;
        const int tot = 64 * 4 * 3;
        for (int i = 0; i < tot; i++) { int v = g_px[i]; if (v < mn) mn = v; if (v > mx) mx = v; }
        if (mn == 0 && mx == 0) InterlockedIncrement64(&g_fb_black);
        if (mn == mx)          InterlockedIncrement64(&g_fb_uniform);
        else                   InterlockedIncrement64(&g_fb_varying);
        vr_log("  readback 64x4: min %d max %d  (%s)", mn, mx,
               (mn == mx) ? "UNIFORM" : "varied");
        /* (0.10, 0.35, 0.20) is (26, 89, 51) in 8-bit. A readback in that range
         * proves the texture was written and read back correctly. */
        vr_log("  the clear colour we asked for is (26,89,51)");
        if (mn == 26 && mx == 89)
            vr_log("  -> matches: the FBO was written to and read back correctly. "
                   "The target works and the game's frame was never involved.");
        else if (mn == 0 && mx == 0)
            vr_log("  -> pure black, so the draw did not reach the texture even "
                   "though the framebuffer reports complete. That is a driver "
                   "behaviour worth recording, not a guess.");
        else
            vr_log("  -> something, but not the clear colour: a partial write or a "
                   "stale read path. The numbers above are the evidence.");
    }

    /* Leave nothing behind. A context still current on this thread when the
     * thread ends takes the driver's per-thread state down with it. */
    g_glBindFramebuffer(0x8D40, 0);
    g_wglMakeCurrent(NULL, NULL);
    if (g_glDeleteFramebuffers) g_glDeleteFramebuffers(1, &fbo);
    if (g_glDeleteTextures)     g_glDeleteTextures(1, &tex);
    if (g_wglDeleteContext)     g_wglDeleteContext(rc);
    ReleaseDC(hwnd, win);
    DestroyWindow(hwnd);
    UnregisterClassA("tdvr_fb_probe", wc.hInstance);
    vr_log("FBO: released framebuffer, texture, context, DC and window");
    return 0;
}

static void tdvr_fb_start(void)
{
    HANDLE t = CreateThread(NULL, 0, tdvr_fb_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else vr_log("FBO: CreateThread failed");
}
#endif /* TDVR_PIXEL_PROBE */
