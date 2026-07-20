# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Unit tests for the pure-math stroke layer (Q3): Bezier eval / sub-curve, the
centripetal Catmull-Rom conversion, and the arc-length walk. No engine and no
``bpy`` — runs under any Python.

Run (either works):
    python claudeMemory/tests/stroke_math_test.py
    blender --factory-startup --python claudeMemory/scripts/run_sync.py -- \
        claudeMemory/tests/stroke_math_test.py
Exits nonzero on failure.
"""

import math
import os
import sys

sys.path.insert(0, os.path.join(
    os.path.dirname(__file__), "..", "..", "scripts", "addons_core", "sculptcore_addon"))

import stroke_math as sm


def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def _close(a, b, eps=1e-6):
    return abs(a - b) <= eps


def _vclose(a, b, eps=1e-6):
    return all(abs(x - y) <= eps for x, y in zip(a, b))


def test_eval_endpoints():
    bez = ((0.0, 0.0), (1.0, 2.0), (3.0, -1.0), (4.0, 0.0))
    if not _vclose(sm.eval_cubic(bez, 0.0), bez[0]):
        _fail("eval_cubic(0) != b0")
    if not _vclose(sm.eval_cubic(bez, 1.0), bez[3]):
        _fail("eval_cubic(1) != b3")
    # Midpoint of a symmetric curve lands where the Bernstein average predicts.
    mid = sm.eval_cubic(bez, 0.5)
    want = tuple((bez[0][i] + 3 * bez[1][i] + 3 * bez[2][i] + bez[3][i]) / 8.0
                 for i in range(2))
    if not _vclose(mid, want):
        _fail("eval_cubic(0.5) wrong: {} vs {}".format(mid, want))
    print("PASS: eval_cubic endpoints + midpoint")


def test_cr_passes_through():
    p0, p1, p2, p3 = (-1.0, 0.0), (0.0, 0.0), (1.0, 1.0), (2.0, 1.0)
    bez = sm.cr_to_bezier(p0, p1, p2, p3)
    if not _vclose(bez[0], p1) or not _vclose(bez[3], p2):
        _fail("cr_to_bezier endpoints must be p1/p2")
    # The curve interpolates p1 at t=0 and p2 at t=1.
    if not _vclose(sm.eval_cubic(bez, 0.0), p1) or not _vclose(sm.eval_cubic(bez, 1.0), p2):
        _fail("cr segment does not interpolate p1/p2")
    print("PASS: cr_to_bezier interpolates control points")


def test_cr_coincident_finite():
    # Jittery input with a repeated point must not blow up (centripetal guard).
    bez = sm.cr_to_bezier((0.0, 0.0), (1.0, 1.0), (1.0, 1.0), (2.0, 0.0))
    for i in range(5):
        p = sm.eval_cubic(bez, i / 4.0)
        if not all(math.isfinite(c) for c in p):
            _fail("coincident control points produced non-finite output")
    print("PASS: cr_to_bezier finite on coincident points")


def test_sub_cubic():
    bez = ((0.0, 0.0), (1.0, 2.0), (3.0, -1.0), (4.0, 0.0))
    # Whole-range restriction is (numerically) the same curve.
    whole = sm.sub_cubic(bez, 0.0, 1.0)
    for i in range(5):
        if not _vclose(sm.eval_cubic(whole, i / 4.0), sm.eval_cubic(bez, i / 4.0), 1e-9):
            _fail("sub_cubic(0,1) is not the identity")
    # A sub-curve's endpoints match the parent at a and b.
    a, b = 0.25, 0.7
    sub = sm.sub_cubic(bez, a, b)
    if not _vclose(sm.eval_cubic(sub, 0.0), sm.eval_cubic(bez, a)):
        _fail("sub_cubic start != parent(a)")
    if not _vclose(sm.eval_cubic(sub, 1.0), sm.eval_cubic(bez, b)):
        _fail("sub_cubic end != parent(b)")
    # An interior point of the sub-curve lies on the parent.
    mid_sub = sm.eval_cubic(sub, 0.5)
    mid_parent = sm.eval_cubic(bez, a + (b - a) * 0.5)
    if not _vclose(mid_sub, mid_parent, 1e-4):
        _fail("sub_cubic interior diverges from parent")
    print("PASS: sub_cubic restriction")


def test_arclength_straight():
    # A cubic with evenly spaced collinear controls is the constant-speed line;
    # walking it at spacing s must place points exactly s apart.
    bez = ((0.0, 0.0), (1.0, 0.0), (2.0, 0.0), (3.0, 0.0))
    s = 0.3
    pts, carry = sm.arc_length_walk(bez, s, 0.0)
    if not pts:
        _fail("straight walk emitted nothing")
    # First point at arc s, evenly spaced thereafter.
    if not _close(pts[0][0], s, 1e-4):
        _fail("first straight point at {:.4f}, expected {:.4f}".format(pts[0][0], s))
    for i in range(1, len(pts)):
        gap = pts[i][0] - pts[i - 1][0]
        if not _close(gap, s, 1e-4):
            _fail("straight spacing {:.4f} != {:.4f}".format(gap, s))
    # Leftover carry is the tail past the last point (< spacing).
    last = pts[-1][0]
    if not _close(carry, 3.0 - last, 1e-4) or carry >= s + 1e-9:
        _fail("straight carry_out wrong: {:.4f}".format(carry))
    print("PASS: arc_length_walk even spacing on a line")


def test_arclength_curve_accuracy():
    # Quarter-circle-ish cubic; consecutive emitted points should be ~spacing
    # apart (32-chord approximation is accurate to well under 2%).
    k = 0.5522847498  # cubic circle-arc constant
    bez = ((1.0, 0.0), (1.0, k), (k, 1.0), (0.0, 1.0))
    s = 0.2
    pts, _ = sm.arc_length_walk(bez, s, 0.0)
    if len(pts) < 3:
        _fail("curve walk emitted too few points")
    for i in range(1, len(pts)):
        gap = math.dist(pts[i], pts[i - 1])
        if abs(gap - s) > 0.02 * s:
            _fail("curve spacing {:.5f} deviates > 2% from {:.4f}".format(gap, s))
    print("PASS: arc_length_walk curve spacing within 2%")


def test_carry_continuity():
    # Two abutting straight segments (x:0->1, x:1->2). Threading the carry must
    # keep the joint from clustering: every consecutive gap stays ~spacing.
    segA = ((0.0, 0.0), (1.0 / 3, 0.0), (2.0 / 3, 0.0), (1.0, 0.0))
    segB = ((1.0, 0.0), (1.0 + 1.0 / 3, 0.0), (1.0 + 2.0 / 3, 0.0), (2.0, 0.0))
    s = 0.3
    a_pts, carry = sm.arc_length_walk(segA, s, 0.0)
    b_pts, _ = sm.arc_length_walk(segB, s, carry)
    allx = [p[0] for p in a_pts] + [p[0] for p in b_pts]
    if len(allx) < 5:
        _fail("carry test emitted too few points")
    for i in range(1, len(allx)):
        gap = allx[i] - allx[i - 1]
        if not _close(gap, s, 1e-4):
            _fail("joint clustering: gap {:.4f} != {:.4f} at x={:.3f}".format(
                gap, s, allx[i]))
    print("PASS: carry continuity across abutting segments (no joint clustering)")


def main():
    test_eval_endpoints()
    test_cr_passes_through()
    test_cr_coincident_finite()
    test_sub_cubic()
    test_arclength_straight()
    test_arclength_curve_accuracy()
    test_carry_continuity()
    print("ALL PASS: stroke_math unit tests")


if __name__ == "__main__":
    main()
