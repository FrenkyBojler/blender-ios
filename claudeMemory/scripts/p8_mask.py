"""P8 A4: multires paint-mask exchange.

On a displaced multires cube:
  1. author a mask in VANILLA sculpt mode (flood fill + value) -> enter the
     mode -> the engine mask is seeded (import direction, vanilla-authored);
  2. paint more mask with the MASK brush -> exit -> re-enter -> the painted
     region survives (export -> grid paint mask -> import round trip);
  3. masked verts resist a DRAW stroke (the mask actually protects).
  4. a sculpt-level round trip (top -> 1 -> top) preserves the mask.

Run: blender --factory-startup --python p8_mask.py  -> %TEMP%/p8_mask.txt
"""
import bpy, os, tempfile
import numpy as np

OUT = os.path.join(tempfile.gettempdir(), "p8_mask.txt")
LEVEL = 3


def _check(lines, label, ok, detail=""):
    lines.append("{:s}: {:s}{:s}".format(
        label, "PASS" if ok else "FAIL", " ({:s})".format(detail) if detail else ""))
    return ok


def _engine_mask(engine, session):
    import ctypes

    n = 0
    nv = ctypes.c_int(0)
    nc, nf, cap = ctypes.c_int(0), ctypes.c_int(0), ctypes.c_int(0)
    engine.capi().lib.Mesh_arraySizes(session.mesh_ptr, ctypes.byref(nv), ctypes.byref(nc),
                                      ctypes.byref(nf), ctypes.byref(cap))
    values = np.zeros(nv.value, dtype=np.float32)
    has = engine.capi().lib.Mesh_readVertFloatAttr(session.mesh_ptr, b".spatial.v.mask", values)
    return values if has else None


def _engine_pos(engine, session):
    import sculptcore
    mgr = engine.manager()
    with sculptcore.construct_from_items(mgr, mgr.get("float"), []) as dump:
        session.mesh().dumpVertCo(dump)
        return dump.numpy().reshape(-1, 4)[:, 1:4].copy()


def main():
    lines = ["P8 multires paint-mask exchange (A4)", ""]
    all_ok = True
    try:
        if "sculptcore_addon" not in bpy.context.preferences.addons:
            bpy.ops.preferences.addon_enable(module="sculptcore_addon")
        from sculptcore_addon import engine, stroke

        context = bpy.context
        bpy.ops.object.select_all(action='SELECT'); bpy.ops.object.delete()
        bpy.ops.mesh.primitive_cube_add(size=2.0)
        ob = context.active_object
        ob.modifiers.new("Multires", 'MULTIRES')
        for _ in range(LEVEL):
            bpy.ops.object.multires_subdivide(modifier="Multires")

        # --- 1. vanilla-authored mask -> import ---------------------------
        bpy.ops.object.mode_set(mode='SCULPT')
        bpy.ops.paint.mask_flood_fill(mode='VALUE', value=0.6)
        bpy.ops.object.mode_set(mode='OBJECT')

        bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
        session = engine.sessions[ob.name]
        mask = _engine_mask(engine, session)
        all_ok &= _check(lines, "vanilla mask imported",
                         mask is not None and abs(float(mask.mean()) - 0.6) < 1e-3,
                         "mean={:.3f}".format(float(mask.mean()) if mask is not None else -1))

        # --- 2. paint with the MASK brush, round-trip through exit --------
        mgr = engine.manager()
        kernel = int(mgr.get("sculptcore::brush::SculptBrushes").items["MASK"])
        sc_brush = stroke._ensure_brush(session)
        sc_brush.strength = 1.0
        sc_brush.radius = 0.5
        sc_brush.spacing = 0.1
        sc_brush.invert = False
        sc_brush.writeProps()
        hit = stroke.raycast(session, (0.0, 0.0, 5.0), (0.0, 0.0, -1.0))
        stroke.stroke_begin(session)
        for _ in range(10):
            stroke.apply_dab(session, kernel, hit[0], hit[1], 0.5)
        stroke.stroke_end(session)
        painted = _engine_mask(engine, session)
        pole = painted is not None and float(painted.max()) > 0.95
        all_ok &= _check(lines, "MASK brush paints", pole,
                         "max={:.3f}".format(float(painted.max()) if painted is not None else -1))

        bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")  # exit
        all_ok &= _check(lines, "exit clean", ob.name not in engine.sessions)

        bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")  # re-enter
        session = engine.sessions[ob.name]
        mask2 = _engine_mask(engine, session)
        # Compare against the pre-exit engine mask via matching statistics
        # (vertex order is rebuilt on re-enter).
        stats_ok = (mask2 is not None
                    and abs(float(mask2.mean()) - float(painted.mean())) < 5e-3
                    and abs(float(mask2.max()) - float(painted.max())) < 5e-3)
        all_ok &= _check(lines, "mask survives exit/re-enter", stats_ok,
                         "mean {:.3f}->{:.3f}".format(float(painted.mean()),
                                                      float(mask2.mean()) if mask2 is not None else -1))

        # --- 3. mask protects against sculpting ---------------------------
        pre = _engine_pos(engine, session)
        kernel_draw = int(mgr.get("sculptcore::brush::SculptBrushes").items["DRAW"])
        stroke.stroke_begin(session)
        for _ in range(10):
            stroke.apply_dab(session, kernel_draw, hit[0], hit[1], 0.5)
        stroke.stroke_end(session)
        post = _engine_pos(engine, session)
        moved = np.linalg.norm(post - pre, axis=1)
        fully_masked = mask2 > 0.99
        protected = float(moved[fully_masked].max()) if fully_masked.any() else 1.0
        all_ok &= _check(lines, "masked verts resist the DRAW stroke",
                         fully_masked.any() and protected < 1e-5,
                         "max moved under mask={:.2e}".format(protected))

        # --- 4. level switch round trip preserves the mask ----------------
        from sculptcore_addon import convert
        convert.set_multires_level(ob, 1)
        convert.set_multires_level(ob, LEVEL)
        mask3 = _engine_mask(engine, session)
        stats_ok = (mask3 is not None
                    and abs(float(mask3.mean()) - float(mask2.mean())) < 5e-3)
        all_ok &= _check(lines, "mask survives a level round trip", stats_ok,
                         "mean {:.3f}->{:.3f}".format(float(mask2.mean()),
                                                      float(mask3.mean()) if mask3 is not None else -1))

        bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
        all_ok &= _check(lines, "final exit clean", ob.name not in engine.sessions)

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
