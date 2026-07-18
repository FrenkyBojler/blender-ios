"""P8: in-level stroke undo/redo on a multires session (meshlog path).

Sculpt a stroke on a multires object in the mode, undo (the CUSTOM_MODE step
seeks the meshlog and the flush re-bakes CD_MDISPS), redo, and verify the
evaluated top-level surface tracks exactly. Level-crossing undo (store
snapshots) is C4 and out of scope here.

Run: blender --factory-startup --python p8_mundo.py  -> %TEMP%/p8_mundo.txt
"""
import bpy, os, tempfile
import numpy as np

OUT = os.path.join(tempfile.gettempdir(), "p8_mundo.txt")
LEVEL = 3


def _eval_verts(ob, depsgraph):
    mesh = ob.evaluated_get(depsgraph).data
    co = np.empty(len(mesh.vertices) * 3, dtype=np.float64)
    mesh.vertices.foreach_get("co", co)
    return co.reshape(-1, 3)


def _eval_top(context, ob, md):
    """Top-level multires surface while in the mode: the modifier is
    suppressed (show_viewport off), so re-enable it for the evaluation."""
    prev_show = md.show_viewport
    prev_levels = md.levels
    md.show_viewport = True
    md.levels = LEVEL
    depsgraph = context.evaluated_depsgraph_get()
    depsgraph.update()
    surf = _eval_verts(ob, depsgraph)
    md.show_viewport = prev_show
    md.levels = prev_levels
    depsgraph.update()
    return surf


def _check(lines, label, ok, detail=""):
    lines.append("{:s}: {:s}{:s}".format(
        label, "PASS" if ok else "FAIL", " ({:s})".format(detail) if detail else ""))
    return ok


def _stroke(engine, stroke, session):
    hit = stroke.raycast(session, (0.0, 0.0, 5.0), (0.0, 0.0, -1.0))
    sc_brush = stroke._ensure_brush(session)
    sc_brush.strength = 1.0
    sc_brush.radius = 0.5
    sc_brush.spacing = 0.1
    sc_brush.invert = False
    sc_brush.writeProps()
    mgr = engine.manager()
    kernel = int(mgr.get("sculptcore::brush::SculptBrushes").items["DRAW"])
    stroke.stroke_begin(session)
    touched = 0
    for _ in range(15):
        touched += stroke.apply_dab(session, kernel, hit[0], hit[1], 0.5)
    stroke.stroke_end(session)
    return touched


def main():
    lines = ["P8 multires in-level stroke undo/redo", ""]
    all_ok = True
    try:
        if "sculptcore_addon" not in bpy.context.preferences.addons:
            bpy.ops.preferences.addon_enable(module="sculptcore_addon")
        from sculptcore_addon import convert, engine, stroke, undo

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

        bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
        session = engine.sessions[ob.name]
        # Timer-run ops push no undo steps; snapshot the pre-stroke state so
        # the stroke step has a real boundary below it (as in normal UI use).
        bpy.ops.ed.undo_push(message="setup")

        # A real stroke bracketed like the operator: dabs + flush + undo push.
        touched = _stroke(engine, stroke, session)
        convert.flush(ob)
        undo.push(context, ob, session)
        all_ok &= _check(lines, "stroke lands", touched > 0)
        stroked_surf = _eval_top(context, ob, md)
        delta_stroke = float(np.linalg.norm(stroked_surf - asset_surf, axis=1).max())
        all_ok &= _check(lines, "bake differs after stroke", delta_stroke > 1e-2,
                         "max={:.3f}".format(delta_stroke))

        # Undo: the CUSTOM_MODE step seeks the meshlog and re-bakes. The
        # boundary decode may swap Main — re-fetch the RNA references.
        bpy.ops.ed.undo()
        ob = bpy.data.objects["Cube"]
        md = ob.modifiers["Multires"]
        undo_surf = _eval_top(context, ob, md)
        err_undo = float(np.linalg.norm(undo_surf - asset_surf, axis=1).max())
        all_ok &= _check(lines, "undo restores the asset surface", err_undo < 1e-3,
                         "err={:.2e}".format(err_undo))

        # Redo brings the stroke back.
        bpy.ops.ed.redo()
        ob = bpy.data.objects["Cube"]
        md = ob.modifiers["Multires"]
        redo_surf = _eval_top(context, ob, md)
        err_redo = float(np.linalg.norm(redo_surf - stroked_surf, axis=1).max())
        all_ok &= _check(lines, "redo restores the stroke", err_redo < 1e-3,
                         "err={:.2e}".format(err_redo))

        all_ok &= _check(lines, "still in mode", ob.mode == 'CUSTOM')
        bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
        all_ok &= _check(lines, "exit clean", ob.name not in engine.sessions)

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
