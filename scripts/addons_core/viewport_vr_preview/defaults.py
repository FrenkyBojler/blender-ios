# SPDX-FileCopyrightText: 2021-2022 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

if "bpy" in locals():
    import importlib
    importlib.reload(action_map)
else:
    from . import action_map

import bpy
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
from .profiles.gamepad import VRActionProfileGamepad

from .actions.action_fly import VRActionFly, VRActionFlyForward, VRActionFlyBack, VRActionFlyLeft, VRActionFlyRight, VRActionFlyUp, VRActionFlyDown, VRActionFlyTurnLeft, VRActionFlyTurnRight
from .actions.action_nav_reset import VRActionNavReset, VRActionNavResetGamepad
from .actions.action_teleport import VRActionTeleport
from .actions.action_nav_grab import VRActionNavGrab
from .actions.action_pose import VRActionControllerGrip, VRActionControllerAim
from .actions.action_haptic import VRActionHaptic, VRActionHapticLeft, VRActionHapticRight, VRActionHapticLeftTrigger, VRActionHapticRightTrigger

def vr_defaults_actionmap_add(session_state, name):
    am = session_state.actionmaps.new(session_state, name, True)

    return am


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
    
    if not am:
        return
    
    action_profile = VRActionProfileGamepad()

    actions = [
        VRActionTeleport(),
        VRActionFly(),
        VRActionFlyForward(),
        VRActionFlyBack(),
        VRActionFlyLeft(),
        VRActionFlyRight(),
        VRActionFlyUp(),
        VRActionFlyDown(),
        VRActionFlyTurnLeft(),
        VRActionFlyTurnRight(),
        VRActionNavResetGamepad(),
        VRActionHapticLeft(),
        VRActionHapticRight(),
        VRActionHapticLeftTrigger(),
        VRActionHapticRightTrigger(),
    ]

    for action in actions:
        action.enable_gamepad()
        action_map_item = action.vr_action_map_add(am)
        if not action_map_item:
            continue
        
        action.vr_action_map_item_add(action_map_item, action_profile)


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
