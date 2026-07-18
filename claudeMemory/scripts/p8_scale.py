"""P8 verification tail: deep levels + production-scale cages, with timings.

Identity round trips on a small cage at levels 5-6 (the deep-level half of
the corpus requirement) and on a production-sized cage (a ~1.1k-face sphere)
at levels 3-4 (~300k subdiv verts), timing each conversion phase so the
enter-time cost at scale is on record.

Run: blender --factory-startup --python p8_scale.py  -> %TEMP%/p8_scale.txt
"""
import bpy, os, tempfile, time
import numpy as np

OUT = os.path.join(tempfile.gettempdir(), "p8_scale.txt")
TOL = 1e-3


def _eval_verts(ob, depsgraph):
    mesh = ob.evaluated_get(depsgraph).data
    co = np.empty(len(mesh.vertices) * 3, dtype=np.float64)
    mesh.vertices.foreach_get("co", co)
    return co.reshape(-1, 3)


def _base_arrays(mesh):
    positions = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", positions)
    corner_verts = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get("vertex_index", corner_verts)
    face_offsets = np.empty(len(mesh.polygons) + 1, dtype=np.int32)
    mesh.polygons.foreach_get("loop_start", face_offsets[:len(mesh.polygons)])
    face_offsets[len(mesh.polygons)] = len(mesh.loops)
    return positions, corner_verts, face_offsets


def _run_case(context, scm, kind, level, lines):
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()
    if kind == "quad_cube":
        bpy.ops.mesh.primitive_cube_add(size=2.0)
    else:
        bpy.ops.mesh.primitive_uv_sphere_add(segments=48, ring_count=24, radius=1.0)
    ob = bpy.context.active_object
    ob.modifiers.new("Multires", 'MULTIRES')
    for _ in range(level):
        bpy.ops.object.multires_subdivide(modifier="Multires")

    depsgraph = context.evaluated_depsgraph_get(); depsgraph.update()
    base = _eval_verts(ob, depsgraph)
    asset = base.copy()
    asset[:, 2] += 0.2 * np.sin(2.3 * base[:, 0]) * np.cos(1.7 * base[:, 1])
    ob.multires_reshape_from_vert_positions(
        depsgraph, np.ascontiguousarray(asset.reshape(-1), dtype=np.float32))
    ob.data.update_tag(); depsgraph.update()
    asset_surf = _eval_verts(ob, depsgraph)

    base_arrays = _base_arrays(ob.data)
    lib = scm.engine.capi().lib
    mr = cage = None
    try:
        t0 = time.perf_counter()
        mr, cage = scm.build_engine(base_arrays, level)
        t1 = time.perf_counter()
        mapping = scm.build_map(context, base_arrays, mr, level)
        t2 = time.perf_counter()
        scm.import_displacement(mr, mapping, asset_surf)
        t3 = time.perf_counter()
        scm.export_bake(ob, depsgraph, mr, mapping)
        t4 = time.perf_counter()
        ob.update_tag(); depsgraph.update()
        baked_surf = _eval_verts(ob, depsgraph)
        err = float(np.linalg.norm(baked_surf - asset_surf, axis=1).max())
        ok = err < TOL
        lines.append(
            "{:10s} L{:d}: verts={:7d} err={:.2e}  engine={:5.1f}s map={:5.1f}s "
            "import={:4.1f}s export={:5.1f}s  {:s}".format(
                kind, level, len(asset_surf), err, t1 - t0, t2 - t1,
                t3 - t2, t4 - t3, "PASS" if ok else "FAIL"))
        return ok
    except Exception as ex:
        lines.append("{:10s} L{:d}: ERROR {!r}".format(kind, level, ex))
        return False
    finally:
        if mr:
            lib.Multires_free(mr)
        if cage:
            lib.freeMesh(cage)


def main():
    lines = ["P8 deep-level + production-scale round-trip", ""]
    all_ok = True
    try:
        if "sculptcore_addon" not in bpy.context.preferences.addons:
            bpy.ops.preferences.addon_enable(module="sculptcore_addon")
        from sculptcore_addon import multires as scm

        context = bpy.context
        for kind, level in (("quad_cube", 5), ("quad_cube", 6),
                            ("uv_sphere", 3), ("uv_sphere", 4)):
            all_ok &= _run_case(context, scm, kind, level, lines)
        lines.append("")
        lines.append("ALL PASS" if all_ok else "SOME FAILED")
    except Exception as ex:
        import traceback
        lines.append("ERROR: {!r}".format(ex))
        lines.append(traceback.format_exc())
    with open(OUT, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    bpy.ops.wm.quit_blender()


bpy.app.timers.register(lambda: (main(), None)[1], first_interval=1.0)
