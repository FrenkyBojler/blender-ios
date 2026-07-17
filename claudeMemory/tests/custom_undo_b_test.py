# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
B-workstream verification for Tier-2 delta undo (undo-integration P6 B1-B3).

Drives the engine dab core directly (the modal stroke operator needs a window),
pushes a CUSTOM_MODE undo step per stroke exactly as the operator does, then
exercises bpy.ops.ed.undo / redo and asserts the Blender Mesh positions are
restored *exactly* (meshlog stores pre-dab values, so undo/redo are bit-exact).

Coverage:
  * Intra-meshlog undo/redo across three stacked strokes (A, B, C): each stop
    is bit-exact.
  * Memfile boundary: undoing past the earliest custom step lands on the base
    memfile snapshot (mesh restored by memfile); redoing back self-corrects via
    the seek-based decode and re-reaches every stroke state exactly.
  * Save round-trip keeps the sculpted mesh (flush contract).

Run:
    set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
    blender --background --factory-startup --python claudeMemory/tests/custom_undo_b_test.py
Exits nonzero on failure.
"""

import os
import sys
import tempfile

import numpy as np
import bpy

OBJ = "Btest"


def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def positions():
    """Re-fetch by name each call — memfile undo can invalidate wrappers."""
    ob = bpy.data.objects[OBJ]
    a = np.empty(len(ob.data.vertices) * 3, dtype=np.float32)
    ob.data.vertices.foreach_get("co", a)
    return a


def assert_pos(label, expected):
    d = float(np.abs(positions() - expected).max())
    if d > 1e-6:
        _fail("{:s}: positions differ by {:.3e} (want exact)".format(label, d))
    print("PASS:", label)


def do_stroke(session, strokemod, undo_mod, context, center):
    mgr = __import__("sculptcore_addon.engine", fromlist=["x"]).manager()
    draw = int(mgr.get("sculptcore::brush::SculptBrushes").items["DRAW"])
    ob = bpy.data.objects[OBJ]
    strokemod.stroke_begin(session, has_dyntopo=False)
    for _ in range(5):
        strokemod.apply_dab(session, draw, center, (0.0, 0.0, 1.0), 0.6)
    strokemod.stroke_end(session)
    from sculptcore_addon import convert
    convert.flush(ob)
    undo_mod.push(context, ob, session)
    return positions()


def main():
    import sculptcore_addon.engine as engine
    from sculptcore_addon import stroke as strokemod
    from sculptcore_addon import undo as undo_mod

    if "sculptcore_addon" not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module="sculptcore_addon")

    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()
    bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=16, radius=1.0)
    bpy.context.active_object.name = OBJ
    context = bpy.context

    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    # Background starts with undo disabled; a base memfile push initializes it
    # and gives the custom steps a boundary to sit above.
    bpy.ops.ed.undo_push(message="Base")
    session = engine.sessions[OBJ]

    brush = strokemod._ensure_brush(session)
    brush.strength = 0.5
    brush.radius = 0.5

    pre = positions()
    post_a = do_stroke(session, strokemod, undo_mod, context, (0.0, 0.0, 1.0))
    post_b = do_stroke(session, strokemod, undo_mod, context, (1.0, 0.0, 0.0))
    post_c = do_stroke(session, strokemod, undo_mod, context, (0.0, 1.0, 0.0))
    if np.abs(post_c - pre).max() < 1e-6:
        _fail("strokes did not change positions (brush setup?)")
    print("PASS: three strokes applied (max delta {:.3f})".format(
        float(np.abs(post_c - pre).max())))

    # -- intra-meshlog undo down to the first stroke -----------------------
    bpy.ops.ed.undo()
    assert_pos("undo C -> B exact", post_b)
    bpy.ops.ed.undo()
    assert_pos("undo B -> A exact", post_a)

    # -- cross the memfile boundary: undoing past A reverts the engine to the
    # pre-stroke state (the type decodes the active step, so our decode fires
    # even though the destination is a memfile step). --
    bpy.ops.ed.undo()
    assert_pos("undo A -> base (engine reverted) exact", pre)

    # -- redo back up: seek decode self-corrects and re-reaches each state --
    bpy.ops.ed.redo()
    assert_pos("redo base -> A exact", post_a)
    bpy.ops.ed.redo()
    assert_pos("redo A -> B exact", post_b)
    bpy.ops.ed.redo()
    assert_pos("redo B -> C exact", post_c)

    # -- eviction: truncate the redo branch with a new stroke --------------
    bpy.ops.ed.undo()  # back to B; C is now the redo branch
    entry_before = int(session.meshlog.entryCount())
    post_d = do_stroke(session, strokemod, undo_mod, context, (-1.0, 0.0, 0.0))
    entry_after = int(session.meshlog.entryCount())
    if np.abs(post_d - post_b).max() < 1e-6:
        _fail("truncating stroke D did not change positions")
    print("PASS: eviction/truncation ran (entryCount {:d} -> {:d})".format(
        entry_before, entry_after))
    bpy.ops.ed.undo()
    assert_pos("undo D -> B exact after truncation", post_b)
    bpy.ops.ed.redo()
    assert_pos("redo B -> D exact after truncation", post_d)

    # -- save round-trip: Mesh ID keeps the sculpted state -----------------
    sculpted = positions()
    tmp = os.path.join(tempfile.gettempdir(), "sculptcore_btest.blend")
    bpy.ops.wm.save_as_mainfile(filepath=tmp)
    bpy.ops.wm.open_mainfile(filepath=tmp)
    reloaded = positions()
    if reloaded.shape != sculpted.shape or float(np.abs(reloaded - sculpted).max()) > 1e-6:
        _fail("saved/reloaded mesh does not match the sculpted mesh")
    print("PASS: save round-trip preserved the sculpted mesh")
    try:
        os.remove(tmp)
    except OSError:
        pass

    print("ALL PASS: Tier-2 delta undo (B1-B3) verified.")


if __name__ == "__main__":
    main()
