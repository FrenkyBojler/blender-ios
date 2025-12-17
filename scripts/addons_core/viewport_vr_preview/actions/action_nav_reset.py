from ..action import VRActionFloat
from ..action_profile import VRDefaultActions


class VRActionNavReset(VRActionFloat):
    def __init__(self):
        super().__init__()
        self.name = VRDefaultActions.NAV_RESET.value
        self.op = "wm.xr_navigation_reset"
        self.op_mode = 'PRESS'

        self.haptic_name = VRDefaultActions.HAPTIC.value
        self.haptic_match_user_paths = True
        self.haptic_duration = 0.3
        self.haptic_frequency = 3000.0
        self.haptic_amplitude = 0.5

        self.op_properties = [("location", False), ("rotation", False), ("scale", True)]


class VRActionNavResetGamepad(VRActionNavReset):
    def __init__(self):
        super().__init__()
        self.haptic_name = VRDefaultActions.HAPTIC_RIGHT.value

