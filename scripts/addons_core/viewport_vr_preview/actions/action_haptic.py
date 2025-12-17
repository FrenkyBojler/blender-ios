from ..defaults import VRDefaultActions
from ..action import VRAction


class VRActionHaptic(VRAction):
    def __init__(self):
        super().__init__()
        self.type = 'VIBRATION'
        self.name = VRDefaultActions.HAPTIC.value


class VRActionHapticLeft(VRActionHaptic):
    def __init__(self):
        super().__init__()
        self.type = 'VIBRATION'
        self.name = VRDefaultActions.HAPTIC_LEFT.value


class VRActionHapticRight(VRActionHaptic):
    def __init__(self):
        super().__init__()
        self.type = 'VIBRATION'
        self.name = VRDefaultActions.HAPTIC_RIGHT.value


class VRActionHapticLeftTrigger(VRActionHaptic):
    def __init__(self):
        super().__init__()
        self.type = 'VIBRATION'
        self.name = VRDefaultActions.HAPTIC_LEFTTRIGGER.value


class VRActionHapticRightTrigger(VRActionHaptic):
    def __init__(self):
        super().__init__()
        self.type = 'VIBRATION'
        self.name = VRDefaultActions.HAPTIC_RIGHTTRIGGER.value

