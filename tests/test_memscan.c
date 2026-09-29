// Offline test for the SDB content matcher.
//
// This runs on Linux, so it includes memscan.h directly — that header is
// deliberately free of windows.h for exactly this reason, the same split that
// made scene_bind_find.h testable.
//
// The test data is built, not recorded: a real SceneDynamicBuffer with a known
// projection, and a pile of things that must not match — zeros, NaN, inf,
// plausible-looking noise, a buffer with a stable matrix that does not match
// its old copy, and a buffer whose projection data has the wrong sign.

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "../hook/memscan.h"

static unsigned char g_buf[4096];

static void fill_identity_matrix(unsigned char* p) {
    float m[16];
    memset(m, 0, sizeof m);
    m[0] = 1.0f;  m[5] = 1.0f;  m[10] = 1.0f;  m[15] = 1.0f;
    m[3] = 0.5f;  m[7] = 2.0f;  m[11] = -3.0f;
    memcpy(p + 0x000, m, sizeof m);       // mubVpMatrix
    memcpy(p + 0x080, m, sizeof m);       // mubViewMatrix
}

// The projection data, per the shader declaration:
//   pd.x = 1/m0   pd.y = 1/m5   pd.z = m8/m0   pd.w = m9/m5
//
// pd.x and pd.y are given explicitly because a test for the sign of pd.x has to
// set it explicitly too — an earlier version of this helper always wrote 1.0,
// so the "wrong sign" case it was meant to exercise never actually happened.
static void fill_projection_data(unsigned char* p, float x, float y,
                                 float z, float w) {
    float pd[4] = { x, y, z, w };
    memcpy(p + 0x070, pd, sizeof pd);
}

// The mono case: a symmetric frustum, so the asymmetry terms are zero.
static void fill_pd_mono(unsigned char* p) {
    fill_projection_data(p, 1.0f, 1.0f, 0.0f, 0.0f);
}

// The stereo case: the two asymmetry terms are exactly the half-IPD, +-0.032 m
// for a 64 mm interpupillary distance. These are the values this whole project
// exists to write, so the matcher must accept them and read them back exactly.
// A stereo SDB, with half-IPD eye offsets in the projection data.
//
// The matrix is NOT identity here. m[8] and m[9] are the values the formula
// divides to produce the eye offsets:
//
//     pd.z = m[8] / m[0]
//     pd.w = m[9] / m[5]
//
// so with m[0] = m[5] = 1.0, m[8] must literally be +0.032 and m[9] -0.032.
// An earlier version of this fixture used the identity matrix, which makes both
// quotients exactly 0, and then asserted that pd.z came out at 0.032: the
// assertion and the fixture contradicted each other and the stereo case failed
// with the formula bit (0x10) unset. The fixture was wrong, not the matcher.
static void fill_pd_stereo(unsigned char* p) {
    float m[16];
    memset(m, 0, sizeof m);
    m[0] = 1.0f;  m[5] = 1.0f;  m[10] = 1.0f;  m[15] = 1.0f;
    m[8] = 0.032f;    // +half-IPD
    m[9] = -0.032f;   // -half-IPD
    memcpy(p + 0x00, m, sizeof m);
    fill_projection_data(p, 1.0f, 1.0f, 0.032f, -0.032f);
}

static void fill_stable_pair(unsigned char* p, float jitter) {
    float m[16];
    memset(m, 0, sizeof m);
    m[0] = 1.0f;  m[5] = 1.0f;  m[10] = 1.0f;  m[15] = 1.0f;
    m[3] = 0.5f + jitter;
    memcpy(p + 0x100, m, sizeof m);              // mubStableVpMatrix
    m[3] = 0.5f;                                 // previous frame
    memcpy(p + 0x140, m, sizeof m);              // mubOldStableVpMatrix
}

static int g_fail = 0;
static void check(const char* what, int got, int want) {
    if (got != want) {
        printf("  FAIL %-46s got 0x%02X want 0x%02X\n", what, got, want);
        g_fail++;
    } else {
        printf("  ok   %-46s 0x%02X\n", what, got);
    }
}

int main(void) {
    td_scan_hit h;
    uint32_t f;

    printf("=== a real mono SDB must match completely ===\n");
    memset(g_buf, 0, sizeof g_buf);
    fill_identity_matrix(g_buf);
    fill_pd_mono(g_buf);
    fill_stable_pair(g_buf, 0.0f);
    f = td_test_candidate(g_buf, &h);
    check("full match", f, TD_SCAN_F_ALL);
    check("  flags finite", (f & TD_SCAN_F_FINITE) != 0, 1);
    check("  flags vp",     (f & TD_SCAN_F_VP) != 0, 1);
    check("  flags pd",     (f & TD_SCAN_F_PD) != 0, 1);
    check("  flags stable", (f & TD_SCAN_F_STABLE) != 0, 1);
    printf("  pd read back = (%.3f %.3f %.3f %.3f)\n",
           (double)h.pd_f[0], (double)h.pd_f[1],
           (double)h.pd_f[2], (double)h.pd_f[3]);
    check("  pd.x preserved", h.pd_f[0] > 0.9 && h.pd_f[0] < 1.1, 1);

    printf("\n=== a stereo SDB, half-IPD offsets, must also match ===\n");
    memset(g_buf, 0, sizeof g_buf);
    fill_identity_matrix(g_buf);
    fill_pd_stereo(g_buf);
    fill_stable_pair(g_buf, 0.0f);
    f = td_test_candidate(g_buf, &h);
    check("stereo match", f, TD_SCAN_F_ALL);
    check("  pd.z is +0.032", h.pd_f[2] > 0.0319 && h.pd_f[2] < 0.0321, 1);
    check("  pd.w is -0.032", h.pd_f[3] < -0.0319 && h.pd_f[3] > -0.0321, 1);

    printf("\n=== all zero: the uninitialised-buffer false positive ===\n");
    memset(g_buf, 0, sizeof g_buf);
    f = td_test_candidate(g_buf, &h);
    check("zeros rejected at vp test", (f & TD_SCAN_F_VP) != 0, 0);

    printf("\n=== NaN and inf anywhere in the struct ===\n");
    memset(g_buf, 0, sizeof g_buf);
    fill_identity_matrix(g_buf);
    fill_pd_mono(g_buf);
    fill_stable_pair(g_buf, 0.0f);
    {
        float nan_v = NAN;
        memcpy(g_buf + 0x0C0, &nan_v, 4);      // inside mubOldViewMatrix
        f = td_test_candidate(g_buf, &h);
        check("NaN rejected outright", f, 0);
    }
    memset(g_buf, 0, sizeof g_buf);
    fill_identity_matrix(g_buf);
    fill_pd_mono(g_buf);
    fill_stable_pair(g_buf, 0.0f);
    {
        float inf_v = INFINITY;
        memcpy(g_buf + 0x1A8, &inf_v, 4);      // inside mubFrameParams
        f = td_test_candidate(g_buf, &h);
        check("inf rejected outright", f, 0);
    }

    printf("\n=== projection data with the wrong sign ===\n");
    memset(g_buf, 0, sizeof g_buf);
    fill_identity_matrix(g_buf);
    fill_projection_data(g_buf, -1.5f, 1.0f, 0.0f, 0.0f);  // pd.x negative
    fill_stable_pair(g_buf, 0.0f);
    f = td_test_candidate(g_buf, &h);
    check("negative pd.x rejected", (f & TD_SCAN_F_PD) != 0, 0);

    printf("\n=== stable matrix far from its old copy ===\n");
    memset(g_buf, 0, sizeof g_buf);
    fill_identity_matrix(g_buf);
    fill_pd_mono(g_buf);
    {
        float m[16];
        memset(m, 0, sizeof m);
        m[0] = 1.0f; m[5] = 1.0f; m[10] = 1.0f; m[15] = 1.0f;
        m[3] = 100.0f;                        // wildly different
        memcpy(g_buf + 0x100, m, sizeof m);
        m[3] = 0.5f;
        memcpy(g_buf + 0x140, m, sizeof m);
    }
    f = td_test_candidate(g_buf, &h);
    check("unstable pair rejected", (f & TD_SCAN_F_STABLE) != 0, 0);

    printf("\n=== stable pair differing only by TAA jitter ===\n");
    memset(g_buf, 0, sizeof g_buf);
    fill_identity_matrix(g_buf);
    fill_pd_mono(g_buf);
    fill_stable_pair(g_buf, 0.0004f);          // sub-pixel
    f = td_test_candidate(g_buf, &h);
    check("jitter accepted", f, TD_SCAN_F_ALL);

    printf("\n=== random bytes must not match ===\n");
    {
        uint32_t s = 0x12345678;
        int mismatches = 0;
        for (int trial = 0; trial < 20000; ++trial) {
            for (int i = 0; i < 0x1F4; i += 4) {
                s = s * 1103515245u + 12345u;
                memcpy(g_buf + i, &s, 4);
            }
            if (td_test_candidate(g_buf, &h) == TD_SCAN_F_ALL) mismatches++;
        }
        printf("  random 500-byte blocks fully matching: %d / 20000\n",
               mismatches);
        check("no random false positive", mismatches, 0);
    }

    printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "ALL CHECKS PASS", g_fail);
    return g_fail ? 1 : 0;
}
