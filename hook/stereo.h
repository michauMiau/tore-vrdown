// stereo.h — per-eye rendering for the Teardown VR Mod
//
// Everything stereo lives here so the hook file stays about hooking.
//
// The engine does not decode its projection from the VP matrix. From the
// shader at image offset 0x1C85E8E:
//
//     vec3 computePixelDir(float2 texCoord)
//     {
//         vec2 screenCoord = texCoordToScreenCoord(texCoord).xy;
//         return mubViewXDir.xyz * (screenCoord.x * mubProjectionData.r + mubProjectionData.z) +
//                mubViewYDir.xyz * (screenCoord.y * mubProjectionData.g + mubProjectionData.w) -
//                mubViewZDir.xyz;
//     }
//
// --- what that means for the eye offset, precisely -------------------------
//
// The engine's own comment above is the whole answer:
//
//     pd.x = 1 / m[0]      = 1 / xScale
//     pd.z = m[8] / m[0]   = the horizontal clip offset, pre-divided
//
// So m[8] IS the horizontal offset term of the projection, and the engine
// divides it through by xScale when packing pd.z. For an eye displaced by sx
// metres we want a clip offset of -sx * xScale, so:
//
//     pd.z = (-sx * xScale) / xScale = -sx
//
// The delta to add to mubProjectionData.z is therefore the world-space eye
// offset in METRES, unchanged: -ipd/2 for the left eye, +ipd/2 for the right.
// No scaling, no xScale, nothing.
//
// This is the trap. The version of this file that read "z is the eye offset,
// z sits next to r which is 1/xScale" wrote:
//
//     pd[2] += eye_shift * pd[0];
//
// which is wrong, because the engine already divided the xScale out when it
// packed pd.z. That version applied an eye offset 1/xScale too large — about
// 0.56x the correct shift at hfov 90 — which reads as mild but wrong
// convergence and cannot be diagnosed from the image alone. The fact that
// pd.r and pd.z are neighbours in the same float4 is what makes the mistake
// so easy to make. See docs/PROJECTION_CONVENTION.md.
//
// --- why prefer this path over the matrix ---------------------------------
//
// Writing mubProjectionData.z cannot disturb the depth row, cannot fight TAA
// and does not need the row-major/column-major question settled. The matrix
// helpers below are still here for the vertex shaders that do use mubVpMatrix,
// and for that one case the flat index depends on the convention, so it is a
// parameter rather than a hardcoded constant.
//
// --- the buffer layout, confirmed against the binary -----------------------
//
// The field sizes in the declaration at image 0x1C76AF4 sum to exactly
// 0x1F4 = 500 bytes, which is the size the engine passes to
// createStructuredBuffer at rva 0x9C030. That is what makes the offsets below
// trustworthy rather than a guess from reading HLSL.
//
// --- where an offset goes in a matrix, and why ----------------------------
//
// In the eye's own view space a world point has x_eye = x_head - sx, so
//
//     v.x = xScale * (x_head - sx) = xScale*x_head - xScale*sx
//
// The constant term is pure clip-space translation. Which flat element that is
// depends on the multiply:
//
//     column-vector, out = M*v, flat m[r*4+c]:   out[0] = m[0]*x + ... + m[3]*w
//         -> the x offset is m[3]
//
//     row-vector, v = M*p, flat m[r*4+c]:       out[0] = p[0]*m[0] + p[1]*m[4]
//                                                          + p[2]*m[8] + p[3]*m[12]
//         -> the x offset is m[12]
//
// The earlier version of this file hardcoded m[3] with a comment claiming it
// was the fourth element of the first row. Under the row-vector reading that
// index is the w row: the write lands in w_clip and does nothing to x, so
// stereo would render two identical images with no parallax and no error
// anywhere. Both indices are now explicit, and the tests in
// teardown-analysis/verify_stereo_independent.py exercise both conventions so
// the two cannot drift apart again.
//
// xScale is recovered from row 0: for a standard perspective, VP row 0 equals
// xScale times the view's right vector, and that vector is unit length, so
// |row 0| == xScale. That holds under rotation and translation, which is why
// it can be read straight out of whatever the game wrote instead of
// re-deriving the projection from fov and aspect.
//
// The tempting wrong answer is to shear m[0][2] (flat index 8). That couples
// the offset to world z, so the separation stops falling off as 1/z — it
// collapses to zero or stays constant, either of which means no parallax.
//
// Nothing here is wired to OpenXR yet: XR_SetEyeView is the seam where the
// runtime's per-eye view matrices will be plugged in. Until a session exists
// the mod renders flat, which is the intended fail-safe.

#ifndef TDVR_STEREO_H
#define TDVR_STEREO_H

#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

// Offsets inside SceneDynamicBuffer, in bytes. From the declaration at image
// 0x1C76AF4; the field sizes sum to exactly 0x1F4 = 500, which is the size the
// engine passes to createStructuredBuffer at rva 0x9C030, so these offsets are
// confirmed against the binary rather than inferred from HLSL alone.
#define TDVR_SDB_VP_OFFSET          0x000  // float4x4  64 B
#define TDVR_SDB_VIEWX_OFFSET       0x040  // float4    16 B
#define TDVR_SDB_VIEWY_OFFSET       0x050  // float4    16 B
#define TDVR_SDB_VIEWZ_OFFSET       0x060  // float4    16 B
#define TDVR_SDB_PROJDATA_OFFSET    0x070  // float4    16 B  <- the eye offset
#define TDVR_SDB_VIEW_OFFSET        0x080  // float4x4  64 B
#define TDVR_SDB_OLDVIEW_OFFSET     0x0C0  // float4x4  64 B
#define TDVR_SDB_STABLEVP_OFFSET    0x100  // float4x4  64 B
#define TDVR_SDB_OLDSTABLE_OFFSET   0x140  // float4x4  64 B  <- TAA history
#define TDVR_SDB_CAMERAPOS_OFFSET   0x180  // float4
#define TDVR_SDB_PLAYERPOS_OFFSET   0x190  // float4
#define TDVR_SDB_FRAMEPARAMS_OFFSET 0x1A0  // float4  r=time g=b a=random
#define TDVR_SDB_TRANSFORM_OFFSET   0x1B0  // float4x4
#define TDVR_SDB_SIZE               0x1F4  // 500 B

// Inside mubProjectionData.
#define TDVR_PD_R  0   // 1 / m[0]  = 1 / xScale
#define TDVR_PD_G  1   // 1 / m[5]  = 1 / yScale
#define TDVR_PD_Z  2   // m[8] / m[0]  horizontal offset, already in metres
#define TDVR_PD_W  3   // m[9] / m[5]  vertical offset

// Which flat index carries the x eye offset, and where w_clip comes from.
typedef enum {
    TDVR_MATRIX_COLUMN_VECTOR = 0,   // out = M*v, offset at flat 3
    TDVR_MATRIX_ROW_VECTOR    = 1    // v = M*p,    offset at flat 12
} tdvr_matrix_layout;

typedef struct {
    float m[16];              // flat, four rows of four
    tdvr_matrix_layout layout;
} tdvr_mat4;

// Projection xScale read back out of a view-projection matrix.
static float tdvr_vp_xscale(const tdvr_mat4* vp) {
    float s = 0.0f;
    int i;
    for (i = 0; i < 4; i++) s += vp->m[i] * vp->m[i];
    return sqrtf(s);
}

// Which convention the engine is using, read off a real matrix rather than
// assumed. A perspective projection has exactly one -1, in the row that
// produces w_clip: at flat 11 under the row-vector reading, at flat 14 (or 2)
// under the column-vector one. Nothing else in a projection matrix sits at -1,
// so the position of that single -1 identifies the convention outright.
//
// This has to be read from a real matrix because computePixelDir does not use
// mubVpMatrix at all, so no shader in the image constrains its layout and
// static analysis cannot settle it. Guessing m[3] would mean either correct
// stereo or a silent no-op depending on the answer.
static tdvr_matrix_layout tdvr_detect_layout(const float* m) {
    int neg = -1, count = 0, i;
    for (i = 0; i < 16; i++) {
        float d = m[i] + 1.0f;
        if (d < 1e-4f && d > -1e-4f) { neg = i; count++; }
    }
    if (count == 1 && neg == 11) return TDVR_MATRIX_ROW_VECTOR;
    return TDVR_MATRIX_COLUMN_VECTOR;
}

// Off-axis frustum for one eye.
//
//   sx       lateral eye offset in metres (+right, -left)
//   out      receives the modified view-projection
//   head     the game's own view-projection for this frame
static void tdvr_eye_vp(tdvr_mat4* out, const tdvr_mat4* head, float sx) {
    *out = *head;
    int idx = (head->layout == TDVR_MATRIX_ROW_VECTOR) ? 12 : 3;
    out->m[idx] -= sx * tdvr_vp_xscale(head);
}

// Both eyes at once. ipd_m is the full interpupillary distance in metres.
static void tdvr_stereo_vp(tdvr_mat4* left, tdvr_mat4* right,
                           const tdvr_mat4* head, float ipd_m) {
    tdvr_eye_vp(left,  head, -ipd_m * 0.5f);
    tdvr_eye_vp(right, head, +ipd_m * 0.5f);
}

// The engine's own path, which needs no matrix at all.
//
// mubProjectionData is (r, g, z, w) with r = 1/m[0] and g = 1/m[5], and z =
// m[8] / m[0]. Because the engine has already divided the clip offset by
// m[0], the delta to add is the world-space eye offset in METRES, unchanged.
// Multiplying by pd[0] here would apply the shift 1/xScale too large.
//
//   pd        in/out, the caller's mubProjectionData
//   eye_shift lateral eye offset in metres, +right
static void tdvr_eye_projection_data(float* pd, float eye_shift) {
    pd[TDVR_PD_Z] += eye_shift;
    pd[TDVR_PD_W] += 0.0f;   // vertical stays 0: roll and pitch are carried by
                             // the basis vectors, not by the projection
}

// Read the eye offset back out, so a caller can verify the round trip.
static float tdvr_projection_data_eye_offset(const float* pd) {
    return pd[TDVR_PD_Z];
}

#ifdef __cplusplus
}
#endif
#endif /* TDVR_STEREO_H */
