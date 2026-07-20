# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Regression: Ctrl-inverted clay with autosmooth exploded the mesh. The chained
BSMOOTH command shares the Brush with the main command, so the inverted flag
negated the smooth strength too — an anti-Laplacian that diverges within a few
dabs. build_program now pins the smooth entry's invert off
(BrushProgram.setCommandInvert); this test drives an inverted [CLAY, BSMOOTH]
program both ways and asserts the pinned program stays bounded while the
unpinned one visibly diverges (the differential proves the mechanism).

Run:
    set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
    blender --background --factory-startup --python claudeMemory/tests/autosmooth_invert_test.py
Exits nonzero on failure.
"""

import sys

import numpy as np
import bpy

def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def _max_radius(session):
    from sculptcore_addon import convert
    pos = convert.mesh_positions(session.mesh_ptr).reshape(-1, 3)
    if not np.isfinite(pos).all():
        return float("inf")
    return float(np.sqrt((pos * pos).sum(axis=1)).max())


def _run_inverted_stroke(name, pin_smooth_invert):
    import sculptcore_addon.engine as engine
    from sculptcore_addon import stroke as strokemod

    bpy.ops.mesh.primitive_uv_sphere_add(segments=24, ring_count=12, radius=1.0)
    ob = bpy.context.active_object
    ob.name = name
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    session = engine.sessions[name]
    mgr = engine.manager()
    clay = int(mgr.get("sculptcore::brush::SculptBrushes").items["CLAY"])

    brush = strokemod._ensure_brush(session)
    brush.strength, brush.radius, brush.spacing = 1.0, 0.5, 0.1
    brush.invert = True  # Ctrl held
    brush.writeProps()

    if pin_smooth_invert:
        prog = strokemod.build_program(session, clay, smooth_factor=1.0)
    else:
        # The pre-fix program: chained smooth inherits the inverted flag.
        bsmooth = int(mgr.get("sculptcore::brush::SculptBrushes").items["BSMOOTH"])
        prog = mgr.construct("sculptcore::brush::BrushProgram")
        prog.addCommand(clay)
        idx = prog.addCommand(bsmooth)
        prog.setCommandFloat(idx, 0, 1.0)

    center, normal, _ = strokemod.raycast(session, (0, 0, 5), (0, 0, -1))
    strokemod.stroke_begin(session)
    for i in range(15):
        # Re-assert the per-dab state the way the operator does (loadProps
        # writes post-dynamics values back into the fields each dab).
        brush.strength, brush.radius, brush.invert = 1.0, 0.5, True
        brush.writeProps()
        c = (center[0] + (i % 5) * 0.02, center[1], center[2])
        strokemod.apply_dyntopo_dab(session, prog, c, normal, 0.5, None, 100 + i)
    strokemod.stroke_end(session)

    r = _max_radius(session)
    if not pin_smooth_invert:
        prog.dispose()
    bpy.ops.object.custom_mode_toggle()
    return r


def main():
    if "sculptcore_addon" not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module="sculptcore_addon")
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()

    r_pinned = _run_inverted_stroke("InvPinned", pin_smooth_invert=True)
    r_raw = _run_inverted_stroke("InvRaw", pin_smooth_invert=False)
    print("max radius after inverted clay+autosmooth: pinned {:.4f}, "
          "unpinned {:.4f}".format(r_pinned, r_raw))

    # Pinned: an inverted clay dents inward, so the max radius stays at the
    # unit sphere. Unpinned: the inverted smooth diverges — ~1.5x within 15
    # dabs (and unboundedly with more).
    if not (r_pinned < 1.1):
        _fail("pinned program still diverges (max radius {:.4f})".format(r_pinned))
    if not (r_raw > 1.25):
        _fail("unpinned program did not diverge ({:.4f} vs {:.4f}) — "
              "differential lost, check the test setup".format(r_raw, r_pinned))
    print("ALL PASS: inverted autosmooth chain stays bounded (pinned invert).")


if __name__ == "__main__":
    main()
