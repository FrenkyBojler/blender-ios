# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Generated engine-only brush properties (brush-mapping M2).

At registration the per-kernel uniform manifest is walked (through a
throwaway engine mesh/tree/brush/executor) and every engine-only float
uniform becomes a ``FloatProperty`` on ``Brush.sculptcore`` — name, default
and range straight from the manifest. The group serializes with the Brush
datablock (asset-compatible). ``mapping.apply_brush`` copies the active
kernel's generated values into the engine brush fields each dab, and the
N-panel draws them in an "Engine" section (M3).

Uniforms the Blender mapping already drives (the common scalar props and the
per-type extras) are excluded, as are non-float uniforms and manifest names
with no bound engine-brush field (the plain ``execBrush`` path reads fields,
not props).
"""

import bpy

from . import engine, mapping

# Uniform names the Blender mapping already drives.
_MAPPED = {"strength", "radius", "spacing", "planeoff", "autosmooth", "pinch", "invert"}

_group_cls = None
# Engine kernel name -> tuple of generated prop names in its manifest.
_kernel_props = {}


def props_for_type(sculpt_brush_type):
    """Generated prop names for a Blender sculpt brush type (may be empty)."""
    entry = mapping.KERNEL_BY_TYPE.get(sculpt_brush_type)
    if entry is None:
        return ()
    return _kernel_props.get(entry, ())


def apply(bl_brush, sc_brush):
    """Copy the generated engine props into the engine brush fields for the
    brush's kernel. Called from mapping.apply_brush before writeProps."""
    names = props_for_type(bl_brush.sculpt_brush_type)
    if not names:
        return
    group = getattr(bl_brush, "sculptcore", None)
    if group is None:
        return
    for name in names:
        setattr(sc_brush, name, getattr(group, name))


def _walk_manifests():
    """Per-kernel engine-only float uniforms via a throwaway executor:
    {kernel_name: [(name, default, has_range, min, max)]}."""
    import numpy as np

    lib = engine.capi().lib
    mgr = engine.manager()

    positions = np.array([0, 0, 0, 1, 0, 0, 0, 1, 0], dtype=np.float32)
    corner_verts = np.array([0, 1, 2], dtype=np.int32)
    face_offsets = np.array([0, 3], dtype=np.int32)
    mesh_ptr = lib.Mesh_fromArrays(positions, 3, corner_verts, 3, face_offsets, 1)
    tree_ptr = lib.Mesh_buildSpatialTree(mesh_ptr, 0, 0, 0)

    tree = mgr.get_bound_pointer(
        mgr.get("sculptcore::spatial::SpatialTree"), tree_ptr, deref=False)
    brush = mgr.construct("sculptcore::brush::Brush")
    ctor = mgr.get_struct("sculptcore::brush::CommandExecutor").find_constructor("main")
    executor = mgr.construct_with(ctor, tree, brush)

    # The reflected `name` member is a litestl string wrapper; read its
    # contents through the runtime's string reader.
    from sculptcore._descriptors import read_litestl_string

    manifests = {}
    try:
        items = mgr.get("sculptcore::brush::SculptBrushes").items
        for kernel_name in sorted(set(mapping.KERNEL_BY_TYPE.values())):
            count = executor.queryUniformManifest(int(items[kernel_name]))
            entries = []
            for i in range(count):
                entry = executor.queriedUniformEntry(i)
                if entry is None or not entry.isFloat:
                    continue
                name = read_litestl_string(entry.name.ptr)
                if name in _MAPPED or not hasattr(brush, name):
                    continue
                # Default from the engine brush FIELD, not the DSL `def`: the
                # plain dab path reads fields, and their authored defaults are
                # the current behavior (e.g. planeSide is +1 as a field but 0
                # in the DSL — a generated 0 would break the plane family).
                entries.append((name, float(getattr(brush, name)),
                                bool(entry.hasRange),
                                float(entry.rangeMin), float(entry.rangeMax)))
            if entries:
                manifests[kernel_name] = entries
    finally:
        executor.dispose()
        brush.dispose()
        lib.SpatialTree_free(tree_ptr)
        lib.freeMesh(mesh_ptr)
    return manifests


def register():
    """Generate and register Brush.sculptcore. Best-effort: without the
    engine the group is skipped and the mode falls back to defaults."""
    global _group_cls, _kernel_props
    try:
        manifests = _walk_manifests()
    except Exception as ex:
        print("SculptCore: engine prop generation unavailable ({!r})".format(ex))
        return

    annotations = {}
    union = {}
    for kernel_name, entries in sorted(manifests.items()):
        names = []
        for name, default, has_range, range_min, range_max in entries:
            names.append(name)
            if name in union:
                continue
            union[name] = True
            kwargs = {"name": name, "default": default}
            if has_range:
                kwargs["min"] = range_min
                kwargs["max"] = range_max
            annotations[name] = bpy.props.FloatProperty(**kwargs)
        _kernel_props[kernel_name] = tuple(names)

    if not annotations:
        return
    _group_cls = type("SculptCoreBrushSettings", (bpy.types.PropertyGroup,),
                      {"__annotations__": annotations})
    bpy.utils.register_class(_group_cls)
    bpy.types.Brush.sculptcore = bpy.props.PointerProperty(
        type=_group_cls, name="SculptCore",
        description="Engine-only brush settings (generated from the kernel manifests)")


def unregister():
    global _group_cls
    _kernel_props.clear()
    if _group_cls is None:
        return
    if hasattr(bpy.types.Brush, "sculptcore"):
        del bpy.types.Brush.sculptcore
    bpy.utils.unregister_class(_group_cls)
    _group_cls = None
