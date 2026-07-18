"""P8 C2: multires sculpt-level switching through the session.

On a displaced multires cube in the mode: the modifier's ``sculpt_levels``
drives the engine's active level (depsgraph handler), the session's views
rebind on switch, sculpting at a coarser level works and cascades into the
top-level bake, and flush restores the sculpt level after dumping the top.

Run: blender --factory-startup --python p8_level.py  -> %TEMP%/p8_level.txt
"""
import bpy, os, tempfile
import numpy as np

OUT = os.path.join(tempfile.gettempdir(), "p8_level.txt")
LEVEL = 3


def _eval_verts(ob, depsgraph):
    mesh = ob.evaluated_get(depsgraph).data
    co = np.empty(len(mesh.vertices) * 3, dtype=np.float64)
    mesh.vertices.foreach_get("co", co)
    return co.reshape(-1, 3)


def _check(lines, label, ok, detail=""):
    lines.append("{:s}: {:s}{:s}".format(
        label, "PASS" if ok else "FAIL", " ({:s})".format(detail) if detail else ""))
    return ok


def main():
    lines = ["P8 multires level switching (C2)", ""]
    all_ok = True
    try:
        if "sculptcore_addon" not in bpy.context.preferences.addons:
            bpy.ops.preferences.addon_enable(module="sculptcore_addon")
        from sculptcore_addon import convert, engine, stroke

        context = bpy.context
        bpy.ops.object.select_all(action='SELECT'); bpy.ops.object.delete()
        bpy.ops.mesh.primitive_cube_add(size=2.0)
        ob = context.active_object
        md = ob.modifiers.new("Multires", 'MULTIRES')
        for _ in range(LEVEL):
            bpy.ops.object.multires_subdivide(modifier="Multires")

        depsgraph = context.evaluated_depsgraph_get(); depsgraph.update()
        base = _eval_verts(ob, depsgraph)
        asset = base.copy()
        asset[:, 2] += 0.25 * np.sin(2.3 * base[:, 0]) * np.cos(1.7 * base[:, 1])
        ob.multires_reshape_from_vert_positions(
            depsgraph, np.ascontiguousarray(asset.reshape(-1), dtype=np.float32))
        ob.data.update_tag(); depsgraph.update()
        asset_surf = _eval_verts(ob, depsgraph)

        # --- Enter at the top sculpt level (subdivide leaves sculpt_levels=3) ---
        bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
        session = engine.sessions[ob.name]
        all_ok &= _check(lines, "enter at top level",
                         session.multires_active_level == LEVEL)
        top_mesh_ptr = session.mesh_ptr
        top_verts = session.verts_num

        # --- Switch down via the modifier property + depsgraph handler ---
        md.sculpt_levels = 2
        depsgraph.update()
        all_ok &= _check(lines, "handler switches level",
                         session.multires_active_level == 2)
        all_ok &= _check(lines, "views rebound",
                         session.mesh_ptr != top_mesh_ptr
                         and session.verts_num < top_verts,
                         "verts {:d} -> {:d}".format(top_verts, session.verts_num))
        all_ok &= _check(lines, "wrappers reset", session.executor is None
                         and session.meshlog is None and session.mesh_obj is None)

        # --- Sculpt at level 2 ---
        hit = stroke.raycast(session, (0.0, 0.0, 5.0), (0.0, 0.0, -1.0))
        all_ok &= _check(lines, "raycast hits level-2 surface", hit is not None)
        sc_brush = stroke._ensure_brush(session)
        sc_brush.strength = 1.0
        sc_brush.radius = 0.6
        sc_brush.spacing = 0.1
        sc_brush.invert = False
        sc_brush.writeProps()
        mgr = engine.manager()
        kernel = int(mgr.get("sculptcore::brush::SculptBrushes").items["DRAW"])
        stroke.stroke_begin(session)
        touched = 0
        for _ in range(20):
            touched += stroke.apply_dab(session, kernel, hit[0], hit[1], 0.6)
        stroke.stroke_end(session)
        all_ok &= _check(lines, "dabs touch level-2 nodes", touched > 0,
                         "nodes={:d}".format(touched))

        # --- Flush: bakes the top level, then restores the sculpt level ---
        convert.flush(ob)
        all_ok &= _check(lines, "flush restores sculpt level",
                         session.multires_active_level == 2)
        all_ok &= _check(lines, "session views valid after flush",
                         session.mesh_ptr is not None and session.tree_ptr is not None)

        # --- Switch back up: the level-2 edit must ride the cascade ---
        md.sculpt_levels = LEVEL
        depsgraph.update()
        all_ok &= _check(lines, "switch back to top",
                         session.multires_active_level == LEVEL)
        hit_top = stroke.raycast(session, (0.0, 0.0, 5.0), (0.0, 0.0, -1.0))
        all_ok &= _check(lines, "raycast hits switched surface", hit_top is not None)

        # --- Exit; the baked result must differ from the asset (the edit) ---
        bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
        all_ok &= _check(lines, "exit clean", ob.name not in engine.sessions)
        md.levels = LEVEL
        depsgraph.update()
        baked = _eval_verts(ob, depsgraph)
        delta = float(np.linalg.norm(baked - asset_surf, axis=1).max())
        all_ok &= _check(lines, "level-2 edit cascades into the bake",
                         delta > 1e-2, "max moved={:.3f}".format(delta))

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
