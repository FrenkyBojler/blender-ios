# SPDX-FileCopyrightText: 2022-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import Menu
from bl_ui import node_add_menu
from bpy.app.translations import (
    pgettext_iface as iface_,
    contexts as i18n_contexts,
)


def node_operator(layout, operator_id, node_type, *, label=None, poll=None, search_weight=0.0, translate=True):
    """Generic function template for the node editor menus."""
    bl_rna = bpy.types.Node.bl_rna_get_subclass(node_type)
    if not label:
        label = bl_rna.name if bl_rna else iface_("Unknown")

    if poll is True or poll is None:
        translation_context = bl_rna.translation_context if bl_rna else i18n_contexts.default
        props = layout.operator(
            operator_id,
            text=label,
            text_ctxt=translation_context,
            translate=translate,
            search_weight=search_weight)
        props.type = node_type
        return props

    return None


# NOTE: This is kept for compatibility's sake, as some scripts import node_add_menu.add_node_type
def add_node_type(layout, node_type, *, label=None, poll=None, search_weight=0.0, translate=True):
    """Add a node type to a menu."""
    return node_operator(
        layout,
        "node.add_node",
        node_type,
        label=label,
        poll=poll,
        search_weight=search_weight,
        translate=translate)


def add_node_type_with_searchable_enum(context, layout, node_idname, property_name, search_weight=0.0):
    return node_operator_with_searchable_enum(
        context,
        layout,
        "node.add_node",
        node_idname,
        property_name,
        search_weight)


def add_node_type_with_outputs(context, layout, node_type, subnames, *, label=None, search_weight=0.0):
    return node_operator_with_outputs(
        context,
        layout,
        "node.add_node",
        node_type,
        subnames,
        label=label,
        search_weight=search_weight)


def add_color_mix_node(context, layout):
    return color_mix_node(context, layout, "node.add_node")


def add_empty_group(layout):
    return new_empty_group(layout, "node.add_empty_group")


def node_operator_with_outputs(context, layout, operator_id, node_type, subnames, *, label=None, search_weight=0.0):
    bl_rna = bpy.types.Node.bl_rna_get_subclass(node_type)
    if not label:
        label = bl_rna.name if bl_rna else "Unknown"

    props = []
    props.append(node_operator(layout, operator_id, node_type, label=label, search_weight=search_weight))
    if getattr(context, "is_menu_search", False):
        for subname in subnames:
            sublabel = "{} ▸ {}".format(iface_(label), iface_(subname))
            item_props = node_operator(layout, operator_id, node_type, label=sublabel,
                                       search_weight=search_weight, translate=False)
            item_props.visible_output = subname
            props.append(item_props)
    return props


def draw_node_group_add_menu(context, layout):
    return draw_group_menu(context, layout, group_operator="node.add_node", empty_group_operator="node.add_empty_group")


def draw_group_menu(context, layout, group_operator, empty_group_operator):
    """Add items to the layout used for interacting with node groups."""
    space_node = context.space_data
    node_tree = space_node.edit_tree
    all_node_groups = context.blend_data.node_groups

    if node_tree in all_node_groups.values():
        layout.separator()
        node_operator(layout, group_operator, "NodeGroupInput")
        node_operator(layout, group_operator, "NodeGroupOutput")

    new_empty_group(layout, empty_group_operator)

    if node_tree:
        from nodeitems_builtins import node_tree_group_type

        prefs = bpy.context.preferences
        show_hidden = prefs.filepaths.show_hidden_files_datablocks

        groups = [
            group for group in context.blend_data.node_groups
            if (group.bl_idname == node_tree.bl_idname and
                not group.contains_tree(node_tree) and
                (show_hidden or not group.name.startswith('.')))
        ]
        if groups:
            layout.separator()
            for group in groups:
                props = node_operator(layout, group_operator, node_tree_group_type[group.bl_idname], label=group.name)
                ops = props.settings.add()
                ops.name = "node_tree"
                ops.value = "bpy.data.node_groups[{!r}]".format(group.name)
                ops = props.settings.add()
                ops.name = "width"
                ops.value = repr(group.default_group_node_width)
                ops = props.settings.add()
                ops.name = "name"
                ops.value = repr(group.name)


def draw_assets_for_catalog(layout, catalog_path):
    layout.template_node_asset_menu_items(catalog_path=catalog_path)


def draw_root_assets(layout):
    layout.menu_contents("NODE_MT_node_add_root_catalogs")


def node_operator_with_searchable_enum(context, layout, operator_id, node_idname, property_name, search_weight=0.0):
    node_operator(layout, operator_id, node_idname, search_weight=search_weight)

    if getattr(context, "is_menu_search", False):
        node_type = getattr(bpy.types, node_idname)
        translation_context = node_type.bl_rna.properties[property_name].translation_context
        for item in node_type.bl_rna.properties[property_name].enum_items_static:
            label = "{} ▸ {}".format(iface_(node_type.bl_rna.name), iface_(item.name, translation_context))
            props = node_operator(
                layout,
                operator_id,
                node_idname,
                label=label,
                translate=False,
                search_weight=search_weight)
            prop = props.settings.add()
            prop.name = property_name
            prop.value = repr(item.identifier)


def color_mix_node(context, layout, operator_id):
    label = iface_("Mix Color")
    props = node_operator(layout, operator_id, "ShaderNodeMix", label=label, translate=False)
    ops = props.settings.add()
    ops.name = "data_type"
    ops.value = "'RGBA'"

    if getattr(context, "is_menu_search", False):
        translation_context = bpy.types.ShaderNodeMix.bl_rna.properties["blend_type"].translation_context
        for item in bpy.types.ShaderNodeMix.bl_rna.properties["blend_type"].enum_items_static:
            sublabel = "{} ▸ {}".format(label, iface_(item.name, translation_context))
            props = node_operator(layout, operator_id, "ShaderNodeMix", label=sublabel, translate=False)
            prop = props.settings.add()
            prop.name = "data_type"
            prop.value = "'RGBA'"
            prop = props.settings.add()
            prop.name = "blend_type"
            prop.value = repr(item.identifier)


def add_simulation_zone(layout, label):
    """Add simulation zone to a menu."""
    props = layout.operator("node.add_simulation_zone", text=label, text_ctxt=i18n_contexts.default)
    props.use_transform = True
    return props


def add_repeat_zone(layout, label):
    props = layout.operator("node.add_repeat_zone", text=label, text_ctxt=i18n_contexts.default)
    props.use_transform = True
    return props


def add_foreach_geometry_element_zone(layout, label):
    props = layout.operator(
        "node.add_foreach_geometry_element_zone",
        text=label,
        text_ctxt=i18n_contexts.default,
    )
    props.use_transform = True
    return props


def add_closure_zone(layout, label):
    props = layout.operator(
        "node.add_closure_zone", text=label, text_ctxt=i18n_contexts.default)
    props.use_transform = True
    return props


def new_empty_group(layout, operator_id):
    props = layout.operator(operator_id, text="New Group", text_ctxt=i18n_contexts.default)
    return props


class AddNodeMenu:
    draw_assets = True

    @staticmethod
    def node_operator(layout, node_type, *, label=None, poll=None, search_weight=0.0, translate=True):
        props = node_add_menu.node_operator(
            layout,
            "node.add_node",
            node_type,
            label=label,
            poll=poll,
            search_weight=search_weight,
            translate=translate)
        props.use_transform = True
        return props

    @staticmethod
    def node_operator_with_searchable_enum(context, layout, node_idname, property_name, search_weight=0.0):
        props = node_add_menu.node_operator_with_searchable_enum(
            context, layout, "node.add_node", node_idname, property_name, search_weight)
        props.use_transform = True
        return props

    @staticmethod
    def node_operator_with_outputs(context, layout, node_type, subnames, *, label=None, search_weight=0.0):
        props = node_add_menu.node_operator_with_outputs(
            context,
            layout,
            "node.add_node",
            node_type,
            subnames,
            label=label,
            search_weight=search_weight)
        props.use_transform = True

        return props

    @staticmethod
    def color_mix_node(context, layout):
        props = color_mix_node(context, layout, "node.add_node")
        props.use_transform = True
        return props

    @staticmethod
    def new_empty_group(layout):
        props = new_empty_group(layout, "node.add_empty_group")
        props.use_transform = True
        return props

    @staticmethod
    def draw_group_menu(context, layout):
        props = draw_group_menu(context, layout, group_operator="node.add_node",
                               empty_group_operator="node.add_empty_group")
        props.use_transform = True
        return props

    @staticmethod
    def simulation_zone(layout, label):
        return add_simulation_zone(layout, label)

    @staticmethod
    def repeat_zone(layout, label):
        return add_repeat_zone(layout, label)

    @staticmethod
    def for_each_element_zone(layout, label):
        return add_foreach_geometry_element_zone(layout, label)

    @staticmethod
    def closure_zone(layout, label):
        return add_closure_zone(layout, label)

    @classmethod
    def draw_menu(cls, layout, path):
        if cls.pathing_dict is None:
            raise ValueError("`pathing_dict` was not set for {}".format(cls))

        layout.menu(cls.pathing_dict[path])


class SwapNodeMenu:
    draw_assets = False

    @staticmethod
    def node_operator(layout, node_type, *, label=None, poll=None, search_weight=0.0, translate=True):
        return node_add_menu.node_operator(
            layout,
            "node.swap_node",
            node_type,
            label=label,
            poll=poll,
            search_weight=search_weight,
            translate=translate)

    @staticmethod
    def node_operator_with_searchable_enum(context, layout, node_idname, property_name, search_weight=0.0):
        return node_add_menu.node_operator_with_searchable_enum(
            context, layout, "node.swap_node", node_idname, property_name, search_weight)

    @staticmethod
    def node_operator_with_outputs(context, layout, node_type, subnames, *, label=None, search_weight=0.0):
        return node_add_menu.node_operator_with_outputs(
            context,
            layout,
            "node.swap_node",
            node_type,
            subnames,
            label=label,
            search_weight=search_weight)

    @staticmethod
    def color_mix_node(context, layout):
        return color_mix_node(context, layout, "node.swap_node")

    @staticmethod
    def new_empty_group(layout):
        return new_empty_group(layout, "node.swap_empty_group")

    @staticmethod
    def draw_group_menu(context, layout):
        return draw_group_menu(context, layout, group_operator="node.swap_node",
                               empty_group_operator="node.swap_empty_group")

    @staticmethod
    def simulation_zone(layout, label):
        props = layout.operator("node.swap_zone", text=label)
        props.input_node_type = "GeometryNodeSimulationInput"
        props.output_node_type = "GeometryNodeSimulationOutput"
        props.add_default_geometry_link = True

        return props

    @staticmethod
    def repeat_zone(layout, label):
        props = layout.operator("node.swap_zone", text=label)
        props.input_node_type = "GeometryNodeRepeatInput"
        props.output_node_type = "GeometryNodeRepeatOutput"
        props.add_default_geometry_link = True

        return props

    @staticmethod
    def for_each_element_zone(layout, label):
        props = layout.operator("node.swap_zone", text=label)
        props.input_node_type = "GeometryNodeForeachGeometryElementInput"
        props.output_node_type = "GeometryNodeForeachGeometryElementOutput"
        props.add_default_geometry_link = False

        return props

    @staticmethod
    def closure_zone(layout, label):
        props = layout.operator("node.swap_zone", text=label)
        props.input_node_type = "NodeClosureInput"
        props.output_node_type = "NodeClosureOutput"
        props.add_default_geometry_link = False

        return props

    @classmethod
    def draw_menu(cls, layout, path):
        if cls.pathing_dict is None:
            raise ValueError("`pathing_dict` was not set for {}".format(cls))

        layout.menu(cls.pathing_dict[path])


class NODE_MT_group_base(Menu):
    bl_label = "Group"

    def draw(self, context):
        layout = self.layout
        self.draw_group_menu(context, layout)
        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_layout_base(Menu):
    bl_label = "Layout"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "NodeFrame", search_weight=-1)
        self.node_operator(layout, "NodeReroute")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


def generate_menu(bl_idname: str, template: Menu, layout_base: Menu, pathing_dict: dict = None):
    return type(bl_idname, (template, layout_base), {"bl_idname": bl_idname, "pathing_dict": pathing_dict})


def generate_menus(menus: dict, template: Menu):
    pathing_dict = {}
    menus = tuple(
        generate_menu(bl_idname, template, layout_base, pathing_dict)
        for bl_idname, layout_base in menus.items()
    )
    generate_pathing_dict(pathing_dict, menus)
    return menus


def generate_pathing_dict(pathing_dict, menus):
    for menu in menus:
        if hasattr(menu, "menu_path"):
            menu_path = menu.menu_path
        else:
            menu_path = menu.bl_label

        pathing_dict[menu_path] = menu.bl_idname


classes = (
    generate_menu("NODE_MT_group_add", template=AddNodeMenu, layout_base=NODE_MT_group_base),
    generate_menu("NODE_MT_group_swap", template=SwapNodeMenu, layout_base=NODE_MT_group_base),
    generate_menu("NODE_MT_category_layout", template=AddNodeMenu, layout_base=NODE_MT_layout_base),
    generate_menu("NODE_MT_layout_swap", template=SwapNodeMenu, layout_base=NODE_MT_layout_base),
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
