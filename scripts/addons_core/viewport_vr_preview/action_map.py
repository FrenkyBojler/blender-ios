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


def vr_registry_sync(*_):
    context = bpy.context
    session_state = context.window_manager.xr_session_state
    if not session_state:
        return
    scene = context.scene
    if not scene.vr_actions_enable:
        return
    if not action_registry.registry.dirty:
        return
    if bpy.types.XrSessionState.is_running(context):
        vr_create_actions(context)
    else:
        action_registry.registry.ensure_actionmaps(session_state)


@persistent
def vr_create_actions(context: bpy.context):
    context = bpy.context
    session_state = context.window_manager.xr_session_state
    if not session_state:
        return

    # Check if actions are enabled.
    scene = context.scene
    if not scene.vr_actions_enable:
        return

    properties.vr_ensure_profile_settings(scene)

    # Ensure default action maps.
    if not defaults.vr_ensure_default_actionmaps(session_state):
        return

    for am in session_state.actionmaps:
        if len(am.actionmap_items) < 1:
            continue

        ok = session_state.action_set_create(context, am)
        if not ok:
            return

        controller_grip_name = ""
        controller_aim_name = ""

        for ami in am.actionmap_items:
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
                profile_data = action_registry.registry.profiles.get(amb.name)
                if profile_data and profile_data.requires_opt_in:
                    setting = properties.vr_profile_setting_ensure(scene, profile_data.name)
                    if not setting.enabled:
                        continue

                ok = session_state.action_binding_create(context, am, ami, amb)
                if not ok:
                    return

        # Set controller pose actions.
        if controller_grip_name and controller_aim_name:
            session_state.controller_pose_actions_set(context, am.name, controller_grip_name, controller_aim_name)

    # Set active action set.
    vr_actionset_active_update(context)


def vr_load_actionmaps(session_state, filepath):
    if not os.path.exists(filepath):
        return False

    spec = importlib.util.spec_from_file_location(os.path.basename(filepath), filepath)
    file = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(file)

    action_map_io.actionconfig_init_from_data(session_state, file.actionconfig_data, file.actionconfig_version)
    test_actionconfig(file.actionconfig_data)

    return True


def vr_save_actionmaps(session_state, filepath, sort=False):
    action_map_io.actionconfig_export_as_data(session_state, filepath, sort=sort)

    print("Saved XR actionmaps: " + filepath)

    return True


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
    bpy.app.handlers.frame_change_post.append(vr_registry_sync)
    bpy.app.handlers.depsgraph_update_post.append(vr_registry_sync)


def unregister():
    del bpy.types.Scene.vr_actions_enable
    del bpy.types.Scene.vr_actions_use_gamepad
    bpy.app.handlers.xr_session_start_pre.remove(vr_create_actions)
    if vr_registry_sync in bpy.app.handlers.frame_change_post:
        bpy.app.handlers.frame_change_post.remove(vr_registry_sync)
    if vr_registry_sync in bpy.app.handlers.depsgraph_update_post:
        bpy.app.handlers.depsgraph_update_post.remove(vr_registry_sync)
