# Off-axis stereo — the math, and the bug it caught

Date: 2026-09-26

## The idea

SceneDynamicBuffer's `mubVpMatrix` is a **row-vector** view-projection
(`v = M * p`, flat row-major `M[r*4+c]`). To draw one eye we shift the
projection asymmetrically by half the interpupillary distance.

## Where the shift goes

In the eye's own view space a world point has `x_eye = x_head - sx`, so

```
v.x = xScale * (x_head - sx) = xScale*x_head - xScale*sx
```

That constant term is pure clip-space translation, so it belongs in
**`m[0][3]` — flat index 3**, the fourth element of the first row:

```c
eye_matrix[3] = head_matrix[3] - sx * xScale;
```

`xScale` is read straight out of the game's matrix: for a standard
perspective, VP row 0 equals `xScale * (view right vector)`, and the right
vector is unit length, so `|row 0| == xScale`. That holds under any rotation
and translation, so no fov/aspect re-derivation is needed.

## The bug this test caught

The first implementation sheared `m[0][2]` (flat index 8) instead — the
"obvious" place to put a horizontal shift. That couples the offset to world z,
which produces:

- **zero** separation between the eyes — no parallax at all
- **different** NDC depth for the same point

Verified directly:

```
separation = 0.000000   <- zero: NO parallax
z_NDC      = 0.739192 vs 0.431779
```

The symptom on screen would have been a game that looks flat with subtly
broken depth, with nothing in the log to explain it.

## A second bug, in the test itself

While checking the formula the test disagreed with the closed-form answer by a
constant factor, which turned out to be an indexing error in the test's
reference projection, not in the formula: the `w_clip` term belongs at
`M[3][2]` = **flat index 14**, not `M[2][3]` = flat index 11. Putting the 1.0
in the wrong cell gives a projection that looks plausible but divides by ~1
instead of by z.

Worth stating plainly: the first suspicion was that the shader-side formula
was wrong. It wasn't. The reference implementation was. Debugging a maths
error by rewriting the maths is how you end up with two wrong answers that
cancel out — hence the test asserts against the closed form
`ipd * xScale / z` rather than against a golden output.

## The test

`hook/verify_stereo.py` — pure stdlib, no numpy. Checks:

1. `xScale` recovered from a rotated+translated VP equals the projection's
2. NDC **y and z identical** between eyes
3. separation `== ipd * xScale / z_view` (closed form, not golden values)
4. separation decreases monotonically with distance
5. the shear-on-`m[8]` variant is asserted to be **wrong**

```
   view z         sep    ipd*xs/z   y-same  z-same
   2.1985    0.016375    0.016375   True   True    OK
   6.1985    0.005808    0.005808   True   True    OK
  19.1985    0.001875    0.001875   True   True    OK
  54.1985    0.000664    0.000664   True   True    OK

RESULT: ALL CHECKS PASS
```

Run it with `python3 hook/verify_stereo.py`.

## Still not done

The maths is right but it is not visible yet: `g_scene_buf` is unknown, so
`stereo_apply()` is a no-op and the game plays flat. That stays until the
scanner locks the buffer on real hardware. And the whole thing renders into
the same backbuffer, so it will look like alternating-eye flicker rather than
stereo until the OpenXR swapchain with per-eye layers is in place — the
matrix maths is the part that can be proven correct without a headset.
