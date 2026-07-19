# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Generated engine-prop verification (brush-mapping M2/M3).

Checks that the manifest walk generated Brush.sculptcore (kelvinlet mu/nu,
plane-family planeSide), that apply_brush routes the values into the engine
brush, that nu measurably reshapes an elastic-deform grab (mu is normalized out), that the
defaults leave existing behavior untouched (CLAY still sculpts), and that
disable/re-enable of the addon regenerates the group cleanly.

Run:
    set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
    blender --factory-startup --python claudeMemory/tests/engine_props_test.py
Exits nonzero on failure.
"""

import sys

import numpy as np
import bpy

OBJ = "PropsTest"


def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def engine_pos(engine, session):
    import sculptcore

    mgr = engine.manager()
    with sculptcore.construct_from_items(mgr, mgr.get("float"), []) as dump:
        session.mesh().dumpVertCo(dump)
        return dump.numpy().reshape(-1, 4)[:, 1:4].copy()


def grab_disp(engine, strokemod, mapping, session, bl_brush, nu):
    """Mean displacement field of one elastic-deform grab at Poisson ratio
    `nu` (the field SHAPE knob; `mu` is normalized out by the kernel - the
    grab center always moves exactly `grabTo`)."""
    bl_brush.sculptcore.nu = nu
    pre = engine_pos(engine, session)
    anchor = strokemod.raycast(session, (0, 0, 5), (0, 0, -1))
    position, normal, _ = anchor
    mapping.apply_brush(bl_brush, None, session.brush_obj,
                        world_radius=0.6, invert=False)
    kernel = mapping.kernel_enum(engine.manager(), bl_brush)
    strokemod.stroke_begin(session)
    cursor = (position[0] + 0.4, position[1], position[2])
    strokemod.apply_grab_dab(session, kernel, position, cursor, normal, 0.6)
    strokemod.stroke_end(session)
    post = engine_pos(engine, session)
    field = post - pre
    # Revert so the next measurement starts from the same surface.
    log = session.meshlog
    log.undo(session.mesh(), session.tree())
    session.meshlog_cursor = max(0, session.meshlog_cursor - 1)
    return field


def main():
    import sculptcore_addon.engine as engine
    from sculptcore_addon import engine_props, mapping
    from sculptcore_addon import stroke as strokemod

    if "sculptcore_addon" not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module="sculptcore_addon")
    context = bpy.context

    # -- generation ---------------------------------------------------------
    if not hasattr(bpy.types.Brush, "sculptcore"):
        _fail("Brush.sculptcore was not generated")
    for kernel, expected in (("KELVINLET", {"mu", "nu"}), ("CLAY", {"planeSide"})):
        names = set(engine_props._kernel_props.get(kernel, ()))
        if not expected <= names:
            _fail("kernel {:s} missing generated props {!r}".format(
                kernel, expected - names))
    print("PASS: Brush.sculptcore generated (kelvinlet mu/nu, plane planeSide)")

    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()
    bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=16, radius=1.0)
    ob = context.active_object
    ob.name = OBJ
    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    session = engine.sessions[OBJ]

    # -- routing: apply_brush copies the group into the engine fields -------
    bl_brush = bpy.data.brushes.new("PropsElastic", mode='SCULPT')
    bl_brush.sculpt_brush_type = 'ELASTIC_DEFORM'
    sc_brush = strokemod._ensure_brush(session)
    bl_brush.sculptcore.mu = 3.5
    bl_brush.sculptcore.nu = 0.25
    mapping.apply_brush(bl_brush, None, sc_brush, world_radius=0.5, invert=False)
    if abs(float(sc_brush.mu) - 3.5) > 1e-6 or abs(float(sc_brush.nu) - 0.25) > 1e-6:
        _fail("apply_brush did not route mu/nu ({} / {})".format(
            float(sc_brush.mu), float(sc_brush.nu)))
    print("PASS: apply_brush routes generated props into engine fields")

    # -- behavior: nu reshapes the elastic field (mu is normalized out) -----
    bl_brush.strength = 1.0
    bl_brush.sculptcore.mu = 1.0
    field_low = grab_disp(engine, strokemod, mapping, session, bl_brush, nu=0.0)
    field_high = grab_disp(engine, strokemod, mapping, session, bl_brush, nu=0.49)
    peak = float(np.linalg.norm(field_low, axis=1).max())
    shape_diff = float(np.linalg.norm(field_high - field_low, axis=1).max())
    if peak < 1e-5:
        _fail("elastic grab did not move the surface")
    # The falloff gate concentrates the grab at the center where the
    # normalization pins displacement to grabTo regardless of nu, so the
    # reshape lives in the mid-falloff ring: small but deterministic (a
    # broken pipeline measures exactly zero).
    if shape_diff < peak * 0.01:
        _fail("nu did not reshape the field (peak {:.5f}, diff {:.5f})".format(
            peak, shape_diff))
    print("PASS: nu reshapes the elastic field (peak {:.5f}, vector diff {:.5f})".format(
        peak, shape_diff))

    # -- defaults keep existing behavior (CLAY still sculpts) ---------------
    bl_clay = bpy.data.brushes.new("PropsClay", mode='SCULPT')
    bl_clay.sculpt_brush_type = 'CLAY'
    bl_clay.strength = 1.0
    pre = engine_pos(engine, session)
    hit = strokemod.raycast(session, (0, 0, 5), (0, 0, -1))
    mapping.apply_brush(bl_clay, None, sc_brush, world_radius=0.5, invert=False)
    kernel = mapping.kernel_enum(engine.manager(), bl_clay)
    strokemod.stroke_begin(session)
    for _ in range(5):
        strokemod.apply_dab(session, kernel, hit[0], hit[1], 0.5)
    strokemod.stroke_end(session)
    moved = float(np.abs(engine_pos(engine, session) - pre).max())
    if moved < 1e-4:
        _fail("CLAY with generated defaults did not sculpt (planeSide default?)")
    print("PASS: CLAY sculpts with generated defaults (moved {:.4f})".format(moved))

    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")

    # -- idempotent re-register ---------------------------------------------
    bpy.ops.preferences.addon_disable(module="sculptcore_addon")
    if hasattr(bpy.types.Brush, "sculptcore"):
        _fail("Brush.sculptcore not removed on addon disable")
    bpy.ops.preferences.addon_enable(module="sculptcore_addon")
    if not hasattr(bpy.types.Brush, "sculptcore"):
        _fail("Brush.sculptcore not regenerated on re-enable")
    if abs(float(bl_brush.sculptcore.nu) - 0.49) > 1e-4:
        _fail("authored value lost across re-register ({})".format(
            float(bl_brush.sculptcore.nu)))
    print("PASS: disable/re-enable regenerates the group, values survive")

    print("ALL PASS: generated engine props verified.")


if __name__ == "__main__":
    main()
