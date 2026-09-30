/* No include here on purpose: iat_census.h includes this header for px_worker,
 * so including it back would recurse. Everything needed here is either a
 * Windows type or already defined by the including translation unit. */

#ifndef TDVR_PIXEL_PROBE
#define TDVR_PIXEL_PROBE 0
#endif

/* The reporter cadence is owned by teardown_vr.c, which includes iat_census.h
 * before this file. Guarded because this header may also be compiled alone. */
#ifndef TDVR_IAT_REPORT_MS
#define TDVR_IAT_REPORT_MS 20000
#endif

/* Can I see the pixels?
 *
 * The frame boundary is settled: GDI32!SwapBuffers is called exactly once per
 * frame, 1:1 with glPolygonMode and two other per-frame calls, in two windows
 * at different frame rates. That was the question that decided where the
 * stereo hook goes. It was only half the answer, though -- knowing WHEN a frame
 * ends says nothing about whether I can READ it. A frame boundary I cannot see
 * pixels through is just a counter.
 *
 * So this does the smallest thing that could possibly fail loudly:
 *
 *   1. get the HDC the game is presenting to, via wglGetCurrentDC
 *   2. read the back buffer with glReadPixels, GL_BACK
 *   3. report whether those pixels are non-uniform
 *
 * The uniformity test is the whole point. A black screen, an uninitialised
 * buffer and a real frame all return "no error" from glReadPixels. What
 * separates them is variance: a real rendered frame has pixels that differ
 * from each other, a blank one does not. Reporting the call succeeded would be
 * the exact class of mistake that produced "899 fps" earlier -- a success
 * signal that proves nothing. So it reports the pixel spread too, and a frame
 * where every sampled pixel is identical gets called out as NOT a frame.
 *
 * It runs on the game's own context, at the game's own present, with no
 * second GL context and no own device. Nothing is created that the game has to
 * tear down, which is how earlier attempts at this died.
 *
 * This is deliberately the dumbest possible probe. Every part of it that
 * could be clever -- async readback, format conversion, tiling -- is left out
 * on purpose and added only once the naive version proves it reads real data.
 */

#if TDVR_PIXEL_PROBE

/* SGL | GL_RGB | GL_UNSIGNED_BYTE, no alignment slack. glReadPixels packs
 * rows with default 4-byte alignment; RGB8 rows are a multiple of 4 only if
 * the width is, and padding between rows is not pixels. Using a width that is a
 * multiple of 4 removes the question entirely for this first probe. */
#define TDVR_PROBE_W 64
#define TDVR_PROBE_H 4

typedef unsigned char  tdvr_u8;
typedef int            tdvr_i32;

static tdvr_u8  g_px[TDVR_PROBE_W * TDVR_PROBE_H * 3];
static volatile LONG64 g_px_reads;
static volatile LONG64 g_px_gl_errors;
static volatile LONG64 g_px_uniform;      /* frames that read back perfectly flat  */
static volatile LONG64 g_px_blank_gl;     /* frames glReadPixels refused            */
static volatile LONG64 g_px_no_context;   /* frames with no current GL context     */
static volatile LONG64 g_px_no_hdc;       /* frames with no current HDC            */
static volatile LONG64 g_px_varying;      /* frames that carried real detail       */
static volatile LONG64 g_px_idle;         /* wait cycles that timed out -- if this  */
                                           /* climbs, the probe is not looking at
                                            * the right counter and every zero below
                                            * is a measurement failure, not a fact */

/* GL tokens. Taken from gl.h rather than hand-typed so a wrong constant cannot
 * silently become "read succeeded, data is garbage". */
#ifndef GL_BACK
#define GL_BACK                 0x0405
#endif
#ifndef GL_RGB
#define GL_RGB                  0x1907
#endif
#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE        0x1401
#endif
#ifndef GL_NO_ERROR
#define GL_NO_ERROR             0
#endif

/* Void* casts, not function-type casts: casting a function pointer to another
 * function pointer type is a constraint violation, and mingw rejects it. Every
 * assignment therefore goes through void* exactly once. */
/* These are POINTERS to functions, not function types. The difference matters:
 * a function type cannot be a struct field, so a union that hides a GetProcAddress
 * result has to hold "pointer to function type" on both sides. */
typedef void      (*TDVR_PFNGLREADPIXELS)(int, int, int, int, int, int, void *);
typedef tdvr_u8 * (*TDVR_PFNGLGETSTRING)(unsigned);
typedef const unsigned char * (*TDVR_PFNGLGETERROR)(void);

/* Resolve the three entry points once. These are GL 1.1 core, so they must come
 * from the same context the game is using -- not from a fresh one, and not from
 * a second load of opengl32. */
static TDVR_PFNGLREADPIXELS     g_glReadPixels;
static TDVR_PFNGLGETSTRING      g_glGetString;
static TDVR_PFNGLGETERROR       g_glGetError;

static int tdvr_px_resolve(void)
{
    if (g_glReadPixels) return 1;

    HMODULE gl = LoadLibraryA("OPENGL32.dll");
    if (!gl) { vr_log("  px: cannot load OPENGL32.dll"); return 0; }

    /* Assignment through a union, not a cast. Casting a FARPROC (a function
     * pointer) directly to another function pointer type is a constraint
     * violation and mingw rejects it outright; the union is the standard way
     * to move a FARPROC into a correctly-typed pointer without one. */
    union { FARPROC p; TDVR_PFNGLREADPIXELS f; } u1;
    union { FARPROC p; TDVR_PFNGLGETSTRING  f; } u2;
    union { FARPROC p; TDVR_PFNGLGETERROR   f; } u3;
    u1.p = GetProcAddress(gl, "glReadPixels");
    u2.p = GetProcAddress(gl, "glGetString");
    u3.p = GetProcAddress(gl, "glGetError");
    g_glReadPixels = u1.f;
    g_glGetString  = u2.f;
    g_glGetError   = u3.f;

    if (!g_glReadPixels || !g_glGetString || !g_glGetError) {
        vr_log("  px: glReadPixels=%p glGetString=%p glGetError=%p",
               (void *)g_glReadPixels, (void *)g_glGetString, (void *)g_glGetError);
        return 0;
    }
    vr_log("  px: GL_READPIXELS resolved (GL 1.1 core, same context as the game)");
    return 1;
}

/* Called from inside the real SwapBuffers, before it runs. The game has just
 * finished drawing this frame and has not yet handed it over, so GL_BACK is
 * still the frame we want. */
/* File scope on purpose. It started life as a static inside tdvr_px_sample(),
 * which made it invisible to the on-game-thread handler added later -- the same
 * duplication trap that produced the hard lockup, where two copies of one
 * piece of state disagreed. One resolver, one pointer, one tried-flag.
 *
 * wglGetCurrentDC has a prototype from wingdi.h, so the typedef is the library's
 * own pointer type, not a local redefinition. */
static HDC (WINAPI *g_wglGetCurrentDC)(void);
static volatile LONG g_px_tried_dc;

static HDC tdvr_px_current_dc(void)
{
    if (!g_px_tried_dc) {
        if (InterlockedCompareExchange(&g_px_tried_dc, 1, 0) == 0) {
            HMODULE gdi = LoadLibraryA("GDI32.dll");
            if (gdi) {
                union { FARPROC p; HDC (WINAPI *f)(void); } u;
                u.p = GetProcAddress(gdi, "wglGetCurrentDC");
                g_wglGetCurrentDC = u.f;
            }
        }
    }
    return g_wglGetCurrentDC ? g_wglGetCurrentDC() : NULL;
}

static void tdvr_px_sample(void)
{
    InterlockedIncrement64(&g_px_reads);

    if (!tdvr_px_resolve()) return;

    /* GL context and HDC come from the game's own current state. If either is
     * absent, this hook fired somewhere that is not the game's present, and
     * that is worth counting separately rather than folding into "no pixels". */
    HDC hdc = tdvr_px_current_dc();
    if (!hdc) { InterlockedIncrement64(&g_px_no_hdc); return; }
    (void)hdc;

    const tdvr_u8 *vendor = g_glGetString ? g_glGetString(0x1F00) : NULL; /* GL_VENDOR */
    if (!vendor || !*vendor) {
        /* No context. glGetString returns NULL here rather than crashing, so
         * this is the cheap way to know whether we are actually inside GL. */
        InterlockedIncrement64(&g_px_no_context);
        return;
    }

    /* Drain any error left by the game so our own status is unambiguous. */
    while (g_glGetError() != GL_NO_ERROR) { }

    g_glReadPixels(0, 0, TDVR_PROBE_W, TDVR_PROBE_H,
                   GL_RGB, GL_UNSIGNED_BYTE, g_px);

    if (g_glGetError() != GL_NO_ERROR) {
        InterlockedIncrement64(&g_px_blank_gl);
        return;
    }

    /* The test that matters. One flat colour, or one repeated value, is what a
     * blank or uninitialised buffer looks like. A rendered frame does not do
     * that -- even a black background has lit geometry on it. */
    int min = 255, max = 0;
    int nonzero = 0;
    int total = TDVR_PROBE_W * TDVR_PROBE_H * 3;
    for (int i = 0; i < total; i++) {
        int v = g_px[i];
        if (v < min) min = v;
        if (v > max) max = v;
        if (v)    nonzero++;
    }

    if (max == min)  InterlockedIncrement64(&g_px_uniform);
    else             InterlockedIncrement64(&g_px_varying);
    (void)nonzero;
}


/* The handler the call stub invokes, on the game's own thread.
 *
 * This is the whole reason the call stub exists. Reading GL_BACK from a worker
 * thread returned "no current HDC" 9624 times out of 9624, because a GL context
 * is current only on the thread that made it current. Called from inside
 * SwapBuffers, the context is current by definition -- the game is presenting
 * through it.
 *
 * It runs before the real SwapBuffers, so the back buffer still holds the frame
 * that was just drawn. That is the only moment worth sampling: after the call
 * returns, the buffer belongs to DWM.
 *
 * Cost matters. glReadPixels forces a pipeline flush and a stall, and this runs
 * inside the game's present, so doing it every frame would halve the frame rate
 * at best. Sampled every Nth frame instead, with N chosen so the probe costs
 * far less than the frame it is measuring. Reporting that ratio is the honest
 * thing to do -- a probe that quietly halves the game's performance and reports
 * a clean number is worse than no probe.
 */
/* Brightness accumulators, declared OUTSIDE the guard that hides the rest of
 * the on-present state. The report below reads them and is gated only by
 * TDVR_PIXEL_PROBE, so keeping them inside TDVR_IAT_CALL_STUB made eight of the
 * thirty-two flag combinations fail to compile -- identical to the guard
 * mismatch that left frame_reporter undefined, and invisible until all
 * combinations are built.
 *
 * I also committed and pushed that regression without reading the build count
 * first. The number under the command was the thing to look at. */
static volatile LONG64 g_px_sum_min;
static volatile LONG64 g_px_sum_max;
static volatile LONG64 g_px_sum_all;
/* The handler's own sample count. It cannot share g_px_reads with the worker:
 * that counter belongs to tdvr_px_sample, which runs on a different thread and
 * has never had a context. Dividing the handler's brightness by the worker's
 * read count produced "0" for a healthy build, and the two numbers in one
 * report were describing two different threads without saying so. */
static volatile LONG64 g_px_on_thread_reads;
/* Three-way split of what the read actually returned. "flat" cannot tell a
 * black frame from a white one, and the difference matters: every one of the
 * reads being pure black in a level, while a legal screen read as an image,
 * says the frame is not in GL_BACK at the moment SwapBuffers runs. */
static volatile LONG64 g_px_pure_black;
static volatile LONG64 g_px_near_black;
static volatile LONG64 g_px_has_light;

#if TDVR_IAT_CALL_STUB
#if TDVR_PIXEL_PROBE
/* The accumulators live OUTSIDE the guard, because the report that reads them
 * is gated only by TDVR_PIXEL_PROBE. Inside this block they were invisible to
 * the PIXEL_PROBE=1 / CALL_STUB=0 builds, so eight of the thirty-two stopped
 * compiling -- the same guard mismatch as the frame_reporter that was never
 * defined, and the same reason it only shows up when all combinations are built.
 *
 * I committed and pushed that regression without reading the count first,
 * which is the part worth remembering: the number under the command was the
 * thing I should have looked at. */
static volatile LONG64 g_px_on_game_thread;
static volatile LONG64 g_px_here_hdc;
static volatile LONG64 g_px_here_ctx;

#ifndef TDVR_PIXEL_EVERY
#define TDVR_PIXEL_EVERY 60      /* one read every 60 presents */
#endif

static void tdvr_px_on_present(HDC real)
{
    InterlockedIncrement64(&g_px_on_game_thread);

    /* real IS the game's HDC. It arrives in RCX by the ordinary calling
     * convention, because the stub never writes that register -- see the
     * contract at the TdvrIatHandler typedef. No lookup, no resolver, no
     * thread-local guessing: the game handed it over by calling us.
     *
     * The two previous attempts to obtain this handle both failed. A worker
     * thread asking wglGetCurrentDC() got NULL 9624 times out of 9624, because
     * a GL context is current only on the thread that made it current. Passing
     * a CONTEXT* and reading ctx->Rcx got garbage, because a stub is not an
     * exception handler and has no context of the caller to give. */
    if (real) InterlockedIncrement64(&g_px_here_hdc);

    static volatile LONG64 n;
    LONG64 k = InterlockedIncrement64(&n);
    if (k % TDVR_PIXEL_EVERY) return;

    if (!real) return;
    if (!tdvr_px_resolve()) return;

    const tdvr_u8 *vendor = g_glGetString ? g_glGetString(0x1F00) : NULL;
    if (vendor && *vendor) InterlockedIncrement64(&g_px_here_ctx);

    while (g_glGetError() != GL_NO_ERROR) { }
    g_glReadPixels(0, 0, TDVR_PROBE_W, TDVR_PROBE_H,
                   GL_RGB, GL_UNSIGNED_BYTE, g_px);
    if (g_glGetError() != GL_NO_ERROR) {
        InterlockedIncrement64(&g_px_blank_gl);
        return;
    }
    InterlockedIncrement64(&g_px_on_thread_reads);
    int min = 255, max = 0;
    int total = TDVR_PROBE_W * TDVR_PROBE_H * 3;
    for (int i = 0; i < total; i++) {
        int v = g_px[i];
        if (v < min) min = v;
        if (v > max) max = v;
    }
    if (min == 0 && max == 0) InterlockedIncrement64(&g_px_pure_black);
    else if (max < 24)     InterlockedIncrement64(&g_px_near_black);
    else                   InterlockedIncrement64(&g_px_has_light);
    if (max == min) InterlockedIncrement64(&g_px_uniform);
    else            InterlockedIncrement64(&g_px_varying);

    /* A variance test says a frame is not uniform. It cannot say what is in
     * it, and "not uniform" is true of a death screen, a loading frame and a
     * menu just as much as of gameplay. So the extremes and the mean of the
     * sample get reported too: a 4x4 read of a real scene spreads across a
     * wide range with a mid-range mean, while a black or white frame collapses
     * to one end. This is the difference between "something was there" and
     * "something was there and I can say roughly what".
     *
     * Accumulated rather than logged per sample, because logging 3000 times
     * would be the same log spam that once made this file look dead. The
     * averages are over samples that actually read, not over presents. */
    InterlockedExchangeAdd64(&g_px_sum_min, (LONG64)min);
    InterlockedExchangeAdd64(&g_px_sum_max, (LONG64)max);
    InterlockedExchangeAdd64(&g_px_sum_all, (LONG64)(min + max) / 2);
}
#endif /* TDVR_PIXEL_PROBE */
#endif /* TDVR_IAT_CALL_STUB */

static void tdvr_px_report(void)
{
    LONG64 reads   = g_px_reads;
    LONG64 ctx     = g_px_no_context;
    LONG64 hdc     = g_px_no_hdc;
    LONG64 glerr   = g_px_blank_gl;
    LONG64 flat    = g_px_uniform;
    LONG64 varying = g_px_varying;
    LONG64 idle   = g_px_idle;
    LONG64 onthreads = g_px_on_thread_reads;
    LONG64 black   = g_px_pure_black;
    LONG64 neblk   = g_px_near_black;
    LONG64 lit     = g_px_has_light;
#if TDVR_IAT_CALL_STUB && TDVR_PIXEL_PROBE
    LONG64 onthr  = g_px_on_game_thread;
    LONG64 thdc   = g_px_here_hdc;
    LONG64 tctx   = g_px_here_ctx;
#endif

    /* Every outcome is reported, including none of them. A probe that says
     * "no pixel activity" without saying which of the four ways it failed is
     * indistinguishable from a probe that was never called. */
    vr_log("PIXEL PROBE: %lld frames at the present", reads);
#if TDVR_IAT_CALL_STUB && TDVR_PIXEL_PROBE
    vr_log("  ON GAME THREAD     %lld presents", onthr);
    vr_log("    of which had HDC  %lld", thdc);
    vr_log("    of which had ctx  %lld", tctx);
    if (thdc == onthr && onthr > 0)
        vr_log("  -> every present carried the game's own HDC: the handler is "
               "on the presenting thread inside the real call.");
    else if (tctx > 0)
        vr_log("  -> GL context is current here, but the HDC was missing, so "
               "the handle is not coming from wglGetCurrentDC.");
    else
        vr_log("  -> neither HDC nor GL context: this is not the thread that "
               "presents. Check that SwapBuffers goes through the stub.");
#endif
    vr_log("  idle wait cycles   %lld", idle);
    if (idle > reads * 100) {
        vr_log("  -> idle cycles vastly exceed samples: the worker is NOT seeing");
        vr_log("     the SwapBuffers counter move. Everything below is unmeasured.");
        return;
    }
    vr_log("  no current HDC       %lld", hdc);
    vr_log("  no current GL ctx    %lld", ctx);
    vr_log("  glReadPixels error   %lld", glerr);
    vr_log("  flat (not a frame)   %lld", flat);
    vr_log("  VARIED (real pixels) %lld", varying);
    if (onthreads > 0) {
        /* Averages over the samples that actually read a frame. Without these
         * three numbers "VARIED" is a yes/no that cannot distinguish a
         * gameplay frame from a loading screen or a death screen -- all three
         * are non-uniform, and only the levels say which one it was. */
        vr_log("  ON-THREAD reads    %lld", onthreads);
        vr_log("  brightness  min %.0f  max %.0f  mid %.0f",
                (double)g_px_sum_min / (double)onthreads,
                (double)g_px_sum_max / (double)onthreads,
                (double)g_px_sum_all / (double)onthreads);
        vr_log("  on-thread: pure black %lld, near black %lld, has light %lld",
                black, neblk, lit);
        if (onthreads > 0 && black == onthreads)
            vr_log("  -> every read returned pure black (0,0,0). GL_BACK is");
            vr_log("     empty at the moment SwapBuffers runs, so the frame is");
            vr_log("     already gone or was never drawn there. Reading earlier,");
            vr_log("     or reading a different buffer, is the next question.");
    }

    if (!reads) {
        vr_log("  never ran -- the present hook is not calling into this");
        return;
    }
    if (hdc == reads) {
        vr_log("  -> every call had no HDC: this is firing off the game's");
        vr_log("     present thread, not inside its GL context");
        return;
    }
    if (ctx == reads - hdc) {
        vr_log("  -> an HDC existed but no GL context: hook is on the wrong thread");
        return;
    }
    if (!varying) {
        vr_log("  -> glReadPixels succeeded but the buffer was flat every time.");
        vr_log("     That is not a rendered frame. Either the read lands on a");
        vr_log("     different buffer than the one being drawn, or the context is");
        vr_log("     current but the game renders to an FBO, not to GL_BACK.");
        return;
    }
    /* Show a few actual bytes so the claim can be checked rather than trusted. */
    vr_log("  first pixels RGB:");
    for (int row = 0; row < 2; row++) {
        char line[128];
        int k = 0;
        for (int col = 0; col < 8; col++) {
            const tdvr_u8 *p = g_px + (row * TDVR_PROBE_W + col) * 3;
            k += _snprintf(line + k, sizeof line - k, "(%3u,%3u,%3u) ",
                           p[0], p[1], p[2]);
            if (k >= (int)sizeof line - 16) break;
        }
        vr_log("    %s", line);
    }
}

static DWORD WINAPI px_reporter(LPVOID arg)
{
    (void)arg;
    for (;;) {
        Sleep(TDVR_IAT_REPORT_MS);
        tdvr_px_report();
    }
}

#endif /* TDVR_PIXEL_PROBE */
