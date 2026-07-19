# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Brush-texture verification (brush-mapping Phase 2).

A BLEND (linear-gradient) texture mapped '3D' (engine Global space) must
modulate a DRAW stroke across a grid plane: the bright half displaces, the
dark half stays put. Also checks the bake cache (hit, then explicit and
depsgraph-shaped invalidation), that an unmapped map mode ('RANDOM') clears
the engine texture, and that a texture-less brush strokes uniformly.

Run:
    set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
    blender --factory-startup --python claudeMemory/scripts/run_sync.py -- \
        claudeMemory/tests/brush_texture_test.py
Exits nonzero on failure.
"""

import sys

import numpy as np
import bpy

OBJ = "TexPlane"


def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def positions(ob):
    mesh = ob.data
    a = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", a)
    return a.reshape(-1, 3)


def band_mean_dz(before, after, lo, hi, axis=0):
    """Mean |dz| of verts whose rest coordinate on `axis` lies in [lo, hi]."""
    sel = (before[:, axis] >= lo) & (before[:, axis] <= hi)
    if not sel.any():
        _fail("empty band [{:f}, {:f}] on axis {:d}".format(lo, hi, axis))
    return float(np.abs(after[sel, 2] - before[sel, 2]).mean())


def main():
    if "sculptcore_addon" not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module="sculptcore_addon")
    import sculptcore_addon.engine as engine
    from sculptcore_addon import convert, mapping, texture
    from sculptcore_addon import stroke as strokemod

    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()
    bpy.ops.mesh.primitive_grid_add(x_subdivisions=64, y_subdivisions=64, size=2.0)
    ob = bpy.context.active_object
    ob.name = OBJ
    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    session = engine.sessions[OBJ]
    mgr = engine.manager()

    tex = bpy.data.textures.new("TexGrad", type='BLEND')
    tex.progression = 'LINEAR'  # tin = (x + 1) / 2 over evaluate x in [-1, 1]
    bl_brush = bpy.data.brushes.new("scp_tex", mode='SCULPT')
    bl_brush.sculpt_brush_type = 'DRAW'
    bl_brush.strength = 1.0
    bl_brush.texture_slot.map_mode = '3D'

    sc_brush = strokemod._ensure_brush(session)
    mapping.apply_brush(bl_brush, None, sc_brush, world_radius=0.9, invert=False)
    kernel = mapping.kernel_enum(mgr, bl_brush)

    # -- control first, on the pristine plane: no texture -> symmetric ------
    texture.apply_texture(bl_brush, sc_brush)
    if int(sc_brush.tex_width) != 0:
        _fail("texture-less brush left an engine texture bound")
    before = positions(ob).copy()
    strokemod.stroke_begin(session, has_dyntopo=False)
    for _ in range(4):
        strokemod.apply_dab(session, kernel, (0.0, 0.0, 0.0), (0.0, 0.0, 1.0), 0.9)
    strokemod.stroke_end(session)
    convert.flush(ob)
    after = positions(ob)
    bright = band_mean_dz(before, after, 0.3, 0.5)
    dark = band_mean_dz(before, after, -0.5, -0.3)
    if not (bright > 1e-4 and dark > 1e-4
            and abs(bright - dark) < 0.3 * max(bright, dark)):
        _fail("untextured stroke is asymmetric (bright {:.6f}, dark {:.6f})".format(
            bright, dark))
    print("PASS: untextured control strokes symmetrically")

    # Revert so the textured stroke also starts from the pristine surface (a
    # deformed right half would sit off the dab plane and skew its falloff).
    session.meshlog.undo(session.mesh(), session.tree())
    session.meshlog_cursor = max(0, session.meshlog_cursor - 1)
    convert.flush(ob)

    bl_brush.texture = tex
    texture.apply_texture(bl_brush, sc_brush)
    if int(sc_brush.tex_width) != texture.BAKE_SIZE or int(sc_brush.tex_height) != texture.BAKE_SIZE:
        _fail("engine texture dims not set ({} x {})".format(
            int(sc_brush.tex_width), int(sc_brush.tex_height)))
    if int(sc_brush.coord_space) != 0:
        _fail("'3D' map mode did not select Global coord space")
    print("PASS: texture bound ({0}x{0}, Global)".format(texture.BAKE_SIZE))

    # -- gradient modulates the stroke --------------------------------------
    # Engine Global UV = world XY, so uv.x = world x; the bake spans evaluate
    # [-1, 1] across uv [0, 1]: world x <= 0 clamps to intensity 0, x = 1
    # reads intensity 1. Symmetric bands around the dab center share the same
    # falloff, so any asymmetry is the texture's.
    before = positions(ob).copy()
    strokemod.stroke_begin(session, has_dyntopo=False)
    for _ in range(4):
        strokemod.apply_dab(session, kernel, (0.0, 0.0, 0.0), (0.0, 0.0, 1.0), 0.9)
    strokemod.stroke_end(session)
    convert.flush(ob)
    after = positions(ob)
    if not np.isfinite(after).all():
        _fail("non-finite positions after textured stroke")
    bright = band_mean_dz(before, after, 0.3, 0.5)
    dark = band_mean_dz(before, after, -0.5, -0.3)
    if not (bright > 1e-4 and bright > 10.0 * max(dark, 1e-9)):
        _fail("texture did not modulate (bright {:.6f}, dark {:.6f})".format(
            bright, dark))
    print("PASS: gradient modulates the stroke (bright {:.5f}, dark {:.6f})".format(
        bright, dark))

    # -- View Plane maps to the brush-centered Projected space --------------
    # Basis at surfaceNo +Z: the |n.z| >= 0.999 branch picks ref (1,0,0), so
    # t1 = ref x n = -Y and uv.x = rel.t1 / (2r) + 0.5 — the bake's x gradient
    # runs along world -y, bright toward -y. The dab column is the same as
    # before, so any y asymmetry is the texture's.
    session.meshlog.undo(session.mesh(), session.tree())
    session.meshlog_cursor = max(0, session.meshlog_cursor - 1)
    convert.flush(ob)
    bl_brush.texture_slot.map_mode = 'VIEW_PLANE'
    texture.apply_texture(bl_brush, sc_brush)
    if int(sc_brush.coord_space) != 4:
        _fail("'VIEW_PLANE' should select the Projected coord space")
    before = positions(ob).copy()
    strokemod.stroke_begin(session, has_dyntopo=False)
    for _ in range(4):
        strokemod.apply_dab(session, kernel, (0.0, 0.0, 0.0), (0.0, 0.0, 1.0), 0.9)
    strokemod.stroke_end(session)
    convert.flush(ob)
    after = positions(ob)
    if not np.isfinite(after).all():
        _fail("non-finite positions after Projected stroke")
    # The tile spans the brush diameter, so the +-0.4 bands read gradient
    # intensities ~0.72 vs ~0.28 (ratio ~2.6, not clamped-to-zero like the
    # Global case) — assert the direction and a conservative ratio.
    bright = band_mean_dz(before, after, -0.5, -0.3, axis=1)
    dark = band_mean_dz(before, after, 0.3, 0.5, axis=1)
    if not (bright > 1e-4 and bright > 1.8 * max(dark, 1e-9)):
        _fail("Projected mapping did not modulate across the brush "
              "(bright {:.6f}, dark {:.6f})".format(bright, dark))
    print("PASS: View Plane -> Projected modulates across the brush "
          "(bright {:.5f}, dark {:.6f})".format(bright, dark))

    # -- clearing the Blender texture clears the engine texture -------------
    bl_brush.texture = None
    texture.apply_texture(bl_brush, sc_brush)
    if int(sc_brush.tex_width) != 0:
        _fail("clearing the Blender texture left the engine texture bound")
    print("PASS: texture-less apply clears the engine texture")

    # -- bake cache: hit, then invalidation ---------------------------------
    bl_brush.texture = tex
    a = texture._bake(tex)
    b = texture._bake(tex)
    if a is not b:
        _fail("second bake missed the cache")
    texture.invalidate(tex.name)
    c = texture._bake(tex)
    if c is a:
        _fail("invalidate() did not drop the bake")

    class _FakeUpdate:
        def __init__(self, id_):
            self.id = id_

    class _FakeDepsgraph:
        updates = [_FakeUpdate(tex)]

    texture.invalidate_from_depsgraph(_FakeDepsgraph())
    if texture._bake(tex) is c:
        _fail("depsgraph invalidation did not drop the bake")
    print("PASS: bake cache hits and invalidates")

    # -- unmapped map mode clears -------------------------------------------
    bl_brush.texture_slot.map_mode = 'RANDOM'
    texture.apply_texture(bl_brush, sc_brush)
    if int(sc_brush.tex_width) != 0:
        _fail("'RANDOM' map mode should clear the engine texture")
    print("PASS: unmapped map mode clears the engine texture")

    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    print("ALL PASS: brush-texture wiring verified.")


if __name__ == "__main__":
    main()
