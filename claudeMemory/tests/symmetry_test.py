# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Plane-mirror symmetry (Q4).

Unit: the SymAxisMap reflection table (all 8 axis combos) and axes_from_mesh.
Integration: replicating the operator's mirror loop (reflect the resolved
primary center + normal, apply) over a symmetric sphere yields symmetric
geometry for X, X+Y+Z, and grab; a primary-only control stays asymmetric.

Run:
    set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
    blender --factory-startup --python claudeMemory/scripts/run_sync.py -- \
        claudeMemory/tests/symmetry_test.py
Exits nonzero on failure.
"""

import sys

import numpy as np
import bpy
import mathutils
from mathutils import kdtree


def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def positions(ob):
    a = np.empty(len(ob.data.vertices) * 3, dtype=np.float32)
    ob.data.vertices.foreach_get("co", a)
    return a.reshape(-1, 3)


def symmetry_error(pos, sign):
    """Max distance from each reflected vertex position to the nearest actual
    vertex — ~0 iff the geometry is symmetric under `sign`."""
    tree = kdtree.KDTree(len(pos))
    for i, p in enumerate(pos):
        tree.insert((float(p[0]), float(p[1]), float(p[2])), i)
    tree.balance()
    worst = 0.0
    for p in pos:
        r = (p[0] * sign[0], p[1] * sign[1], p[2] * sign[2])
        _, _, dist = tree.find(r)
        worst = max(worst, dist)
    return worst


def test_table():
    import sculptcore_addon.symmetry as symmetry
    # Exact SymAxisMap expectations.
    if symmetry.mirror_signs(0) != []:
        _fail("no-symmetry must yield no mirrors")
    if symmetry.mirror_signs(symmetry.AXIS_X) != [(-1.0, 1.0, 1.0)]:
        _fail("X mirror wrong: {}".format(symmetry.mirror_signs(symmetry.AXIS_X)))
    xy = symmetry.mirror_signs(symmetry.AXIS_X | symmetry.AXIS_Y)
    if sorted(xy) != sorted([(-1.0, 1.0, 1.0), (1.0, -1.0, 1.0), (-1.0, -1.0, 1.0)]):
        _fail("X+Y mirrors wrong: {}".format(xy))
    xyz = symmetry.mirror_signs(symmetry.AXIS_X | symmetry.AXIS_Y | symmetry.AXIS_Z)
    if len(xyz) != 7:
        _fail("X+Y+Z must give 7 reflections, got {:d}".format(len(xyz)))
    # Every reflection is a distinct non-identity sign vector.
    if len(set(xyz)) != 7 or (1.0, 1.0, 1.0) in xyz:
        _fail("X+Y+Z reflections not distinct / contains identity")
    print("PASS: SymAxisMap table (X, X+Y, X+Y+Z)")


def test_axes_from_mesh():
    import sculptcore_addon.symmetry as symmetry
    bpy.ops.mesh.primitive_cube_add()
    mesh = bpy.context.active_object.data
    mesh.use_mirror_x = mesh.use_mirror_y = mesh.use_mirror_z = False
    if symmetry.axes_from_mesh(mesh) != 0:
        _fail("axes_from_mesh should be 0 with all flags off")
    mesh.use_mirror_x = True
    mesh.use_mirror_z = True
    if symmetry.axes_from_mesh(mesh) != (symmetry.AXIS_X | symmetry.AXIS_Z):
        _fail("axes_from_mesh did not read X|Z")
    bpy.ops.object.delete()
    print("PASS: axes_from_mesh reads use_mirror flags")


def _sym_draw(engine, stroke, convert, mgr, symmetry, name, signs, grab=False):
    """Apply a stroke plus mirror images (operator logic, object space) and
    return final positions."""
    bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=16, radius=1.0)
    ob = bpy.context.active_object
    ob.name = name
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    session = engine.sessions[name]

    b = stroke._ensure_brush(session)
    b.strength = 0.6
    b.radius = 0.3
    b.spacing = 0.1
    b.invert = False
    b.writeProps()
    kind = "GRAB" if grab else "DRAW"
    kernel = int(mgr.get("sculptcore::brush::SculptBrushes").items[kind])

    # Resolve the primary center once (a fixed center isolates the reflection
    # logic from the engine's raycast imprecision, which re-hitting the
    # deforming surface each round would otherwise compound). Mirrors reflect it.
    hit = stroke.raycast(session, (0.5, 0.3, 5.0), (0.0, 0.0, -1.0))
    if hit is None:
        _fail("primary raycast missed")
    pos, nor, _ = hit

    stroke.stroke_begin(session)
    for i in range(6):
        if grab:
            # Drag the cursor away from the anchor to build a real grab delta.
            cursor = (pos[0], pos[1] + 0.05 * (i + 1), pos[2])
            stroke.apply_grab_dab(session, kernel, pos, cursor, nor, 0.4)
            for s in signs:
                stroke.apply_grab_dab(
                    session, kernel, symmetry.reflect(pos, s),
                    symmetry.reflect(cursor, s), symmetry.reflect(nor, s),
                    0.4, accum_add=True)
        else:
            stroke.apply_dab(session, kernel, pos, nor, 0.3)
            for s in signs:
                stroke.apply_dab(session, kernel, symmetry.reflect(pos, s),
                                 symmetry.reflect(nor, s), 0.3)
    stroke.stroke_end(session)
    convert.flush(ob)
    pos = positions(ob).copy()
    bpy.ops.object.custom_mode_toggle()
    return pos


def main():
    if "sculptcore_addon" not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module="sculptcore_addon")
    import sculptcore_addon.engine as engine
    import sculptcore_addon.stroke as stroke
    import sculptcore_addon.convert as convert
    import sculptcore_addon.symmetry as symmetry
    mgr = engine.manager()
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()

    test_table()
    test_axes_from_mesh()

    X = (-1.0, 1.0, 1.0)

    # Control: primary only must be clearly asymmetric across X.
    ctrl = _sym_draw(engine, stroke, convert, mgr, symmetry, "ctrl", [])
    ctrl_err = symmetry_error(ctrl, X)
    if ctrl_err < 0.05:
        _fail("control (no mirror) is already X-symmetric ({:.4f}) - test insensitive".format(
            ctrl_err))
    print("PASS: primary-only control is asymmetric (X error {:.3f})".format(ctrl_err))

    # X symmetry.
    px = _sym_draw(engine, stroke, convert, mgr, symmetry, "symX",
                   symmetry.mirror_signs(symmetry.AXIS_X))
    ex = symmetry_error(px, X)
    if ex > 1e-3:
        _fail("X-symmetric stroke left X asymmetry {:.5f}".format(ex))
    print("PASS: X-mirror stroke stays symmetric (X error {:.6f})".format(ex))

    # X+Y+Z symmetry: symmetric across every axis.
    pall = _sym_draw(engine, stroke, convert, mgr, symmetry, "symAll",
                     symmetry.mirror_signs(symmetry.AXIS_X | symmetry.AXIS_Y | symmetry.AXIS_Z))
    for axis, sign in (("X", (-1, 1, 1)), ("Y", (1, -1, 1)), ("Z", (1, 1, -1))):
        e = symmetry_error(pall, sign)
        if e > 1e-3:
            _fail("X+Y+Z stroke left {:s} asymmetry {:.5f}".format(axis, e))
    print("PASS: X+Y+Z stroke symmetric across all three planes")

    # Grab + X symmetry.
    pg = _sym_draw(engine, stroke, convert, mgr, symmetry, "symGrab",
                   symmetry.mirror_signs(symmetry.AXIS_X), grab=True)
    eg = symmetry_error(pg, X)
    if eg > 1e-3:
        _fail("grab X-symmetric stroke left asymmetry {:.5f}".format(eg))
    print("PASS: grab X-mirror stroke stays symmetric (X error {:.6f})".format(eg))

    # Dyntopo + X symmetry: both sides remesh on the shared cadence. Dyntopo is
    # seed-driven so topology is not bit-symmetric, but both halves must gain a
    # comparable amount of geometry (no side starved of the spacing budget).
    test_dyntopo_symmetry(engine, stroke, convert, mgr, symmetry)

    print("ALL PASS: symmetry")


def test_dyntopo_symmetry(engine, stroke, convert, mgr, symmetry):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=16, radius=1.0)
    ob = bpy.context.active_object
    ob.name = "symDt"
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    session = engine.sessions["symDt"]
    b = stroke._ensure_brush(session)
    b.strength = 0.4
    b.radius = 0.3
    b.spacing = 0.1
    b.invert = False
    b.writeProps()
    draw = int(mgr.get("sculptcore::brush::SculptBrushes").items["DRAW"])
    prog = stroke.build_program(session, draw, 0.0)
    detail = 0.03
    params = stroke.build_dyntopo_params(session, detail, detail * 0.5)
    signs = symmetry.mirror_signs(symmetry.AXIS_X)

    stroke.stroke_begin(session, has_dyntopo=True)
    stroke_s = 0.0
    last = float("-inf")
    seed = 0
    for i in range(20):
        # March across +x so the primary hits fresh territory; the mirror hits
        # the corresponding -x territory.
        x = 0.2 + 0.5 * (i / 19.0)
        hit = stroke.raycast(session, (x, 0.0, 5.0), (0, 0, -1))
        if hit is None:
            continue
        pos, nor, _ = hit
        stroke_s += 1.0
        due = stroke.dyntopo_due(stroke_s, last, 3.0)  # shared across images
        if due:
            last = stroke_s
        seed += 1
        stroke.apply_dyntopo_dab(session, prog, pos, nor, 0.3,
                                 params if due else None, seed)
        for s in signs:
            seed += 1
            stroke.apply_dyntopo_dab(session, prog, symmetry.reflect(pos, s),
                                     symmetry.reflect(nor, s), 0.3,
                                     params if due else None, seed)
    stroke.stroke_end(session)
    convert.flush(ob)

    pos = positions(ob)
    left = int((pos[:, 0] < -1e-4).sum())
    right = int((pos[:, 0] > 1e-4).sum())
    bpy.ops.object.custom_mode_toggle()
    if left == 0 or right == 0:
        _fail("dyntopo symmetry: a side was not remeshed (L={:d} R={:d})".format(left, right))
    ratio = min(left, right) / max(left, right)
    if ratio < 0.8:
        _fail("dyntopo symmetry unbalanced: L={:d} R={:d} (ratio {:.2f})".format(
            left, right, ratio))
    print("PASS: dyntopo + X symmetry remeshes both sides (L={:d} R={:d}, ratio {:.2f})".format(
        left, right, ratio))


if __name__ == "__main__":
    main()
