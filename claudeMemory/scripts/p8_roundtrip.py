"""P8 A1 round-trip: seed a multires level from displaced positions, dump back.

Builds a cube multires, dumps its level-L grid samples (A2), pushes them onto a
sphere (a non-trivial displacement with consistent seam replicas — the push is a
pure function of position), seeds a fresh multires from those positions (A1 =
Multires_fromLevelPositions), dumps again (A2), and checks the two match. This
gates writeback losslessness: A1(A2 dump) must reproduce the input surface.

Run: blender --factory-startup --python p8_roundtrip.py
Writes %TEMP%/p8_roundtrip.txt.
"""
import bpy, os, tempfile, ctypes
import numpy as np

OUT = os.path.join(tempfile.gettempdir(), "p8_roundtrip.txt")
LEVELS = [1, 2, 3, 4]


def _decl(lib):
    f32p = np.ctypeslib.ndpointer(dtype=np.float32, flags="C_CONTIGUOUS")
    i32p = np.ctypeslib.ndpointer(dtype=np.int32, flags="C_CONTIGUOUS")
    lib.Mesh_fromArrays.argtypes = [f32p, ctypes.c_int, i32p, ctypes.c_int, i32p, ctypes.c_int]
    lib.Mesh_fromArrays.restype = ctypes.c_void_p
    lib.freeMesh.argtypes = [ctypes.c_void_p]
    lib.Multires_new.argtypes = [ctypes.c_void_p] + [ctypes.c_int] * 4
    lib.Multires_new.restype = ctypes.c_void_p
    lib.Multires_free.argtypes = [ctypes.c_void_p]
    lib.Multires_levelSampleCount.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.Multires_levelSampleCount.restype = ctypes.c_int
    lib.Multires_levelPositionsOut.argtypes = [ctypes.c_void_p, ctypes.c_int, f32p]
    lib.Multires_levelPositionsOut.restype = ctypes.c_int
    lib.Multires_fromLevelPositions.argtypes = [ctypes.c_void_p, ctypes.c_int, f32p, ctypes.c_int]
    lib.Multires_fromLevelPositions.restype = ctypes.c_int


def _cube_arrays():
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()
    bpy.ops.mesh.primitive_cube_add(size=2.0)
    mesh = bpy.context.active_object.data
    positions = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", positions)
    corner_verts = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get("vertex_index", corner_verts)
    face_offsets = np.empty(len(mesh.polygons) + 1, dtype=np.int32)
    mesh.polygons.foreach_get("loop_start", face_offsets[:len(mesh.polygons)])
    face_offsets[len(mesh.polygons)] = len(mesh.loops)
    return positions, corner_verts, face_offsets


def _new_cube_mr(lib, arrays, level):
    positions, corner_verts, face_offsets = arrays
    cage = lib.Mesh_fromArrays(positions, len(positions) // 3,
                               corner_verts, len(corner_verts),
                               face_offsets, len(face_offsets) - 1)
    mr = lib.Multires_new(cage, level, 0, 0, 0)
    return mr, cage


def _dump(lib, mr, level):
    n = lib.Multires_levelSampleCount(mr, level)
    out = np.empty(n * 3, dtype=np.float32)
    lib.Multires_levelPositionsOut(mr, level, out)
    return out.reshape(-1, 3).astype(np.float64)


def main():
    lines = ["P8 A1 seed/writeback round-trip (displaced positions)", ""]
    try:
        import sculptcore_addon.engine as engine
        if "sculptcore_addon" not in bpy.context.preferences.addons:
            bpy.ops.preferences.addon_enable(module="sculptcore_addon")
        lib = engine.capi().lib
        _decl(lib)

        all_ok = True
        for level in LEVELS:
            arrays = _cube_arrays()
            # Base surface at this level.
            mr0, cage0 = _new_cube_mr(lib, arrays, level)
            base = _dump(lib, mr0, level)
            lib.Multires_free(mr0)
            lib.freeMesh(cage0)

            # Displace: push every sample onto the unit sphere (pure function of
            # position, so seam replicas stay consistent).
            norms = np.linalg.norm(base, axis=1, keepdims=True)
            norms[norms == 0] = 1.0
            target = (base / norms).astype(np.float32)

            # Seed a fresh multires from the displaced positions, dump back.
            mr1, cage1 = _new_cube_mr(lib, arrays, level)
            flat = np.ascontiguousarray(target.reshape(-1), dtype=np.float32)
            changed = lib.Multires_fromLevelPositions(mr1, level, flat, len(target))
            got = _dump(lib, mr1, level)
            lib.Multires_free(mr1)
            lib.freeMesh(cage1)

            err = float(np.abs(got - target.astype(np.float64)).max())
            ok = err < 1e-5
            all_ok = all_ok and ok
            lines.append("level {:d}: samples={:d} changed={:d} max_err={:.3e}  {:s}".format(
                level, len(target), changed, err, "PASS" if ok else "FAIL"))

        lines.append("")
        lines.append("RESULT: {:s}".format("ALL PASS" if all_ok else "FAILURES"))
    except Exception as ex:
        import traceback
        lines.append("ERROR: {!r}".format(ex))
        lines.append(traceback.format_exc())
    with open(OUT, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))


bpy.app.timers.register(lambda: (main(), None)[1], first_interval=1.0)
