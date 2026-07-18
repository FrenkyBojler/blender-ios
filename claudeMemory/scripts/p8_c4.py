"""P8 C4: level-crossing undo/redo on a multires session (store-blob fallback).

History: two strokes at level 3, a sculpt-level switch to 2 (memfile property
step, as the UI would push), one stroke at level 2. Undo all the way down to
the imported asset and redo back up; steps whose meshlog died at the level
switch must restore exactly via their store snapshots, and the level must
follow the history.

Run: blender --factory-startup --python p8_c4.py  -> %TEMP%/p8_c4.txt
"""
import bpy, os, tempfile
import numpy as np

OUT = os.path.join(tempfile.gettempdir(), "p8_c4.txt")
LEVEL = 3
TOL = 1e-3


def _eval_verts(ob, depsgraph):
    mesh = ob.evaluated_get(depsgraph).data
    co = np.empty(len(mesh.vertices) * 3, dtype=np.float64)
    mesh.vertices.foreach_get("co", co)
    return co.reshape(-1, 3)


def _eval_top(context):
    """Top-level surface of the (possibly suppressed) multires modifier."""
    ob = bpy.data.objects["Cube"]
    md = ob.modifiers["Multires"]
    prev_show, prev_levels = md.show_viewport, md.levels
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


def _close(a, b):
    return float(np.linalg.norm(a - b, axis=1).max())


def _stroke(engine, stroke, convert, undo, context, x_offset):
    """One pushed stroke: dab column offset in x so strokes are distinct."""
    ob = bpy.data.objects["Cube"]
    session = engine.sessions[ob.name]
    hit = stroke.raycast(session, (x_offset, 0.0, 5.0), (0.0, 0.0, -1.0))
    sc_brush = stroke._ensure_brush(session)
    sc_brush.strength = 1.0
    sc_brush.radius = 0.4
    sc_brush.spacing = 0.1
    sc_brush.invert = False
    sc_brush.writeProps()
    mgr = engine.manager()
    kernel = int(mgr.get("sculptcore::brush::SculptBrushes").items["DRAW"])
    stroke.stroke_begin(session)
    touched = 0
    for _ in range(10):
        touched += stroke.apply_dab(session, kernel, hit[0], hit[1], 0.4)
    stroke.stroke_end(session)
    convert.flush(ob)
    undo.push(context, ob, session)
    return touched


def main():
    lines = ["P8 C4 level-crossing undo/redo", ""]
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
        surf_asset = _eval_top(context)

        bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
        bpy.ops.ed.undo_push(message="setup")

        _stroke(engine, stroke, convert, undo, context, -0.5)   # A at L3
        surf_a = _eval_top(context)
        _stroke(engine, stroke, convert, undo, context, 0.5)    # B at L3
        surf_b = _eval_top(context)

        # Level switch, pushed like a UI property edit.
        md = bpy.data.objects["Cube"].modifiers["Multires"]
        md.sculpt_levels = 2
        depsgraph = context.evaluated_depsgraph_get(); depsgraph.update()
        bpy.ops.ed.undo_push(message="Level")
        session = engine.sessions["Cube"]
        all_ok &= _check(lines, "switched to level 2",
                         session.multires_active_level == 2)

        _stroke(engine, stroke, convert, undo, context, 0.0)    # D at L2
        surf_d = _eval_top(context)
        all_ok &= _check(lines, "history distinct",
                         _close(surf_d, surf_b) > 1e-2
                         and _close(surf_b, surf_a) > 1e-2
                         and _close(surf_a, surf_asset) > 1e-2)

        def undo_and_surf():
            bpy.ops.ed.undo()
            return _eval_top(context)

        def redo_and_surf():
            bpy.ops.ed.redo()
            return _eval_top(context)

        s = undo_and_surf()   # revert D (meshlog fast path)
        all_ok &= _check(lines, "undo D (fast path)", _close(s, surf_b) < TOL,
                         "err={:.2e}".format(_close(s, surf_b)))
        s = undo_and_surf()   # revert level switch (memfile prop step)
        session = engine.sessions["Cube"]
        all_ok &= _check(lines, "undo switch returns to level 3",
                         session.multires_active_level == LEVEL)
        all_ok &= _check(lines, "surface stable across switch undo",
                         _close(s, surf_b) < TOL, "err={:.2e}".format(_close(s, surf_b)))
        s = undo_and_surf()   # revert B (blob fallback)
        all_ok &= _check(lines, "undo B (blob path)", _close(s, surf_a) < TOL,
                         "err={:.2e}".format(_close(s, surf_a)))
        s = undo_and_surf()   # revert A (blob_before = imported asset)
        all_ok &= _check(lines, "undo A restores the asset", _close(s, surf_asset) < TOL,
                         "err={:.2e}".format(_close(s, surf_asset)))

        s = redo_and_surf()   # A
        all_ok &= _check(lines, "redo A", _close(s, surf_a) < TOL,
                         "err={:.2e}".format(_close(s, surf_a)))
        s = redo_and_surf()   # B
        all_ok &= _check(lines, "redo B", _close(s, surf_b) < TOL,
                         "err={:.2e}".format(_close(s, surf_b)))
        s = redo_and_surf()   # level switch
        session = engine.sessions["Cube"]
        all_ok &= _check(lines, "redo switch returns to level 2",
                         session.multires_active_level == 2)
        s = redo_and_surf()   # D (blob fallback)
        all_ok &= _check(lines, "redo D (blob path)", _close(s, surf_d) < TOL,
                         "err={:.2e}".format(_close(s, surf_d)))

        ob = bpy.data.objects["Cube"]
        all_ok &= _check(lines, "still in mode", ob.mode == 'CUSTOM')
        with context.temp_override(object=ob, active_object=ob):
            bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
        all_ok &= _check(lines, "exit clean", "Cube" not in engine.sessions)

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
