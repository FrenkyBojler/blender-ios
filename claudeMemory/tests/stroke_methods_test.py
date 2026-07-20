# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Anchored / Drag-Dot stroke methods (Q5): the no-compounding regression ported
from the reference app's `sculptcore_anchored_dragdot.test.ts`.

Driven through the engine preview-dab API the operator uses (rollback previous
provisional dab, apply new inside a preview bracket, commit exactly one at the
end). Checks: a 2-point "direct" stroke and a 5-point "wander" stroke ending at
the same place land within 10% of each other (compounding would make the wander
far larger); a no-rollback control does compound; commit is undoable; cancel
(rollback, no commit) leaves the mesh unchanged; an off-surface first raycast is
the anchored-refusal trigger.

Run:
    set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
    blender --factory-startup --python claudeMemory/scripts/run_sync.py -- \
        claudeMemory/tests/stroke_methods_test.py
Exits nonzero on failure.
"""

import sys

import numpy as np
import bpy


def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def positions(ob):
    a = np.empty(len(ob.data.vertices) * 3, dtype=np.float32)
    ob.data.vertices.foreach_get("co", a)
    return a.reshape(-1, 3)


def checksum(ob):
    """Sum of squared vertex-position magnitudes (the reference's checksum)."""
    p = positions(ob)
    return float((p * p).sum())


def _new_sphere(name):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=48, ring_count=24, radius=1.0)
    ob = bpy.context.active_object
    ob.name = name
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    return ob, enginemod.sessions[name]


def _configure(session, radius=0.4, strength=0.7):
    b = strokemod._ensure_brush(session)
    b.strength = strength
    b.radius = radius
    b.spacing = 0.1
    b.invert = False
    b.writeProps()


def _preview_dab(session, kernel, center, normal, radius, roll):
    """One preview dab (rollback the previous provisional group first when
    `roll`, matching the operator's no-compounding path)."""
    executor = strokemod._ensure_executor(session)
    if roll and executor.previewActive():
        executor.rollbackPreviewDab()
    cv = strokemod._float3(enginemod.manager(), *center)
    try:
        executor.beginPreviewDab(cv, radius)
    finally:
        cv.dispose()
    strokemod.apply_dab(session, kernel, center, normal, radius)


def _hit(session, x, y):
    return strokemod.raycast(session, (x, y, 5.0), (0.0, 0.0, -1.0))


def _dragdot_stroke(session, kernel, xy_path, radius, roll=True, commit=True):
    """Run a Drag-Dot stroke: one dab per input at the cursor, each rolling back
    the last. Returns after commit (or leaves it rolled back on cancel)."""
    executor = strokemod._ensure_executor(session)
    strokemod.stroke_begin(session)
    for (x, y) in xy_path:
        hit = _hit(session, x, y)
        if hit is None:
            continue
        center, normal, _ = hit
        _preview_dab(session, kernel, center, normal, radius, roll)
    if commit and executor.previewActive():
        executor.commitPreviewDab()
    elif not commit and executor.previewActive():
        executor.rollbackPreviewDab()
    strokemod.stroke_end(session)


def test_dragdot_no_compounding():
    draw = int(enginemod.manager().get("sculptcore::brush::SculptBrushes").items["DRAW"])
    final = (0.0, 0.0)  # both strokes end at the sphere's top

    # Direct: start near the target, one hop to it.
    ob, session = _new_sphere("dd_direct")
    _configure(session)
    base = checksum(ob)
    _dragdot_stroke(session, draw, [(0.3, 0.0), final], 0.4)
    convertmod.flush(ob)
    direct = checksum(ob) - base
    bpy.ops.object.custom_mode_toggle()

    # Wander: five points through unrelated places, ending at the same target.
    ob, session = _new_sphere("dd_wander")
    _configure(session)
    _dragdot_stroke(session, draw,
                    [(0.4, 0.3), (-0.4, 0.2), (0.1, -0.4), (-0.2, -0.1), final], 0.4)
    convertmod.flush(ob)
    wander = checksum(ob) - base
    bpy.ops.object.custom_mode_toggle()

    # Control: the same wander with no rollback compounds (5 dabs, all kept).
    ob, session = _new_sphere("dd_compound")
    _configure(session)
    _dragdot_stroke(session, draw,
                    [(0.4, 0.3), (-0.4, 0.2), (0.1, -0.4), (-0.2, -0.1), final],
                    0.4, roll=False)
    convertmod.flush(ob)
    compound = checksum(ob) - base
    bpy.ops.object.custom_mode_toggle()

    if direct <= 1e-4:
        _fail("direct drag-dot did not displace the mesh ({:.6f})".format(direct))
    if abs(wander - direct) > 0.1 * abs(direct):
        _fail("drag-dot compounding: |wander {:.5f} - direct {:.5f}| > 10%".format(
            wander, direct))
    if compound <= 2.0 * abs(direct):
        _fail("no-rollback control did not compound (compound {:.5f} vs direct {:.5f})".format(
            compound, direct))
    print("PASS: drag-dot no-compounding (direct {:.4f}, wander {:.4f}, compound {:.4f})".format(
        direct, wander, compound))


def _anchored_stroke(session, kernel, center, normal, radii, roll=True, commit=True):
    """Anchored: one dab pinned at `center`, radius stepping through `radii`
    (the drag-length growth), each rolling back the last."""
    executor = strokemod._ensure_executor(session)
    strokemod.stroke_begin(session)
    for r in radii:
        _preview_dab(session, kernel, center, normal, r, roll)
    if commit and executor.previewActive():
        executor.commitPreviewDab()
    strokemod.stroke_end(session)


def test_anchored_no_compounding():
    draw = int(enginemod.manager().get("sculptcore::brush::SculptBrushes").items["DRAW"])

    ob, session = _new_sphere("an_direct")
    _configure(session)
    base = checksum(ob)
    hit = _hit(session, 0.0, 0.0)
    center, normal, _ = hit
    _anchored_stroke(session, draw, center, normal, [0.1, 0.5])
    convertmod.flush(ob)
    direct = checksum(ob) - base
    bpy.ops.object.custom_mode_toggle()

    ob, session = _new_sphere("an_wander")
    _configure(session)
    hit = _hit(session, 0.0, 0.0)
    center, normal, _ = hit
    _anchored_stroke(session, draw, center, normal, [0.1, 0.35, 0.2, 0.45, 0.5])
    convertmod.flush(ob)
    wander = checksum(ob) - base
    bpy.ops.object.custom_mode_toggle()

    if direct <= 1e-4:
        _fail("direct anchored stroke did not displace ({:.6f})".format(direct))
    if abs(wander - direct) > 0.1 * abs(direct):
        _fail("anchored compounding: |wander {:.5f} - direct {:.5f}| > 10%".format(
            wander, direct))
    print("PASS: anchored no-compounding (direct {:.4f}, wander {:.4f})".format(
        direct, wander))


def test_cancel_unchanged():
    draw = int(enginemod.manager().get("sculptcore::brush::SculptBrushes").items["DRAW"])
    ob, session = _new_sphere("cancel")
    _configure(session)
    before = positions(ob).copy()
    # A drag-dot stroke that is cancelled (rolled back, never committed).
    _dragdot_stroke(session, draw, [(0.3, 0.0), (0.1, 0.1), (0.0, 0.0)], 0.4, commit=False)
    convertmod.flush(ob)
    after = positions(ob)
    drift = float(np.abs(after - before).max())
    bpy.ops.object.custom_mode_toggle()
    if drift > 1e-5:
        _fail("cancel left the mesh changed (max drift {:.6f})".format(drift))
    print("PASS: cancel leaves the mesh unchanged (max drift {:.2e})".format(drift))


def test_commit_undoable():
    draw = int(enginemod.manager().get("sculptcore::brush::SculptBrushes").items["DRAW"])
    ob, session = _new_sphere("undo")
    _configure(session)
    before = positions(ob).copy()
    _dragdot_stroke(session, draw, [(0.3, 0.0), (0.0, 0.0)], 0.4)
    convertmod.flush(ob)
    if float(np.abs(positions(ob) - before).max()) <= 1e-4:
        _fail("committed stroke did not change the mesh")
    # Undo the meshlog step (the preview bracket sits inside it).
    session.meshlog.undo(session.mesh(), session.tree())
    session.meshlog_cursor -= 1
    session.mesh().recalc_normals()
    convertmod.flush(ob)
    drift = float(np.abs(positions(ob) - before).max())
    bpy.ops.object.custom_mode_toggle()
    if drift > 1e-5:
        _fail("undo did not restore the mesh (max drift {:.6f})".format(drift))
    print("PASS: committed preview stroke undoes exactly (max drift {:.2e})".format(drift))


def test_anchored_refusal_condition():
    ob, session = _new_sphere("refuse")
    # The operator refuses an anchored stroke whose first raycast misses; verify
    # the trigger: a ray well outside the sphere returns no hit.
    if _hit(session, 5.0, 5.0) is not None:
        _fail("off-surface raycast unexpectedly hit (refusal trigger broken)")
    if _hit(session, 0.0, 0.0) is None:
        _fail("on-surface raycast missed (sanity)")
    bpy.ops.object.custom_mode_toggle()
    print("PASS: anchored-refusal trigger (off-surface start yields no hit)")


def main():
    global strokemod, enginemod, convertmod
    if "sculptcore_addon" not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module="sculptcore_addon")
    import sculptcore_addon.engine as enginemod
    import sculptcore_addon.stroke as strokemod
    import sculptcore_addon.convert as convertmod
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()

    test_dragdot_no_compounding()
    test_anchored_no_compounding()
    test_cancel_unchanged()
    test_commit_undoable()
    test_anchored_refusal_condition()
    print("ALL PASS: anchored / drag-dot stroke methods")


if __name__ == "__main__":
    main()
