"""P8 A3 + export round-trip: SculptCore-vertex <-> Blender-subdiv-vertex map
via base-position nearest-neighbour, then bake SculptCore's displaced surface
through the dedup vertcos seam (B2) and confirm it lands tear-free.

Both engines dedup the same subdivided cage to equal vertex counts. Match by
undisplaced position (SculptCore discrete base vs Blender base; gap O(4^-L)),
transfer SculptCore's displaced positions into Blender subdiv-vertex order, bake
(multires_reshape_from_vert_positions), re-evaluate. Validate the map is
bijective and the baked surface's max edge matches the intended (no tear from a
mis-paired vertex).

Run: blender --factory-startup --python p8_export.py  -> %TEMP%/p8_export.txt
"""
import bpy, os, tempfile, ctypes
import numpy as np

OUT = os.path.join(tempfile.gettempdir(), "p8_export.txt")


def _decl(lib):
    f32p = np.ctypeslib.ndpointer(dtype=np.float32, flags="C_CONTIGUOUS")
    i32p = np.ctypeslib.ndpointer(dtype=np.int32, flags="C_CONTIGUOUS")
    cip = ctypes.POINTER(ctypes.c_int)
    lib.Mesh_fromArrays.argtypes = [f32p, ctypes.c_int, i32p, ctypes.c_int, i32p, ctypes.c_int]
    lib.Mesh_fromArrays.restype = ctypes.c_void_p
    lib.Multires_new.argtypes = [ctypes.c_void_p] + [ctypes.c_int] * 4
    lib.Multires_new.restype = ctypes.c_void_p
    lib.Multires_setActiveLevel.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.Multires_setActiveLevel.restype = ctypes.c_int
    lib.Multires_activeMesh.argtypes = [ctypes.c_void_p]
    lib.Multires_activeMesh.restype = ctypes.c_void_p
    lib.Mesh_arraySizes.argtypes = [ctypes.c_void_p] + [cip] * 4
    lib.Mesh_toArrays.argtypes = [ctypes.c_void_p, f32p, i32p, i32p, i32p]


def _sc_level_verts(lib, arrays, level):
    positions, corner_verts, face_offsets = arrays
    cage = lib.Mesh_fromArrays(positions, len(positions) // 3,
                               corner_verts, len(corner_verts),
                               face_offsets, len(face_offsets) - 1)
    mr = lib.Multires_new(cage, level, 0, 0, 0)
    lib.Multires_setActiveLevel(mr, level)
    m = lib.Multires_activeMesh(mr)
    nv, nc, nf, dom = (ctypes.c_int() for _ in range(4))
    lib.Mesh_arraySizes(m, ctypes.byref(nv), ctypes.byref(nc), ctypes.byref(nf), ctypes.byref(dom))
    pos = np.empty(nv.value * 3, dtype=np.float32)
    cvv = np.empty(nc.value, dtype=np.int32)
    fo = np.empty(nf.value + 1, dtype=np.int32)
    vm = np.empty(dom.value, dtype=np.int32)
    lib.Mesh_toArrays(m, pos, cvv, fo, vm)
    return pos.reshape(-1, 3).astype(np.float64)


def _eval_verts(mesh):
    co = np.empty(len(mesh.vertices) * 3, dtype=np.float64)
    mesh.vertices.foreach_get("co", co)
    return co.reshape(-1, 3)


def _max_edge(mesh, co):
    cv = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get("vertex_index", cv)
    q = cv.reshape(-1, 4)
    return float(np.linalg.norm(co[q] - co[q[:, [1, 2, 3, 0]]], axis=2).max())


def _nn(a, b):
    """For each row of a, index of nearest row in b + that distance."""
    idx = np.empty(len(a), dtype=np.int64)
    dist = np.empty(len(a), dtype=np.float64)
    for i in range(0, len(a), 256):
        blk = a[i:i + 256]
        d = np.linalg.norm(blk[:, None, :] - b[None, :, :], axis=2)
        idx[i:i + 256] = d.argmin(axis=1)
        dist[i:i + 256] = d.min(axis=1)
    return idx, dist


def main():
    lines = ["P8 A3 map + export round-trip (SculptCore surface -> MDISPS bake)", ""]
    try:
        import sculptcore_addon.engine as engine
        if "sculptcore_addon" not in bpy.context.preferences.addons:
            bpy.ops.preferences.addon_enable(module="sculptcore_addon")
        lib = engine.capi().lib
        _decl(lib)

        for level in (2, 3, 4):
            bpy.ops.object.select_all(action='SELECT'); bpy.ops.object.delete()
            bpy.ops.mesh.primitive_cube_add(size=2.0)
            ob = bpy.context.active_object
            m = ob.data
            positions = np.empty(len(m.vertices) * 3, dtype=np.float32)
            m.vertices.foreach_get("co", positions)
            cvv = np.empty(len(m.loops), dtype=np.int32)
            m.loops.foreach_get("vertex_index", cvv)
            fo = np.empty(len(m.polygons) + 1, dtype=np.int32)
            m.polygons.foreach_get("loop_start", fo[:len(m.polygons)])
            fo[len(m.polygons)] = len(m.loops)
            arrays = (positions, cvv, fo)

            sc_base = _sc_level_verts(lib, arrays, level)
            # SculptCore's intended surface: displace its base per vertex.
            sc_disp = sc_base.copy()
            sc_disp[:, 2] += 0.3 * np.sin(2.0 * sc_base[:, 0]) * np.cos(3.0 * sc_base[:, 1])

            ob.modifiers.new("Multires", 'MULTIRES')
            for _ in range(level):
                bpy.ops.object.multires_subdivide(modifier="Multires")
            depsgraph = bpy.context.evaluated_depsgraph_get(); depsgraph.update()
            bl_base = _eval_verts(ob.evaluated_get(depsgraph).data)

            if len(bl_base) != len(sc_base):
                lines.append("level {:d}: COUNT MISMATCH sc={:d} bl={:d}".format(
                    level, len(sc_base), len(bl_base)))
                continue

            # Blender subdiv-vertex i -> nearest SculptCore vertex (by base pos).
            idx, dist = _nn(bl_base, sc_base)
            bijective = len(np.unique(idx)) == len(sc_base)
            vertcos = sc_disp[idx]

            flat = np.ascontiguousarray(vertcos.reshape(-1), dtype=np.float32)
            ob.multires_reshape_from_vert_positions(depsgraph, flat)
            ob.data.update_tag(); ob.update_tag(); depsgraph.update()
            em = ob.evaluated_get(depsgraph).data
            got = _eval_verts(em)
            intended_edge = _max_edge(em, vertcos)
            got_edge = _max_edge(em, got)
            ok = bijective and abs(got_edge - intended_edge) < 1e-3
            lines.append(
                "level {:d}: verts={:d} bijective={} match_gap={:.2e}  "
                "intended_edge={:.3e} baked_edge={:.3e}  {:s}".format(
                    level, len(sc_base), bijective, float(dist.max()),
                    intended_edge, got_edge, "PASS" if ok else "FAIL"))
    except Exception as ex:
        import traceback
        lines.append("ERROR: {!r}".format(ex))
        lines.append(traceback.format_exc())
    with open(OUT, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))


bpy.app.timers.register(lambda: (main(), None)[1], first_interval=1.0)
