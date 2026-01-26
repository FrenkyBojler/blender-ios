# SPDX-FileCopyrightText: 2021-2022 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

from enum import Enum

# Default action maps.
class VRDefaultActionmaps(Enum):
    DEFAULT = "blender_default"
    GAMEPAD = "blender_default_gamepad"


# Default actions.
class VRDefaultActions(Enum):
    EMPTY = "empty"
    CONTROLLER_GRIP = "controller_grip"
    CONTROLLER_AIM = "controller_aim"
    TELEPORT = "teleport"
    NAV_GRAB = "nav_grab"
    FLY = "fly"
    FLY_FORWARD = "fly_forward"
    FLY_BACK = "fly_back"
    FLY_LEFT = "fly_left"
    FLY_RIGHT = "fly_right"
    FLY_UP = "fly_up"
    FLY_DOWN = "fly_down"
    FLY_TURNLEFT = "fly_turnleft"
    FLY_TURNRIGHT = "fly_turnright"
    NAV_RESET = "nav_reset"
    SWAP_HANDS = "swap_hands"
    HAPTIC = "haptic"
    HAPTIC_LEFT = "haptic_left"
    HAPTIC_RIGHT = "haptic_right"
    HAPTIC_LEFTTRIGGER = "haptic_lefttrigger"
    HAPTIC_RIGHTTRIGGER = "haptic_righttrigger"



class VRActionProfile():
    requires_opt_in = False
    ui_label = None
    name = None
    profile = None
    def __init__(self):

        self.action_map = {
            VRDefaultActions.CONTROLLER_GRIP.value: {
                "component_paths": ["/input/grip/pose", "/input/grip/pose"],
                "pose_location": (0.0, 0.0, 0.0),
                "pose_rotation": (0.0, 0.0, 0.0),
            },
            
            VRDefaultActions.CONTROLLER_AIM.value: {
                "component_paths": ["/input/aim/pose", "/input/aim/pose"],
                "pose_location": (0.0, 0.0, 0.0),
                "pose_rotation": (0.0, 0.0, 0.0),
            },

            VRDefaultActions.TELEPORT.value: {
                "component_paths": ["/input/trigger/value", "/input/trigger/value"],
                "threshold": 0.3,
                "axis_region": "ANY",
            },
            
            VRDefaultActions.NAV_GRAB.value: {
                "component_paths": ["/input/squeeze/click", "/input/squeeze/click"],
                "threshold": 0.3,
                "axis_region": "ANY",
            },
            
            VRDefaultActions.FLY_FORWARD.value: {
                "component_paths": ["/input/thumbstick/y"],
                "threshold": 0.3,
                "axis_region": "POSITIVE",
            },
            
            VRDefaultActions.FLY_BACK.value: {
                "component_paths": ["/input/thumbstick/y"],
                "threshold": 0.3,
                "axis_region": "NEGATIVE",
            },
            
            VRDefaultActions.FLY_LEFT.value: {
                "component_paths": ["/input/thumbstick/x"],
                "threshold": 0.3,
                "axis_region": "NEGATIVE",
            },
            
            VRDefaultActions.FLY_RIGHT.value: {
                "component_paths": ["/input/thumbstick/x"],
                "threshold": 0.3,
                "axis_region": "POSITIVE",
            },
            
            VRDefaultActions.FLY_UP.value: {
                "component_paths": ["/input/thumbstick/y"],
                "threshold": 0.3,
                "axis_region": "POSITIVE",
            },
            
            VRDefaultActions.FLY_DOWN.value: {
                "component_paths": ["/input/thumbstick/y"],
                "threshold": 0.3,
                "axis_region": "NEGATIVE",
            },
            
            VRDefaultActions.FLY_TURNLEFT.value: {
                "component_paths": ["/input/thumbstick/x"],
                "threshold": 0.3,
                "axis_region": "NEGATIVE",
            },
            
            VRDefaultActions.FLY_TURNRIGHT.value: {
                "component_paths": ["/input/thumbstick/x"],
                "threshold": 0.3,
                "axis_region": "POSITIVE",
            },

            VRDefaultActions.NAV_RESET.value: {
                "component_paths": ["/input/x/click", "/input/a/click"],
                "threshold": 0.3,
                "axis_region": "ANY",
            },
                        
            VRDefaultActions.HAPTIC.value: {
                "component_paths": ["/output/haptic", "/output/haptic"],
            },
        }
