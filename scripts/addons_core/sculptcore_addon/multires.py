# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Multires import/export (P8).

Convert Blender's `CD_MDISPS` multires displacement into a SculptCore
`Multires` stack on enter and bake it back on flush/exit. The multires modifier
itself is ignored while the mode is active. Both directions round-trip
*absolute top-level positions* (never frames): SculptCore's base is discrete
Catmull-Clark, Blender's is the CC limit surface, but the absolute-position
exchange absorbs the difference (see claudeMemory/research/grid-correspondence.md).

The engine grid samples and Blender's subdivided vertices are two samplings of
the same cage; they are paired by nearest-neighbour on the *undisplaced* base
surface (a stable bijection at usable levels), and the map is cached per session.
"""

from . import engine


def modifier(ob):
    """The object's multires modifier, or None."""
    for md in ob.modifiers:
        if md.type == 'MULTIRES':
            return md
    return None


def _base_reference_positions(context, base_arrays, level):
    """Undisplaced subdivided-base vertex positions in multires-eval order, via
    a throwaway object built from the cage arrays (no MDISPS) carrying a fresh
    multires subdivided to `level`. This is the neutral reference the engine and
    the real object's samples are matched against."""
    import bpy
    import numpy as np

    positions, corner_verts, face_offsets = base_arrays
    verts = positions.reshape(-1, 3).tolist()
    faces = [
        [int(v) for v in corner_verts[face_offsets[f]:face_offsets[f + 1]]]
        for f in range(len(face_offsets) - 1)
    ]
    mesh = bpy.data.meshes.new("_sc_multires_ref")
    mesh.from_pydata(verts, [], faces)

    obj = bpy.data.objects.new("_sc_multires_ref", mesh)
    context.scene.collection.objects.link(obj)
    try:
        obj.modifiers.new("multires", 'MULTIRES')
        with context.temp_override(object=obj, active_object=obj, selected_objects=[obj]):
            for _ in range(level):
                bpy.ops.object.multires_subdivide(modifier="multires")
        depsgraph = context.evaluated_depsgraph_get()
        depsgraph.update()
        eval_mesh = obj.evaluated_get(depsgraph).data
        base = np.empty(len(eval_mesh.vertices) * 3, dtype=np.float64)
        eval_mesh.vertices.foreach_get("co", base)
        base = base.reshape(-1, 3)
    finally:
        bpy.data.objects.remove(obj, do_unlink=True)
        bpy.data.meshes.remove(mesh)
    return base


def _nearest(query, reference):
    """For each row of `query`, the index of the nearest row in `reference`
    (KD-tree — the brute-force pairing is O(N*M) and unusable at production
    vertex counts)."""
    import numpy as np
    from mathutils.kdtree import KDTree

    tree = KDTree(len(reference))
    for i, co in enumerate(reference):
        tree.insert(co, i)
    tree.balance()
    idx = np.empty(len(query), dtype=np.int64)
    for i, co in enumerate(query):
        _co, index, _dist = tree.find(co)
        idx[i] = index
    return idx


class MultiresMap:
    """Correspondence between engine grid samples and Blender subdivided
    vertices for one object, built once from the undisplaced base."""

    def __init__(self, level, engine_sample_to_blender, blender_to_engine_sample):
        self.level = level
        # engine grid-sample index -> Blender subdiv-vertex index (import seed).
        self.engine_sample_to_blender = engine_sample_to_blender
        # Blender subdiv-vertex index -> engine grid-sample index (export bake).
        self.blender_to_engine_sample = blender_to_engine_sample


def build_engine(base_arrays, level):
    """Build an engine Multires stack over the base cage. Returns (mr, cage);
    the caller keeps `cage` alive for the stack's lifetime and frees both."""
    lib = engine.capi().lib
    positions, corner_verts, face_offsets = base_arrays
    cage = lib.Mesh_fromArrays(
        positions, len(positions) // 3,
        corner_verts, len(corner_verts),
        face_offsets, len(face_offsets) - 1,
    )
    if not cage:
        raise engine.EngineError("SculptCore: engine rejected multires cage mesh")
    mr = lib.Multires_new(cage, level, 0, 0, 0)
    if not mr:
        lib.freeMesh(cage)
        raise engine.EngineError("SculptCore: Multires_new failed")
    return mr, cage


def build_map(context, base_arrays, mr_ptr, level):
    """Pair engine grid samples with Blender subdiv vertices on the base
    surface. `mr_ptr` is a freshly built (undisplaced) engine Multires."""
    import numpy as np

    lib = engine.capi().lib
    count = lib.Multires_levelSampleCount(mr_ptr, level)
    engine_base = np.empty(count * 3, dtype=np.float32)
    lib.Multires_levelPositionsOut(mr_ptr, level, engine_base)
    engine_base = engine_base.reshape(-1, 3).astype(np.float64)

    blender_base = _base_reference_positions(context, base_arrays, level)
    engine_sample_to_blender = _nearest(engine_base, blender_base)
    blender_to_engine_sample = _nearest(blender_base, engine_base)
    return MultiresMap(level, engine_sample_to_blender, blender_to_engine_sample)


def import_displacement(mr_ptr, mapping, blender_top_positions):
    """Seed the engine stack from Blender's displaced top-level positions
    (subdiv-vertex order). Returns the changed-vert count."""
    import numpy as np

    lib = engine.capi().lib
    # Each engine grid sample takes the displaced position of its paired
    # Blender subdiv vertex (seam replicas resolve to equal values).
    seed = np.ascontiguousarray(
        blender_top_positions[mapping.engine_sample_to_blender], dtype=np.float32)
    return lib.Multires_fromLevelPositions(
        mr_ptr, mapping.level, seed.reshape(-1), len(seed))


def export_bake(ob, depsgraph, mr_ptr, mapping):
    """Bake the engine stack's top-level surface into the object's CD_MDISPS
    via the dedup subdiv-vertex reshape seam."""
    import numpy as np

    lib = engine.capi().lib
    count = lib.Multires_levelSampleCount(mr_ptr, mapping.level)
    engine_top = np.empty(count * 3, dtype=np.float32)
    lib.Multires_levelPositionsOut(mr_ptr, mapping.level, engine_top)
    engine_top = engine_top.reshape(-1, 3)

    vertcos = np.ascontiguousarray(
        engine_top[mapping.blender_to_engine_sample].reshape(-1), dtype=np.float32)
    ob.multires_reshape_from_vert_positions(depsgraph, vertcos)
    ob.data.update_tag()
