# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Scene-level SculptCore settings (save with the file). Minimal for now: the
dynamic-topology toggle and its target edge length. Engine-only brush
uniforms grow into a generated Brush.sculptcore group later (brush-mapping
M2).
"""

import bpy


def register():
    bpy.types.Scene.sculptcore_dyntopo = bpy.props.BoolProperty(
        name="Dynamic Topology",
        description="Dynamically remesh under the brush while sculpting",
        default=False,
    )
    # Detail size/mode come from Blender's own dyntopo settings
    # (tool_settings.sculpt.detail_*, see stroke.dyntopo_max_edge); only the
    # enable flag and the engine's remesh cadence are addon state.
    bpy.types.Scene.sculptcore_dyntopo_spacing = bpy.props.FloatProperty(
        name="Detail Spacing",
        description="Stroke travel between remesh passes, in brush diameters "
                    "(0 remeshes on every dab). Higher values remesh less often "
                    "for cheaper strokes",
        default=0.5,
        min=0.0,
        soft_max=2.0,
    )
    # Engine remesher tuning (DynTopoParams; defaults mirror the engine's).
    bpy.types.Scene.sculptcore_dyntopo_flips = bpy.props.BoolProperty(
        name="Edge Flips",
        description="Flip interior edges to the shorter diagonal each round, "
                    "keeping triangles well-shaped and refinement convergent "
                    "(disable for the pre-flip baseline behavior)",
        default=True,
    )
    bpy.types.Scene.sculptcore_dyntopo_smooth = bpy.props.BoolProperty(
        name="Tangential Smooth",
        description="Slide remeshed vertices toward their neighborhood "
                    "centroid in the tangent plane, equalizing triangle sizes "
                    "without shrinking the surface or eroding sculpted detail",
        default=False,
    )
    bpy.types.Scene.sculptcore_dyntopo_smooth_lambda = bpy.props.FloatProperty(
        name="Smooth Factor",
        description="Relaxation step for the tangential smoothing",
        default=0.5,
        min=0.0,
        max=1.0,
    )
    bpy.types.Scene.sculptcore_dyntopo_max_rounds = bpy.props.IntProperty(
        name="Max Rounds",
        description="Refinement rounds per remesh pass before giving up "
                    "(later dabs finish any remaining work)",
        default=50,
        min=1,
        soft_max=100,
    )
    bpy.types.Scene.sculptcore_dyntopo_split_budget = bpy.props.IntProperty(
        name="Split Budget",
        description="Maximum edge splits per remesh pass, bounding the cost "
                    "of a first touch on coarse geometry (0 = unlimited; "
                    "later dabs finish the refinement)",
        default=0,
        min=0,
        soft_max=100000,
    )
    bpy.types.Scene.sculptcore_dyntopo_collapse_budget = bpy.props.IntProperty(
        name="Collapse Budget",
        description="Maximum edge collapses per remesh pass, bounding the "
                    "cost of decimating dense geometry (0 = unlimited)",
        default=0,
        min=0,
        soft_max=100000,
    )
    bpy.types.Scene.sculptcore_reproject_uvs = bpy.props.BoolProperty(
        name="Reproject UVs",
        description="Re-anchor UVs when smoothing slides vertices along the "
                    "surface (smooth brushes, autosmooth and the dyntopo "
                    "tangential smooth), so textures do not swim",
        default=True,
    )
    bpy.types.Scene.sculptcore_uv_margin = bpy.props.FloatProperty(
        name="Chart Margin",
        description="Padding added around each UV chart before packing "
                    "(Project UVs from Seams)",
        default=0.01,
        min=0.0,
        max=0.25,
        subtype='FACTOR',
    )


def unregister():
    del bpy.types.Scene.sculptcore_dyntopo
    del bpy.types.Scene.sculptcore_dyntopo_spacing
    del bpy.types.Scene.sculptcore_dyntopo_flips
    del bpy.types.Scene.sculptcore_dyntopo_smooth
    del bpy.types.Scene.sculptcore_dyntopo_smooth_lambda
    del bpy.types.Scene.sculptcore_dyntopo_max_rounds
    del bpy.types.Scene.sculptcore_dyntopo_split_budget
    del bpy.types.Scene.sculptcore_dyntopo_collapse_budget
    del bpy.types.Scene.sculptcore_reproject_uvs
    del bpy.types.Scene.sculptcore_uv_margin
