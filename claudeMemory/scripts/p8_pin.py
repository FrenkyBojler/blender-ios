"""P8 B: pin the MDISPS<->SculptCore grid convention via the bake round-trip.

SculptCore emits its level-L cube grid samples in a KNOWN order
(grid-major, v*w+u, +u = corner edge, +v = prev-corner edge). We feed them into
Blender's multires bake (Object.multires_reshape_from_positions, per-grid
[grid][y*size+x] order) under each candidate (grid corner parity) x (intra-grid
transpose) convention, evaluate the multires, and measure the eval mesh's max
edge length. The correct convention reproduces a tear-free surface (small max
edge); wrong ones misassign samples and tear at grid seams (large max edge).

Run: blender --factory-startup --python p8_pin.py  -> %TEMP%/p8_pin.txt
"""
import bpy, os, tempfile, ctypes
import numpy as np

OUT = os.path.join(tempfile.gettempdir(), "p8_pin.txt")
LEVEL = 3


def _decl(lib):
    f32p = np.ctypeslib.ndpointer(dtype=np.float32, flags="C_CONTIGUOUS")
    i32p = np.ctypeslib.ndpointer(dtype=np.int32, flags="C_CONTIGUOUS")
    lib.Mesh_fromArrays.argtypes = [f32p, ctypes.c_int, i32p, ctypes.c_int, i32p, ctypes.c_int]
    lib.Mesh_fromArrays.restype = ctypes.c_void_p
    lib.Multires_new.argtypes = [ctypes.c_void_p] + [ctypes.c_int] * 4
    lib.Multires_new.restype = ctypes.c_void_p
    lib.Multires_levelSampleCount.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.Multires_levelSampleCount.restype = ctypes.c_int
    lib.Multires_levelPositionsOut.argtypes = [ctypes.c_void_p, ctypes.c_int, f32p]
    lib.Multires_levelPositionsOut.restype = ctypes.c_int


def _cube_arrays(mesh):
    positions = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", positions)
    corner_verts = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get("vertex_index", corner_verts)
    face_offsets = np.empty(len(mesh.polygons) + 1, dtype=np.int32)
    mesh.polygons.foreach_get("loop_start", face_offsets[:len(mesh.polygons)])
    face_offsets[len(mesh.polygons)] = len(mesh.loops)
    return positions, corner_verts, face_offsets


def _sculptcore_grid(lib, arrays, level):
    positions, corner_verts, face_offsets = arrays
    cage = lib.Mesh_fromArrays(positions, len(positions) // 3,
                               corner_verts, len(corner_verts),
                               face_offsets, len(face_offsets) - 1)
    mr = lib.Multires_new(cage, level, 0, 0, 0)
    n = lib.Multires_levelSampleCount(mr, level)
    out = np.empty(n * 3, dtype=np.float32)
    lib.Multires_levelPositionsOut(mr, level, out)
    w = (1 << (level - 1)) + 1
    ng = n // (w * w)
    # sc[grid, v, u, :] in SculptCore native order.
    return out.reshape(ng, w, w, 3), ng, w


def _max_edge(eval_mesh):
    """Max polygon-edge length (multires eval mesh is all quads)."""
    nv = len(eval_mesh.vertices)
    co = np.empty(nv * 3, dtype=np.float64)
    eval_mesh.vertices.foreach_get("co", co)
    co = co.reshape(-1, 3)
    nc = len(eval_mesh.loops)
    cv = np.empty(nc, dtype=np.int32)
    eval_mesh.loops.foreach_get("vertex_index", cv)
    quads = cv.reshape(-1, 4)
    a = co[quads]
    b = co[quads[:, [1, 2, 3, 0]]]
    d = np.linalg.norm(a - b, axis=2)
    return float(d.max())


def _remap(sc, ng, w, off, direction, transpose):
    """SculptCore [grid,v,u] -> Blender flat [grid*w*w + y*w + x]. Per-face
    corner permutation cb=(off+direction*corner)%4 and an intra-grid transpose."""
    out = np.empty((ng, w, w, 3), dtype=np.float32)
    for g in range(ng):
        face, corner = g // 4, g % 4
        gb = face * 4 + (off + direction * corner) % 4
        for v in range(w):
            for u in range(w):
                x, y = (v, u) if transpose else (u, v)
                out[gb, y, x] = sc[g, v, u]
    return np.ascontiguousarray(out.reshape(-1), dtype=np.float32)


def main():
    lines = ["P8 B convention pinning (max eval-mesh edge; smallest = correct)", ""]
    try:
        import sculptcore_addon.engine as engine
        if "sculptcore_addon" not in bpy.context.preferences.addons:
            bpy.ops.preferences.addon_enable(module="sculptcore_addon")
        lib = engine.capi().lib
        _decl(lib)

        bpy.ops.object.select_all(action='SELECT'); bpy.ops.object.delete()
        bpy.ops.mesh.primitive_cube_add(size=2.0)
        ob = bpy.context.active_object
        arrays = _cube_arrays(ob.data)
        sc_base, ng, w = _sculptcore_grid(lib, arrays, LEVEL)
        lines.append("cube: grids={:d} grid_size={:d} (level {:d})".format(ng, w, LEVEL))

        # --- Geometric diagnostic: read the grid/orientation correspondence ---
        # SculptCore grid g corner (0,0), +u step (1,0), +v step (0,1).
        vco = np.empty(len(ob.data.vertices) * 3, dtype=np.float64)
        ob.data.vertices.foreach_get("co", vco); vco = vco.reshape(-1, 3)
        cvi = np.empty(len(ob.data.loops), dtype=np.int32)
        ob.data.loops.foreach_get("vertex_index", cvi)
        blc = vco[cvi]  # Blender loop corner-vertex positions.
        # Does SculptCore grid g's (0,0) equal Blender loop g's corner vertex?
        match = int(np.sum(np.linalg.norm(sc_base[:, 0, 0, :] - blc, axis=1) < 1e-5))
        lines.append("grid(0,0)==loop-corner matches: {:d}/{:d}".format(match, ng))
        for g in (0, 1):
            lines.append("  g{:d}: (0,0)={} +u={} +v={}".format(
                g, np.round(sc_base[g, 0, 0], 3).tolist(),
                np.round(sc_base[g, 1, 0] - sc_base[g, 0, 0], 3).tolist(),
                np.round(sc_base[g, 0, 1] - sc_base[g, 0, 0], 3).tolist()))
        lines.append("  blender face0 loop corners: {}".format(
            [np.round(blc[i], 3).tolist() for i in range(4)]))
        lines.append("")

        # Asymmetric displacement (function of position -> seam replicas stay
        # consistent) so a wrong convention tears visibly at grid seams.
        sc = sc_base.copy()
        sc[..., 2] += 0.02 * np.sin(2.0 * sc[..., 0]) * np.cos(3.0 * sc[..., 1])

        # Multires modifier subdivided to LEVEL.
        md = ob.modifiers.new("Multires", 'MULTIRES')
        for _ in range(LEVEL):
            bpy.ops.object.multires_subdivide(modifier="Multires")
        depsgraph = bpy.context.evaluated_depsgraph_get()

        # Undisplaced reference max edge (baseline smooth surface).
        depsgraph.update()
        base_edge = _max_edge(ob.evaluated_get(depsgraph).data)
        lines.append("undisplaced baseline max edge: {:.4e}".format(base_edge))
        # SculptCore's own intended within-grid max edge (no seam tears): the
        # value the correct convention should reproduce.
        du = np.linalg.norm(sc[:, :, :-1, :] - sc[:, :, 1:, :], axis=-1).max()
        dv = np.linalg.norm(sc[:, :-1, :, :] - sc[:, 1:, :, :], axis=-1).max()
        lines.append("SculptCore intended within-grid max edge: {:.4e}".format(max(du, dv)))
        lines.append("")

        results = []
        for off in range(4):
            for direction in (1, 3):  # 3 == -1 mod 4
                for transpose in (0, 1):
                    flat = _remap(sc, ng, w, off, direction, transpose)
                    ob.multires_reshape_from_positions(depsgraph, flat)
                    ob.data.update_tag()
                    ob.update_tag()
                    depsgraph.update()
                    me = _max_edge(ob.evaluated_get(depsgraph).data)
                    tag = "off={:d} dir={:d} transpose={:d}".format(off, direction, transpose)
                    results.append((me, tag))
        for me, tag in sorted(results):
            lines.append("{:s}:  max_edge={:.4e}".format(tag, me))

        results.sort()
        lines.append("")
        lines.append("WINNER (smallest max edge): {:s}  (max_edge={:.4e})".format(
            results[0][1], results[0][0]))
        lines.append("runner-up: {:s} ({:.4e})".format(results[1][1], results[1][0]))
    except Exception as ex:
        import traceback
        lines.append("ERROR: {!r}".format(ex))
        lines.append(traceback.format_exc())
    with open(OUT, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))


bpy.app.timers.register(lambda: (main(), None)[1], first_interval=1.0)
