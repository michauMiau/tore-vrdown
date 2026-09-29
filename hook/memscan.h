// ---------------------------------------------------------------------------
// memscan.h - find the SceneDynamicBuffer by its contents, read-only.
//
// The strategy change. Everything so far inferred the SDB's location from
// static analysis of a packed executable, and every inference was wrong:
// 0x9E130 had a perfect signature and fired zero times; 0x087F40 was the only
// function reading renderer+0xB90 and also fired zero times; the frame hooks do
// not contain the 0x1F4 size at all, and renderer+0xB80..+0xB98 turned out to
// be null in the live object. Teardown ships packed, so the static image is the
// worst possible source of truth. The running process is not packed.
//
// So: walk what the renderer already points at, and recognise a 500-byte
// buffer by the shape of the data in it. A candidate must have, in one page:
//
//   +0x000  float4x4  a plausible view-projection   (finite, right magnitude)
//   +0x070  float4    pd = (1/m0, 1/m5, m8/m0, m9/m5)
//   +0x140  float4x4  mubOldStableVpMatrix, which for a running game is very
//                     close to mubStableVpMatrix at +0x100
//
// The last test is the strong one. mubVpMatrix at +0x000 is the current frame's
// camera, so it moves every frame; the two stable matrices are temporally
// filtered and sit almost on top of each other. A 500-byte block of noise will
// pass a finiteness check; it will not pass that.
//
// This module only ever reads. It writes nothing to the game, patches nothing,
// and holds no hook, so a mistake in it cannot corrupt a frame.
// ---------------------------------------------------------------------------
#ifndef TDVR_MEMSCAN_H
#define TDVR_MEMSCAN_H

#include <stdint.h>
#include <string.h>

#define TD_SCAN_HDR   32
#define TD_SCAN_MAX   (48 * 1024 * 1024)   // give up rather than stall the game

typedef struct {
    uint64_t addr;        // where the 500-byte block starts
    uint32_t vp[16];      // mubVpMatrix, as raw bits
    uint32_t pd[4];       // mubProjectionData
    float    pd_f[4];
    float    max_vp;      // largest |element| in the view-projection
    float    aspect;      // pd.x / pd.y, the horizontal aspect it implies
    uint32_t flags;       // which tests passed
} td_scan_hit;

#define TD_SCAN_F_FINITE  0x01
#define TD_SCAN_F_VP      0x02
#define TD_SCAN_F_PD      0x04
#define TD_SCAN_F_STABLE  0x08
// The algebraic one. A block without this flag is a shape match only and must
// never be treated as a SceneDynamicBuffer — the v27 candidates all cleared the
// other four bits and none of them was real.
#define TD_SCAN_F_FORMULA 0x10
#define TD_SCAN_F_ALL     0x1F

// There is deliberately no region test in this header.
//
// An earlier version had one, comparing `r->state & 0x0100` for PAGE_GUARD
// and `r->state & 0xFF` for PAGE_NOACCESS. That was wrong twice over: the MBI
// puts MEM_COMMIT in `State` and the page protection in a different field
// entirely, so the two masks were being applied to the same value, and
// PAGE_GUARD (0x100) can never be compared against `Protect & 0xFF` because it
// lives in the high byte. It was never called, so it never crashed — which is
// exactly why it survived so long next to a real crash in the code that *was*
// running.
//
// The walk does its region test inside td_read, where MEMORY_BASIC_INFORMATION
// is available with its real field names. This header stays free of
// windows.h so it can be tested on Linux, which is also why it cannot hold a
// correct region test at all.

static float td_f32(uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// Is this a plausible projection? Reject denormals, NaN, inf, and values so
// large they cannot be a matrix element of a normal 3D projection. A game
// running normally keeps these in roughly [1e-3, 1e3].
//
// The ceiling is 1e6, not infinity, and the reason is concrete: the first live
// walk reported matches with maxVP of 1.3e10 and 2.9e8. Those are the float
// reinterpretations of byte patterns like 0x4C8F0000 — that is, a 64-bit
// pointer read as two floats. A heap address is not a frustum matrix, and
// letting one through is what filled the hit table with fourteen copies of the
// same eight addresses.
//
// The floor matters just as much: an all-zero block is the uninitialised-buffer
// false positive, and 1e-3 rejects it without a separate test.
static int td_vp_plausible(const uint32_t* v, float* maxabs) {
    float m = 0.0f;
    for (int i = 0; i < 16; ++i) {
        float f = td_f32(v[i]);
        if (!(f == f)) return 0;                       // NaN
        if (f > 1e6f || f < -1e6f) return 0;          // inf, or a pointer
        float a = f < 0 ? -f : f;
        if (a > m) m = a;
    }
    if (m < 1e-3f) return 0;
    *maxabs = m;
    return 1;
}

static int td_msb(uint32_t e) { return (e >> 23) & 0xFF; }

static int td_pd_plausible(const uint32_t* pd, float out[4]) {
    for (int i = 0; i < 4; ++i) out[i] = td_f32(pd[i]);
    // pd.r = 1/m0 and pd.g = 1/m5. For any sane projection these are positive
    // and small — the cotangent of the half-FOV, so between about 0.01 and 100.
    //
    // The lower bound is not cosmetic. The first live run matched a run of
    // buffers whose pd was entirely zero and whose mubVpMatrix held values
    // like 1.3e10, which is the float reinterpretation of the byte pattern
    // 0x4C8F0000 — a pointer, not a projection. Zero passes "not NaN" and
    // "within 10 metres", so a 1/m0 of zero sailed through. 1/m0 is zero only
    // if m0 is infinite, which no real frustum matrix has.
    for (int i = 0; i < 2; ++i) {
        if (!(out[i] == out[i])) return 0;
        if (out[i] <= 0.01f) return 0;
        if (out[i] > 100.0f) return 0;
    }
    // pd.z and pd.w are eye offsets in metres. In mono rendering they are the
    // projection's asymmetry terms, so a few tenths at most; a huge value here
    // means we are looking at the wrong block. This is deliberately loose —
    // in stereo they are exactly the half-IPD, +-0.032 m, and the whole point
    // is that we must not reject them for being small.
    for (int i = 2; i < 4; ++i) {
        if (!(out[i] == out[i])) return 0;
        if (out[i] > 10.0f || out[i] < -10.0f) return 0;
    }
    return 1;
}

static uint32_t td_sub(uint32_t a, uint32_t b, int* ok) {
    if (td_msb(a) < td_msb(b)) { *ok = 0; return 0; }
    uint32_t r = a - b;
    if (td_msb(r) > td_msb(a)) { *ok = 0; return 0; }   // underflow
    *ok = 1;
    return r;
}

// Are the two temporally-stable matrices actually close to each other?
//
// +0x100 is mubStableVpMatrix and +0x140 is mubOldStableVpMatrix. They are the
// same matrix from consecutive frames, jittered by at most a fraction of a
// pixel of TAA offset and by whatever the reprojection error is.
//
// Compare the top 12 mantissa bits and require the exponents to be equal or
// adjacent. An earlier version of this function *skipped* an element when the
// exponents differed, which inverted the test: a stable matrix paired with a
// wildly different one passed, because the element that should have rejected
// it was the element being ignored. Any real difference now fails.
//
// The tolerance is deliberately loose — 48/4096 of a binade is about 1.2% —
// because TAA jitter and reprojection error are both legitimately present, and
// the test is for structure, not for equality.
static int td_stable_close(const uint32_t* a, const uint32_t* b) {
    for (int i = 0; i < 16; ++i) {
        uint32_t ea = (a[i] >> 23) & 0xFF;
        uint32_t eb = (b[i] >> 23) & 0xFF;
        // A float's biased exponent for 1.0 is 0x7F, for 0.5 it is 0x7E, and
        // 0xFF is reserved for inf and NaN. An earlier version of this test
        // used a 0x7E cutoff, which rejected 1.0 itself and so every candidate
        // including a genuine SceneDynamicBuffer.
        //
        // Reject only inf/NaN, plus anything so large it cannot be a projection
        // element. 0xA0 is about 2^25, far above any value a real frustum
        // matrix holds, and far below the range where the exponent test below
        // would start confusing itself.
        if (ea >= 0xFF || eb >= 0xFF) return 0;
        if (ea > 0xA0 || eb > 0xA0) return 0;
        // Exponents must agree, or be adjacent where a value sits near a
        // binade boundary. Anything more than a factor of two apart is not
        // the same matrix.
        int d = (int)ea - (int)eb;
        if (d < 0) d = -d;
        if (d > 1) return 0;
        uint32_t ma = a[i] & 0x007FFFFF, mb = b[i] & 0x007FFFFF;
        if (d == 0) {
            uint32_t dm = ma > mb ? ma - mb : mb - ma;
            if (dm > 0x30000) return 0;            // ~0.4% of a binade
        }
    }
    return 1;
}

// Test one 500-byte candidate. Returns the flag bits that passed, 0 if the
// block is plainly not a SceneDynamicBuffer.
// Test one 500-byte candidate.
//
// Order matters: the cheap shape checks run first and the algebraic one last,
// because the formula test is the only one that cannot be fooled, so there is
// no point paying for it on blocks that are already obviously wrong.
static int td_matches_shader_formula(const unsigned char* p, float* aspect_out);
static uint32_t td_test_candidate(const unsigned char* p, td_scan_hit* out) {
    const uint32_t* vp    = (const uint32_t*)(p + 0x000);
    const uint32_t* pd    = (const uint32_t*)(p + 0x070);
    const uint32_t* stable= (const uint32_t*)(p + 0x100);
    const uint32_t* oldst = (const uint32_t*)(p + 0x140);

    uint32_t f = 0;
    float maxabs = 0.0f;

    // Finiteness across the whole struct. Cheap, and it rejects most noise.
    for (int i = 0; i < 0x1F4 / 4; ++i) {
        uint32_t e = td_msb(((const uint32_t*)p)[i]);
        if (e == 0xFF) return 0;                     // NaN or inf anywhere
    }
    f |= TD_SCAN_F_FINITE;

    if (!td_vp_plausible(vp, &maxabs)) return f;
    f |= TD_SCAN_F_VP;

    if (!td_pd_plausible(pd, out->pd_f)) return f;
    f |= TD_SCAN_F_PD;

    if (!td_stable_close(stable, oldst)) return f;
    f |= TD_SCAN_F_STABLE;

    // The decisive one.
    float aspect = 0.0f;
    if (!td_matches_shader_formula(p, &aspect)) return f;
    f |= TD_SCAN_F_FORMULA;

    out->flags = f;
    out->max_vp = maxabs;
    out->aspect = aspect;
    memcpy(out->vp, vp, sizeof out->vp);
    memcpy(out->pd, pd, sizeof out->pd);
    return f;
}

// The authoritative test: does the block satisfy the shader's own formula?
//
// A range check — "the numbers are plausible" — is not enough, and the v27 run
// proved it. It produced ten candidates that all looked right and were all
// wrong: pd.x/pd.y came out at 1.00 where a 16:9 frustum requires about 1.78,
// and maxVP was uncorrelated with 1/pd.x, which is impossible if m[0] really is
// 1/pd.x. Those were ramps on the heap, not projections.
//
// So this checks the identity itself, for m[0] and m[5], to a relative
// tolerance. This is the one test that cannot be faked by a smooth gradient,
// because a gradient has no reason to satisfy an algebraic relation that
// involves reciprocals of two of its own fields.
//
// Returns 0 on failure, 1 on success, and writes the recovered m[0] and m[5] so
// the caller can log the aspect ratio it implied.
static int td_matches_shader_formula(const unsigned char* p, float* aspect_out) {
    const float* m  = (const float*)(p + 0x000);
    const float* pd = (const float*)(p + 0x070);

    float m0 = m[0], m5 = m[5];

    // Every field must be a real number BEFORE any arithmetic on it.
    //
    // This guard is not defensive noise, it fixes a bug that produced twenty
    // false candidates in one pass. With m0 == 0, 1/m0 is inf, so
    // (pd.x - inf)/inf evaluates to NaN, and NaN > 1e-3f is false — the
    // comparison silently *passes*. Every all-zero hole in the object then
    // looked like a valid projection.
    if (!(fabsf(m0) < 1e6f) || !(fabsf(m5) < 1e6f)) return 0;
    if (m0 == 0.0f || m5 == 0.0f) return 0;

    float pd0 = pd[0], pd1 = pd[1], pd2 = pd[2], pd3 = pd[3];
    if (!(fabsf(pd0) < 1e6f) || !(fabsf(pd1) < 1e6f)) return 0;
    if (!(fabsf(pd2) < 1e6f) || !(fabsf(pd3) < 1e6f)) return 0;

    float inv0 = 1.0f / m0;
    float inv5 = 1.0f / m5;
    if (!(fabsf(inv0) < 1e6f) || !(fabsf(inv5) < 1e6f)) return 0;
    if (inv0 <= 0.0f) return 0;

    // Written as `!(x <= tol)` rather than `x > tol` on purpose. A NaN fails the
    // first form and passes the second, and a NaN here means the arithmetic
    // above overflowed, which is exactly the case that must be rejected.
    if (!(fabsf((pd0 - inv0) / inv0) <= 1e-3f)) return 0;
    if (!(fabsf((pd1 - inv5) / inv5) <= 1e-3f)) return 0;

    // pd.x and pd.y are 1/(aspect * tan(fov/2)) and 1/tan(fov/2), so their
    // ratio is the horizontal aspect. A ramp gives 1.00. Accept anything from
    // half-square to ultra-wide, which covers 4:3, 16:9, 21:9 and 32:9 without
    // becoming a test for one particular game setting.
    float aspect = pd[0] / pd[1];
    if (!(aspect > 0.5f && aspect < 4.0f)) return 0;

    // pd.z and pd.w are eye offsets in metres. The half-IPD of a human head is
    // 0.031 m, and even a generous stereo baseline is under 0.1 m. Anything past
    // half a metre is not an eye offset in any form.
    if (fabsf(pd[2]) > 0.5f) return 0;
    if (fabsf(pd[3]) > 0.5f) return 0;

    // m[8] and m[9] must also reproduce pd.z and pd.w through the same
    // division, or the off-axis terms are unrelated to the matrix.
    if (fabsf((m[8] / m0) - pd[2]) > 1e-3f) return 0;
    if (fabsf((m[9] / m5) - pd[3]) > 1e-3f) return 0;

    if (aspect_out) *aspect_out = aspect;
    return 1;
}

#endif /* TDVR_MEMSCAN_H */
