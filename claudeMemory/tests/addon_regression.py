"""Consolidated headless regression suite for the SculptCore sculpt-mode addon.

Runs the whole addon vertical against a built Blender: mode lifecycle,
conversion round trips, the stroke path, per-brush sculpting, attribute
round trips (mask / face sets), grab-family, tool/panel registration, and
the session-reconcile handlers. Each section is independent (fresh object)
and asserts an engine-observable result — no GUI, no C module built beyond
the sculptcore_capi shared lib.

Run (from the repo root, engine lib built via `node make.mjs build python`):

    set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python   # dev checkout
    blender --background --factory-startup \
        --python claudeMemory/tests/addon_regression.py

Exits nonzero if any section fails. This is dev scaffolding (claudeMemory/);
fold the load-bearing parts into the shipping addon's tests before the PR.
"""

import sys
import traceback

import numpy as np

import bpy

ADDON = "sculptcore_addon"


# -- helpers ---------------------------------------------------------------

def _enable():
    if ADDON not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module=ADDON)


def _disable():
    if ADDON in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_disable(module=ADDON)


def fresh_sphere(name, segments=32, rings=16):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=segments, ring_count=rings, radius=1.0)
    ob = bpy.context.active_object
    ob.name = name
    bpy.context.view_layer.objects.active = ob
    return ob


def positions(ob):
    a = np.empty(len(ob.data.vertices) * 3, dtype=np.float32)
    ob.data.vertices.foreach_get("co", a)
    return a.reshape(-1, 3)


def enter(name):
    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    import sculptcore_addon.engine as engine
    return engine.sessions[name]


def exit_mode():
    bpy.ops.object.custom_mode_toggle()


def dab_stroke(session, kernel, center, normal, n=10, step=0.01, radius=0.6):
    import sculptcore_addon.stroke as stroke
    stroke.stroke_begin(session)
    for i in range(n):
        stroke.apply_dab(session, kernel, (center[0] + i * step, center[1], center[2]),
                         normal, radius)
    stroke.stroke_end(session)


def kernel(name):
    import sculptcore_addon.engine as engine
    return int(engine.manager().get("sculptcore::brush::SculptBrushes").items[name])


# -- sections --------------------------------------------------------------

def test_enter_exit_roundtrip():
    import sculptcore_addon.engine as engine
    ob = fresh_sphere("RT")
    before = positions(ob).copy()
    session = enter("RT")
    assert session.verts_num == len(ob.data.vertices)
    assert not session.topology_changed()
    exit_mode()
    assert "RT" not in engine.sessions
    assert np.array_equal(before, positions(ob)), "no-stroke round trip changed positions"


def test_stroke_moves_and_flushes():
    import sculptcore_addon.stroke as stroke
    import sculptcore_addon.convert as convert
    ob = fresh_sphere("STK")
    session = enter("STK")
    b = stroke._ensure_brush(session)
    b.strength, b.radius, b.spacing = 0.8, 0.6, 0.1
    b.writeProps()
    center, normal, _ = stroke.raycast(session, (0, 0, 5), (0, 0, -1))
    assert abs(center[2] - 1.0) < 0.05, "raycast missed the sphere pole"
    before = positions(ob).copy()
    dab_stroke(session, kernel("DRAW"), center, normal, n=50)
    convert.flush(ob)
    assert np.abs(positions(ob) - before).sum() > 1.0, "DRAW stroke did not move the mesh"
    exit_mode()


def test_brush_parity():
    import sculptcore_addon.stroke as stroke
    import sculptcore_addon.convert as convert
    import sculptcore_addon.mapping as mapping
    expect = {
        'DRAW': "out", 'DRAW_SHARP': "out", 'INFLATE': "out",
        'CLAY': "move", 'CLAY_STRIPS': "move", 'PLANE': "move",
        'MULTIPLANE_SCRAPE': "move", 'SMOOTH': "move", 'PINCH': "move",
        'SNAKE_HOOK': "move", 'MASK': "mask",
    }
    import sculptcore_addon.engine as engine
    mgr = engine.manager()
    for bl_type, kind in sorted(expect.items()):
        ob = fresh_sphere("P_" + bl_type)
        session = enter("P_" + bl_type)
        b = stroke._ensure_brush(session)
        b.strength, b.radius, b.spacing = 0.8, 0.6, 0.1
        if bl_type == 'PINCH':
            b.pinch = 0.8
        b.writeProps()
        k = int(mgr.get("sculptcore::brush::SculptBrushes").items[mapping.KERNEL_BY_TYPE[bl_type]])
        center, normal, _ = stroke.raycast(session, (0, 0, 5), (0, 0, -1))
        before = positions(ob).copy()
        dab_stroke(session, k, center, normal, n=10, radius=0.6)
        convert.flush(ob)
        delta = positions(ob) - before
        moved = float(np.abs(delta).sum())
        net = float((delta @ np.array(normal)).sum())
        if kind == "out":
            assert moved > 1.0 and net > 1.0, (bl_type, moved, net)
        elif kind == "move":
            assert moved > 1e-2, (bl_type, moved)
        else:
            assert moved < 1e-3, (bl_type, moved)
        exit_mode()
    # Unsupported types are refused (crash guard).
    class _FB:
        def __init__(self, t):
            self.sculpt_brush_type = t
    for t in mapping.UNSUPPORTED:
        assert mapping.kernel_enum(mgr, _FB(t)) is None, t


def test_grab_family():
    import sculptcore_addon.stroke as stroke
    import sculptcore_addon.convert as convert
    # snake hook: standard path; grab: anchor path.
    ob = fresh_sphere("SNK")
    session = enter("SNK")
    b = stroke._ensure_brush(session)
    b.strength, b.radius, b.spacing = 1.0, 0.7, 0.1
    b.writeProps()
    center, normal, _ = stroke.raycast(session, (0, 0, 5), (0, 0, -1))
    before = positions(ob).copy()
    dab_stroke(session, kernel("SNAKEHOOK"), center, normal, n=12, step=0.05, radius=0.7)
    convert.flush(ob)
    assert np.abs(positions(ob) - before).sum() > 1e-2, "snake hook did not move"
    exit_mode()

    # grab + elastic deform (kelvinlet): both use the anchor / grabTo path.
    for kname in ("GRAB", "KELVINLET"):
        ob = fresh_sphere("GRB_" + kname)
        session = enter("GRB_" + kname)
        b = stroke._ensure_brush(session)
        b.strength, b.radius, b.spacing = 1.0, 0.7, 0.1
        b.mu, b.nu = 1.0, 0.4
        b.writeProps()
        center, normal, _ = stroke.raycast(session, (0, 0, 5), (0, 0, -1))
        before = positions(ob).copy()
        stroke.stroke_begin(session)
        for i in range(12):
            stroke.apply_grab_dab(session, kernel(kname), center,
                                  (center[0] + i * 0.05, center[1], center[2]), normal, 0.7)
        stroke.stroke_end(session)
        convert.flush(ob)
        assert np.abs(positions(ob) - before).sum() > 1e-2, kname + " did not move"
        exit_mode()


def test_mask_roundtrip():
    import sculptcore_addon.stroke as stroke
    import sculptcore_addon.convert as convert

    def read_mask(ob):
        attr = ob.data.attributes.get(".sculpt_mask")
        if attr is None:
            return None
        out = np.empty(len(ob.data.vertices), dtype=np.float32)
        attr.data.foreach_get("value", out)
        return out

    ob = fresh_sphere("MSK")
    assert read_mask(ob) is None
    session = enter("MSK")
    b = stroke._ensure_brush(session)
    b.strength, b.radius, b.spacing = 1.0, 0.7, 0.1
    b.writeProps()
    center, normal, _ = stroke.raycast(session, (0, 0, 5), (0, 0, -1))
    dab_stroke(session, kernel("MASK"), center, normal, n=8, radius=0.7)
    convert.flush(ob)
    mask = read_mask(ob)
    assert mask is not None and (mask > 1e-3).sum() > 0, "mask not written"
    exit_mode()
    persisted = read_mask(ob)
    assert persisted is not None
    enter("MSK")
    convert.flush(ob)
    exit_mode()
    assert np.allclose(read_mask(ob), persisted, atol=1e-4), "mask changed across re-enter"


def test_faceset_roundtrip():
    import sculptcore_addon.stroke as stroke
    import sculptcore_addon.convert as convert

    def read_fs(ob):
        attr = ob.data.attributes.get(".sculpt_face_set")
        if attr is None:
            return None
        out = np.empty(len(ob.data.polygons), dtype=np.int32)
        attr.data.foreach_get("value", out)
        return out

    ob = fresh_sphere("FS")
    assert read_fs(ob) is None
    session = enter("FS")
    b = stroke._ensure_brush(session)
    b.strength, b.radius, b.spacing, b.activeGroup = 1.0, 0.7, 0.1, 5
    b.writeProps()
    center, normal, _ = stroke.raycast(session, (0, 0, 5), (0, 0, -1))
    dab_stroke(session, kernel("POLYGROUP"), center, normal, n=8, radius=0.7)
    convert.flush(ob)
    fs = read_fs(ob)
    assert fs is not None and (fs == 5).sum() > 0, "face set not written"
    exit_mode()
    persisted = read_fs(ob)
    enter("FS")
    convert.flush(ob)
    exit_mode()
    assert np.array_equal(read_fs(ob), persisted), "face sets changed across re-enter"


def test_color_roundtrip():
    import sculptcore_addon.stroke as stroke
    import sculptcore_addon.convert as convert

    def read_color(ob):
        attr = ob.data.color_attributes.active_color
        if attr is None:
            return None
        out = np.empty(len(ob.data.vertices) * 4, dtype=np.float32)
        attr.data.foreach_get("color", out)
        return out.reshape(-1, 4)

    ob = fresh_sphere("COL")
    assert read_color(ob) is None
    session = enter("COL")
    b = stroke._ensure_brush(session)
    b.strength, b.radius, b.spacing = 1.0, 0.7, 0.1
    bc = b.brushColor.vec
    bc[0], bc[1], bc[2], bc[3] = 1.0, 0.0, 0.0, 1.0  # red
    b.writeProps()
    center, normal, _ = stroke.raycast(session, (0, 0, 5), (0, 0, -1))
    dab_stroke(session, kernel("COLOR"), center, normal, n=8, radius=0.7)
    convert.flush(ob)
    col = read_color(ob)
    assert col is not None, "flush did not create a color attribute"
    reddish = int(((col[:, 0] > 0.5) & (col[:, 1] < 0.5)).sum())
    assert reddish > 0, "no verts painted red"
    exit_mode()
    persisted = read_color(ob)
    # Re-enter loads the color and a no-op round trip preserves it.
    enter("COL")
    convert.flush(ob)
    exit_mode()
    assert np.allclose(read_color(ob), persisted, atol=1e-4), "color changed across re-enter"


def test_falloff_presets():
    """apply_brush bakes the Blender falloff preset into the engine LUT;
    SHARP concentrates displacement near the dab center more than SMOOTH,
    CONSTANT spreads it most."""
    import sculptcore_addon.engine as engine
    import sculptcore_addon.stroke as stroke
    import sculptcore_addon.mapping as mapping
    import sculptcore_addon.convert as convert
    draw = kernel("DRAW")

    def inner_fraction(preset, hardness=0.0):
        tag = preset + "_%d" % int(hardness * 100)
        bpy.ops.mesh.primitive_grid_add(x_subdivisions=60, y_subdivisions=60, size=4.0)
        ob = bpy.context.active_object
        ob.name = "FO_" + tag
        bpy.context.view_layer.objects.active = ob
        br = bpy.data.brushes.new("fo_" + tag, mode='SCULPT')
        br.sculpt_brush_type = 'DRAW'
        br.strength = 1.0
        br.curve_distance_falloff_preset = preset
        br.hardness = hardness
        session = enter("FO_" + tag)
        sc = stroke._ensure_brush(session)
        sc.radius = 1.2
        mapping.apply_brush(br, None, sc, world_radius=1.2, invert=False)
        center, normal, _ = stroke.raycast(session, (0, 0, 5), (0, 0, -1))
        before = positions(ob).copy()
        stroke.stroke_begin(session)
        stroke.apply_dab(session, draw, center, normal, 1.2)
        stroke.stroke_end(session)
        convert.flush(ob)
        disp = np.linalg.norm(positions(ob) - before, axis=1)
        dist = np.linalg.norm(before[:, :2] - np.array(center[:2]), axis=1)
        exit_mode()
        total = disp.sum()
        return (disp[dist < 0.6].sum() / total) if total > 1e-6 else 0.0

    sharp, smooth, const = (inner_fraction(p) for p in ('SHARP', 'SMOOTH', 'CONSTANT'))
    assert sharp > smooth > const, (sharp, smooth, const)
    # Hardness flattens the falloff: a hard SMOOTH spreads more (lower inner
    # fraction, toward the constant disc) than a soft one.
    smooth_hard = inner_fraction('SMOOTH', hardness=0.9)
    assert smooth_hard < smooth, (smooth_hard, smooth)


def test_autosmooth():
    """A [main, SMOOTH] program per dab (execProgram) runs both commands and
    changes the result vs the main brush alone. (Smoothing *quality* is a
    subtle visual thing checked interactively; here we assert the program
    path is exercised and affects geometry.)"""
    import sculptcore_addon.engine as engine
    import sculptcore_addon.stroke as stroke
    import sculptcore_addon.convert as convert
    draw = kernel("DRAW")

    def run(autosmooth):
        ob = fresh_sphere("AS_%d" % int(autosmooth * 100), segments=48, rings=32)
        session = enter(ob.name)
        b = stroke._ensure_brush(session)
        b.strength, b.radius, b.spacing = 0.7, 0.5, 0.1
        b.writeProps()
        center, normal, _ = stroke.raycast(session, (0, 0, 5), (0, 0, -1))
        before = positions(ob).copy()
        stroke.stroke_begin(session)
        prog = stroke.build_program(session, draw, autosmooth) if autosmooth else None
        for i in range(24):
            c = (center[0] + (i % 6) * 0.02, center[1] + (i // 6) * 0.02, center[2])
            if prog is not None:
                stroke.apply_dab_program(session, prog, c, normal, 0.5)
            else:
                stroke.apply_dab(session, draw, c, normal, 0.5)
        stroke.stroke_end(session)
        convert.flush(ob)
        out = positions(ob) - before
        exit_mode()
        return out

    plain, smoothed = run(0.0), run(1.0)
    assert np.abs(plain).sum() > 1e-2 and np.abs(smoothed).sum() > 1e-2
    # The smooth pass measurably changes the accumulated result.
    assert np.abs(plain - smoothed).sum() > 1e-2, "autosmooth had no effect"


def test_dyntopo():
    """A dyntopo stroke remeshes under the brush; the slow-path flush
    rebuilds the Blender mesh geometry to match, then reverts to fast path."""
    import sculptcore_addon.engine as engine
    import sculptcore_addon.stroke as stroke
    import sculptcore_addon.convert as convert
    draw = kernel("DRAW")

    ob = fresh_sphere("DT", segments=16, rings=8)
    v0 = len(ob.data.vertices)
    session = enter("DT")
    b = stroke._ensure_brush(session)
    b.strength, b.radius, b.spacing = 0.5, 0.5, 0.1
    b.writeProps()
    center, normal, _ = stroke.raycast(session, (0, 0, 5), (0, 0, -1))
    prog = stroke.build_program(session, draw)
    params = stroke.build_dyntopo_params(session, 0.05, 0.02)

    stroke.stroke_begin(session, has_dyntopo=True)
    for i in range(12):
        c = (center[0] + (i % 4) * 0.02, center[1] + (i // 4) * 0.02, center[2])
        stroke.apply_dyntopo_dab(session, prog, c, normal, 0.5, params, 1000 + i)
    stroke.stroke_end(session)
    assert session.topology_changed(), "dyntopo did not change topology"

    convert.flush(ob)  # slow path
    v1 = len(ob.data.vertices)
    assert v1 > v0 and v1 == session.verts_num, (v0, v1, session.verts_num)
    assert not session.topology_changed(), "topo stamp not resynced"
    assert len(ob.data.polygons) > 0
    # The bulk rebuild must produce valid topology (validate() returns True
    # only when it had to *fix* something).
    assert ob.data.validate() is False, "rebuilt mesh needed correction"
    assert len(ob.data.edges) > 0, "edges not derived"

    # Fast-path flush after dyntopo: the engine index space now has freelist
    # gaps, so positions must write in live order (regression for the
    # out-of-bounds scatter bug).
    before = positions(ob).copy()
    stroke.stroke_begin(session)  # no dyntopo
    stroke.apply_dab(session, draw, center, normal, 0.5)
    stroke.stroke_end(session)
    convert.flush(ob)  # fast path on the gappy engine mesh
    assert len(ob.data.vertices) == v1
    assert np.abs(positions(ob) - before).sum() > 1e-3, "post-dyntopo edit lost"
    exit_mode()
    assert len(ob.data.vertices) == v1


def test_tool_and_panels():
    from bl_ui.space_toolsystem_toolbar import VIEW3D_PT_tools_active
    import sculptcore_addon.ui as ui
    ids = {t.idname for t in VIEW3D_PT_tools_active._tools_flatten(
        VIEW3D_PT_tools_active._tools.get('CUSTOM', ())) if t is not None}
    assert "sculptcore.brush" in ids, ids
    ob = fresh_sphere("UI")
    assert not ui.SCULPTCORE_PT_brush_engine.poll(bpy.context)
    # The vanilla brush-panel subclasses registered (P10 Phase B).
    settings_panel = getattr(bpy.types, "SCULPTCORE_PT_tools_brush_settings")
    assert not settings_panel.poll(bpy.context)
    enter("UI")
    assert ui.SCULPTCORE_PT_brush_engine.poll(bpy.context)
    assert ui.SCULPTCORE_PT_dyntopo.poll(bpy.context)
    assert settings_panel.poll(bpy.context)
    exit_mode()


def test_mask_flood_fill():
    import ctypes
    import sculptcore_addon.convert as convert
    import sculptcore_addon.engine as engine

    def engine_mask(session):
        lib = engine.capi().lib
        nv = ctypes.c_int(0); nc = ctypes.c_int(0); ne = ctypes.c_int(0); nf = ctypes.c_int(0)
        lib.Mesh_arraySizes(session.mesh_ptr, ctypes.byref(nv), ctypes.byref(nc),
                            ctypes.byref(ne), ctypes.byref(nf))
        values = np.zeros(nv.value, dtype=np.float32)
        lib.Mesh_readVertFloatAttr(session.mesh_ptr, convert._SC_MASK, values)
        return values

    ob = fresh_sphere("MFF")
    bpy.ops.ed.undo_push(message="Base")
    session = enter("MFF")
    assert bpy.ops.sculptcore.mask_flood_fill.poll()

    bpy.ops.sculptcore.mask_flood_fill(mode='VALUE', value=1.0)
    assert np.allclose(engine_mask(session), 1.0), "fill did not reach the engine"
    bpy.ops.sculptcore.mask_flood_fill(mode='INVERT')
    assert np.allclose(engine_mask(session), 0.0), "invert did not flip the fill"

    # The attribute-snapshot undo steps restore each state in turn.
    bpy.ops.ed.undo()
    assert np.allclose(engine_mask(session), 1.0), "undo did not restore the fill"
    bpy.ops.ed.redo()
    assert np.allclose(engine_mask(session), 0.0), "redo did not re-apply the invert"
    bpy.ops.ed.undo()
    assert np.allclose(engine_mask(session), 1.0)

    # Exit flushes the engine mask into the Blender attribute.
    exit_mode()
    attr = ob.data.attributes.get(".sculpt_mask")
    assert attr is not None, "exit did not create the mask attribute"
    flushed = np.zeros(len(ob.data.vertices), dtype=np.float32)
    attr.data.foreach_get("value", flushed)
    assert np.allclose(flushed, 1.0), "flushed mask does not match engine state"


def test_mask_filter():
    import sculptcore_addon.convert as convert
    import sculptcore_addon.engine as engine

    def engine_mask(session):
        values = np.zeros(convert.mesh_vert_num(session.mesh_ptr), dtype=np.float32)
        engine.capi().lib.Mesh_readVertFloatAttr(session.mesh_ptr, convert._SC_MASK, values)
        return values

    ob = fresh_sphere("MFL")
    bpy.ops.ed.undo_push(message="Base")
    session = enter("MFL")

    # Seed: mask only the upper hemisphere (hard edge at the equator).
    verts = positions(ob)
    seed = (verts[:, 2] > 0.0).astype(np.float32)
    engine.capi().lib.Mesh_writeVertFloatAttr(
        session.mesh_ptr, convert._SC_MASK, np.ascontiguousarray(seed))
    masked_before = int((engine_mask(session) > 0.5).sum())

    bpy.ops.sculptcore.mask_filter(filter_type='GROW', auto_iteration_count=False)
    grown = engine_mask(session)
    assert (grown >= seed - 1e-6).all(), "grow lowered a mask value"
    assert (grown > 0.5).sum() > masked_before, "grow did not extend the boundary"

    bpy.ops.sculptcore.mask_filter(filter_type='SHRINK', auto_iteration_count=False)
    bpy.ops.sculptcore.mask_filter(filter_type='SHRINK', auto_iteration_count=False)
    shrunk = engine_mask(session)
    assert (shrunk > 0.5).sum() < masked_before, "two shrinks did not pull inside the seed"

    bpy.ops.sculptcore.mask_filter(filter_type='SMOOTH', auto_iteration_count=False)
    smoothed = engine_mask(session)
    boundary = (smoothed > 0.05) & (smoothed < 0.95)
    assert boundary.any(), "smooth left no intermediate values at the edge"

    # Undo unwinds the filter chain (snapshot steps).
    bpy.ops.ed.undo()  # smooth
    bpy.ops.ed.undo()  # shrink 2
    bpy.ops.ed.undo()  # shrink 1
    assert np.allclose(engine_mask(session), grown), "undo chain did not restore the grow state"
    exit_mode()


def test_face_sets_create():
    import sculptcore_addon.convert as convert
    import sculptcore_addon.engine as engine

    def engine_groups(session):
        values = np.zeros(convert.mesh_face_num(session.mesh_ptr), dtype=np.int32)
        engine.capi().lib.Mesh_readFaceIntAttr(session.mesh_ptr, convert._SC_GROUP, values)
        return values

    ob = fresh_sphere("FSC")
    bpy.ops.ed.undo_push(message="Base")
    session = enter("FSC")

    # No mask -> nothing to create.
    result = bpy.ops.sculptcore.face_sets_create(mode='MASKED')
    assert result == {'CANCELLED'}

    bpy.ops.sculptcore.mask_flood_fill(mode='VALUE', value=1.0)
    result = bpy.ops.sculptcore.face_sets_create(mode='MASKED')
    assert result == {'FINISHED'}
    groups = engine_groups(session)
    new_id = groups.max()
    assert new_id >= 1 and (groups == new_id).all(), "fully-masked mesh -> one face set"

    bpy.ops.ed.undo()
    assert engine_groups(session).max() < new_id, "undo did not restore the groups"
    bpy.ops.ed.redo()
    assert (engine_groups(session) == new_id).all()

    # Exit flushes into the Blender face-set attribute.
    exit_mode()
    attr = ob.data.attributes.get(".sculpt_face_set")
    assert attr is not None
    flushed = np.zeros(len(ob.data.polygons), dtype=np.int32)
    attr.data.foreach_get("value", flushed)
    assert (flushed == new_id).all()


def test_face_set_edit():
    import sculptcore_addon.convert as convert
    import sculptcore_addon.engine as engine

    def engine_groups(session):
        values = np.zeros(convert.mesh_face_num(session.mesh_ptr), dtype=np.int32)
        engine.capi().lib.Mesh_readFaceIntAttr(session.mesh_ptr, convert._SC_GROUP, values)
        return values

    ob = fresh_sphere("FSE")
    bpy.ops.ed.undo_push(message="Base")
    session = enter("FSE")

    # Upper-hemisphere face set (via mask), rest unset (0).
    verts = positions(ob)
    seed = (verts[:, 2] > 0.3).astype(np.float32)
    engine.capi().lib.Mesh_writeVertFloatAttr(
        session.mesh_ptr, convert._SC_MASK, np.ascontiguousarray(seed))
    bpy.ops.sculptcore.face_sets_create(mode='MASKED')
    groups = engine_groups(session)
    target = groups.max()
    count0 = int((groups == target).sum())
    assert 0 < count0 < len(groups), "seed set should cover part of the sphere"

    # Headless execute has no cursor pick; the fallback edits the highest set.
    bpy.ops.sculptcore.face_set_edit(mode='GROW')
    count_grow = int((engine_groups(session) == target).sum())
    assert count_grow > count0, "grow did not extend the set"

    bpy.ops.sculptcore.face_set_edit(mode='SHRINK')
    bpy.ops.sculptcore.face_set_edit(mode='SHRINK')
    count_shrink = int((engine_groups(session) == target).sum())
    assert count_shrink < count0, "two shrinks did not pull inside the seed set"

    bpy.ops.ed.undo()  # shrink 2
    bpy.ops.ed.undo()  # shrink 1
    assert int((engine_groups(session) == target).sum()) == count_grow
    exit_mode()


def test_smooth_semantics():
    import sculptcore_addon.stroke as stroke
    import sculptcore_addon.engine as engine_mod
    import sculptcore_addon.mapping as mapping

    # Vanilla iteration_strengths shapes.
    assert stroke.smooth_iteration_strengths(1.0) == [1.0] * 4
    assert stroke.smooth_iteration_strengths(0.5) == [1.0, 1.0]
    passes = stroke.smooth_iteration_strengths(0.3)
    assert len(passes) == 2 and passes[0] == 1.0 and abs(passes[1] - 0.2) < 1e-6
    assert stroke.smooth_iteration_strengths(0.0) == []
    assert stroke.smooth_iteration_strengths(2.0) == [1.0] * 4, "strength clamps to 1"

    # Smoothing maps to the boundary-aware kernel.
    ob = fresh_sphere("SMS")
    enter("SMS")
    brush = bpy.context.tool_settings.sculpt.brush
    prev = brush.sculpt_brush_type
    brush.sculpt_brush_type = 'SMOOTH'
    mgr = engine_mod.manager()
    bsmooth = int(mgr.get("sculptcore::brush::SculptBrushes").items["BSMOOTH"])
    assert mapping.kernel_enum(mgr, brush) == bsmooth
    brush.sculpt_brush_type = prev
    exit_mode()


def test_dyntopo_detail_settings():
    import sculptcore_addon.stroke as stroke

    ob = fresh_sphere("DTD")
    ob.scale = (2.0, 2.0, 2.0)  # exercises the object scale in CONSTANT
    bpy.context.view_layer.update()
    sd = bpy.context.scene.tool_settings.sculpt
    px = 1.0

    sd.detail_type_method = 'BRUSH'
    sd.detail_percent = 25.0
    assert abs(stroke.dyntopo_max_edge(sd, ob, 0.4, 100, px) - 0.1) < 1e-6

    sd.detail_type_method = 'RELATIVE'
    sd.detail_size = 12.0
    expect = (0.4 / 100) * 12.0 * px / 0.4
    assert abs(stroke.dyntopo_max_edge(sd, ob, 0.4, 100, px) - expect) < 1e-6

    sd.detail_type_method = 'CONSTANT'
    sd.constant_detail_resolution = 5.0
    expect = 1.0 / (5.0 * 2.0)
    assert abs(stroke.dyntopo_max_edge(sd, ob, 0.0, 100, px) - expect) < 1e-6

    # Refine-method mapping covers every enum item.
    items = sd.bl_rna.properties["detail_refine_method"].enum_items
    for item in items:
        assert item.identifier in stroke._DYNTOPO_REFINE_MODES, item.identifier

    # Engine remesher tuning flows into DynTopoParams; an unset refine
    # method (older files: DNA flags 0 -> RNA '') falls back to Both.
    scene = bpy.context.scene
    session = enter("DTD")
    params = stroke.build_dyntopo_params(session, 0.1, 0.04)
    scene.sculptcore_dyntopo_flips = False
    scene.sculptcore_dyntopo_smooth = True
    scene.sculptcore_dyntopo_smooth_lambda = 0.25
    scene.sculptcore_dyntopo_max_rounds = 7
    scene.sculptcore_dyntopo_split_budget = 1234
    scene.sculptcore_dyntopo_collapse_budget = 55
    stroke.configure_dyntopo_params(params, scene, 'COLLAPSE')
    assert int(params.mode) == 1 and params.do_flips is False
    assert params.do_smooth is True and abs(params.smooth_lambda - 0.25) < 1e-6
    assert params.max_rounds == 7 and params.max_splits == 1234
    assert params.max_collapses == 55
    stroke.configure_dyntopo_params(params, scene, '')
    assert int(params.mode) == 2, "unset refine method must fall back to Both"
    exit_mode()


def test_subdivision_set_op():
    ob = fresh_sphere("SDS")
    md = ob.modifiers.new("Multires", 'MULTIRES')
    for _ in range(3):
        bpy.ops.object.multires_subdivide(modifier="Multires")
    md.sculpt_levels = 2
    enter("SDS")
    assert bpy.ops.sculptcore.subdivision_set.poll()
    bpy.ops.sculptcore.subdivision_set(level=1, relative=False)
    assert md.sculpt_levels == 1
    bpy.ops.sculptcore.subdivision_set(level=1, relative=True)
    assert md.sculpt_levels == 2
    bpy.ops.sculptcore.subdivision_set(level=99, relative=False)
    assert md.sculpt_levels == md.total_levels, "level clamps to the stack top"
    exit_mode()


def test_lifecycle_handlers():
    import sculptcore_addon.engine as engine
    import sculptcore_addon.handlers as handlers
    assert handlers._on_undo_redo in bpy.app.handlers.undo_post
    assert handlers._on_load in bpy.app.handlers.load_post
    ob = fresh_sphere("LC")
    session = enter("LC")
    handlers._reconcile()
    assert "LC" in engine.sessions, "reconcile freed an in-mode session"
    exit_mode()
    # Object deleted under a (re-created) session -> reconcile frees it.
    ob = fresh_sphere("LC2")
    session = enter("LC2")
    bpy.data.objects.remove(ob, do_unlink=True)
    handlers._reconcile()
    assert "LC2" not in engine.sessions and session._freed, "stale session not reconciled"
    # load_post drops everything.
    ob = fresh_sphere("LC3")
    enter("LC3")
    handlers._on_load()
    assert not engine.sessions, "load_post did not clear sessions"


SECTIONS = [
    ("enter/exit round trip", test_enter_exit_roundtrip),
    ("stroke moves + flush", test_stroke_moves_and_flushes),
    ("brush parity + guard", test_brush_parity),
    ("grab family", test_grab_family),
    ("mask round trip", test_mask_roundtrip),
    ("face-set round trip", test_faceset_roundtrip),
    ("color round trip", test_color_roundtrip),
    ("falloff presets", test_falloff_presets),
    ("autosmooth program", test_autosmooth),
    ("dyntopo + slow flush", test_dyntopo),
    ("tool + panels", test_tool_and_panels),
    ("mask flood fill", test_mask_flood_fill),
    ("mask filter", test_mask_filter),
    ("face sets create", test_face_sets_create),
    ("face set edit", test_face_set_edit),
    ("smooth semantics", test_smooth_semantics),
    ("dyntopo detail settings", test_dyntopo_detail_settings),
    ("subdivision set", test_subdivision_set_op),
    ("lifecycle handlers", test_lifecycle_handlers),
]


def main():
    _enable()
    failures = 0
    for name, fn in SECTIONS:
        # Fresh scene per section for isolation.
        bpy.ops.wm.read_factory_settings(use_empty=True)
        _enable()  # read_factory_settings keeps addons; ensure anyway.
        try:
            fn()
            print("ok   " + name)
        except Exception:
            failures += 1
            print("FAIL " + name)
            traceback.print_exc()
    print("\n%d/%d sections passed" % (len(SECTIONS) - failures, len(SECTIONS)))
    if failures:
        sys.exit(1)
    print("ALL-OK")


main()
