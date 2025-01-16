# SPDX-FileCopyrightText: 2009-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from bpy.types import Menu


# Panel mix-in class (don't register).
class PresetPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'HEADER'
    bl_label = "Presets"
    path_menu = Menu.path_menu

    @classmethod
    def draw_panel_header(cls, layout):
        layout.emboss = 'NONE'
        layout.popover(
            panel=cls.__name__,
            icon='PRESET',
            text="",
        )

    @classmethod
    def draw_menu(cls, layout, text=None):
        if text is None:
            text = cls.bl_label

        layout.popover(
            panel=cls.__name__,
            icon='PRESET',
            text=text,
        )

    def draw(self, context):
        layout = self.layout
        layout.emboss = 'PULLDOWN_MENU'
        layout.operator_context = 'EXEC_DEFAULT'

        Menu.draw_preset(self, context)


def draw_action_and_slot_selector_for_id(layout, animated_id):
    """ Draw the action and slot selector for the provided ID, using the given
        UI layout.

        The ID must be an animatable ID.

        Note that the slot selector is only drawn when the ID has an assigned
        Action.
    """

    layout.template_action(animated_id, new="action.new", unlink="action.unlink")

    adt = animated_id.animation_data
    if not adt or not adt.action:
        return

    # Only show the slot selector when a layered Action is assigned.
    if adt.action.is_action_layered:
        layout.context_pointer_set("animated_id", animated_id)
        layout.template_search(
            adt, "action_slot",
            adt, "action_suitable_slots",
            new="anim.slot_new_for_id",
            unlink="anim.slot_unassign_from_id",
        )
