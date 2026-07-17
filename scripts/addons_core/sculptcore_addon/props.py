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
    bpy.types.Scene.sculptcore_detail = bpy.props.FloatProperty(
        name="Detail Size",
        description="Target edge length for dynamic topology, in object space",
        default=0.05,
        min=0.005,
        max=1.0,
        soft_max=0.5,
    )


def unregister():
    del bpy.types.Scene.sculptcore_dyntopo
    del bpy.types.Scene.sculptcore_detail
