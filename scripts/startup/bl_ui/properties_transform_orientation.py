"""
Transform Orientation proxy collection and operators.

Provides a unified ``template_list`` that merges the built-in enum
orientations (Global, Local, …) and user-created custom orientations
into a single selectable list. A pair of wrapper operators handle
creation / deletion while keeping the proxy in sync.
"""

import bpy
from bpy.types import (
    Operator,
    PropertyGroup,
    UIList,
)
from bpy.props import (
    BoolProperty,
    CollectionProperty,
    IntProperty,
    StringProperty,
)


# Proxy PropertyGroups

def on_proxy_item_renamed(self, context):
    """Sync a renamed proxy item back to the real custom orientation."""
    if not self.is_custom:
        return
    scene = context.scene
    old_name = self.identifier
    new_name = self.name
    if old_name == new_name:
        return

    # Check for duplicate names among other custom orientations.
    for co in scene.custom_transform_orientations:
        if co.name != old_name and co.name == new_name:
            # Revert to the old name and show an error.
            self.name = old_name
            def _draw_error(self_menu, _ctx):
                self_menu.layout.label(
                    text=f"Orientation '{new_name}' already exists",
                    icon='ERROR',
                )
            bpy.context.window_manager.popup_menu(_draw_error, title="Rename Error")
            return

    # Also check if the new name collides with a built-in orientation identifier.
    slot = scene.transform_orientation_slots[0]
    prop = slot.bl_rna.properties["type"]
    custom_names = {co.name for co in scene.custom_transform_orientations}
    for ei in prop.enum_items:
        if ei.identifier not in custom_names and ei.name == new_name:
            self.name = old_name
            return

    for co in scene.custom_transform_orientations:
        if co.name == old_name:
            co.name = new_name
            self.identifier = new_name
            slot = scene.transform_orientation_slots[0]
            if slot.type == old_name:
                slot.type = new_name
            break


class VIEW3D_PG_transform_orientation_item(PropertyGroup):
    """Single entry inside the proxy orientation list."""
    name: StringProperty(update=on_proxy_item_renamed)
    identifier: StringProperty()
    icon: StringProperty()
    is_custom: BoolProperty(default=False)


# active-index get / set (bound to the list-owner below)

def get_active_index(_self):
    scene = bpy.context.scene
    if not hasattr(scene, "orientations_proxy"):
        return 0
    proxy = scene.orientations_proxy
    slot = scene.transform_orientation_slots[0]
    current_type = slot.type

    for i, item in enumerate(proxy.items):
        if item.identifier == current_type:
            return i

    # Proxy may be stale – force rebuild and retry.
    update_proxy_collection(scene)
    for i, item in enumerate(proxy.items):
        if item.identifier == current_type:
            return i

    # Fallback to last item
    if len(proxy.items) > 0:
        return len(proxy.items) - 1
    return 0


def set_active_index(_self, value):
    scene = bpy.context.scene
    if not hasattr(scene, "orientations_proxy"):
        return
    proxy = scene.orientations_proxy
    slot = scene.transform_orientation_slots[0]
    if 0 <= value < len(proxy.items):
        slot.type = proxy.items[value].identifier


class VIEW3D_PG_transform_orientation_list(PropertyGroup):
    """Owner of the proxy orientation collection."""
    items: CollectionProperty(type=VIEW3D_PG_transform_orientation_item)
    active_idx: IntProperty(
        name="Active Orientation",
        get=get_active_index,
        set=set_active_index,
    )


# Proxy rebuild helper

def update_proxy_collection(scene):
    """Rebuild the proxy collection with default enums + custom orientations."""
    proxy = scene.orientations_proxy
    proxy_items = proxy.items

    slot = scene.transform_orientation_slots[0]
    prop = slot.bl_rna.properties["type"]

    # Collect default (static) enum items.
    custom_names = {co.name for co in scene.custom_transform_orientations}
    static_enums = []
    for item in prop.enum_items:
        if item.identifier not in custom_names:
            icon = item.icon if hasattr(item, "icon") else 'NONE'
            static_enums.append((item.identifier, item.name, icon))

    custom_orientations = list(scene.custom_transform_orientations)
    total_expected = len(static_enums) + len(custom_orientations)

    # Only rebuild when contents actually changed.
    needs_rebuild = (len(proxy_items) != total_expected)
    if not needs_rebuild:
        for i, (uid, _name, _icon) in enumerate(static_enums):
            if proxy_items[i].identifier != uid:
                needs_rebuild = True
                break
    if not needs_rebuild:
        offset = len(static_enums)
        for i, co in enumerate(custom_orientations):
            if proxy_items[offset + i].identifier != co.name:
                needs_rebuild = True
                break

    if needs_rebuild:
        proxy_items.clear()
        for uid, name, icon in static_enums:
            item = proxy_items.add()
            item.identifier = uid
            item.name = name
            item.icon = icon or 'NONE'
            item.is_custom = False
        for co in custom_orientations:
            item = proxy_items.add()
            item.identifier = co.name
            item.name = co.name
            item.icon = 'OBJECT_ORIGIN'
            item.is_custom = True


# Wrapper operators around bpy.ops.transform.create_orientation and bpy.ops.transform.delete_orientation
class TRANSFORM_OT_create_orientation_proxy(Operator):
    """Create transform orientation from selection"""
    bl_idname = "transform.create_orientation_proxy"
    bl_label = "Create Orientation"
    bl_options = {'REGISTER', 'UNDO'}

    name: StringProperty(name="Name", default="")
    use: BoolProperty(name="Use After Creation", default=True)
    use_view: BoolProperty(name="Use View", default=False)
    overwrite: BoolProperty(name="Overwrite Previous", default=False)

    def execute(self, context):
        result = bpy.ops.transform.create_orientation(
            name=self.name,
            use=self.use,
            use_view=self.use_view,
            overwrite=self.overwrite,
        )
        if 'FINISHED' in result:
            scene = context.scene
            if hasattr(scene, "orientations_proxy"):
                update_proxy_collection(scene)
        return {'FINISHED'}


class TRANSFORM_OT_delete_orientation_proxy(Operator):
    """Delete the active custom transform orientation"""
    bl_idname = "transform.delete_orientation_proxy"
    bl_label = "Delete Orientation"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        scene = context.scene
        slot = scene.transform_orientation_slots[0]
        return slot.custom_orientation is not None

    def execute(self, context):
        scene = context.scene

        # Remember the current position so we can select the next item.
        prev_idx = 0
        if hasattr(scene, "orientations_proxy"):
            proxy = scene.orientations_proxy
            slot = scene.transform_orientation_slots[0]
            for i, item in enumerate(proxy.items):
                if item.identifier == slot.type:
                    prev_idx = i
                    break

        result = bpy.ops.transform.delete_orientation()
        if 'FINISHED' in result:
            if hasattr(scene, "orientations_proxy"):
                update_proxy_collection(scene)
                proxy = scene.orientations_proxy
                if len(proxy.items) > 0:
                    new_idx = min(prev_idx, len(proxy.items) - 1)
                    slot = scene.transform_orientation_slots[0]
                    slot.type = proxy.items[new_idx].identifier
        return {'FINISHED'}


# UIList
class VIEW3D_UL_transform_orientations(UIList):
    """Draw a single row in the orientation template_list."""
    def draw_item(self, _context, layout, _data, item, _icon, _active_data, _active_propname):
        if self.layout_type in {'DEFAULT', 'COMPACT'}:
            if item.is_custom:
                layout.prop(
                    item, "name", text="",
                    icon=item.icon if item.icon else 'NONE',
                    emboss=False,
                )
            else:
                layout.label(text=item.name, icon=item.icon if item.icon else 'NONE')
        elif self.layout_type == 'GRID':
            layout.alignment = 'CENTER'
            layout.label(text="", icon=item.icon if item.icon else 'NONE')


# Registration

classes = (
    VIEW3D_PG_transform_orientation_item,
    VIEW3D_PG_transform_orientation_list,
    TRANSFORM_OT_create_orientation_proxy,
    TRANSFORM_OT_delete_orientation_proxy,
    VIEW3D_UL_transform_orientations,
)
