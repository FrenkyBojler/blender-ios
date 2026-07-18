"""P8 P0 numeric validation: cross-engine Catmull-Clark base-surface agreement.

Builds a multires cube in SculptCore (engine c-api) and dumps level-L grid
sample positions (Multires_levelPositionsOut / A2), then compares them as a
point cloud against Blender's own Catmull-Clark Subdivision-Surface evaluation
of the same cube at the same level. Both are discrete CC, so at zero
displacement every SculptCore grid sample must coincide with a Blender
subdivided-surface vertex to float tolerance (plan §5). Grid-index
transpose/parity is a separate concern (needs Blender-side grid indices) and is
not tested here.

Run: blender --factory-startup --python p8_validate.py
Writes results to %TEMP%/p8_validate.txt.
"""
import bpy, os, tempfile, ctypes
import numpy as np

OUT = os.path.join(tempfile.gettempdir(), "p8_validate.txt")
LEVELS = [1, 2, 3, 4]
TOL = 1e-4


def _log(lines):
    with open(OUT, "w") as f:
        f.write("\n".join(lines) + "\n")


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


def _cube_arrays():
    """Blender's default 2x2x2 cube as engine Mesh_fromArrays inputs."""
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()
    bpy.ops.mesh.primitive_cube_add(size=2.0)
    ob = bpy.context.active_object
    ob.name = "P8Cube"
    mesh = ob.data
    verts_num = len(mesh.vertices)
    corners_num = len(mesh.loops)
    faces_num = len(mesh.polygons)
    positions = np.empty(verts_num * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", positions)
    corner_verts = np.empty(corners_num, dtype=np.int32)
    mesh.loops.foreach_get("vertex_index", corner_verts)
    face_offsets = np.empty(faces_num + 1, dtype=np.int32)
    mesh.polygons.foreach_get("loop_start", face_offsets[:faces_num])
    face_offsets[faces_num] = corners_num
    return ob, positions, corner_verts, face_offsets


def _sculptcore_points(lib, positions, corner_verts, face_offsets, level):
    cage = lib.Mesh_fromArrays(positions, len(positions) // 3,
                               corner_verts, len(corner_verts),
                               face_offsets, len(face_offsets) - 1)
    if not cage:
        raise RuntimeError("Mesh_fromArrays failed")
    mr = lib.Multires_new(cage, level, 0, 0, 0)
    if not mr:
        raise RuntimeError("Multires_new failed")
    n = lib.Multires_levelSampleCount(mr, level)
    out = np.empty(n * 3, dtype=np.float32)
    written = lib.Multires_levelPositionsOut(mr, level, out)
    pts = out.reshape(-1, 3)[:written].astype(np.float64)
    lib.Multires_free(mr)
    lib.freeMesh(cage)
    return pts


def _blender_subsurf_points(ob, level):
    """Catmull-Clark subdivided cube vertices at `level` via the modifier."""
    for md in list(ob.modifiers):
        ob.modifiers.remove(md)
    md = ob.modifiers.new("subsurf", 'SUBSURF')
    md.subdivision_type = 'CATMULL_CLARK'
    md.levels = level
    md.render_levels = level
    depsgraph = bpy.context.evaluated_depsgraph_get()
    depsgraph.update()
    eval_ob = ob.evaluated_get(depsgraph)
    eval_mesh = eval_ob.data
    n = len(eval_mesh.vertices)
    co = np.empty(n * 3, dtype=np.float64)
    eval_mesh.vertices.foreach_get("co", co)
    ob.modifiers.remove(ob.modifiers[0])
    return co.reshape(-1, 3)


def _max_nn_dist(a, b):
    """max over points in `a` of the nearest-neighbor distance to `b`."""
    worst = 0.0
    chunk = 512
    for i in range(0, len(a), chunk):
        block = a[i:i + chunk]
        d = np.sqrt(((block[:, None, :] - b[None, :, :]) ** 2).sum(axis=2))
        worst = max(worst, float(d.min(axis=1).max()))
    return worst


def main():
    lines = ["P8 P0 cross-engine CC base-surface validation", ""]
    try:
        import sculptcore_addon.engine as engine
        if "sculptcore_addon" not in bpy.context.preferences.addons:
            bpy.ops.preferences.addon_enable(module="sculptcore_addon")
        lib = engine.capi().lib
        _decl(lib)

        for level in LEVELS:
            ob, positions, corner_verts, face_offsets = _cube_arrays()
            sc = _sculptcore_points(lib, positions, corner_verts, face_offsets, level)
            bl = _blender_subsurf_points(ob, level)
            sc_to_bl = _max_nn_dist(sc, bl)
            lines.append(
                "level {:d}: sc_samples={:d} bl_verts={:d}  sc<->bl_discrete={:.3e}".format(
                    level, len(sc), len(bl), sc_to_bl))

        lines.append("")
        lines.append("-- limit-vs-discrete discriminator (coarse points on the fine surface?) --")
        # A limit evaluation places level-L points ON the limit surface, so they
        # also lie on any finer level's surface (nn ~ 0). Discrete refinement
        # control points sit OFF the finer surface (nn grows).
        FINE = 6
        ob, positions, corner_verts, face_offsets = _cube_arrays()
        sc_coarse = _sculptcore_points(lib, positions, corner_verts, face_offsets, 2)
        sc_fine = _sculptcore_points(lib, positions, corner_verts, face_offsets, FINE)
        bl_coarse = _blender_subsurf_points(ob, 2)
        bl_fine = _blender_subsurf_points(ob, FINE)
        sc_self = _max_nn_dist(sc_coarse, sc_fine)
        bl_self = _max_nn_dist(bl_coarse, bl_fine)
        lines.append("SculptCore L2 points -> L{:d} surface: {:.3e}  ({:s})".format(
            FINE, sc_self, "LIMIT" if sc_self < TOL else "DISCRETE"))
        lines.append("Blender    L2 points -> L{:d} surface: {:.3e}  ({:s})".format(
            FINE, bl_self, "LIMIT" if bl_self < TOL else "DISCRETE"))
        # Cross-engine at a common near-limit resolution: do both converge to the
        # SAME limit surface? (SculptCore coarse points vs Blender fine surface.)
        cross = _max_nn_dist(sc_coarse, bl_fine)
        lines.append("SculptCore L2 points -> Blender L{:d} surface: {:.3e}".format(FINE, cross))
        # Same limit? Both near-limit surfaces should coincide (SC discrete at L6
        # is very close to the limit; Blender L6 IS the limit).
        same_limit = _max_nn_dist(sc_fine[::20], bl_fine)
        lines.append("SculptCore L{0:d} -> Blender L{0:d} (same-limit check): {1:.3e}  ({2:s})".format(
            FINE, same_limit, "SAME LIMIT" if same_limit < 1e-3 else "DIVERGENT"))
    except Exception as ex:
        import traceback
        lines.append("ERROR: {!r}".format(ex))
        lines.append(traceback.format_exc())
    _log(lines)
    print("\n".join(lines))


# Run after the addon/engine are importable (timer defers past registration).
bpy.app.timers.register(lambda: (main(), None)[1], first_interval=1.0)
