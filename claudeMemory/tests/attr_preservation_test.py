# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
User-attribute preservation through the topology-rebuild path (mesh-convert v2:
"restore all user-created attributes across a topology change / undo").

The topology-rebuild flush drops all Blender customdata (clear_geometry). This
test seeds a mesh with a UV map, a custom POINT FLOAT attribute, and a custom
FACE INT attribute, drives a dyntopo stroke (forcing the rebuild path), and
checks the layers survive; then undoes back to the original topology and checks
every layer returns to its exact original values (the meshlog reverts the engine
copies, which the bridge writes back).

Run:
    set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
    blender --background --factory-startup --python claudeMemory/tests/attr_preservation_test.py
Exits nonzero on failure.
"""

import sys

import numpy as np
import bpy

OBJ = "AttrUndo"
UV_NAME = "UVMap"
VFLOAT = "myfloat"
FINT = "myint"


def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def _read(mesh, name, prop, dtype, ncomp):
    attr = mesh.attributes.get(name)
    if attr is None:
        return None
    out = np.empty(len(attr.data) * ncomp, dtype=dtype)
    attr.data.foreach_get(prop, out)
    return out


def main():
    import sculptcore_addon.engine as engine
    from sculptcore_addon import stroke as strokemod
    from sculptcore_addon import undo as undo_mod
    from sculptcore_addon import convert

    if "sculptcore_addon" not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module="sculptcore_addon")

    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()
    bpy.ops.mesh.primitive_uv_sphere_add(segments=16, ring_count=8, radius=1.0)
    ob = bpy.context.active_object
    ob.name = OBJ
    mesh = ob.data
    context = bpy.context

    # Seed a custom POINT FLOAT and a custom FACE INT layer (the UV map already
    # exists on a UV sphere). Distinct per-element values so a drop or a wrong
    # remap is caught.
    vfloat = mesh.attributes.new(VFLOAT, 'FLOAT', 'POINT')
    vf_vals = np.arange(len(mesh.vertices), dtype=np.float32) * 0.5 + 1.0
    vfloat.data.foreach_set("value", vf_vals)
    fint = mesh.attributes.new(FINT, 'INT', 'FACE')
    fi_vals = (np.arange(len(mesh.polygons), dtype=np.int32) + 7)
    fint.data.foreach_set("value", fi_vals)

    v0 = (len(mesh.vertices), len(mesh.polygons))
    uv0 = _read(mesh, UV_NAME, "vector", np.float32, 2)
    if uv0 is None:
        _fail("test setup: no active UV map on the sphere")
    vf0 = _read(mesh, VFLOAT, "value", np.float32, 1)
    fi0 = _read(mesh, FINT, "value", np.int32, 1)

    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    bpy.ops.ed.undo_push(message="Base")
    session = engine.sessions[OBJ]
    mgr = engine.manager()
    draw = int(mgr.get("sculptcore::brush::SculptBrushes").items["DRAW"])

    brush = strokemod._ensure_brush(session)
    brush.strength, brush.radius, brush.spacing = 0.5, 0.5, 0.1
    brush.writeProps()

    center, normal, _ = strokemod.raycast(session, (0, 0, 5), (0, 0, -1))
    prog = strokemod.build_program(session, draw)
    params = strokemod.build_dyntopo_params(session, 0.05, 0.02)

    # One dyntopo stroke → topology-rebuild flush (clear_geometry + recreate).
    strokemod.stroke_begin(session, has_dyntopo=True)
    for i in range(12):
        c = (center[0] + (i % 4) * 0.02, center[1] + (i // 4) * 0.02, center[2])
        strokemod.apply_dyntopo_dab(session, prog, c, normal, 0.5, params, 1000 + i)
    strokemod.stroke_end(session)
    convert.flush(ob)
    undo_mod.push(context, ob, session)

    mesh = bpy.data.objects[OBJ].data
    v1 = (len(mesh.vertices), len(mesh.polygons))
    if not (v1[0] > v0[0]):
        _fail("dyntopo stroke did not remesh ({} -> {})".format(v0, v1))

    # Post-stroke: the layers must still exist (not dropped by clear_geometry),
    # sized to the new topology.
    for name, ncomp, prop, dtype in ((UV_NAME, 2, "vector", np.float32),
                                     (VFLOAT, 1, "value", np.float32),
                                     (FINT, 1, "value", np.int32)):
        got = _read(mesh, name, prop, dtype, ncomp)
        if got is None:
            _fail("attribute {!r} was dropped on the dyntopo rebuild".format(name))
    print("PASS: UV + custom attributes survived the dyntopo rebuild {} -> {}".format(v0, v1))

    # Undo → original topology and exact original attribute values.
    bpy.ops.ed.undo()
    mesh = bpy.data.objects[OBJ].data
    v_undo = (len(mesh.vertices), len(mesh.polygons))
    if v_undo != v0:
        _fail("undo did not restore original topology ({} != {})".format(v_undo, v0))

    uv1 = _read(mesh, UV_NAME, "vector", np.float32, 2)
    vf1 = _read(mesh, VFLOAT, "value", np.float32, 1)
    fi1 = _read(mesh, FINT, "value", np.int32, 1)
    if uv1 is None or vf1 is None or fi1 is None:
        _fail("undo dropped a bridged attribute (uv={}, vfloat={}, fint={})".format(
            uv1 is not None, vf1 is not None, fi1 is not None))
    uvd = float(np.abs(uv1 - uv0).max())
    vfd = float(np.abs(vf1 - vf0).max())
    if uvd > 1e-5:
        _fail("UV map not restored exactly on undo (max diff {:.3e})".format(uvd))
    if vfd > 1e-5:
        _fail("custom float attribute not restored on undo (max diff {:.3e})".format(vfd))
    if not np.array_equal(fi1, fi0):
        _fail("custom int face attribute not restored on undo")
    print("PASS: undo restored UV + custom attributes to original values exactly")

    print("ALL PASS: user attributes preserved through topology rebuild + undo.")


if __name__ == "__main__":
    main()
