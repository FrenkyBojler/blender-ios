"""P8 verification tail: multires conversion over a cage corpus.

Runs the full import->export round trip (build_engine / build_map /
import_displacement / export_bake) on displaced multires assets built over
cages that stress the correspondence: n-gons, triangle fans, open
boundaries, mixed valence, and creased edges. Reports per-cage/per-level
map bijectivity and identity-round-trip error; FAILs are cages whose
correspondence breaks (candidates for a documented refusal at enter).

Run: blender --factory-startup --python p8_corpus.py  -> %TEMP%/p8_corpus.txt
"""
import bpy, os, tempfile
import numpy as np

OUT = os.path.join(tempfile.gettempdir(), "p8_corpus.txt")
LEVELS = (2, 3, 4)
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


def _make_cage(kind):
    """Build a cage object for `kind`; returns the object."""
    if kind == "quad_cube":
        bpy.ops.mesh.primitive_cube_add(size=2.0)
        return bpy.context.active_object
    if kind == "ngon_cube":
        # Cube with one face turned into a pentagon (n-gon) via a poked
        # neighbour: dissolve one edge -> two quads merge into a hexagon.
        bpy.ops.mesh.primitive_cube_add(size=2.0)
        ob = bpy.context.active_object
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.mesh.select_all(action='DESELECT')
        bpy.ops.object.mode_set(mode='OBJECT')
        ob.data.edges[0].select = True
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.mesh.dissolve_edges()
        bpy.ops.object.mode_set(mode='OBJECT')
        return ob
    if kind == "tri_cone":
        # Triangle fan around an 8-pole + boundary loop at the base.
        bpy.ops.mesh.primitive_cone_add(vertices=8, radius1=1.0, depth=1.5)
        return bpy.context.active_object
    if kind == "boundary_grid":
        # Open grid: boundary-heavy, no closed surface.
        bpy.ops.mesh.primitive_grid_add(x_subdivisions=4, y_subdivisions=4, size=2.0)
        return bpy.context.active_object
    if kind == "open_cylinder":
        # Open tube: two boundary loops, quads.
        bpy.ops.mesh.primitive_cylinder_add(vertices=12, radius=1.0, depth=2.0,
                                            end_fill_type='NOTHING')
        return bpy.context.active_object
    if kind == "creased_cube":
        bpy.ops.mesh.primitive_cube_add(size=2.0)
        ob = bpy.context.active_object
        crease = ob.data.attributes.new("crease_edge", 'FLOAT', 'EDGE')
        values = [0.0] * len(ob.data.edges)
        for i in range(0, len(values), 2):
            values[i] = 1.0
        crease.data.foreach_set("value", values)
        return ob
    raise ValueError(kind)


def _run_case(context, scm, kind, level, lines):
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()
    ob = _make_cage(kind)
    ob.modifiers.new("Multires", 'MULTIRES')
    for _ in range(level):
        bpy.ops.object.multires_subdivide(modifier="Multires")

    depsgraph = context.evaluated_depsgraph_get(); depsgraph.update()
    base = _eval_verts(ob, depsgraph)
    asset = base.copy()
    asset[:, 2] += 0.2 * np.sin(2.3 * base[:, 0]) * np.cos(1.7 * base[:, 1])
    asset[:, 0] += 0.1 * np.sin(3.1 * base[:, 1] + 0.5)
    ob.multires_reshape_from_vert_positions(
        depsgraph, np.ascontiguousarray(asset.reshape(-1), dtype=np.float32))
    ob.data.update_tag(); depsgraph.update()
    asset_surf = _eval_verts(ob, depsgraph)

    base_arrays = _base_arrays(ob.data)
    mr = cage = None
    lib = scm.engine.capi().lib
    try:
        mr, cage = scm.build_engine(base_arrays, level)
        mapping = scm.build_map(context, base_arrays, mr, level)

        # Bijectivity: the export map must cover every engine sample set and
        # the import map every subdiv vertex (dedup sets equal-sized).
        fwd = mapping.engine_sample_to_blender
        rev = mapping.blender_to_engine_sample
        n_subdiv = len(asset_surf)
        cover_fwd = len(np.unique(fwd))
        bijective = cover_fwd == n_subdiv and len(rev) == n_subdiv

        changed = scm.import_displacement(mr, mapping, asset_surf)
        cnt = lib.Multires_levelSampleCount(mr, level)
        etop = np.empty(cnt * 3, dtype=np.float32)
        lib.Multires_levelPositionsOut(mr, level, etop)
        etop = etop.reshape(-1, 3)[rev].astype(np.float64)
        import_err = float(np.linalg.norm(etop - asset_surf, axis=1).max())

        scm.export_bake(ob, depsgraph, mr, mapping)
        ob.update_tag(); depsgraph.update()
        baked_surf = _eval_verts(ob, depsgraph)
        export_err = float(np.linalg.norm(baked_surf - asset_surf, axis=1).max())

        ok = bijective and import_err < TOL and export_err < TOL and changed > 0
        lines.append(
            "{:14s} L{:d}: verts={:6d} bijective={:s} import={:.2e} export={:.2e}  {:s}".format(
                kind, level, n_subdiv, "yes" if bijective else "NO ",
                import_err, export_err, "PASS" if ok else "FAIL"))
        return ok
    except Exception as ex:
        lines.append("{:14s} L{:d}: ERROR {!r}".format(kind, level, ex))
        return False
    finally:
        if mr:
            lib.Multires_free(mr)
        if cage:
            lib.freeMesh(cage)


def main():
    lines = ["P8 cage corpus round-trip", ""]
    all_ok = True
    try:
        if "sculptcore_addon" not in bpy.context.preferences.addons:
            bpy.ops.preferences.addon_enable(module="sculptcore_addon")
        from sculptcore_addon import multires as scm

        context = bpy.context
        kinds = ("quad_cube", "ngon_cube", "tri_cone", "boundary_grid",
                 "open_cylinder", "creased_cube")
        for kind in kinds:
            for level in LEVELS:
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
