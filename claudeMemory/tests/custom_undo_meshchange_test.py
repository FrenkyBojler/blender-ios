# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Mesh-changing foreign step guard (undo-integration P6 §4.1 corner): a foreign
memfile undo can change the mesh topology under a custom-undo mode, which skips
the generic refresh. `convert.resync_if_diverged` rebuilds the session when the
engine's vertex count no longer matches the Mesh, so a following stroke sculpts
the correct geometry instead of corrupting a stale engine mesh.

This simulates the divergence directly (a bmesh delete standing in for the
post-foreign-undo mesh) and asserts the guard rebuilds and a stroke then works.

Run:
    set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
    blender --background --factory-startup --python claudeMemory/tests/custom_undo_meshchange_test.py
"""

import sys

import numpy as np
import bpy
import bmesh

OBJ = "MeshChange"


def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def main():
    import sculptcore_addon.engine as engine
    from sculptcore_addon import stroke as strokemod
    from sculptcore_addon import undo as undo_mod
    from sculptcore_addon import convert

    if "sculptcore_addon" not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module="sculptcore_addon")

    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()
    bpy.ops.mesh.primitive_uv_sphere_add(segments=24, ring_count=12, radius=1.0)
    ob = bpy.context.active_object
    ob.name = OBJ
    context = bpy.context

    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    bpy.ops.ed.undo_push(message="Base")
    session = engine.sessions[OBJ]
    mgr = engine.manager()
    draw = int(mgr.get("sculptcore::brush::SculptBrushes").items["DRAW"])
    brush = strokemod._ensure_brush(session)
    brush.strength, brush.radius = 0.5, 0.5
    brush.writeProps()

    # One normal stroke.
    strokemod.stroke_begin(session, has_dyntopo=False)
    for _ in range(4):
        strokemod.apply_dab(session, draw, (0.0, 0.0, 1.0), (0.0, 0.0, 1.0), 0.6)
    strokemod.stroke_end(session)
    convert.flush(ob)
    undo_mod.push(context, ob, session)
    gen0 = session.generation
    v_before = len(ob.data.vertices)

    # Simulate a foreign topology change under us: delete some vertices from the
    # Blender Mesh directly (stands in for undoing into a different-mesh memfile
    # step). The engine session still holds the old topology.
    bm = bmesh.new()
    bm.from_mesh(ob.data)
    doomed = list(bm.verts)[:12]
    bmesh.ops.delete(bm, geom=doomed, context='VERTS')
    bm.to_mesh(ob.data)
    bm.free()
    ob.data.update()
    v_after = len(ob.data.vertices)
    if v_after >= v_before:
        _fail("bmesh delete did not reduce vertex count")

    # The guard must notice the divergence and rebuild the session.
    rebuilt = convert.resync_if_diverged(ob)
    if not rebuilt:
        _fail("resync_if_diverged did not rebuild on vertex-count mismatch")
    session = engine.sessions[OBJ]
    if session.generation <= gen0:
        _fail("rebuilt session did not bump generation")
    if session.verts_num != v_after:
        _fail("rebuilt session vert count {} != Mesh {}".format(session.verts_num, v_after))
    print("PASS: guard rebuilt session to the changed mesh "
          "({} -> {} verts, gen {} -> {})".format(v_before, v_after, gen0, session.generation))

    # A stroke on the rebuilt session must work (no crash / no stale-mesh
    # corruption). Cast at the equator, intact after the pole delete.
    hit = strokemod.raycast(session, (5, 0, 0), (-1, 0, 0))
    if hit is None:
        _fail("raycast missed the rebuilt mesh")
    center, normal, _ = hit
    # The rebuild constructed a fresh engine brush; publish props to it the
    # way the stroke operator does (apply_brush -> writeProps) each invoke.
    brush = strokemod._ensure_brush(session)
    brush.strength, brush.radius = 0.5, 0.5
    brush.writeProps()
    before = np.empty(v_after * 3, dtype=np.float32)
    ob.data.vertices.foreach_get("co", before)
    strokemod.stroke_begin(session, has_dyntopo=False)
    for _ in range(4):
        strokemod.apply_dab(session, draw, center, normal, 0.6)
    strokemod.stroke_end(session)
    convert.flush(ob)
    undo_mod.push(context, ob, session)
    after = np.empty(v_after * 3, dtype=np.float32)
    ob.data.vertices.foreach_get("co", after)
    if not (np.isfinite(after).all() and float(np.abs(after - before).max()) > 1e-6):
        _fail("stroke on rebuilt session did not change the mesh")
    print("PASS: stroke on the rebuilt session works")

    # And its delta undo works.
    bpy.ops.ed.undo()
    undone = np.empty(v_after * 3, dtype=np.float32)
    ob.data.vertices.foreach_get("co", undone)
    if float(np.abs(undone - before).max()) > 1e-6:
        _fail("undo on rebuilt session not exact")
    print("PASS: delta undo on the rebuilt session is exact")

    print("ALL PASS: mesh-change divergence guard verified.")


if __name__ == "__main__":
    main()
