"""Mask box/lasso gesture verification (GUI: projection needs a real
region/rv3d, so this cannot run under --background).

Covers: the box gesture's execute() path over explicit coords (full-view box
masks everything; half-view box masks a strict subset; INVERT round-trips),
and the lasso point-in-polygon helper on synthetic data.

Run: blender --factory-startup --python mask_gesture_test.py
  -> %TEMP%/mask_gesture_test.txt
"""
import os
import tempfile

import numpy as np

import bpy

OUT = os.path.join(tempfile.gettempdir(), "mask_gesture_test.txt")


def _check(lines, label, ok, detail=""):
    lines.append("{:s}: {:s}{:s}".format(
        label, "PASS" if ok else "FAIL", " ({:s})".format(detail) if detail else ""))
    return ok


def main():
    lines = ["mask gesture test", ""]
    all_ok = True
    try:
        if "sculptcore_addon" not in bpy.context.preferences.addons:
            bpy.ops.preferences.addon_enable(module="sculptcore_addon")
        import sculptcore_addon.convert as convert
        import sculptcore_addon.engine as engine
        import sculptcore_addon.gestures as gestures

        bpy.ops.object.select_all(action='SELECT')
        bpy.ops.object.delete()
        bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=16)
        ob = bpy.context.active_object

        window = bpy.context.window_manager.windows[0]
        area = next(a for a in window.screen.areas if a.type == 'VIEW_3D')
        region = next(r for r in area.regions if r.type == 'WINDOW')
        override = {"window": window, "area": area, "region": region}

        with bpy.context.temp_override(**override):
            bpy.ops.view3d.view_all(center=True)
            bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
        session = engine.sessions[ob.name]

        def engine_mask():
            values = np.zeros(convert.mesh_vert_num(session.mesh_ptr), dtype=np.float32)
            engine.capi().lib.Mesh_readVertFloatAttr(
                session.mesh_ptr, convert._SC_MASK, values)
            return values

        # Full-view box fills every vertex.
        with bpy.context.temp_override(**override):
            result = bpy.ops.sculptcore.mask_box_gesture(
                mode='VALUE', value=1.0,
                xmin=0, xmax=region.width, ymin=0, ymax=region.height)
        all_ok &= _check(lines, "full box finished", result == {'FINISHED'})
        all_ok &= _check(lines, "full box masks all", bool((engine_mask() == 1.0).all()))

        # Left-half box inverts a strict subset.
        with bpy.context.temp_override(**override):
            result = bpy.ops.sculptcore.mask_box_gesture(
                mode='INVERT',
                xmin=0, xmax=region.width // 2, ymin=0, ymax=region.height)
        inverted = int((engine_mask() == 0.0).sum())
        total = len(engine_mask())
        all_ok &= _check(lines, "half box inverts a proper subset",
                         result == {'FINISHED'} and 0 < inverted < total,
                         "{:d}/{:d}".format(inverted, total))

        # Undo restores the full fill (snapshot step).
        with bpy.context.temp_override(**override):
            bpy.ops.ed.undo()
        all_ok &= _check(lines, "undo restores full fill",
                         bool((engine_mask() == 1.0).all()))

        # Lasso helper on synthetic data: unit square about the origin.
        pts = np.array([[0.0, 0.0], [2.0, 0.0], [0.5, 0.5], [-2.0, 0.0]])
        valid = np.ones(len(pts), dtype=bool)
        square = [(-1.0, -1.0), (1.0, -1.0), (1.0, 1.0), (-1.0, 1.0)]
        inside = gestures.points_in_polygon(pts, valid, square)
        all_ok &= _check(lines, "polygon test", list(inside) == [True, False, True, False],
                         repr(list(inside)))

        with bpy.context.temp_override(**override):
            bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")

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
