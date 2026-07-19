# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Scale + performance gate for the plain-Mesh path (P3 / P5 verification).

Measures the conversion paths that P3 puts a number on -- enter (build the
engine mesh + spatial tree from the Mesh ID) and the positions-only fast-path
flush -- plus a representative DRAW stroke, at increasing vertex counts. Enter
is broken into its phases (array gather, `Mesh_fromArrays`, spatial-tree build,
attribute load, draw-provider register/fill) so a hidden O(n^2) shows up as a
phase that scales super-linearly, not just a big total.

It also runs the byte-identical no-stroke round trip P3 asks for over a small
topology corpus (quad grid, triangulated grid, an n-gon fan): enter then flush
with no stroke must reproduce the input positions exactly.

Targets recorded from the plan (informational -- the harness prints PASS/WARN
against them but only *fails* on a correctness error, so it is a benchmark, not
a flaky wall-clock gate):
  - enter <= a few hundred ms
  - fast-path flush <= tens of ms
at ~1M vertices.

Run:
    set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
    blender --factory-startup --python claudeMemory/scripts/run_sync.py -- \
        claudeMemory/tests/scale_bench.py
Optional: SCULPTCORE_BENCH_SIZES="50000,200000,1000000" overrides the sizes.
Exits nonzero only on a correctness failure (round-trip mismatch, crash).
"""

import math
import os
import sys
import time

import numpy as np
import bpy

# Informational thresholds (see module docstring); scaled to the mesh size.
ENTER_MS_AT_1M = 400.0
FLUSH_MS_AT_1M = 50.0


def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def _sizes():
    raw = os.environ.get("SCULPTCORE_BENCH_SIZES")
    if raw:
        return [int(x) for x in raw.split(",") if x.strip()]
    return [50_000, 200_000, 500_000, 1_000_000]


def positions(ob):
    mesh = ob.data
    a = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", a)
    return a.reshape(-1, 3)


def _fresh_grid(target_verts, name):
    """A flat quad grid with about `target_verts` vertices, centered at the
    origin spanning [-1, 1]^2 (so a world-space brush radius is meaningful)."""
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()
    # grid_add's subdivisions are the vertex counts per side.
    side = max(2, int(round(math.sqrt(target_verts))))
    bpy.ops.mesh.primitive_grid_add(x_subdivisions=side, y_subdivisions=side, size=2.0)
    ob = bpy.context.active_object
    ob.name = name
    return ob


class Timer:
    def __enter__(self):
        self.t = time.perf_counter()
        return self

    def __exit__(self, *a):
        self.ms = (time.perf_counter() - self.t) * 1000.0


def _enter_timed(ob, convert, engine):
    """Replicate convert.enter's plain-Mesh path with a per-phase timer,
    registering a real session so the downstream flush/stroke are the shipping
    code paths. Returns (session, {phase: ms})."""
    from sculptcore_addon.session import Session

    capi = engine.capi()
    lib = capi.lib
    phases = {}

    with Timer() as t:
        pos, corner_verts, face_offsets = convert._gather_arrays(ob.data)
    phases["gather"] = t.ms
    verts_num = len(pos) // 3

    with Timer() as t:
        mesh_ptr = lib.Mesh_fromArrays(
            pos, verts_num, corner_verts, len(corner_verts),
            face_offsets, len(face_offsets) - 1)
    phases["fromArrays"] = t.ms
    if not mesh_ptr:
        _fail("engine rejected mesh at {:d} verts".format(verts_num))

    with Timer() as t:
        tree_ptr = lib.Mesh_buildSpatialTree(mesh_ptr, 0, 0, 0)
    phases["buildTree"] = t.ms
    if not tree_ptr:
        _fail("spatial tree build failed at {:d} verts".format(verts_num))

    with Timer() as t:
        convert._load_mask(ob.data, mesh_ptr, verts_num)
        convert._load_face_sets(ob.data, mesh_ptr)
        convert._load_color(ob.data, mesh_ptr, verts_num)
        convert._load_uv(ob.data, mesh_ptr)
    phases["loadAttrs"] = t.ms

    session = Session(ob.name, mesh_ptr, tree_ptr, verts_num)
    engine.sessions[ob.name] = session

    with Timer() as t:
        session.draw_key = int(ob.session_uid)
        lib.sc_external_draw_register(session.draw_key, tree_ptr)
        lib.sc_external_draw_enable_dynamic(tree_ptr)
        lib.sc_external_draw_update(session.draw_key)
    phases["drawProvider"] = t.ms

    phases["total"] = sum(v for k, v in phases.items() if k != "total")
    return session, phases


def _stroke(ob, session, convert, engine, mapping, strokemod, ndabs=24):
    """A DRAW stroke of `ndabs` dabs swept across the surface; returns
    (total_ms, per_dab_ms, flush_ms)."""
    mgr = engine.manager()
    bl_brush = bpy.data.brushes.get("scp_bench")
    if bl_brush is None:
        bl_brush = bpy.data.brushes.new("scp_bench", mode='SCULPT')
    bl_brush.sculpt_brush_type = 'DRAW'
    bl_brush.strength = 0.5
    sc_brush = strokemod._ensure_brush(session)
    kernel = mapping.kernel_enum(mgr, bl_brush)
    radius = 0.25
    mapping.apply_brush(bl_brush, None, sc_brush, world_radius=radius, invert=False)

    touched = 0
    with Timer() as t:
        strokemod.stroke_begin(session, has_dyntopo=False)
        for i in range(ndabs):
            x = -0.8 + 1.6 * i / (ndabs - 1)
            touched += strokemod.apply_dab(session, kernel, (x, 0.0, 0.0),
                                           (0.0, 0.0, 1.0), radius)
        strokemod.stroke_end(session)
    total = t.ms
    if touched == 0:
        _fail("stroke touched no nodes")

    with Timer() as t:
        convert.flush(ob)
    return total, total / ndabs, t.ms


def _roundtrip_corpus(convert, engine):
    """Byte-identical no-stroke round trip over a small topology corpus."""
    import bmesh

    def _quad_grid():
        return _fresh_grid(4_000, "rt_quad")

    def _tri_grid():
        ob = _fresh_grid(4_000, "rt_tri")
        mesh = ob.data
        bm = bmesh.new()
        bm.from_mesh(mesh)
        bmesh.ops.triangulate(bm, faces=bm.faces)
        bm.to_mesh(mesh)
        bm.free()
        mesh.update()
        return ob

    def _ngon_fan():
        # A single large n-gon plus its triangulated neighbours: a circle fan.
        bpy.ops.object.select_all(action='SELECT')
        bpy.ops.object.delete()
        bpy.ops.mesh.primitive_circle_add(vertices=512, fill_type='NGON', radius=1.0)
        ob = bpy.context.active_object
        ob.name = "rt_ngon"
        return ob

    for label, factory in (("quad grid", _quad_grid),
                           ("triangulated grid", _tri_grid),
                           ("n-gon fan", _ngon_fan)):
        ob = factory()
        before = positions(ob).copy()
        convert.enter(ob)
        convert.flush(ob)
        after = positions(ob)
        if before.shape != after.shape:
            _fail("{:s}: vertex count changed on round trip ({} -> {})".format(
                label, before.shape, after.shape))
        maxdiff = float(np.abs(after - before).max()) if before.size else 0.0
        if maxdiff != 0.0:
            _fail("{:s}: no-stroke round trip not byte-identical (maxdiff {:g})".format(
                label, maxdiff))
        convert.exit_(ob)
        print("PASS: {:s} round trip byte-identical ({:d} verts)".format(
            label, before.shape[0]))


def main():
    if "sculptcore_addon" not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module="sculptcore_addon")
    import sculptcore_addon.engine as engine
    from sculptcore_addon import convert, mapping
    from sculptcore_addon import stroke as strokemod

    print("=== no-stroke round-trip corpus ===")
    _roundtrip_corpus(convert, engine)

    print("\n=== scale benchmark (plain Mesh path) ===")
    header = ("verts", "gather", "fromArr", "buildTree", "loadAttr", "draw",
              "ENTER", "flush", "dab", "rtOK")
    print(("{:>9} " + "{:>8} " * 8 + "{:>5}").format(*header))

    for target in _sizes():
        ob = _fresh_grid(target, "bench")
        nverts = len(ob.data.vertices)
        before = positions(ob).copy()

        session, phases = _enter_timed(ob, convert, engine)

        # No-stroke flush + byte-identical check (the fast path on a pristine
        # session: topology unchanged, so positions-only).
        with Timer() as t:
            convert.flush(ob)
        flush_ms = t.ms
        after = positions(ob)
        rt_ok = after.shape == before.shape and np.array_equal(after, before)
        if not rt_ok:
            _fail("{:d} verts: no-stroke flush changed positions".format(nverts))

        _total, per_dab, _post_flush = _stroke(
            ob, session, convert, engine, mapping, strokemod)

        print(("{:9d} " + "{:8.1f} " * 8 + "{:>5}").format(
            nverts, phases["gather"], phases["fromArrays"], phases["buildTree"],
            phases["loadAttrs"], phases["drawProvider"], phases["total"],
            flush_ms, per_dab, "ok" if rt_ok else "BAD"))

        # Informational threshold check, scaled linearly to 1M.
        scale = nverts / 1_000_000.0
        enter_budget = ENTER_MS_AT_1M * scale
        flush_budget = FLUSH_MS_AT_1M * scale
        if phases["total"] > enter_budget * 1.5:
            print("  WARN: enter {:.0f} ms exceeds ~{:.0f} ms budget at this size".format(
                phases["total"], enter_budget))
        if flush_ms > flush_budget * 1.5:
            print("  WARN: flush {:.0f} ms exceeds ~{:.0f} ms budget at this size".format(
                flush_ms, flush_budget))

        convert.exit_(ob)

    print("\nALL PASS: scale benchmark completed (no correctness failures).")


if __name__ == "__main__":
    main()
