# SPDX-FileCopyrightText: 2021-2023 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

if "bpy" in locals():
    import importlib
    importlib.reload(properties)
else:
    from . import properties

import bpy
from bpy.app.translations import (
    pgettext_n as n_,
    pgettext_iface as iface_,
    contexts as i18n_contexts,
)
from bpy.types import (
    Menu,
    Operator,
    Panel,
    UIList,
)
# Add space_view3d.py to module search path for VIEW3D_PT_object_type_visibility import.
import os.path
import sys
sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), '../../startup/bl_ui')))
from space_view3d import VIEW3D_PT_object_type_visibility


class VIEW3D_PT_vr_world_space_panel:
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'XR'
    bl_xr_panel_mount_point = 'HEAD_FOLLOW'


class VRButtonsPanel(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "VR"


# Session.
class VIEW3D_PT_vr_session(VRButtonsPanel, Panel):
    bl_label = "VR Session"

    def draw(self, context):
        layout = self.layout
        session_settings = context.window_manager.xr_session_settings
        scene = context.scene

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        is_session_running = bpy.types.XrSessionState.is_running(context)

        # Using SNAP_FACE because it looks like a stop icon -- I shouldn't
        # have commit rights...
        toggle_info = ((iface_("Start VR Session"), 'PLAY') if not is_session_running
                       else (iface_("Stop VR Session"), 'SNAP_FACE'))
        layout.operator("wm.xr_session_toggle", text=toggle_info[0],
                        translate=False, icon=toggle_info[1])

        layout.separator()

        col = layout.column(align=True, heading="Tracking")
        col.prop(session_settings, "use_positional_tracking", text="Positional")
        col.prop(session_settings, "use_absolute_tracking", text="Absolute")

        col = layout.column(align=True, heading="Actions")
        col.prop(scene, "vr_actions_enable")


# View.
class VIEW3D_PT_vr_session_view(VRButtonsPanel, Panel):
    bl_label = "View"

    def draw(self, context):
        layout = self.layout
        session_settings = context.window_manager.xr_session_settings

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        col = layout.column(align=True, heading="Show")
        col.prop(session_settings, "show_floor", text="Floor")
        col.prop(session_settings, "show_passthrough", text="Passthrough")
        col.prop(session_settings, "show_annotation", text="Annotations")

        col.prop(session_settings, "show_selection", text="Selection")
        col.prop(session_settings, "show_controllers", text="Controllers")
        col.prop(session_settings, "show_custom_overlays", text="Custom Overlays")
        col.prop(session_settings, "show_object_extras", text="Object Extras")

        col = col.row(align=True, heading=" ")
        col.scale_x = 2.0
        col.popover(
            panel="VIEW3D_PT_vr_session_view_object_type_visibility",
            icon_value=session_settings.icon_from_show_object_viewport,
            text="",
        )

        col = layout.column(align=True)
        col.prop(session_settings, "controller_draw_style", text="Controller Style")

        col = layout.column(align=True)
        col.prop(session_settings, "clip_start", text="Clip Start")
        col.prop(session_settings, "clip_end", text="End", text_ctxt=i18n_contexts.id_camera)

        col = layout.column(align=True)
        col.prop(session_settings, "view_scale", text="View Scale")

        col = layout.column(align=True)
        col.prop(session_settings, "fly_speed", text="Fly Speed")


class VIEW3D_PT_vr_session_view_object_type_visibility(VIEW3D_PT_object_type_visibility):
    def draw(self, context):
        session_settings = context.window_manager.xr_session_settings
        self.draw_ex(context, session_settings, False)  # Pass session settings instead of 3D view.


class VIEW3D_PT_vr_session_view_object_type_visibility_world_space(
        VIEW3D_PT_vr_world_space_panel, VIEW3D_PT_object_type_visibility):
    bl_label = VIEW3D_PT_vr_session_view_object_type_visibility.bl_label
    bl_xr_panel_mount_point = 'HEAD_FOLLOW'

    def draw(self, context):
        session_settings = context.window_manager.xr_session_settings
        self.draw_ex(context, session_settings, False)  # Pass session settings instead of 3D view.


# Location Scouting.
class VIEW3D_UL_vr_captures(UIList):
    def draw_item(self, context, layout, _data, item, icon, _active_data, _active_propname, index):
        capture = item

        layout.emboss = 'NONE'

        layout.label(icon='OUTLINER_OB_CAMERA')
        layout.prop(capture, "name", text="")


class VIEW3D_PT_vr_location_scouting(VRButtonsPanel, Panel):
    bl_label = "Location Scouting"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        pass


class VIEW3D_PT_vr_location_scouting_captures(VRButtonsPanel, Panel):
    bl_label = "Captured Shots"
    bl_parent_id = "VIEW3D_PT_vr_location_scouting"

    def draw(self, context):
        layout = self.layout
        scene = context.scene

        row = layout.row()
        row.template_list("VIEW3D_UL_vr_captures", "", scene, "vr_captures", scene, "vr_captures_selected", rows=5)

        col = row.column(align=True)

        col.operator("view3d.vr_location_scouting_remove_capture", icon='REMOVE', text="")

        col.separator()

        col.operator("view3d.vr_location_scouting_add_camera_from_capture", icon='OUTLINER_OB_CAMERA', text="")
        col.operator("view3d.vr_location_scouting_add_marker_from_capture", icon='MARKER', text="")

        col.separator()

        col.operator("view3d.vr_location_scouting_browse_captures", icon='TRIA_UP', text="").backward = True
        col.operator("view3d.vr_location_scouting_browse_captures", icon='TRIA_DOWN', text="").backward = False

        is_reviewing = context.window_manager.vr_capture_review_running
        capture_review_text = "Review VR Captures" if not is_reviewing else "Exit Review"
        capture_review_icon = 'HIDE_OFF' if not is_reviewing else 'CANCEL'
        layout.operator(
            "view3d.vr_location_scouting_capture_review",
            text=capture_review_text,
            icon=capture_review_icon,
            depress=is_reviewing)


class VIEW3D_PT_vr_location_scouting_viewfinder(VRButtonsPanel, Panel):
    bl_label = "VR Viewfinder"
    bl_parent_id = "VIEW3D_PT_vr_location_scouting"

    def draw_header(self, context):
        layout = self.layout
        session_settings = context.window_manager.xr_session_settings

        layout.prop(session_settings, "viewfinder_enabled", text="")

    def draw(self, context):
        layout = self.layout
        session_settings = context.window_manager.xr_session_settings

        layout.enabled = session_settings.viewfinder_enabled
        session_settings = context.window_manager.xr_session_settings

        layout.use_property_split = True

        layout.prop(session_settings, "viewfinder_hand", text="Hand", expand=True)
        layout.prop(session_settings, "viewfinder_scale", text="Scale")

        col = layout.column(align=True, heading="Display")
        col.prop(session_settings, "viewfinder_crosshair_enabled", text="Crosshair")


class VIEW3D_PT_vr_location_scouting_viewfinder_passepartout(VRButtonsPanel, Panel):
    bl_label = "Passepartout"
    bl_parent_id = "VIEW3D_PT_vr_location_scouting_viewfinder"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout
        session_settings = context.window_manager.xr_session_settings

        layout.enabled = session_settings.viewfinder_enabled
        session_settings = context.window_manager.xr_session_settings

        layout.use_property_split = True

        layout.prop(session_settings, "viewfinder_passepartout_overscan", text="Overscan")
        layout.prop(session_settings, "viewfinder_passepartout_opacity", text="Opacity")


VR_TEMP_UI_ITEMS = (
    ('ONE', "One", "First temporary UI test item"),
    ('TWO', "Two", "Second temporary UI test item"),
    ('THREE', "Three", "Third temporary UI test item"),
    ('FOUR', "Four", "Fourth temporary UI test item"),
)


class VIEW3D_OT_vr_temp_ui_report(Operator):
    bl_idname = "view3d.vr_temp_ui_report"
    bl_label = "XR Temp UI Action"
    bl_description = "Simple action used by XR temporary UI test menus"

    message: bpy.props.StringProperty(
        name="Message",
        default="XR temporary UI action",
    )

    def execute(self, _context):
        self.report({'INFO'}, self.message)
        return {'FINISHED'}


class VIEW3D_OT_vr_temp_ui_enum(Operator):
    bl_idname = "view3d.vr_temp_ui_enum"
    bl_label = "XR Enum Menu Test"
    bl_description = "Open an enum-menu temporary region from world-space UI"

    choice: bpy.props.EnumProperty(
        name="Choice",
        items=VR_TEMP_UI_ITEMS,
        default='ONE',
    )

    def execute(self, _context):
        self.report({'INFO'}, f"Enum menu choice: {self.choice}")
        return {'FINISHED'}


class VIEW3D_OT_vr_temp_ui_search(Operator):
    bl_idname = "view3d.vr_temp_ui_search"
    bl_label = "XR Search Popup Test"
    bl_description = "Open a search-popup temporary region from world-space UI"
    bl_property = "choice"

    choice: bpy.props.EnumProperty(
        name="Search",
        items=VR_TEMP_UI_ITEMS,
    )

    def invoke(self, context, _event):
        context.window_manager.invoke_search_popup(self)
        return {'RUNNING_MODAL'}

    def execute(self, _context):
        self.report({'INFO'}, f"Search popup choice: {self.choice}")
        return {'FINISHED'}


class VIEW3D_OT_vr_temp_ui_dialog(Operator):
    bl_idname = "view3d.vr_temp_ui_dialog"
    bl_label = "XR Dialog Test"
    bl_description = "Open a dialog temporary region from world-space UI"

    name: bpy.props.StringProperty(
        name="Name",
        default="XR Dialog",
    )
    amount: bpy.props.FloatProperty(
        name="Amount",
        default=0.5,
        min=0.0,
        max=1.0,
    )
    enabled: bpy.props.BoolProperty(
        name="Enabled",
        default=True,
    )

    def invoke(self, context, _event):
        return context.window_manager.invoke_props_dialog(self, width=240)

    def draw(self, _context):
        layout = self.layout
        layout.prop(self, "name")
        layout.prop(self, "amount")
        layout.prop(self, "enabled")

    def execute(self, _context):
        self.report({'INFO'}, f"Dialog submitted: {self.name}")
        return {'FINISHED'}


class VIEW3D_OT_vr_temp_ui_popup(Operator):
    bl_idname = "view3d.vr_temp_ui_popup"
    bl_label = "XR Popup Test"
    bl_description = "Open a popup temporary region from world-space UI"

    name: bpy.props.StringProperty(
        name="Label",
        default="XR Popup",
    )
    show_extra: bpy.props.BoolProperty(
        name="Show Extra Option",
        default=False,
    )

    def invoke(self, context, _event):
        return context.window_manager.invoke_popup(self, width=240)

    def draw(self, _context):
        layout = self.layout
        layout.label(text="Popup Content")
        layout.prop(self, "name")
        layout.prop(self, "show_extra")
        layout.operator_menu_enum("view3d.vr_temp_ui_enum", "choice", text="Nested Enum Menu")

    def execute(self, _context):
        self.report({'INFO'}, f"Popup confirmed: {self.name}")
        return {'FINISHED'}


class VIEW3D_OT_vr_temp_ui_confirm(Operator):
    bl_idname = "view3d.vr_temp_ui_confirm"
    bl_label = "XR Confirm Test"
    bl_description = "Open a confirm temporary region from world-space UI"

    def invoke(self, context, event):
        return context.window_manager.invoke_confirm(self, event)

    def execute(self, _context):
        self.report({'INFO'}, "Confirm dialog accepted")
        return {'FINISHED'}


class VIEW3D_MT_vr_temp_ui_submenu(Menu):
    bl_label = "XR Temp Submenu"

    def draw(self, _context):
        layout = self.layout
        props = layout.operator("view3d.vr_temp_ui_report", text="Submenu Action A")
        props.message = "Submenu action A"
        props = layout.operator("view3d.vr_temp_ui_report", text="Submenu Action B")
        props.message = "Submenu action B"


class VIEW3D_MT_vr_temp_ui_menu(Menu):
    bl_label = "XR Temp Menu"

    def draw(self, _context):
        layout = self.layout
        props = layout.operator("view3d.vr_temp_ui_report", text="Menu Action")
        props.message = "Menu action"
        layout.operator_menu_enum("view3d.vr_temp_ui_enum", "choice", text="Enum Menu")
        layout.menu("VIEW3D_MT_vr_temp_ui_submenu", text="Nested Menu")


class VIEW3D_PT_vr_temp_ui_popover_world_space(VIEW3D_PT_vr_world_space_panel, Panel):
    bl_label = "XR Temp Popover"
    bl_xr_panel_mount_point = 'HEAD_FOLLOW'

    def draw(self, _context):
        layout = self.layout
        layout.label(text="Popover Content")
        layout.operator("view3d.vr_temp_ui_search", text="Search Popup")
        layout.operator_menu_enum("view3d.vr_temp_ui_enum", "choice", text="Enum Menu")
        layout.menu("VIEW3D_MT_vr_temp_ui_submenu", text="Nested Menu")


# Landmarks.
class VIEW3D_MT_vr_landmark_menu(Menu):
    bl_label = "Landmark Controls"

    def draw(self, _context):
        layout = self.layout

        layout.operator("view3d.vr_camera_landmark_from_session")
        layout.operator("view3d.vr_landmark_from_camera")
        layout.operator("view3d.update_vr_landmark")
        layout.separator()
        layout.operator("view3d.cursor_to_vr_landmark")
        layout.operator("view3d.camera_to_vr_landmark")
        layout.operator("view3d.add_camera_from_vr_landmark")


class VIEW3D_UL_vr_landmarks(UIList):
    def draw_item(self, context, layout, _data, item, icon, _active_data,
                  _active_propname, index):
        landmark = item
        landmark_active_idx = context.scene.vr_landmarks_active

        layout.emboss = 'NONE'

        layout.prop(landmark, "name", text="")

        icon = (
            'RADIOBUT_ON' if (index == landmark_active_idx) else 'RADIOBUT_OFF'
        )
        props = layout.operator(
            "view3d.vr_landmark_activate", text="", icon=icon)
        props.index = index


class VIEW3D_PT_vr_landmarks(VRButtonsPanel, Panel):
    bl_label = "Landmarks"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        landmark_selected = properties.VRLandmark.get_selected_landmark(context)

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        row = layout.row()

        row.template_list("VIEW3D_UL_vr_landmarks", "", scene, "vr_landmarks",
                          scene, "vr_landmarks_selected", rows=3)

        col = row.column(align=True)
        col.operator("view3d.vr_landmark_add", icon='ADD', text="")
        col.operator("view3d.vr_landmark_remove", icon='REMOVE', text="")
        col.operator("view3d.vr_landmark_from_session", icon='PLUS', text="")

        col.menu("VIEW3D_MT_vr_landmark_menu", icon='DOWNARROW_HLT', text="")

        if landmark_selected:
            layout.prop(landmark_selected, "type")

            if landmark_selected.type == 'OBJECT':
                layout.prop(landmark_selected, "base_pose_object")
                layout.prop(landmark_selected, "base_scale", text="Scale")
            elif landmark_selected.type == 'CUSTOM':
                layout.prop(landmark_selected,
                            "base_pose_location", text="Location")
                layout.prop(landmark_selected,
                            "base_pose_angle", text="Angle")
                layout.prop(landmark_selected,
                            "base_scale", text="Scale")


# Actions.
class VIEW3D_PT_vr_actionmaps(VRButtonsPanel, Panel):
    bl_label = "Action Maps"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout
        scene = context.scene

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        col = layout.column(align=True)
        col.prop(scene, "vr_actions_use_gamepad", text="Gamepad")

        col = layout.column(align=True, heading="Extensions")
        col.prop(scene, "vr_actions_enable_reverb_g2", text="HP Reverb G2")
        col.prop(scene, "vr_actions_enable_vive_cosmos", text="HTC Vive Cosmos")
        col.prop(scene, "vr_actions_enable_vive_focus", text="HTC Vive Focus")
        col.prop(scene, "vr_actions_enable_huawei", text="Huawei")


# Viewport feedback.
class VIEW3D_PT_vr_viewport_feedback(VRButtonsPanel, Panel):
    bl_label = "Viewport Feedback"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout
        view3d = context.space_data

        col = layout.column(align=True)
        col.label(icon='STATUS_WARNING', text="Note:")
        col.label(text="Settings here may have a significant")
        col.label(text="performance impact!")

        layout.separator()

        layout.prop(view3d.shading, "vr_show_virtual_camera")
        layout.prop(view3d.shading, "vr_show_controllers")
        layout.prop(view3d.shading, "vr_show_landmarks")
        layout.prop(view3d.shading, "vr_show_captures")
        layout.prop(view3d, "mirror_xr_session")


# Info.
class VIEW3D_PT_vr_info(VRButtonsPanel, Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "VR"
    bl_label = "VR Info"

    @classmethod
    def poll(cls, context):
        return not bpy.app.build_options.xr_openxr

    def draw(self, context):
        import platform
        layout = self.layout
        missing_support_string = n_("Built without VR/OpenXR features")
        layout.label(icon='STATUS_ERROR', text=missing_support_string)


class VIEW3D_PT_vr_session_world_space(VIEW3D_PT_vr_world_space_panel, Panel):
    bl_label = "VR Session"
    bl_xr_panel_mount_point = 'LEFT_HAND'

    def draw(self, context):
        layout = self.layout
        session_settings = context.window_manager.xr_session_settings
        scene = context.scene

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        is_session_running = bpy.types.XrSessionState.is_running(context)

        # Using SNAP_FACE because it looks like a stop icon -- I shouldn't
        # have commit rights...
        toggle_info = ((iface_("Start VR Session"), 'PLAY') if not is_session_running
                       else (iface_("Stop VR Session"), 'SNAP_FACE'))
        layout.operator("wm.xr_session_toggle", text=toggle_info[0],
                        translate=False, icon=toggle_info[1])

        layout.separator()

        col = layout.column(align=True, heading="Tracking")
        col.prop(session_settings, "use_positional_tracking", text="Positional")
        col.prop(session_settings, "use_absolute_tracking", text="Absolute")

        col = layout.column(align=True, heading="Actions")
        col.prop(scene, "vr_actions_enable")


class VIEW3D_PT_vr_session_view_world_space(VIEW3D_PT_vr_world_space_panel, Panel):
    bl_label = "View"
    bl_xr_panel_mount_point = 'HEAD_FOLLOW'

    def draw(self, context):
        layout = self.layout
        session_settings = context.window_manager.xr_session_settings

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        col = layout.column(align=True, heading="Show")
        col.prop(session_settings, "show_floor", text="Floor")
        col.prop(session_settings, "show_passthrough", text="Passthrough")
        col.prop(session_settings, "show_annotation", text="Annotations")

        col.prop(session_settings, "show_selection", text="Selection")
        col.prop(session_settings, "show_controllers", text="Controllers")
        col.prop(session_settings, "show_custom_overlays", text="Custom Overlays")
        col.prop(session_settings, "show_object_extras", text="Object Extras")

        col = col.row(align=True, heading=" ")
        col.scale_x = 2.0
        col.popover(
            panel="VIEW3D_PT_vr_session_view_object_type_visibility_world_space",
            icon_value=session_settings.icon_from_show_object_viewport,
            text="",
        )

        col = layout.column(align=True)
        col.prop(session_settings, "controller_draw_style", text="Controller Style")

        col = layout.column(align=True)
        col.prop(session_settings, "clip_start", text="Clip Start")
        col.prop(session_settings, "clip_end", text="End", text_ctxt=i18n_contexts.id_camera)

        col = layout.column(align=True)
        col.prop(session_settings, "view_scale", text="View Scale")

        col = layout.column(align=True)
        col.prop(session_settings, "fly_speed", text="Fly Speed")

class VIEW3D_PT_vr_landmarks_world_space(VIEW3D_PT_vr_world_space_panel, Panel):
    bl_label = "Landmarks"
    bl_options = {'DEFAULT_CLOSED'}
    bl_xr_panel_mount_point = 'LEFT_HAND'

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        landmark_selected = properties.VRLandmark.get_selected_landmark(context)

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        row = layout.row()

        row.template_list("VIEW3D_UL_vr_landmarks", "", scene, "vr_landmarks",
                          scene, "vr_landmarks_selected", rows=3)

        col = row.column(align=True)
        col.operator("view3d.vr_landmark_add", icon='ADD', text="")
        col.operator("view3d.vr_landmark_remove", icon='REMOVE', text="")
        col.operator("view3d.vr_landmark_from_session", icon='PLUS', text="")

        col.menu("VIEW3D_MT_vr_landmark_menu", icon='DOWNARROW_HLT', text="")

        if landmark_selected:
            layout.prop(landmark_selected, "type")

            if landmark_selected.type == 'OBJECT':
                layout.prop(landmark_selected, "base_pose_object")
                layout.prop(landmark_selected, "base_scale", text="Scale")
            elif landmark_selected.type == 'CUSTOM':
                layout.prop(landmark_selected,
                            "base_pose_location", text="Location")
                layout.prop(landmark_selected,
                            "base_pose_angle", text="Angle")
                layout.prop(landmark_selected,
                            "base_scale", text="Scale")


# Actions.
class VIEW3D_PT_vr_actionmaps_world_space(VIEW3D_PT_vr_world_space_panel, Panel):
    bl_label = "Action Maps"
    bl_options = {'DEFAULT_CLOSED'}
    bl_xr_panel_mount_point = 'HEAD_FOLLOW'

    def draw(self, context):
        layout = self.layout
        scene = context.scene

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        col = layout.column(align=True)
        col.prop(scene, "vr_actions_use_gamepad", text="Gamepad")

        col = layout.column(align=True, heading="Extensions")
        col.prop(scene, "vr_actions_enable_reverb_g2", text="HP Reverb G2")
        col.prop(scene, "vr_actions_enable_vive_cosmos", text="HTC Vive Cosmos")
        col.prop(scene, "vr_actions_enable_vive_focus", text="HTC Vive Focus")
        col.prop(scene, "vr_actions_enable_huawei", text="Huawei")


class VIEW3D_PT_vr_temp_ui_world_space(VIEW3D_PT_vr_world_space_panel, Panel):
    bl_label = "Temp UI Tests"
    bl_options = {'DEFAULT_CLOSED'}
    bl_xr_panel_mount_point = 'HEAD_FOLLOW'

    def draw(self, _context):
        layout = self.layout

        layout.label(text="Menu / Popover")
        row = layout.row(align=True)
        row.menu("VIEW3D_MT_vr_temp_ui_menu", text="Menu")
        row.popover(panel="VIEW3D_PT_vr_temp_ui_popover_world_space", text="Popover")

        layout.separator()

        layout.label(text="Popup Variants")
        col = layout.column(align=True)
        col.operator_menu_enum("view3d.vr_temp_ui_enum", "choice", text="Enum Menu")
        col.operator("view3d.vr_temp_ui_search", text="Search Popup")
        col.operator("view3d.vr_temp_ui_dialog", text="Dialog")
        col.operator("view3d.vr_temp_ui_popup", text="Popup")
        col.operator("view3d.vr_temp_ui_confirm", text="Confirm")


"""
class VIEW3D_PT_vr_viewport_feedback_world_space(VIEW3D_PT_vr_viewport_feedback):
    bl_region_type = 'XR'
    bl_category = ""

class VIEW3D_PT_vr_info_world_space(VIEW3D_PT_vr_info):
    bl_region_type = 'XR'
    bl_category = ""
"""

classes = (
    VIEW3D_PT_vr_session,
    VIEW3D_PT_vr_session_view,
    VIEW3D_PT_vr_session_view_object_type_visibility,
    VIEW3D_PT_vr_location_scouting,
    VIEW3D_PT_vr_location_scouting_captures,
    VIEW3D_PT_vr_location_scouting_viewfinder,
    VIEW3D_PT_vr_location_scouting_viewfinder_passepartout,
    VIEW3D_PT_vr_session_view_object_type_visibility_world_space,
    VIEW3D_OT_vr_temp_ui_report,
    VIEW3D_OT_vr_temp_ui_enum,
    VIEW3D_OT_vr_temp_ui_search,
    VIEW3D_OT_vr_temp_ui_dialog,
    VIEW3D_OT_vr_temp_ui_popup,
    VIEW3D_OT_vr_temp_ui_confirm,
    VIEW3D_PT_vr_landmarks,
    VIEW3D_PT_vr_actionmaps,
    VIEW3D_PT_vr_viewport_feedback,

    VIEW3D_UL_vr_landmarks,
    VIEW3D_UL_vr_captures,
    VIEW3D_MT_vr_landmark_menu,
    VIEW3D_MT_vr_temp_ui_submenu,
    VIEW3D_MT_vr_temp_ui_menu,
    VIEW3D_PT_vr_session_world_space,
    VIEW3D_PT_vr_session_view_world_space,
    VIEW3D_PT_vr_temp_ui_popover_world_space,
    VIEW3D_PT_vr_landmarks_world_space,
    VIEW3D_PT_vr_actionmaps_world_space,
    VIEW3D_PT_vr_temp_ui_world_space,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)

    # View3DShading is the only per 3D-View struct with custom property
    # support, so "abusing" that to get a per 3D-View option.
    bpy.types.View3DShading.vr_show_virtual_camera = bpy.props.BoolProperty(
        name="Show VR Camera",
        default=False
    )
    bpy.types.View3DShading.vr_show_controllers = bpy.props.BoolProperty(
        name="Show VR Controllers",
        default=False
    )
    bpy.types.View3DShading.vr_show_landmarks = bpy.props.BoolProperty(
        name="Show Landmarks",
        default=False
    )
    bpy.types.View3DShading.vr_show_captures = bpy.props.BoolProperty(
        name="Show Location Scouting Captures",
        default=True
    )


def unregister():
    for cls in classes:
        bpy.utils.unregister_class(cls)

    del bpy.types.View3DShading.vr_show_virtual_camera
    del bpy.types.View3DShading.vr_show_controllers
    del bpy.types.View3DShading.vr_show_landmarks
    del bpy.types.View3DShading.vr_show_captures
