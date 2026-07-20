# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
StrokeSpacer spline behavior (Q3), driven with synthetic 2D input (no engine
dabs). Verifies: even spacing along the emitted path, jitter smoothing (the
spline's turning is bounded where the raw polyline is angular), and slow/fast
event-rate parity (the same underlying path sampled sparsely vs densely emits
near-identical dab paths) — extending the existing 40-vs-8-move gate.

Run:
    blender --factory-startup --python claudeMemory/scripts/run_sync.py -- \
        claudeMemory/tests/stroke_spacer_test.py
Exits nonzero on failure.
"""

import math
import sys

import bpy


def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def _run(spacer_cls, path, spacing):
    """Feed a 2D polyline through a fresh spacer (add per point + trailing
    flush) and return the full list of emitted (x, y) points."""
    spacer = spacer_cls()
    out = []
    for p in path:
        out.extend(spacer.add(p, spacing))
    out.extend(spacer.flush(spacing))
    return out


def _gaps(pts):
    return [math.dist(pts[i], pts[i - 1]) for i in range(1, len(pts))]


def _turn_angles(pts):
    """Turn angle (radians) at each interior vertex of a polyline."""
    angles = []
    for i in range(1, len(pts) - 1):
        ax, ay = pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1]
        bx, by = pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1]
        la = math.hypot(ax, ay)
        lb = math.hypot(bx, by)
        if la < 1e-9 or lb < 1e-9:
            continue
        c = max(-1.0, min(1.0, (ax * bx + ay * by) / (la * lb)))
        angles.append(math.acos(c))
    return angles


def _min_dist_to_polyline(p, poly):
    best = float("inf")
    for i in range(1, len(poly)):
        a, b = poly[i - 1], poly[i]
        abx, aby = b[0] - a[0], b[1] - a[1]
        L2 = abx * abx + aby * aby
        if L2 < 1e-12:
            d = math.dist(p, a)
        else:
            t = ((p[0] - a[0]) * abx + (p[1] - a[1]) * aby) / L2
            t = max(0.0, min(1.0, t))
            d = math.dist(p, (a[0] + abx * t, a[1] + aby * t))
        best = min(best, d)
    return best


def main():
    if "sculptcore_addon" not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module="sculptcore_addon")
    from sculptcore_addon.stroke import StrokeSpacer

    spacing = 10.0

    # -- even spacing along a smooth arc ------------------------------------
    arc = [(200.0 * math.cos(t), 200.0 * math.sin(t))
           for t in [i / 40.0 * (math.pi / 2) for i in range(41)]]
    pts = _run(StrokeSpacer, arc, spacing)
    if len(pts) < 5:
        _fail("arc emitted too few points ({:d})".format(len(pts)))
    gaps = _gaps(pts)
    # First gap is raw-dab -> first spline dab; allow the interior to be even.
    bad = [g for g in gaps if abs(g - spacing) > 0.06 * spacing]
    if bad:
        _fail("uneven spacing: {:d}/{:d} gaps deviate >6% (e.g. {:.3f})".format(
            len(bad), len(gaps), bad[0]))
    print("PASS: even spacing on a smooth arc ({:d} dabs)".format(len(pts)))

    # -- jitter smoothing: bounded curvature --------------------------------
    # A zig-zag with sharp raw turns; the centripetal spline must round them.
    # Walk finely (small spacing) so the rounded corners are sampled densely
    # enough to reveal the bounded per-step turning (turn ~ curvature*spacing).
    fine = 3.0
    jitter = []
    for i in range(12):
        x = i * 30.0
        y = 0.0 if i % 2 == 0 else 60.0
        jitter.append((x, y))
    raw_max = max(_turn_angles(jitter))
    spline = _run(StrokeSpacer, jitter, fine)
    spline_turns = _turn_angles(spline)
    spline_max = max(spline_turns) if spline_turns else 0.0
    # Curvature is bounded: the sharpest raw corner (a tangent discontinuity) is
    # replaced by smooth turning spread over many small steps.
    if spline_max > 0.5 * raw_max:
        _fail("spline turns too sharp: {:.3f} > 0.5*{:.3f} rad".format(spline_max, raw_max))
    print("PASS: jitter smoothing (raw max turn {:.1f} deg -> spline {:.1f} deg over {:d} dabs)".format(
        math.degrees(raw_max), math.degrees(spline_max), len(spline)))

    # -- slow/fast event-rate parity ----------------------------------------
    # Same underlying S-curve sampled at 8 vs 40 points; emitted paths coincide.
    def s_curve(n):
        return [(i / (n - 1) * 300.0, 80.0 * math.sin(i / (n - 1) * math.pi))
                for i in range(n)]

    sparse = _run(StrokeSpacer, s_curve(8), spacing)
    dense = _run(StrokeSpacer, s_curve(40), spacing)
    if len(sparse) < 5 or len(dense) < 5:
        _fail("event-rate runs emitted too few points")
    # Every sparse dab lies near the dense path and vice versa.
    max_dev = max(_min_dist_to_polyline(p, dense) for p in sparse)
    max_dev = max(max_dev, max(_min_dist_to_polyline(p, sparse) for p in dense))
    if max_dev > 0.5 * spacing:
        _fail("slow/fast paths diverge: max deviation {:.3f} > {:.3f}".format(
            max_dev, 0.5 * spacing))
    print("PASS: slow/fast parity (8 vs 40 moves, max path deviation {:.3f}px)".format(
        max_dev))

    # -- degenerate inputs don't crash --------------------------------------
    if _run(StrokeSpacer, [(5.0, 5.0)], spacing) != [(5.0, 5.0)]:
        _fail("single-point stroke must emit exactly the raw dab")
    _run(StrokeSpacer, [(1.0, 1.0), (1.0, 1.0), (1.0, 1.0)], spacing)  # coincident
    if _run(StrokeSpacer, [(0.0, 0.0), (50.0, 0.0)], 0.0)[:1] != [(0.0, 0.0)]:
        _fail("zero spacing must fall back to per-input emission")
    print("PASS: degenerate inputs handled")

    print("ALL PASS: StrokeSpacer spline behavior")


if __name__ == "__main__":
    main()
