# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Dyntopo cadence decoupled from dab cadence (Q2).

Two parts:
  * ``dyntopo_due`` cadence unit: N dabs at dab-step ``s`` must mark
    ~strokeLen/spacing remeshes due, not N (and spacing 0 stays every-dab).
  * Integration: a decoupled-cadence dyntopo stroke remeshes fewer times than
    the every-dab baseline (fewer added vertices for the same dab count) while
    still refining the surface and deforming it on the off-cadence dabs.

Run:
    set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
    blender --factory-startup --python claudeMemory/scripts/run_sync.py -- \
        claudeMemory/tests/dyntopo_cadence_test.py
Exits nonzero on failure.
"""

import sys

import numpy as np
import bpy


def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def positions(ob):
    a = np.empty(len(ob.data.vertices) * 3, dtype=np.float32)
    ob.data.vertices.foreach_get("co", a)
    return a.reshape(-1, 3)


def _simulate_cadence(stroke, n, step, spacing):
    """Mirror the operator's stroke_s / last_dyntopo_s bookkeeping and count how
    many of ``n`` dabs are remesh-due."""
    stroke_s = 0.0
    last = float("-inf")
    dues = 0
    for _ in range(n):
        stroke_s += step
        if stroke.dyntopo_due(stroke_s, last, spacing):
            last = stroke_s
            dues += 1
    return dues, stroke_s


def test_cadence_unit(stroke):
    n, step = 40, 1.0
    # spacing 0 => every dab.
    dues, _ = _simulate_cadence(stroke, n, step, 0.0)
    if dues != n:
        _fail("spacing 0 must be every-dab ({:d} != {:d})".format(dues, n))
    # spacing 5*step => ~n/5 remeshes.
    dues, total = _simulate_cadence(stroke, n, step, 5.0)
    expected = total / 5.0
    if abs(dues - expected) > 1.5:
        _fail("cadence off: {:d} dues vs ~{:.1f} expected".format(dues, expected))
    if dues >= n:
        _fail("decoupled cadence did not reduce remesh count")
    print("PASS: dyntopo_due cadence ({:d} dabs, spacing 5*step -> {:d} remeshes)".format(
        n, dues))


def _dyntopo_stroke(engine, stroke, convert, mgr, name, spacing, step, ndabs):
    """Fresh sphere + a decoupled-cadence dyntopo stroke along a moving path.
    Returns (due_count, start_verts, end_verts, moved)."""
    bpy.ops.mesh.primitive_uv_sphere_add(segments=48, ring_count=24, radius=1.0)
    ob = bpy.context.active_object
    ob.name = name
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    session = engine.sessions[name]

    b = stroke._ensure_brush(session)
    b.strength = 0.4
    b.radius = 0.35
    b.spacing = 0.1
    b.invert = False
    b.writeProps()
    kernel = int(mgr.get("sculptcore::brush::SculptBrushes").items["DRAW"])
    prog = stroke.build_program(session, kernel, 0.0)
    detail = 0.03
    params = stroke.build_dyntopo_params(session, detail, detail * 0.5)

    start_verts = len(ob.data.vertices)
    before = positions(ob).copy()

    stroke_s = 0.0
    last = float("-inf")
    dues = 0
    stroke.stroke_begin(session, has_dyntopo=True)
    for i in range(ndabs):
        # March a raycast across the top of the sphere so each dab hits fresh
        # territory (a stationary dab would converge and stop adding verts).
        x = -0.6 + 1.2 * (i / (ndabs - 1))
        hit = stroke.raycast(session, (x, 0.0, 5.0), (0, 0, -1))
        if hit is None:
            continue
        center, normal, _ = hit
        stroke_s += step
        due = stroke.dyntopo_due(stroke_s, last, spacing)
        if due:
            last = stroke_s
            dues += 1
        stroke.apply_dyntopo_dab(session, prog, center, normal, 0.35,
                                 params if due else None, i + 1)
    stroke.stroke_end(session)
    convert.flush(ob)

    end_verts = len(ob.data.vertices)
    # Compare against the pre-stroke positions over the shared vertex prefix
    # (dyntopo only appends), so this measures real deformation.
    after = positions(ob)
    k = min(len(before), len(after))
    moved = float(np.abs(after[:k] - before[:k]).sum())
    bpy.ops.object.custom_mode_toggle()
    return dues, start_verts, end_verts, moved


def main():
    if "sculptcore_addon" not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module="sculptcore_addon")
    import sculptcore_addon.engine as engine
    import sculptcore_addon.stroke as stroke
    import sculptcore_addon.convert as convert
    mgr = engine.manager()
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()

    test_cadence_unit(stroke)

    ndabs, step = 40, 1.0
    every = _dyntopo_stroke(engine, stroke, convert, mgr, "dt_every", 0.0, step, ndabs)
    cadence = _dyntopo_stroke(engine, stroke, convert, mgr, "dt_cadence", 5.0, step, ndabs)
    d_every, s0_e, v_every, m_every = every
    d_cad, s0_c, v_cad, m_cad = cadence

    print("every-dab : due={:d} verts {:d}->{:d} moved={:.3f}".format(
        d_every, s0_e, v_every, m_every))
    print("cadence   : due={:d} verts {:d}->{:d} moved={:.3f}".format(
        d_cad, s0_c, v_cad, m_cad))

    if d_cad >= d_every:
        _fail("cadence stroke did not reduce remesh count ({:d} >= {:d})".format(
            d_cad, d_every))
    if v_cad <= s0_c:
        _fail("cadence stroke did not refine the surface at all")
    if v_cad >= v_every:
        _fail("cadence remeshed as much as every-dab (no cost saving)")
    if m_cad <= 1e-3:
        _fail("off-cadence dabs did not deform the surface")
    print("PASS: decoupled cadence refines with fewer remeshes and still deforms")
    print("ALL PASS: dyntopo cadence")


if __name__ == "__main__":
    main()
