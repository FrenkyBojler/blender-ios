# SPDX-FileCopyrightText: 2021-2022 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

if "bpy" in locals():
    import importlib
    importlib.reload(action_map)
else:
    from . import action_map

import bpy
from bpy.app.handlers import persistent
from enum import Enum
import math
import os.path
from .defaults import VRDefaultActions

class VRActionBinding():
    def __init__(self, action, path, threshold=None):
        self.action = action
        self.path = path
        self.threshold = threshold

class VRActionProfile():
    def __init__(self, name):
        self.name = name
        self.profile = None

        # TODO: Fill in with proper default values
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