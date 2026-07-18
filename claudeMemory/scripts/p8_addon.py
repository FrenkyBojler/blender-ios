"""P8 C1/C3 core: full MDISPS import -> engine -> export round-trip through the
sculptcore_addon.multires module, on a displaced multires asset.

Build a displaced multires cube (bake a bump into CD_MDISPS), then: build the
engine stack from the base cage, build the sample<->vertex map, import the
object's displaced top-level surface into the engine, and export it back. With
no engine edits the baked surface must reproduce the asset (identity round-trip).

Run: blender --factory-startup --python p8_addon.py  -> %TEMP%/p8_addon.txt
"""
import bpy, os, tempfile
import numpy as np

OUT = os.path.join(tempfile.gettempdir(), "p8_addon.txt")


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


def main():
    lines = ["P8 addon multires import/export round-trip", ""]
    try:
        if "sculptcore_addon" not in bpy.context.preferences.addons:
            bpy.ops.preferences.addon_enable(module="sculptcore_addon")
        from sculptcore_addon import multires as scm

        context = bpy.context
        for level in (2, 3):
            bpy.ops.object.select_all(action='SELECT'); bpy.ops.object.delete()
            bpy.ops.mesh.primitive_cube_add(size=2.0)
            ob = bpy.context.active_object
            ob.modifiers.new("Multires", 'MULTIRES')
            for _ in range(level):
                bpy.ops.object.multires_subdivide(modifier="Multires")

            depsgraph = context.evaluated_depsgraph_get(); depsgraph.update()
            # Bake a displacement into the asset (its "sculpted" MDISPS state).
            base = _eval_verts(ob, depsgraph)
            asset = base.copy()
            asset[:, 2] += 0.25 * np.sin(2.3 * base[:, 0]) * np.cos(1.7 * base[:, 1])
            ob.multires_reshape_from_vert_positions(
                depsgraph, np.ascontiguousarray(asset.reshape(-1), dtype=np.float32))
            ob.data.update_tag(); depsgraph.update()
            asset_surf = _eval_verts(ob, depsgraph)

            # --- Import into the engine via the addon module ---
            base_arrays = _base_arrays(ob.data)
            mr, cage = scm.build_engine(base_arrays, level)
            mapping = scm.build_map(context, base_arrays, mr, level)
            changed = scm.import_displacement(mr, mapping, asset_surf)

            # Engine now holds the surface: dump its top level, compare (via the
            # export map) to the asset.
            lib = scm.engine.capi().lib
            cnt = lib.Multires_levelSampleCount(mr, level)
            etop = np.empty(cnt * 3, dtype=np.float32)
            lib.Multires_levelPositionsOut(mr, level, etop)
            etop = etop.reshape(-1, 3)[mapping.blender_to_engine_sample].astype(np.float64)
            import_err = float(np.linalg.norm(etop - asset_surf, axis=1).max())

            # --- Export back to MDISPS ---
            scm.export_bake(ob, depsgraph, mr, mapping)
            ob.update_tag(); depsgraph.update()
            baked_surf = _eval_verts(ob, depsgraph)
            export_err = float(np.linalg.norm(baked_surf - asset_surf, axis=1).max())

            lib.Multires_free(mr); lib.freeMesh(cage)
            ok = import_err < 1e-3 and export_err < 1e-3
            lines.append(
                "level {:d}: verts={:d} changed={:d} import_err={:.2e} "
                "export_err={:.2e}  {:s}".format(
                    level, len(asset_surf), changed, import_err, export_err,
                    "PASS" if ok else "FAIL"))
    except Exception as ex:
        import traceback
        lines.append("ERROR: {!r}".format(ex))
        lines.append(traceback.format_exc())
    with open(OUT, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))


bpy.app.timers.register(lambda: (main(), None)[1], first_interval=1.0)
