# SPDX-FileCopyrightText: 2021-2022 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

if "bpy" in locals():
    import importlib
    importlib.reload(action_registry)
    importlib.reload(defaults)
    importlib.reload(properties)
else:
    from . import action_map_io, action_registry, defaults, properties

import bpy
from bpy.app.handlers import persistent
from bpy_extras.io_utils import ExportHelper, ImportHelper
import importlib.util
import os.path


def vr_actionset_active_update(context):
    session_state = context.window_manager.xr_session_state
    if not session_state or len(session_state.actionmaps) < 1:
        return

    scene = context.scene

    if scene.vr_actions_use_gamepad and session_state.actionmaps.find(
            session_state, defaults.VRDefaultActionmaps.GAMEPAD.value):
        session_state.active_action_set_set(context, defaults.VRDefaultActionmaps.GAMEPAD.value)
    else:
        # Use first action map.
        session_state.active_action_set_set(context, session_state.actionmaps[0].name)


def vr_actions_use_gamepad_update(self, context):
    vr_actionset_active_update(context)


@persistent
def vr_create_actions(context: bpy.context):
    print("vr_create_actions...")
    context = bpy.context
    session_state = context.window_manager.xr_session_state
    if not session_state:
        return

    # Check if actions are enabled.
    scene = context.scene
    if not scene.vr_actions_enable:
        return

    # Ensure default action maps.
    if not action_registry.VRActionRegistry().ensure_actionmaps(session_state):
        return

    print("vr_create_actions: begin registration")
    for am in session_state.actionmaps:
        print(f"vr_create_actions: registering actionmap {am.name}")

        if len(am.actionmap_items) < 1:
            continue

        ok = session_state.action_set_create(context, am)
        if not ok:
            return

        controller_grip_name = ""
        controller_aim_name = ""

        for ami in am.actionmap_items:
            print(f"vr_create_actions: registering action {ami.name} for actionmap {am.name}")
            
            if len(ami.bindings) < 1:
                continue

            ok = session_state.action_create(context, am, ami)
            if not ok:
                return

            if ami.type == 'POSE':
                if ami.pose_is_controller_grip:
                    controller_grip_name = ami.name
                if ami.pose_is_controller_aim:
                    controller_aim_name = ami.name

            for amb in ami.bindings:
                print(f"vr_create_actions: creating action map binding {amb.name} ({amb.profile}) for action {ami.name} for actionmap {am.name}")
                ok = session_state.action_binding_create(context, am, ami, amb)
                if not ok:
                    return

        # Set controller pose actions.
        if controller_grip_name and controller_aim_name:
            session_state.controller_pose_actions_set(context, am.name, controller_grip_name, controller_aim_name)

    # Set active action set.
    vr_actionset_active_update(context)


def register():
    bpy.types.Scene.vr_actions_enable = bpy.props.BoolProperty(
        name="Use Controller Actions",
        description="Enable default VR controller actions, including controller poses and haptics",
        default=True,
    )
    bpy.types.Scene.vr_actions_use_gamepad = bpy.props.BoolProperty(
        description="Use input from gamepad instead of motion controllers",
        default=False,
        update=vr_actions_use_gamepad_update,
    )
    bpy.app.handlers.xr_session_start_pre.append(vr_create_actions)


def unregister():
    del bpy.types.Scene.vr_actions_enable
    del bpy.types.Scene.vr_actions_use_gamepad
    
    registry = action_registry.VRActionRegistry()
    registry.destroy_profile_settings()

    bpy.app.handlers.xr_session_start_pre.remove(vr_create_actions)
