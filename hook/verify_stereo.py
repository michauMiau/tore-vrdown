#!/usr/bin/env python3
"""Off-axis stereo for a row-vector view-projection matrix.

Teardown stores mubVpMatrix as HLSL row-vector:  v = M * p,  M flat row-major,
so M[r*4+c].

Off-axis (asymmetric) frustum, derived directly:
    The eye sits at lateral offset sx from the head centre. In the eye's own
    view space a world point has x_eye = x_head - sx, so

        v.x = xScale * (x_head - sx) = xScale*x_head - xScale*sx

    The shift therefore lands entirely in the CLIP-SPACE TRANSLATION term
    m[0][3] (flat index 3), not in any shear term:

        eye_matrix[3] = head_matrix[3] - sx * xScale

    Adding to m[0][2] instead (the obvious "shear" guess) puts the offset in
    the z coupling, which is multiplied by the world z - it produces ZERO
    separation and corrupts depth. That is the mistake this test guards.

Invariants checked:
  1. xScale recovered from a combined VP equals the projection's xScale
     (row 0 of VP is xScale * the view's right vector, which is unit length)
  2. NDC y and z are IDENTICAL between the two eyes
  3. NDC x separation == ipd * xScale / z_view   (correct parallax falloff)
  4. separation decreases monotonically with distance
"""
import math

ZN, ZF = 0.1, 500.0
IPD = 0.064


def perspective(fov_y_deg, aspect, zn=ZN, zf=ZF):
    """D3D left-handed perspective, row-vector flat, v = M * p with p.w = 1.

    Index layout in the flat array (M[r*4 + c]):
        row 0  ->  v.x
        row 1  ->  v.y
        row 2  ->  v.z
        row 3  ->  v.w      (the homogeneous divide)

    So a term of  z  contributing to w lives at M[3][2] = flat index 14, NOT
    at M[2][3] = flat index 11. Putting the 1.0 in the wrong cell is the
    easiest way to get a projection that looks sane but divides by ~1 instead
    of by z, and every NDC coordinate comes out wrong by a constant-ish factor.
    """
    t = 1.0 / math.tan(math.radians(fov_y_deg) / 2.0)
    m = [0.0] * 16
    m[0] = t / aspect          # v.x = xScale * x
    m[5] = t                   # v.y = t * y
    m[10] = zf / (zf - zn)      # v.z = a*z
    m[11] = -zn * zf / (zf - zn)   # v.z += b   (row 2, col 3)
    m[14] = 1.0                # v.w = z      (row 3, col 2)  <-- the divide
    m[15] = 0.0
    return m


def view(eye, target, up):
    """Rigid world->view matrix, row-vector flat. D3D left-handed (+z forward)."""
    f = [target[i] - eye[i] for i in range(3)]
    n = math.sqrt(sum(c * c for c in f)); f = [c / n for c in f]
    s = [f[1] * up[2] - f[2] * up[1],
         f[2] * up[0] - f[0] * up[2],
         f[0] * up[1] - f[1] * up[0]]
    n = math.sqrt(sum(c * c for c in s)); s = [c / n for c in s]
    u = [s[1] * f[2] - s[2] * f[1],
         s[2] * f[0] - s[0] * f[2],
         s[0] * f[1] - s[1] * f[0]]
    V = [0.0] * 16
    V[0], V[1], V[2] = s                       # row 0 = right
    V[4], V[5], V[6] = u                       # row 1 = up
    V[8], V[9], V[10] = f                      # row 2 = forward (+z)
    V[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2])
    V[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2])
    V[14] = -(f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2])
    # V[15] stays 0. A rigid view matrix contributes NO translation to the
    # homogeneous coordinate: w_clip is produced entirely by the projection
    # (P[11] = 1, so w = z_view). Setting V[15] = 1 would make w = z_view + 1
    # and scale every NDC coordinate by the wrong denominator.
    return V


def mul(A, B):
    """A*B under v = M*p (A is the projection side, applied last)."""
    return [sum(A[r * 4 + k] * B[k * 4 + c] for k in range(4))
            for r in range(4) for c in range(4)]


def xscale(vp):
    return math.sqrt(sum(c * c for c in vp[0:4]))


def offaxis(vp, sx):
    e = list(vp)
    e[3] -= sx * xscale(vp)
    return e


def shear_mistake(vp, sx):
    """The wrong version, kept so the test can show it is actually wrong."""
    e = list(vp)
    e[8] += sx / ZN
    return e


def ndc(m, p):
    v = [sum(m[r * 4 + c] * p[c] for c in range(4)) for r in range(4)]
    return [v[0] / v[3], v[1] / v[3], v[2] / v[3]]


def main():
    fov, aspect = 90.0, 16 / 9
    P = perspective(fov, aspect)
    xs_p = P[0]
    print(f"projection xScale            = {xs_p:.6f}")

    # rotated + translated camera
    V = view((3.0, 2.0, -5.0), (0.5, 1.0, 4.0), (0, 1, 0))
    VP = mul(P, V)
    xs = xscale(VP)
    ok1 = abs(xs - xs_p) < 1e-9
    print(f"xScale recovered from VP      = {xs:.6f}   [{'OK' if ok1 else 'FAIL'}]")
    print()

    cam_pos = [3.0, 2.0, -5.0]
    fwd = [V[8], V[9], V[10]]
    right = [V[0], V[1], V[2]]
    upv = [V[4], V[5], V[6]]

    pts = []
    for d in (1.5, 3.0, 6.0, 12.0, 25.0, 60.0):
        pts.append([cam_pos[i] + fwd[i] * d for i in range(3)] + [1.0])
    for off in (2.0, -2.0, 5.0, -5.0):
        d = 8.0
        pts.append([cam_pos[i] + fwd[i] * d + right[i] * off for i in range(3)] + [1.0])
    pts.sort(key=lambda p: sum(V[r * 4 + c] * p[c] for r in [2] for c in range(3)))

    L = offaxis(VP, -IPD / 2)
    R = offaxis(VP, +IPD / 2)
    WL = shear_mistake(VP, -IPD / 2)
    WR = shear_mistake(VP, +IPD / 2)

    ok2 = ok3 = True
    prev = None
    mono_ok = True
    print(f"{'view z':>9}  {'sep':>10}  {'ipd*xs/z':>10}   y-same  z-same")
    for p in pts:
        vd = [sum(V[r * 4 + c] * p[c] for c in range(4)) for r in range(4)]
        z = vd[2]
        if z <= 0.01:
            continue
        pl, pr = ndc(L, p), ndc(R, p)
        sep = abs(pl[0] - pr[0])
        expect = IPD * xs / z
        ys = abs(pl[1] - pr[1]) < 1e-12
        zs = abs(pl[2] - pr[2]) < 1e-12
        good = abs(sep - expect) < 1e-9
        ok3 &= good
        ok2 &= ys and zs
        if prev is not None and sep > prev + 1e-12:
            mono_ok = False
        prev = sep
        print(f"{z:9.4f}  {sep:10.6f}  {expect:10.6f}   {ys!s:5}  {zs!s:5}"
              f"   {'OK' if good else 'FAIL'}")

    print()
    print(f"y and z identical between eyes : {'OK' if ok2 else 'FAIL'}")
    print(f"separation == ipd*xScale/z     : {'OK' if ok3 else 'FAIL'}")
    print(f"separation falls off with dist : {'OK' if mono_ok else 'FAIL'}")
    print()

    p = pts[3]
    vl, vr = ndc(WL, p), ndc(WR, p)
    print("the shear-on-m[8] version on the same point:")
    print(f"  separation = {abs(vl[0] - vr[0]):.6f}   <- zero: NO parallax")
    print(f"  z_NDC      = {vl[2]:.6f} vs {vr[2]:.6f}")
    print()

    allok = ok1 and ok2 and ok3 and mono_ok
    print("RESULT:", "ALL CHECKS PASS" if allok else "FAILURES PRESENT")
    return 0 if allok else 1


if __name__ == "__main__":
    raise SystemExit(main())
