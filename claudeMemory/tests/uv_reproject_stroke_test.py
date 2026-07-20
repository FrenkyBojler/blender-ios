# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
UV slide-reprojection through the Blender stroke path (P11 E2/A3). On a flat
jittered grid whose UV map is the identity (uv = vertex xy), a BSMOOTH stroke
slides vertices tangentially; with `Brush.reproject_uvs` the engine re-anchors
the UVs (uv tracks the new xy), without it they swim. Undo must restore the
UVs with the positions (the corner captures ride the meshlog step).

Run:
    set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
    blender --background --factory-startup --python claudeMemory/tests/uv_reproject_stroke_test.py
Exits nonzero on failure.
"""

import sys

import numpy as np
import bpy

def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def _make_jittered_grid(name, seed=7):
    """A 33x33 unit grid, xy-jittered so smoothing actually slides verts, with
    an identity UV map (uv = vertex xy)."""
    bpy.ops.mesh.primitive_grid_add(x_subdivisions=32, y_subdivisions=32, size=2.0)
    ob = bpy.context.active_object
    ob.name = name
    mesh = ob.data
    n = len(mesh.vertices)
    pos = np.empty(n * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", pos)
    pos = pos.reshape(-1, 3)
    rng = np.random.default_rng(seed)
    interior = (np.abs(pos[:, 0]) < 0.99) & (np.abs(pos[:, 1]) < 0.99)
    jitter = rng.uniform(-0.02, 0.02, size=(n, 2)).astype(np.float32)
    pos[interior, 0] += jitter[interior, 0]
    pos[interior, 1] += jitter[interior, 1]
    mesh.vertices.foreach_set("co", pos.reshape(-1))

    cv = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get("vertex_index", cv)
    uv = pos[cv][:, :2].reshape(-1).astype(np.float32)
    layer = mesh.uv_layers.active or mesh.uv_layers.new(name="UVMap")
    layer.data.foreach_set("uv", uv)
    mesh.update()
    return ob


def _uv_drift(ob):
    """max |uv - co.xy| over all loops (0 = UVs perfectly anchored)."""
    mesh = ob.data
    pos = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", pos)
    pos = pos.reshape(-1, 3)
    cv = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get("vertex_index", cv)
    uv = np.empty(len(mesh.loops) * 2, dtype=np.float32)
    mesh.uv_layers.active.data.foreach_get("uv", uv)
    d = uv.reshape(-1, 2) - pos[cv][:, :2]
    return float(np.sqrt((d * d).sum(axis=1)).max())


def _read_uvs(ob):
    uv = np.empty(len(ob.data.loops) * 2, dtype=np.float32)
    ob.data.uv_layers.active.data.foreach_get("uv", uv)
    return uv


def _smooth_stroke(name, reproject):
    import sculptcore_addon.engine as engine
    from sculptcore_addon import convert
    from sculptcore_addon import stroke as strokemod
    from sculptcore_addon import undo as undo_mod

    ob = bpy.data.objects[name]
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    bpy.ops.ed.undo_push(message="Base")
    session = engine.sessions[name]
    mgr = engine.manager()
    bsmooth = int(mgr.get("sculptcore::brush::SculptBrushes").items["BSMOOTH"])
    brush = strokemod._ensure_brush(session)
    brush.strength, brush.radius, brush.spacing = 1.0, 0.6, 0.1
    brush.reproject_uvs = reproject
    brush.writeProps()
    strokemod.stroke_begin(session)
    for i in range(10):
        strokemod.apply_dab(session, bsmooth, (-0.5 + i * 0.11, 0.0, 0.0), (0, 0, 1), 0.6)
    strokemod.stroke_end(session)
    session.uv_dirty = reproject
    convert.flush(ob)
    undo_mod.push(bpy.context, ob, session)
    return session


def main():
    if "sculptcore_addon" not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module="sculptcore_addon")
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()

    _make_jittered_grid("ReprojOn")
    _make_jittered_grid("ReprojOff")
    uv0 = _read_uvs(bpy.data.objects["ReprojOn"])

    _smooth_stroke("ReprojOn", reproject=True)
    drift_on = _uv_drift(bpy.data.objects["ReprojOn"])
    # Undo (still in the mode, session live) must restore the UVs exactly.
    bpy.ops.ed.undo()
    uv_undo = _read_uvs(bpy.data.objects["ReprojOn"])
    max_diff = float(np.abs(uv_undo - uv0).max())
    if max_diff > 1e-5:
        _fail("undo did not restore reprojected UVs (max diff {:.3e})".format(max_diff))
    print("PASS: undo restored the stroke's UV reprojection")
    bpy.ops.object.custom_mode_toggle()

    _smooth_stroke("ReprojOff", reproject=False)
    bpy.ops.object.custom_mode_toggle()
    drift_off = _uv_drift(bpy.data.objects["ReprojOff"])

    print("uv drift: reproject on {:.5f}, off {:.5f}".format(drift_on, drift_off))
    if drift_off < 1e-3:
        _fail("smooth stroke barely slid any UVs — test setup ineffective")
    if not (drift_on < drift_off * 0.1):
        _fail("reprojection did not anchor UVs ({:.5f} vs {:.5f})".format(
            drift_on, drift_off))
    print("ALL PASS: stroke UV reprojection (anchored UVs + exact undo).")


if __name__ == "__main__":
    main()
