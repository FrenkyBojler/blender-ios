# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Per-brush-type parity harness (brush-mapping M5).

Dabs every supported brush type on a fresh sphere and asserts the expected
engine-observable effect: DRAW/DRAW_SHARP/INFLATE displace net-outward along
the dab normal, the plane family / SMOOTH / PINCH / SNAKE_HOOK move vertices,
and MASK leaves positions untouched (it paints the mask attribute). Also
guards that every UNSUPPORTED type is refused by ``kernel_enum`` (the stroke
operator's no-crash path) and that ``apply_brush`` runs against a real
Blender Brush for every mapped type without a field-name error.

Run:
    set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
    blender --factory-startup --python claudeMemory/scripts/run_sync.py -- \
        claudeMemory/tests/brush_parity_test.py
Exits nonzero on failure.
"""

import sys

import numpy as np
import bpy

# Expected single-dab-series effect per supported brush type.
#   "out"  net displacement along the dab normal is clearly positive
#   "move" verts move (sign unconstrained: plane/smooth/pinch)
#   "mask" positions stay put (the brush paints the mask attribute)
EXPECT = {
    'DRAW': "out",
    'DRAW_SHARP': "out",
    'INFLATE': "out",
    'CLAY': "move",
    'CLAY_STRIPS': "move",
    'PLANE': "move",
    'MULTIPLANE_SCRAPE': "move",
    'SMOOTH': "move",
    'PINCH': "move",
    'MASK': "mask",
    'SNAKE_HOOK': "move",
}


def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def fresh_sphere(name):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=16, radius=1.0)
    ob = bpy.context.active_object
    ob.name = name
    return ob


def positions(ob):
    mesh = ob.data
    a = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", a)
    return a.reshape(-1, 3)


def main():
    if "sculptcore_addon" not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module="sculptcore_addon")
    import sculptcore_addon.engine as engine
    import sculptcore_addon.stroke as stroke
    import sculptcore_addon.mapping as mapping
    import sculptcore_addon.convert as convert

    mgr = engine.manager()
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()

    # -- guard: unsupported types are refused (no crash path) ---------------
    class _FakeBrush:
        def __init__(self, t):
            self.sculpt_brush_type = t

    for t in sorted(mapping.UNSUPPORTED):
        if mapping.kernel_enum(mgr, _FakeBrush(t)) is not None:
            _fail("kernel_enum accepted unsupported type {:s}".format(t))
    print("PASS: all {:d} unsupported types refused".format(len(mapping.UNSUPPORTED)))

    # -- apply_brush against a real Brush for every mapped type -------------
    sphere0 = fresh_sphere("apply_check")
    bpy.context.view_layer.objects.active = sphere0
    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    sc_brush = stroke._ensure_brush(engine.sessions["apply_check"])
    bl_brush = bpy.data.brushes.new("scp_parity", mode='SCULPT')
    unified = bpy.context.tool_settings.sculpt.unified_paint_settings
    for t in sorted(mapping.KERNEL_BY_TYPE):
        bl_brush.sculpt_brush_type = t
        mapping.apply_brush(bl_brush, unified, sc_brush, world_radius=0.5, invert=False)
    bpy.ops.object.custom_mode_toggle()
    print("PASS: apply_brush runs for all {:d} mapped types".format(
        len(mapping.KERNEL_BY_TYPE)))

    # -- per-type effect ----------------------------------------------------
    for bl_type, expect in sorted(EXPECT.items()):
        if bl_type not in mapping.KERNEL_BY_TYPE:
            _fail("EXPECT lists unmapped type {:s}".format(bl_type))
        name = "S_" + bl_type
        ob = fresh_sphere(name)
        bpy.context.view_layer.objects.active = ob
        bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
        session = engine.sessions[name]

        b = stroke._ensure_brush(session)
        b.strength = 0.8
        b.radius = 0.6
        b.spacing = 0.1
        b.invert = False
        # Per-type extras mirror mapping.apply_brush (no bpy Brush here).
        if bl_type == 'PINCH':
            b.pinch = 0.8
        b.writeProps()

        kernel = int(mgr.get("sculptcore::brush::SculptBrushes").items[
            mapping.KERNEL_BY_TYPE[bl_type]])
        hit = stroke.raycast(session, (0, 0, 5), (0, 0, -1))
        if hit is None:
            _fail("{:s}: raycast missed the sphere".format(bl_type))
        center, normal, _ = hit

        before = positions(ob).copy()
        stroke.stroke_begin(session)
        for i in range(10):
            stroke.apply_dab(session, kernel,
                             (center[0] + i * 0.01, center[1], center[2]),
                             normal, 0.6)
        stroke.stroke_end(session)
        convert.flush(ob)
        after = positions(ob)
        if not np.isfinite(after).all():
            _fail("{:s}: non-finite positions after stroke".format(bl_type))
        delta = after - before
        moved = float(np.abs(delta).sum())
        net = float((delta @ np.array(normal)).sum())

        if expect == "out" and not (moved > 1.0 and net > 1.0):
            _fail("{:s}: expected net-outward (moved {:.3f}, net {:.3f})".format(
                bl_type, moved, net))
        elif expect == "move" and not moved > 1e-2:
            _fail("{:s}: expected movement (moved {:.4f})".format(bl_type, moved))
        elif expect == "mask" and not moved < 1e-3:
            _fail("{:s}: mask brush moved positions (moved {:.4f})".format(
                bl_type, moved))
        print("PASS: {:s} move={:.3f} net={:.3f}".format(bl_type, moved, net))

        bpy.ops.object.custom_mode_toggle()

    print("ALL PASS: per-brush-type parity verified.")


if __name__ == "__main__":
    main()
