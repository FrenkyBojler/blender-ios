# SPDX-FileCopyrightText: 2023 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import (
    Panel,
)

class USERPREF_PT_vr_navigation(Panel):
    bl_space_type = 'PREFERENCES'
    bl_region_type = 'WINDOW'
    bl_context = "navigation"
    bl_label = "VR Navigation"

    def draw(self, context):
        layout = self.layout
        width = context.region.width
        ui_scale = context.preferences.system.ui_scale
        # No horizontal margin if region is rather small.
        is_wide = width > (350 * ui_scale)

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        row = layout.row()
        if is_wide:
            row.label()  # Needed so col below is centered.

        col = row.column()
        col.ui_units_x = 50

        # Implemented by sub-classes.
        self.draw_centered(context, col)

        if is_wide:
            row.label()  # Needed so col above is centered.

    def draw_centered(self, context, layout):
        prefs = context.preferences
        inputs = prefs.inputs

        col = layout.column()

        col.row().prop(inputs, "xr_snap_turn", text="Snap Turn")
        col.row().prop(inputs, "xr_invert_rotation", text="Invert Rotation")
        col.row().prop(inputs, "xr_single_controller", text="Single Controller Mode")

classes = (
    USERPREF_PT_vr_navigation,
)

def register():
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)

def unregister():
    from bpy.utils import unregister_class
    for cls in classes:
        unregister_class(cls)
