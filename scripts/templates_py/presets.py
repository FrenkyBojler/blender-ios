import bpy
from bl_operators.presets import AddPresetBase
from bl_ui.utils import PresetPanel


# Subdirectory inside "presets" directory where preset Python files will be
# stored & read from. "presets" directory itself can change depending on user
# preferences and Blender configuration, therefore absolute path isn't kept.
my_presets_subdir = "object/transforms"


# Creating a menu that will display presets as items.
class PROPERTIES_MT_object_transforms_presets(bpy.types.Menu):
    bl_label = "Object Transform Presets"
    preset_operator = "script.execute_preset"
    preset_subdir = my_presets_subdir

    draw = bpy.types.Menu.draw_preset


# Operator for adding and removing presets
class PROPERTIES_OT_object_transforms_presets_add(bpy.types.Operator, AddPresetBase):
    bl_idname = "object.transform_presets_add"
    bl_label = "Add Object Transform Presets"
    preset_subdir = my_presets_subdir

    # Preset menu that needs to be updated. Must point to one defined above.
    preset_menu = "PROPERTIES_MT_object_transforms_presets"

    # Common variables for preset properties.
    preset_defines = [
        "obj = bpy.context.object",
    ]

    # Properties that should be stored in the preset.
    preset_values = [
        "obj.location",
        "obj.rotation_euler",
        "obj.rotation_quaternion",
        "obj.rotation_axis_angle",
        "obj.rotation_mode",
        "obj.scale",

        "obj.delta_location",
        "obj.delta_rotation_euler",
        "obj.delta_rotation_quaternion",
        "obj.delta_scale",
    ]


# Panel that will hold prests menu and operators for adding/removing presets.
class PROPERTIES_PT_object_transforms_presets(PresetPanel, bpy.types.Panel):
    bl_label = "Object Transform Presets"
    preset_subdir = my_presets_subdir
    preset_operator = "script.execute_preset"

    # `bl_idname` of the operator that will add/remove presets. Must point to one defined above.
    preset_add_operator = "object.transform_presets_add"

    # Optional poll for defining the context in which the panel can appear.
    @classmethod
    def poll(cls, context):
        return context.space_data.type == 'PROPERTIES'


# Function that will append presets panel to existing layout.
"""NOTE:
If presets are registered on custom panels that add-on creates, this function can be included
inside the panel class as a method. Removing the text from `layout.popover`, and also removing
emboss with `layout.emboss = 'NONE' before it's declared will give the same exact look as
Blender's built-in presets in panel headers.
"""
def draw_header_preset(self, context):
    layout = self.layout
    layout.popover("PROPERTIES_PT_object_transforms_presets",
                   text="Object Transforms Presets", icon='PRESET')


# Register classes.
classes = [
    PROPERTIES_MT_object_transforms_presets,
    PROPERTIES_OT_object_transforms_presets_add,
    PROPERTIES_PT_object_transforms_presets,
]
for cls in classes:
    bpy.utils.register_class(cls)

# Append layout function to existing UI.
# If function is included inside a custom UI this is not needed anymore.
bpy.types.OBJECT_PT_transform.prepend(draw_header_preset)
