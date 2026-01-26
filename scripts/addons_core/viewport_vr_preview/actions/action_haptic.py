from ..action_profile import VRDefaultActions, VRDefaultActionmaps
from ..action import VRAction


class VRActionHaptic(VRAction):
    type = 'VIBRATION'
    name = VRDefaultActions.HAPTIC.value
    included_maps = {VRDefaultActionmaps.DEFAULT.value}


class VRActionHapticLeft(VRActionHaptic):
    type = 'VIBRATION'
    name = VRDefaultActions.HAPTIC_LEFT.value
    map_name = VRDefaultActionmaps.GAMEPAD.value
    included_maps = {VRDefaultActionmaps.GAMEPAD.value}


class VRActionHapticRight(VRActionHaptic):
    type = 'VIBRATION'
    name = VRDefaultActions.HAPTIC_RIGHT.value
    map_name = VRDefaultActionmaps.GAMEPAD.value
    included_maps = {VRDefaultActionmaps.GAMEPAD.value}


class VRActionHapticLeftTrigger(VRActionHaptic):
    type = 'VIBRATION'
    name = VRDefaultActions.HAPTIC_LEFTTRIGGER.value
    map_name = VRDefaultActionmaps.GAMEPAD.value
    included_maps = {VRDefaultActionmaps.GAMEPAD.value}


class VRActionHapticRightTrigger(VRActionHaptic):
    type = 'VIBRATION'
    name = VRDefaultActions.HAPTIC_RIGHTTRIGGER.value
    map_name = VRDefaultActionmaps.GAMEPAD.value
    included_maps = {VRDefaultActionmaps.GAMEPAD.value}

