# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Dependency-free stroke geometry: centripetal Catmull-Rom to cubic Bezier,
Bezier evaluation and sub-curve extraction, and an arc-length walk that emits
points at even intervals with a carry threaded across segments.

Ported from the reference app's ``stroke_math.ts``. It is the pure-math layer
under ``StrokeSpacer``: given the raw 2D control points of a mouse path it
produces evenly spaced points along a smooth spline (centripetal alpha = 0.5
avoids the cusps and self-intersections a uniform Catmull-Rom makes on jittery
input). Points are plain tuples of floats; every routine is dimension-agnostic
(2D screen space is the only current caller, but the world-space slice helpers
work in 3D too), so this module imports nothing and is unit-testable on its own.
"""

import math

# Below this, two control points are treated as coincident and their knot
# interval is nudged up so the centripetal tangents stay finite.
_EPS = 1e-9


def _sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def _add(a, b):
    return tuple(x + y for x, y in zip(a, b))


def _scale(a, s):
    return tuple(x * s for x in a)


def _lerp(a, b, t):
    return tuple(x + (y - x) * t for x, y in zip(a, b))


def _dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def cr_to_bezier(p0, p1, p2, p3, alpha=0.5):
    """The centripetal Catmull-Rom segment from ``p1`` to ``p2`` as a cubic
    Bezier ``(b0, b1, b2, b3)``. ``p0``/``p3`` are the outer control points that
    define the endpoint tangents; clamp them to ``p1``/``p2`` at the ends of a
    path. ``alpha`` is the parameterization exponent (0 uniform, 0.5 centripetal,
    1 chordal); 0.5 is the cusp-free default."""
    # Knot spacing t_i = t_{i-1} + |p_i - p_{i-1}|**alpha, floored so coincident
    # points don't divide by zero.
    t0 = 0.0
    t1 = t0 + max(_dist(p0, p1) ** alpha, _EPS)
    t2 = t1 + max(_dist(p1, p2) ** alpha, _EPS)
    t3 = t2 + max(_dist(p2, p3) ** alpha, _EPS)

    # Non-uniform Catmull-Rom endpoint tangents (reduces to (p2-p0)/2 and
    # (p3-p1)/2 for uniform knots).
    m1 = _add(_add(_scale(_sub(p1, p0), 1.0 / (t1 - t0)),
                   _scale(_sub(p2, p0), -1.0 / (t2 - t0))),
              _scale(_sub(p2, p1), 1.0 / (t2 - t1)))
    m2 = _add(_add(_scale(_sub(p2, p1), 1.0 / (t2 - t1)),
                   _scale(_sub(p3, p1), -1.0 / (t3 - t1))),
              _scale(_sub(p3, p2), 1.0 / (t3 - t2)))

    # Hermite -> Bezier over the local interval [t1, t2] (length h): the Bezier
    # start tangent 3*(b1-b0) equals h*m1, so b1 = p1 + h/3 * m1.
    h = t2 - t1
    b1 = _add(p1, _scale(m1, h / 3.0))
    b2 = _sub(p2, _scale(m2, h / 3.0))
    return (tuple(p1), b1, b2, tuple(p2))


def eval_cubic(bez, t):
    """Evaluate the cubic Bezier ``bez`` at parameter ``t`` in [0, 1]."""
    b0, b1, b2, b3 = bez
    u = 1.0 - t
    c0 = u * u * u
    c1 = 3.0 * u * u * t
    c2 = 3.0 * u * t * t
    c3 = t * t * t
    return tuple(b0[i] * c0 + b1[i] * c1 + b2[i] * c2 + b3[i] * c3
                 for i in range(len(b0)))


def _split_cubic(bez, t):
    """De Casteljau split of ``bez`` at ``t`` into (left, right) cubics."""
    b0, b1, b2, b3 = bez
    ab = _lerp(b0, b1, t)
    bc = _lerp(b1, b2, t)
    cd = _lerp(b2, b3, t)
    abc = _lerp(ab, bc, t)
    bcd = _lerp(bc, cd, t)
    abcd = _lerp(abc, bcd, t)
    return (b0, ab, abc, abcd), (abcd, bcd, cd, b3)


def sub_cubic(bez, a, b):
    """The restriction of the cubic Bezier ``bez`` to the parameter interval
    ``[a, b]`` (0 <= a <= b <= 1), as a new cubic Bezier."""
    _, right = _split_cubic(bez, a)  # right spans [a, 1] reparameterized to [0, 1]
    span = 1.0 - a
    t = 0.0 if span <= _EPS else (b - a) / span
    left, _ = _split_cubic(right, t)  # left spans [a, b]
    return left


def arc_length_walk(bez, spacing, carry, chords=32):
    """Walk ``bez`` at even arc-length ``spacing``, returning
    ``(points, carry_out)``.

    ``carry`` is the arc length already accumulated toward the next point when
    this segment begins (0 at the very first segment); ``carry_out`` is the
    leftover past the last emitted point, to be threaded into the next segment's
    walk so cadence never clusters at a joint. Arc length is approximated with
    ``chords`` straight chords; the parameter is interpolated linearly inside the
    chord that contains each target distance."""
    if spacing <= 0.0:
        return [], carry

    # Sample the curve and build the cumulative chord-length table.
    pts = [eval_cubic(bez, i / chords) for i in range(chords + 1)]
    cum = [0.0] * (chords + 1)
    for i in range(chords):
        cum[i + 1] = cum[i] + _dist(pts[i], pts[i + 1])
    total = cum[chords]

    emitted = []
    # First target sits `spacing - carry` into this segment; then every spacing.
    target = spacing - carry
    seg = 0
    while target <= total + 1e-12:
        while seg < chords and cum[seg + 1] < target:
            seg += 1
        if seg >= chords:
            chord_len = cum[chords] - cum[chords - 1]
            frac = 1.0 if chord_len <= _EPS else (target - cum[chords - 1]) / chord_len
            t = (chords - 1 + frac) / chords
        else:
            chord_len = cum[seg + 1] - cum[seg]
            frac = 0.0 if chord_len <= _EPS else (target - cum[seg]) / chord_len
            t = (seg + frac) / chords
        emitted.append(eval_cubic(bez, min(max(t, 0.0), 1.0)))
        target += spacing

    # `target - spacing` is the arc position of the last emitted point (or
    # `-carry` if none emitted); the remainder becomes the next segment's carry.
    carry_out = total - (target - spacing)
    return emitted, carry_out
