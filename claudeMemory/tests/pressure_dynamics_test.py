# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Pressure dynamics plumbing (P7 M4).

Drives the engine's device-dynamics seam exactly as the stroke operator does
(int-keyed configure per stroke + per-dab pressure samples) and asserts the
engine scales the stroke accordingly:

  * strength dynamics: identical dab sites at pressure 1.0 vs 0.3 displace in
    ~that ratio;
  * radius dynamics: pressure 0.5 shrinks the affected vertex footprint;
  * with no dynamics configured, pushed samples change nothing.

Simulated events cannot carry real pen pressure, so this verifies the engine
side; the operator wiring (event.pressure -> pushDeviceInput) is exercised in
the GUI with mouse pressure 1.0 (a configured no-op).

Run:
    set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
    blender --factory-startup --python claudeMemory/tests/pressure_dynamics_test.py
Exits nonzero on failure.
"""

import sys

import numpy as np
import bpy

OBJ = "PressureTest"


def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def engine_pos(engine, session):
    import sculptcore

    mgr = engine.manager()
    with sculptcore.construct_from_items(mgr, mgr.get("float"), []) as dump:
        session.mesh().dumpVertCo(dump)
        return dump.numpy().reshape(-1, 4)[:, 1:4].copy()


def stroke_at(engine, strokemod, mapping, session, kernel, center, normal, pressure,
              *, dabs=5, radius=0.4):
    strokemod.stroke_begin(session)
    brush = session.brush_obj
    for _ in range(dabs):
        brush.clearDeviceInputs()
        brush.pushDeviceInput(mapping.DEVICE_PRESSURE, pressure)
        strokemod.apply_dab(session, kernel, center, normal, radius)
    strokemod.stroke_end(session)


def site_disp(pre, post, center, radius=0.6):
    """Max displacement among vertices near `center` (on the pre positions)."""
    near = np.linalg.norm(pre - np.array(center), axis=1) < radius
    return float(np.linalg.norm(post[near] - pre[near], axis=1).max())


def site_moved_count(pre, post, center, radius=0.8, eps=1e-4):
    near = np.linalg.norm(pre - np.array(center), axis=1) < radius
    return int((np.linalg.norm(post[near] - pre[near], axis=1) > eps).sum())


def main():
    import sculptcore_addon.engine as engine
    from sculptcore_addon import mapping
    from sculptcore_addon import stroke as strokemod

    if "sculptcore_addon" not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module="sculptcore_addon")
    context = bpy.context

    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()
    bpy.ops.mesh.primitive_uv_sphere_add(segments=48, ring_count=24, radius=1.0)
    ob = context.active_object
    ob.name = OBJ
    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    session = engine.sessions[OBJ]

    mgr = engine.manager()
    draw = int(mgr.get("sculptcore::brush::SculptBrushes").items["DRAW"])
    brush = strokemod._ensure_brush(session)
    brush.strength, brush.radius, brush.spacing = 1.0, 0.4, 0.1
    brush.writeProps()

    pre = engine_pos(engine, session)

    # -- strength dynamics: pressure scales displacement --------------------
    # Single dabs: accumulation saturates nonlinearly (the bulge escapes the
    # brush sphere at full pressure), so only one dab gives the clean ratio.
    brush.clearPropDynamics(mapping.PROP_STRENGTH)
    brush.clearPropDynamics(mapping.PROP_RADIUS)
    brush.addPropDynamic(mapping.PROP_STRENGTH, mapping.DEVICE_PRESSURE,
                         mapping.MIX_MULTIPLY, 1.0)
    stroke_at(engine, strokemod, mapping, session, draw, (0, 0, 1), (0, 0, 1), 1.0,
              dabs=1)
    stroke_at(engine, strokemod, mapping, session, draw, (0, 0, -1), (0, 0, -1), 0.3,
              dabs=1)
    post = engine_pos(engine, session)
    d_full = site_disp(pre, post, (0, 0, 1))
    d_low = site_disp(pre, post, (0, 0, -1))
    ratio = d_low / max(d_full, 1e-9)
    if d_full < 1e-3:
        _fail("full-pressure stroke did not move the surface")
    if not (0.2 <= ratio <= 0.4):
        _fail("strength pressure ratio {:.3f} not ~0.3 (full {:.4f}, low {:.4f})".format(
            ratio, d_full, d_low))
    print("PASS: strength dynamics scale with pressure "
          "(full {:.4f}, low {:.4f}, ratio {:.2f})".format(d_full, d_low, ratio))

    # -- radius dynamics: pressure shrinks the footprint --------------------
    pre2 = engine_pos(engine, session)
    brush.clearPropDynamics(mapping.PROP_STRENGTH)
    brush.addPropDynamic(mapping.PROP_RADIUS, mapping.DEVICE_PRESSURE,
                         mapping.MIX_MULTIPLY, 1.0)
    stroke_at(engine, strokemod, mapping, session, draw, (1, 0, 0), (1, 0, 0), 1.0)
    stroke_at(engine, strokemod, mapping, session, draw, (-1, 0, 0), (-1, 0, 0), 0.5)
    post2 = engine_pos(engine, session)
    n_full = site_moved_count(pre2, post2, (1, 0, 0))
    n_low = site_moved_count(pre2, post2, (-1, 0, 0))
    if n_full == 0:
        _fail("full-pressure radius stroke moved nothing")
    if not (n_low < n_full * 0.7):
        _fail("radius pressure did not shrink the footprint ({:d} vs {:d})".format(
            n_low, n_full))
    print("PASS: radius dynamics shrink the footprint "
          "({:d} verts at 1.0 vs {:d} at 0.5)".format(n_full, n_low))

    # -- no dynamics configured: pushed samples are inert -------------------
    pre3 = engine_pos(engine, session)
    brush.clearPropDynamics(mapping.PROP_STRENGTH)
    brush.clearPropDynamics(mapping.PROP_RADIUS)
    stroke_at(engine, strokemod, mapping, session, draw, (0, 1, 0), (0, 1, 0), 1.0)
    stroke_at(engine, strokemod, mapping, session, draw, (0, -1, 0), (0, -1, 0), 0.3)
    post3 = engine_pos(engine, session)
    d_a = site_disp(pre3, post3, (0, 1, 0))
    d_b = site_disp(pre3, post3, (0, -1, 0))
    if abs(d_a - d_b) > d_a * 0.05:
        _fail("pushed samples changed a stroke with no dynamics "
              "({:.4f} vs {:.4f})".format(d_a, d_b))
    print("PASS: samples are inert without configured dynamics "
          "({:.4f} vs {:.4f})".format(d_a, d_b))

    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    if OBJ in engine.sessions:
        _fail("session leaked after exit")
    print("ALL PASS: pressure dynamics plumbing verified.")


if __name__ == "__main__":
    main()
