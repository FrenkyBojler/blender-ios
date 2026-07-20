# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
BSMOOTH feature preservation with migrated edge flags (P11 A5 — the P9 Q1b
follow-through). A ridge mesh with its crest edges marked `sharp_edge` is
smoothed with the BSMOOTH kernel: the crest must survive far better than the
same stroke on an unmarked copy (where BSMOOTH degenerates to plain smooth).

Run:
    set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
    blender --background --factory-startup --python claudeMemory/tests/bsmooth_boundary_test.py
Exits nonzero on failure.
"""

import sys

import numpy as np
import bpy

RIDGE_H = 0.4


def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def _make_ridge(name, mark_sharp):
    """A 9x9 grid with the center row raised into a ridge; optionally mark the
    crest edges sharp."""
    bpy.ops.mesh.primitive_grid_add(x_subdivisions=8, y_subdivisions=8, size=2.0)
    ob = bpy.context.active_object
    ob.name = name
    mesh = ob.data
    n = len(mesh.vertices)
    pos = np.empty(n * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", pos)
    pos = pos.reshape(-1, 3)
    crest = np.abs(pos[:, 1]) < 1e-4
    pos[crest, 2] = RIDGE_H
    mesh.vertices.foreach_set("co", pos.reshape(-1))
    if mark_sharp:
        ev = np.empty(len(mesh.edges) * 2, dtype=np.int32)
        mesh.edges.foreach_get("vertices", ev)
        ev = ev.reshape(-1, 2)
        sharp = crest[ev[:, 0]] & crest[ev[:, 1]]
        if not sharp.any():
            _fail("setup: no crest edges found")
        mesh.attributes.new("sharp_edge", 'BOOLEAN', 'EDGE').data.foreach_set(
            "value", sharp)
    mesh.update()
    return ob


def _crest_height(ob):
    mesh = ob.data
    pos = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", pos)
    pos = pos.reshape(-1, 3)
    crest = np.abs(pos[:, 1]) < 0.05
    return float(pos[crest, 2].mean())


def _smooth_stroke(name):
    import sculptcore_addon.engine as engine
    from sculptcore_addon import stroke as strokemod

    bpy.context.view_layer.objects.active = bpy.data.objects[name]
    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    session = engine.sessions[name]
    mgr = engine.manager()
    bsmooth = int(mgr.get("sculptcore::brush::SculptBrushes").items["BSMOOTH"])
    brush = strokemod._ensure_brush(session)
    brush.strength, brush.radius, brush.spacing = 1.0, 0.8, 0.1
    brush.writeProps()
    strokemod.stroke_begin(session)
    for i in range(8):
        strokemod.apply_dab(session, bsmooth, (-0.6 + i * 0.17, 0.0, RIDGE_H), (0, 0, 1), 0.8)
    strokemod.stroke_end(session)
    from sculptcore_addon import convert
    convert.flush(bpy.data.objects[name])
    bpy.ops.object.custom_mode_toggle()


def main():
    if "sculptcore_addon" not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module="sculptcore_addon")
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()

    marked = _make_ridge("RidgeMarked", mark_sharp=True)
    plain = _make_ridge("RidgePlain", mark_sharp=False)

    h0 = _crest_height(marked)
    _smooth_stroke("RidgeMarked")
    _smooth_stroke("RidgePlain")
    h_marked = _crest_height(bpy.data.objects["RidgeMarked"])
    h_plain = _crest_height(bpy.data.objects["RidgePlain"])

    loss_marked = h0 - h_marked
    loss_plain = h0 - h_plain
    print("crest height: start {:.4f}, marked {:.4f} (loss {:.4f}), "
          "plain {:.4f} (loss {:.4f})".format(h0, h_marked, loss_marked,
                                              h_plain, loss_plain))
    if loss_plain < 0.05:
        _fail("plain smooth barely moved the crest — stroke setup ineffective")
    if not (loss_marked < loss_plain * 0.25):
        _fail("marked crest eroded too much ({:.4f} vs plain {:.4f}) — "
              "sharp flags not constraining BSMOOTH".format(loss_marked, loss_plain))
    print("ALL PASS: BSMOOTH preserves a sharp-marked crest "
          "({:.1%} of the unmarked erosion).".format(loss_marked / loss_plain))


if __name__ == "__main__":
    main()
