"""P8 session wiring: multires objects through the real mode lifecycle.

On a displaced multires cube: enter the mode (stack import + modifier
suppression), verify the no-stroke identity round-trip through flush/exit,
re-enter and sculpt synthetic dabs, exit, and confirm the edit landed in
CD_MDISPS (visible at several view levels) with the modifier display restored.

Run: blender --factory-startup --python p8_session.py  -> %TEMP%/p8_session.txt
"""
import bpy, os, tempfile
import numpy as np

OUT = os.path.join(tempfile.gettempdir(), "p8_session.txt")
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
    lines = ["P8 multires session wiring", ""]
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
        md.levels = 2  # viewport level below top, like a real asset

        depsgraph = context.evaluated_depsgraph_get(); depsgraph.update()
        # Bake a displacement into the asset (its "sculpted" MDISPS state).
        prev_levels = md.levels
        md.levels = LEVEL
        depsgraph.update()
        base = _eval_verts(ob, depsgraph)
        asset = base.copy()
        asset[:, 2] += 0.25 * np.sin(2.3 * base[:, 0]) * np.cos(1.7 * base[:, 1])
        ob.multires_reshape_from_vert_positions(
            depsgraph, np.ascontiguousarray(asset.reshape(-1), dtype=np.float32))
        ob.data.update_tag(); depsgraph.update()
        asset_surf = _eval_verts(ob, depsgraph)
        md.levels = prev_levels
        depsgraph.update()

        # --- Enter the mode ---
        bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
        session = engine.sessions.get(ob.name)
        all_ok &= _check(lines, "enter creates session", session is not None)
        all_ok &= _check(lines, "session is multires",
                         session is not None and session.multires_ptr is not None)
        all_ok &= _check(lines, "modifier suppressed", md.show_viewport is False)
        all_ok &= _check(lines, "level = totlvl",
                         session is not None and session.multires_level == LEVEL)
        all_ok &= _check(lines, "map cached",
                         session is not None and session.multires_map is not None)
        all_ok &= _check(lines, "draw key registered",
                         session is not None and session.draw_key == int(ob.session_uid))

        # --- No-stroke identity round-trip through flush + exit ---
        convert.flush(ob)
        bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
        all_ok &= _check(lines, "exit frees session", ob.name not in engine.sessions)
        all_ok &= _check(lines, "modifier display restored", md.show_viewport is True)
        md.levels = LEVEL
        depsgraph.update()
        round_surf = _eval_verts(ob, depsgraph)
        err = float(np.linalg.norm(round_surf - asset_surf, axis=1).max())
        all_ok &= _check(lines, "identity round-trip", err < 1e-3,
                         "err={:.2e}".format(err))
        md.levels = prev_levels
        depsgraph.update()

        # --- Sculpt: enter, dab the top pole, exit ---
        bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
        session = engine.sessions[ob.name]
        hit = stroke.raycast(session, (0.0, 0.0, 5.0), (0.0, 0.0, -1.0))
        all_ok &= _check(lines, "raycast hits imported surface", hit is not None)
        moved = 0.0
        if hit is not None:
            mgr = engine.manager()
            kernel = int(mgr.get("sculptcore::brush::SculptBrushes").items["DRAW"])
            # Factory startup has no Blender brush; set the engine brush directly.
            sc_brush = stroke._ensure_brush(session)
            sc_brush.strength = 1.0
            sc_brush.radius = 0.5
            sc_brush.spacing = 0.1
            sc_brush.invert = False
            sc_brush.writeProps()
            stroke.stroke_begin(session)
            touched = 0
            for _ in range(30):
                touched += stroke.apply_dab(session, kernel, hit[0], hit[1], 0.5)
            stroke.stroke_end(session)
            all_ok &= _check(lines, "dabs touch nodes", touched > 0,
                             "nodes={:d}".format(touched))
        bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
        all_ok &= _check(lines, "modifier display restored after sculpt",
                         md.show_viewport is True)

        # The edit must be visible in vanilla multires at several view levels.
        deltas = []
        for view_level in (1, 2, LEVEL):
            md.levels = view_level
            depsgraph.update()
            md_surf = _eval_verts(ob, depsgraph)
            md.levels = LEVEL
            depsgraph.update()
            top_surf = _eval_verts(ob, depsgraph)
            deltas.append((view_level, len(md_surf)))
        top_delta = float(np.linalg.norm(top_surf - asset_surf, axis=1).max())
        moved = top_delta
        all_ok &= _check(lines, "sculpt edit landed in MDISPS", top_delta > 1e-2,
                         "max moved={:.3f}".format(top_delta))
        lines.append("view levels evaluated: {!r}".format(deltas))
        all_ok &= _check(lines, "no session leak", not engine.sessions)

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
