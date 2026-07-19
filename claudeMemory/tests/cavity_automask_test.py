# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Cavity automasking verification (brush-mapping Phase 2).

The engine masks every kernel's strength by a per-vertex cavity factor
(automask.h, a port of Blender's `calc_cavity_factor`). This measures that
factor directly: each configuration strokes the same dab on the same surface
and divides the resulting displacement by an unmasked baseline stroke, so the
falloff cancels and the ratio *is* the mask.

Checks, on a grid plane with a smooth groove cut across it:
  - the groove's concave floor keeps nearly full strength and its convex
    shoulders are suppressed (Blender's convention: the effect stays in
    cavities), with the inverted mode mirroring both about 0.5;
  - flat geometry reads exactly 0.5 — a zero cavity estimate sits at the
    remap's center, so an unmasked stroke halves there;
  - `cavity_factor = 0` flattens the mask to a uniform 0.5;
  - a custom curve reshapes that 0.5 back to full strength.

Run:
    set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
    blender --factory-startup --python claudeMemory/scripts/run_sync.py -- \
        claudeMemory/tests/cavity_automask_test.py
Exits nonzero on failure.
"""

import sys

import numpy as np
import bpy

OBJ = "CavityPlane"

# Groove cut across the plane at y = 0: half-width and depth. Wide enough that
# the 2-step cavity blur reads a genuinely concave neighborhood at this grid
# resolution.
GROOVE_W = 0.15
GROOVE_D = 0.12

RADIUS = 0.9


def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def positions(ob):
    mesh = ob.data
    a = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", a)
    return a.reshape(-1, 3)


def main():
    if "sculptcore_addon" not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module="sculptcore_addon")
    import sculptcore_addon.engine as engine
    from sculptcore_addon import convert, mapping
    from sculptcore_addon import stroke as strokemod

    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()
    bpy.ops.mesh.primitive_grid_add(x_subdivisions=128, y_subdivisions=128, size=2.0)
    ob = bpy.context.active_object
    ob.name = OBJ

    # Cut the groove before the engine sees the mesh.
    rest = positions(ob).copy()
    co = rest.copy()
    inside = np.abs(co[:, 1]) < GROOVE_W
    profile = 0.5 * (1.0 + np.cos(np.pi * np.abs(co[inside, 1]) / GROOVE_W))
    co[inside, 2] = -GROOVE_D * profile
    ob.data.vertices.foreach_set("co", co.reshape(-1))
    ob.data.update()
    rest = positions(ob).copy()

    # Measurement regions, all well inside the dab: the groove floor (concave),
    # its shoulders where the profile meets the plane (convex), and untouched
    # flat plane beyond them.
    in_dab = np.abs(rest[:, 0]) <= 0.4
    dist_y = np.abs(rest[:, 1])
    floor = in_dab & (dist_y <= 0.4 * GROOVE_W)
    shoulder = in_dab & (dist_y >= 0.85 * GROOVE_W) & (dist_y <= 1.1 * GROOVE_W)
    flat = in_dab & (dist_y >= 0.35) & (dist_y <= 0.5)
    for name, region in (("floor", floor), ("shoulder", shoulder), ("flat", flat)):
        if region.sum() < 20:
            _fail("{:s} region too small ({:d} verts)".format(name, int(region.sum())))

    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    session = engine.sessions[OBJ]
    mgr = engine.manager()

    bl_brush = bpy.data.brushes.new("scp_cavity", mode='SCULPT')
    bl_brush.sculpt_brush_type = 'DRAW'
    bl_brush.strength = 1.0
    settings = bl_brush.mesh_automasking_settings
    sc_brush = strokemod._ensure_brush(session)
    kernel = mapping.kernel_enum(mgr, bl_brush)

    def stroke():
        """One dab through the full mapping; returns the per-vertex dz and
        reverts the mesh so the next configuration starts from the same
        surface."""
        before = positions(ob).copy()
        mapping.apply_brush(bl_brush, None, sc_brush,
                            world_radius=RADIUS, invert=False)
        strokemod.stroke_begin(session, has_dyntopo=False)
        strokemod.apply_dab(session, kernel, (0.0, 0.0, 0.0), (0.0, 0.0, 1.0), RADIUS)
        strokemod.stroke_end(session)
        convert.flush(ob)
        after = positions(ob)
        if not np.isfinite(after).all():
            _fail("non-finite positions after a dab")
        dz = after[:, 2] - before[:, 2]
        session.meshlog.undo(session.mesh(), session.tree())
        session.meshlog_cursor = max(0, session.meshlog_cursor - 1)
        convert.flush(ob)
        return dz

    def mask_of(dz, base, region):
        """Mean measured mask factor over a region (displacement / baseline).
        Only verts the baseline actually moved carry information."""
        sel = region & (np.abs(base) > 1e-5)
        if sel.sum() < 20:
            _fail("baseline moved too few verts in a region ({:d})".format(int(sel.sum())))
        return float(np.mean(dz[sel] / base[sel]))

    # -- baseline: automasking off ------------------------------------------
    base = stroke()
    if not np.abs(base).max() > 1e-4:
        _fail("baseline stroke did not move the mesh")
    if mask_of(base, base, flat) != 1.0:
        _fail("baseline is not its own unit mask")
    print("PASS: baseline stroke moves the surface (max dz {:.5f})".format(
        float(np.abs(base).max())))

    # -- cavity on: the effect stays in the cavity ---------------------------
    settings.use_automasking_cavity = True
    dz = stroke()
    flat_mask = mask_of(dz, base, flat)
    floor_mask = mask_of(dz, base, floor)
    shoulder_mask = mask_of(dz, base, shoulder)
    if abs(flat_mask - 0.5) > 0.02:
        _fail("flat geometry should sit at the remap center 0.5, read {:.4f}".format(
            flat_mask))
    if not floor_mask > 0.7:
        _fail("the concave groove floor should keep its strength, read {:.4f}".format(
            floor_mask))
    if not shoulder_mask < 0.3:
        _fail("the convex shoulders should be suppressed, read {:.4f}".format(
            shoulder_mask))
    print("PASS: cavity keeps the effect in the groove "
          "(floor {:.4f}, shoulder {:.4f}, flat {:.4f})".format(
              floor_mask, shoulder_mask, flat_mask))

    # -- inverted mirrors both factors about 0.5 ----------------------------
    settings.use_automasking_cavity = False
    settings.use_automasking_cavity_inverted = True
    dz = stroke()
    for name, region, upright in (("floor", floor, floor_mask),
                                  ("shoulder", shoulder, shoulder_mask)):
        value = mask_of(dz, base, region)
        if abs(value - (1.0 - upright)) > 0.02:
            _fail("inverted cavity should mirror {:s} about 0.5 "
                  "(got {:.4f}, expected {:.4f})".format(name, value, 1.0 - upright))
    print("PASS: inverted cavity mirrors the factor about 0.5")

    # -- factor 0 flattens the mask to a uniform 0.5 ------------------------
    settings.use_automasking_cavity_inverted = False
    settings.use_automasking_cavity = True
    settings.cavity_factor = 0.0
    dz = stroke()
    for name, region in (("flat", flat), ("floor", floor)):
        value = mask_of(dz, base, region)
        if abs(value - 0.5) > 0.02:
            _fail("cavity_factor 0 should mask uniformly at 0.5, {:s} read {:.4f}".format(
                name, value))
    print("PASS: cavity_factor 0 is a uniform 0.5 mask")

    # -- a custom curve reshapes that uniform 0.5 back to full strength ------
    curve = settings.cavity_curve.curves[0]
    curve.points[0].location = (0.0, 1.0)
    curve.points[1].location = (1.0, 1.0)
    settings.cavity_curve.update()
    settings.use_automasking_custom_cavity_curve = True
    dz = stroke()
    curved = mask_of(dz, base, flat)
    if abs(curved - 1.0) > 0.02:
        _fail("a constant-1 custom curve should restore full strength, read {:.4f}".format(
            curved))
    print("PASS: the custom cavity curve reshapes the mask ({:.4f})".format(curved))

    # -- turning it off restores the unmasked stroke ------------------------
    settings.use_automasking_custom_cavity_curve = False
    settings.use_automasking_cavity = False
    dz = stroke()
    if abs(mask_of(dz, base, floor) - 1.0) > 1e-3:
        _fail("disabling cavity did not restore the unmasked stroke")
    print("PASS: disabling cavity restores the unmasked stroke")

    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    print("ALL PASS: cavity automasking verified.")


if __name__ == "__main__":
    main()
