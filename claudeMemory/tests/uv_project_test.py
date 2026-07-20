# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
UV projection from seams in the mode (P11 A2/A3/A4): the
`sculptcore.uv_project_from_seams` operator must generate packed per-corner
UVs from the migrated seam edges, write them back to the active UV map,
rebuild the derived UV-chart classification (constraints see the new charts),
and undo/redo through the attribute-snapshot step.

Run:
    set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
    blender --background --factory-startup --python claudeMemory/tests/uv_project_test.py
Exits nonzero on failure.
"""

import ctypes
import sys

import numpy as np
import bpy

OBJ = "UVProject"


def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def _uvs(mesh):
    layer = mesh.uv_layers.active
    if layer is None:
        return None
    out = np.empty(len(mesh.loops) * 2, dtype=np.float32)
    layer.data.foreach_get("uv", out)
    return out


def main():
    import sculptcore_addon.engine as engine

    if "sculptcore_addon" not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module="sculptcore_addon")

    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()
    bpy.ops.mesh.primitive_uv_sphere_add(segments=16, ring_count=8, radius=1.0)
    ob = bpy.context.active_object
    ob.name = OBJ
    mesh = ob.data

    # Seam the equator ring -> two charts (north / south hemispheres).
    ev = np.empty(len(mesh.edges) * 2, dtype=np.int32)
    mesh.edges.foreach_get("vertices", ev)
    ev = ev.reshape(-1, 2)
    pos = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", pos)
    z = pos.reshape(-1, 3)[:, 2]
    seam = (np.abs(z[ev[:, 0]]) < 0.05) & (np.abs(z[ev[:, 1]]) < 0.05)
    if not seam.any():
        _fail("setup: no equator edges")
    mesh.attributes.new("uv_seam", 'BOOLEAN', 'EDGE').data.foreach_set("value", seam)

    uv0 = _uvs(mesh)
    if uv0 is None:
        _fail("setup: sphere has no UV map")

    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    bpy.ops.ed.undo_push(message="Base")
    session = engine.sessions[OBJ]
    lib = engine.capi().lib

    result = bpy.ops.sculptcore.uv_project_from_seams('EXEC_DEFAULT', margin=0.01)
    if 'FINISHED' not in result:
        _fail("operator did not finish: {}".format(result))
    if not session.uv_dirty:
        _fail("operator did not mark the session uv_dirty")

    mesh = bpy.data.objects[OBJ].data
    uv1 = _uvs(mesh)
    if float(np.abs(uv1 - uv0).max()) < 1e-4:
        _fail("active UV map unchanged after projection")
    if uv1.min() < -1e-4 or uv1.max() > 1.0001:
        _fail("projected UVs outside [0,1] ({} .. {})".format(uv1.min(), uv1.max()))
    print("PASS: operator projected + packed UVs into the active map")

    # The derived classification must now carry BC_UVCHART (16) on the seam
    # ring verts (chart boundary), recomputed by the C API after generation.
    vclass = np.zeros(len(mesh.vertices), dtype=np.int32)
    ok = lib.Mesh_readAttr(session.mesh_ptr, 1, b".boundary.vert.class", 32,
                           vclass.ctypes.data_as(ctypes.c_void_p))
    seam_verts = np.unique(ev[seam])
    if not ok or not np.all(vclass[seam_verts] & 16):
        _fail("BC_UVCHART missing on chart-boundary verts after projection")
    print("PASS: UV-chart classification rebuilt (constraints see the charts)")

    # Undo restores the original UVs; redo restores the projection.
    bpy.ops.ed.undo()
    mesh = bpy.data.objects[OBJ].data
    uv_undo = _uvs(mesh)
    if float(np.abs(uv_undo - uv0).max()) > 1e-5:
        _fail("undo did not restore the original UVs (max diff {:.3e})".format(
            float(np.abs(uv_undo - uv0).max())))
    bpy.ops.ed.redo()
    mesh = bpy.data.objects[OBJ].data
    uv_redo = _uvs(mesh)
    if float(np.abs(uv_redo - uv1).max()) > 1e-5:
        _fail("redo did not restore the projected UVs (max diff {:.3e})".format(
            float(np.abs(uv_redo - uv1).max())))
    print("PASS: undo/redo round-trips the projection")

    # Exit writes the projected UVs into the persistent Mesh.
    bpy.ops.object.custom_mode_toggle()
    mesh = bpy.data.objects[OBJ].data
    uv_exit = _uvs(mesh)
    if float(np.abs(uv_exit - uv1).max()) > 1e-5:
        _fail("exit lost the projected UVs")
    print("PASS: exit persisted the projected UVs to the Mesh")

    print("ALL PASS: UV projection operator (project/classify/undo/redo/exit).")


if __name__ == "__main__":
    main()
