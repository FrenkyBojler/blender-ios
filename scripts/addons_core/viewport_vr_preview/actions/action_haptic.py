from ..action_profile import VRDefaultActions, VRDefaultActionmaps
from ..action import VRAction


class VRActionHaptic(VRAction):
    def __init__(self):
        super().__init__()
        self.type = 'VIBRATION'
        self.name = VRDefaultActions.HAPTIC.value
        self.included_maps = {VRDefaultActionmaps.DEFAULT.value}


class VRActionHapticLeft(VRActionHaptic):
    def __init__(self):
        super().__init__()
        self.type = 'VIBRATION'
        self.name = VRDefaultActions.HAPTIC_LEFT.value
        self.map_name = VRDefaultActionmaps.GAMEPAD.value
        self.included_maps = {VRDefaultActionmaps.GAMEPAD.value}


class VRActionHapticRight(VRActionHaptic):
    def __init__(self):
        super().__init__()
        self.type = 'VIBRATION'
        self.name = VRDefaultActions.HAPTIC_RIGHT.value
        self.map_name = VRDefaultActionmaps.GAMEPAD.value
        self.included_maps = {VRDefaultActionmaps.GAMEPAD.value}


class VRActionHapticLeftTrigger(VRActionHaptic):
    def __init__(self):
        super().__init__()
        self.type = 'VIBRATION'
        self.name = VRDefaultActions.HAPTIC_LEFTTRIGGER.value
        self.map_name = VRDefaultActionmaps.GAMEPAD.value
        self.included_maps = {VRDefaultActionmaps.GAMEPAD.value}


class VRActionHapticRightTrigger(VRActionHaptic):
    def __init__(self):
        super().__init__()
        self.type = 'VIBRATION'
        self.name = VRDefaultActions.HAPTIC_RIGHTTRIGGER.value
        self.map_name = VRDefaultActionmaps.GAMEPAD.value
        self.included_maps = {VRDefaultActionmaps.GAMEPAD.value}

