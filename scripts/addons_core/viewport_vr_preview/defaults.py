# SPDX-FileCopyrightText: 2021-2022 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

if "bpy" in locals():
    import importlib
    importlib.reload(action_map)
else:
    from . import action_map

import bpy
from enum import Enum
import math
import os.path

from .action_profile import VRDefaultActions, VRDefaultActionprofiles, VRDefaultActionbindings, VRDefaultActionmaps

from .profiles.huawei import VRActionProfileHuawei
from .profiles.index import VRActionProfileIndex
from .profiles.oculus import VRActionProfileOculus
from .profiles.reverb_g2 import VRActionProfileReverbG2
from .profiles.simple import VRActionProfileSimple
from .profiles.vive import VRActionProfileVive
from .profiles.vive_cosmos import VRActionProfileViveCosmos
from .profiles.vive_focus import VRActionProfileViveFocus
from .profiles.wmr import VRActionProfileWMR

from .action import VRActionHaptic
from .actions.action_fly import VRActionFlyForward, VRActionFlyBack, VRActionFlyLeft, VRActionFlyRight, VRActionFlyUp, VRActionFlyDown, VRActionFlyTurnLeft, VRActionFlyTurnRight
from .actions.action_nav_reset import VRActionNavReset
from .actions.action_teleport import VRActionTeleport
from .actions.action_nav_grab import VRActionNavGrab
from .actions.action_pose import VRActionControllerGrip, VRActionControllerAim


def vr_defaults_actionmap_add(session_state, name):
    am = session_state.actionmaps.new(session_state, name, True)

    return am


def vr_defaults_action_add(am,
                           name,
                           user_paths,
                           op,
                           op_mode,
                           bimanual,
                           haptic_name,
                           haptic_match_user_paths,
                           haptic_duration,
                           haptic_frequency,
                           haptic_amplitude,
                           haptic_mode,
                           op_properties=None):

    ami = am.actionmap_items.new(name, True)
    if ami:
        ami.type = 'FLOAT'
        for path in user_paths:
            ami.user_paths.new(path)
        ami.op = op
        ami.op_mode = op_mode
        ami.bimanual = bimanual
        ami.haptic_name = haptic_name
        ami.haptic_match_user_paths = haptic_match_user_paths
        ami.haptic_duration = haptic_duration
        ami.haptic_frequency = haptic_frequency
        ami.haptic_amplitude = haptic_amplitude
        ami.haptic_mode = haptic_mode

        if op_properties:
            ami_props = ami.op_properties
            for attr, value in op_properties:
                try:
                    setattr(ami_props, attr, value)
                except AttributeError:
                    print(f"Warning: property '{attr}' not found in action map item '{ami_name}'")
                except Exception as ex:
                    print(f"Warning: {ex!r}")

    return ami


def vr_defaults_pose_action_add(am,
                                name,
                                user_paths,
                                is_controller_grip,
                                is_controller_aim):
    ami = am.actionmap_items.new(name, True)
    if ami:
        ami.type = 'POSE'
        for path in user_paths:
            ami.user_paths.new(path)
        ami.pose_is_controller_grip = is_controller_grip
        ami.pose_is_controller_aim = is_controller_aim

    return ami


def vr_defaults_haptic_action_add(am,
                                  name,
                                  user_paths):
    ami = am.actionmap_items.new(name, True)
    if ami:
        ami.type = 'VIBRATION'
        for path in user_paths:
            ami.user_paths.new(path)

    return ami


def vr_defaults_actionbinding_add(ami,
                                  name,
                                  profile,
                                  component_paths,
                                  threshold,
                                  axis0_region,
                                  axis1_region):
    amb = ami.bindings.new(name, True)
    if amb:
        amb.profile = profile
        for path in component_paths:
            amb.component_paths.new(path)
        amb.threshold = threshold
        amb.axis0_region = axis0_region
        amb.axis1_region = axis1_region

    return amb


def vr_defaults_pose_actionbinding_add(ami,
                                       name,
                                       profile,
                                       component_paths,
                                       location,
                                       rotation):
    amb = ami.bindings.new(name, True)
    if amb:
        amb.profile = profile
        for path in component_paths:
            amb.component_paths.new(path)
        amb.pose_location = location
        amb.pose_rotation = rotation

    return amb


def vr_defaults_haptic_actionbinding_add(ami,
                                         name,
                                         profile,
                                         component_paths):
    amb = ami.bindings.new(name, True)
    if amb:
        amb.profile = profile
        for path in component_paths:
            amb.component_paths.new(path)

    return amb


def vr_defaults_actionbindings_add(ami, action_name, action_profiles):
    for profile in action_profiles:
        if profile.action_map[action_name] is None:
            continue

        amb = ami.bindings.new(profile.name, True)
        if amb:
            amb.profile = profile.profile
            for path in profile.action_map[action_name]["component_paths"]:
                amb.component_paths.new(path)
            amb.threshold = profile.action_map[action_name]["threshold"]
            amb.axis0_region = profile.action_map[action_name]["axis_region"]
            amb.axis1_region = "ANY"


def vr_defaults_pose_actionbindings_add(ami, action_name, action_profiles):
    for profile in action_profiles:
        if profile.action_map[action_name] is None:
            continue

        amb = ami.bindings.new(profile.name, True)
        if amb:
            amb.profile = profile.profile
            for path in profile.action_map[action_name]["component_paths"]:
                amb.component_paths.new(path)
            amb.pose_location = profile.action_map[action_name]["pose_location"]
            amb.pose_rotation = profile.action_map[action_name]["pose_rotation"]


def vr_defaults_haptic_actionbindings_add(ami, action_name, action_profiles):
    for profile in action_profiles:
        if profile.action_map[action_name] is None:
            continue

        amb = ami.bindings.new(profile.name, True)
        if amb:
            amb.profile = profile.profile
            for path in profile.action_map[action_name]["component_paths"]:
                amb.component_paths.new(path)


def vr_defaults_create_default(session_state):
    am = vr_defaults_actionmap_add(session_state,
                                   VRDefaultActionmaps.DEFAULT.value)
    if not am:
        return

    action_profiles = [
        VRActionProfileHuawei(),
        VRActionProfileIndex(),
        VRActionProfileOculus(),
        VRActionProfileReverbG2(),
        VRActionProfileSimple(),
        VRActionProfileVive(),
        VRActionProfileViveCosmos(),
        VRActionProfileViveFocus(),
        VRActionProfileWMR(),
    ]
    
    actions = [
        VRActionControllerGrip(),
        VRActionControllerAim(),
        VRActionTeleport(),
        VRActionNavGrab(),
        VRActionFlyForward(),
        VRActionFlyBack(),
        VRActionFlyLeft(),
        VRActionFlyRight(),
        VRActionFlyUp(),
        VRActionFlyDown(),
        VRActionFlyTurnLeft(),
        VRActionFlyTurnRight(),
        VRActionNavReset(),
        VRActionHaptic(),
    ]

    for action in actions:
        action_map_item = action.vr_action_map_add(am)
        if not action_map_item:
            continue
        
        for action_profile in action_profiles:
            action.vr_action_map_item_add(action_map_item, action_profile)


def vr_defaults_create_default_gamepad(session_state):
    am = vr_defaults_actionmap_add(session_state,
                                   VRDefaultActionmaps.GAMEPAD.value)

    ami = vr_defaults_action_add(am,
                                 VRDefaultActions.TELEPORT.value,
                                 ["/user/gamepad"],
                                 "wm.xr_navigation_teleport",
                                 'MODAL',
                                 False,
                                 "",
                                 False,
                                 0.0,
                                 0.0,
                                 0.0,
                                 'PRESS')
    if ami:
        vr_defaults_actionbinding_add(ami,
                                      VRDefaultActionbindings.GAMEPAD.value,
                                      VRDefaultActionprofiles.GAMEPAD.value,
                                      ["/input/trigger_right/value"],
                                      0.3,
                                      'ANY',
                                      'ANY')

    ami = vr_defaults_action_add(am,
                                 VRDefaultActions.FLY.value,
                                 ["/user/gamepad"],
                                 "wm.xr_navigation_fly",
                                 'MODAL',
                                 False,
                                 "",
                                 False,
                                 0.0,
                                 0.0,
                                 0.0,
                                 'PRESS')
    if ami:
        vr_defaults_actionbinding_add(ami,
                                      VRDefaultActionbindings.GAMEPAD.value,
                                      VRDefaultActionprofiles.GAMEPAD.value,
                                      ["/input/trigger_left/value"],
                                      0.3,
                                      'ANY',
                                      'ANY')

    ami = vr_defaults_action_add(am,
                                 VRDefaultActions.FLY_FORWARD.value,
                                 ["/user/gamepad"],
                                 "wm.xr_navigation_fly",
                                 'MODAL',
                                 False,
                                 "",
                                 False,
                                 0.0,
                                 0.0,
                                 0.0,
                                 'PRESS')
    if ami:
        vr_defaults_actionbinding_add(ami,
                                      VRDefaultActionbindings.GAMEPAD.value,
                                      VRDefaultActionprofiles.GAMEPAD.value,
                                      ["/input/thumbstick_left/y"],
                                      0.3,
                                      'POSITIVE',
                                      'ANY')

    ami = vr_defaults_action_add(am,
                                 VRDefaultActions.FLY_BACK.value,
                                 ["/user/gamepad"],
                                 "wm.xr_navigation_fly",
                                 'MODAL',
                                 False,
                                 "",
                                 False,
                                 0.0,
                                 0.0,
                                 0.0,
                                 'PRESS')
    if ami:
        vr_defaults_actionbinding_add(ami,
                                      VRDefaultActionbindings.GAMEPAD.value,
                                      VRDefaultActionprofiles.GAMEPAD.value,
                                      ["/input/thumbstick_left/y"],
                                      0.3,
                                      'NEGATIVE',
                                      'ANY')

    ami = vr_defaults_action_add(am,
                                 VRDefaultActions.FLY_LEFT.value,
                                 ["/user/gamepad"],
                                 "wm.xr_navigation_fly",
                                 'MODAL',
                                 False,
                                 "",
                                 False,
                                 0.0,
                                 0.0,
                                 0.0,
                                 'PRESS')
    if ami:
        vr_defaults_actionbinding_add(ami,
                                      VRDefaultActionbindings.GAMEPAD.value,
                                      VRDefaultActionprofiles.GAMEPAD.value,
                                      ["/input/thumbstick_left/x"],
                                      0.3,
                                      'NEGATIVE',
                                      'ANY')

    ami = vr_defaults_action_add(am,
                                 VRDefaultActions.FLY_RIGHT.value,
                                 ["/user/gamepad"],
                                 "wm.xr_navigation_fly",
                                 'MODAL',
                                 False,
                                 "",
                                 False,
                                 0.0,
                                 0.0,
                                 0.0,
                                 'PRESS')
    if ami:
        vr_defaults_actionbinding_add(ami,
                                      VRDefaultActionbindings.GAMEPAD.value,
                                      VRDefaultActionprofiles.GAMEPAD.value,
                                      ["/input/thumbstick_left/x"],
                                      0.3,
                                      'POSITIVE',
                                      'ANY')

    ami = vr_defaults_action_add(am,
                                 VRDefaultActions.FLY_UP.value,
                                 ["/user/gamepad"],
                                 "wm.xr_navigation_fly",
                                 'MODAL',
                                 False,
                                 "",
                                 False,
                                 0.0,
                                 0.0,
                                 0.0,
                                 'PRESS')
    if ami:
        vr_defaults_actionbinding_add(ami,
                                      VRDefaultActionbindings.GAMEPAD.value,
                                      VRDefaultActionprofiles.GAMEPAD.value,
                                      ["/input/thumbstick_right/y"],
                                      0.3,
                                      'POSITIVE',
                                      'ANY')

    ami = vr_defaults_action_add(am,
                                 VRDefaultActions.FLY_DOWN.value,
                                 ["/user/gamepad"],
                                 "wm.xr_navigation_fly",
                                 'MODAL',
                                 False,
                                 "",
                                 False,
                                 0.0,
                                 0.0,
                                 0.0,
                                 'PRESS')
    if ami:
        vr_defaults_actionbinding_add(ami,
                                      VRDefaultActionbindings.GAMEPAD.value,
                                      VRDefaultActionprofiles.GAMEPAD.value,
                                      ["/input/thumbstick_right/y"],
                                      0.3,
                                      'NEGATIVE',
                                      'ANY')

    ami = vr_defaults_action_add(am,
                                 VRDefaultActions.FLY_TURNLEFT.value,
                                 ["/user/gamepad"],
                                 "wm.xr_navigation_fly",
                                 'MODAL',
                                 False,
                                 "",
                                 False,
                                 0.0,
                                 0.0,
                                 0.0,
                                 'PRESS')
    if ami:
        vr_defaults_actionbinding_add(ami,
                                      VRDefaultActionbindings.GAMEPAD.value,
                                      VRDefaultActionprofiles.GAMEPAD.value,
                                      ["/input/thumbstick_right/x"],
                                      0.3,
                                      'NEGATIVE',
                                      'ANY')

    ami = vr_defaults_action_add(am,
                                 VRDefaultActions.FLY_TURNRIGHT.value,
                                 ["/user/gamepad"],
                                 "wm.xr_navigation_fly",
                                 'MODAL',
                                 False,
                                 "",
                                 False,
                                 0.0,
                                 0.0,
                                 0.0,
                                 'PRESS')
    if ami:
        vr_defaults_actionbinding_add(ami,
                                      VRDefaultActionbindings.GAMEPAD.value,
                                      VRDefaultActionprofiles.GAMEPAD.value,
                                      ["/input/thumbstick_right/x"],
                                      0.3,
                                      'POSITIVE',
                                      'ANY')

    ami = vr_defaults_action_add(am,
                                 VRDefaultActions.NAV_RESET.value,
                                 ["/user/gamepad"],
                                 "wm.xr_navigation_reset",
                                 'PRESS',
                                 False,
                                 "haptic_right",
                                 True,
                                 0.3,
                                 3000.0,
                                 0.5,
                                 'PRESS')
    if ami:
        vr_defaults_actionbinding_add(ami,
                                      VRDefaultActionbindings.GAMEPAD.value,
                                      VRDefaultActionprofiles.GAMEPAD.value,
                                      ["/input/a/click"],
                                      0.3,
                                      'ANY',
                                      'ANY')

    ami = vr_defaults_haptic_action_add(am,
                                        VRDefaultActions.HAPTIC_LEFT.value,
                                        ["/user/gamepad"])
    if ami:
        vr_defaults_haptic_actionbinding_add(ami,
                                             VRDefaultActionbindings.GAMEPAD.value,
                                             VRDefaultActionprofiles.GAMEPAD.value,
                                             ["/output/haptic_left"])

    ami = vr_defaults_haptic_action_add(am,
                                        VRDefaultActions.HAPTIC_RIGHT.value,
                                        ["/user/gamepad"])
    if ami:
        vr_defaults_haptic_actionbinding_add(ami,
                                             VRDefaultActionbindings.GAMEPAD.value,
                                             VRDefaultActionprofiles.GAMEPAD.value,
                                             ["/output/haptic_right"])

    ami = vr_defaults_haptic_action_add(am,
                                        VRDefaultActions.HAPTIC_LEFTTRIGGER.value,
                                        ["/user/gamepad"])
    if ami:
        vr_defaults_haptic_actionbinding_add(ami,
                                             VRDefaultActionbindings.GAMEPAD.value,
                                             VRDefaultActionprofiles.GAMEPAD.value,
                                             ["/output/haptic_left_trigger"])

    ami = vr_defaults_haptic_action_add(am,
                                        VRDefaultActions.HAPTIC_RIGHTTRIGGER.value,
                                        ["/user/gamepad"])
    if ami:
        vr_defaults_haptic_actionbinding_add(ami,
                                             VRDefaultActionbindings.GAMEPAD.value,
                                             VRDefaultActionprofiles.GAMEPAD.value,
                                             ["/output/haptic_right_trigger"])


def vr_get_default_config_path():
    filepath = os.path.join(os.path.dirname(os.path.abspath(__file__)), "configs")
    return os.path.join(filepath, "default.py")


def vr_ensure_default_actionmaps(session_state):
    loaded = True

    for name in VRDefaultActionmaps:
        if not session_state.actionmaps.find(session_state, name.value):
            loaded = False
            break

    if loaded:
        return loaded

    # Load default action maps.
    filepath = vr_get_default_config_path()

    if not os.path.exists(filepath):
        # Create and save default action maps.
        vr_defaults_create_default(session_state)
        vr_defaults_create_default_gamepad(session_state)

        action_map.vr_save_actionmaps(session_state, filepath, sort=False)

    loaded = action_map.vr_load_actionmaps(session_state, filepath)

    return loaded
