from ..action import VRActionFloat, VRActionPathType
from ..action_profile import VRDefaultActions, VRDefaultActionmaps


class VRActionNavReset(VRActionFloat):
    name = VRDefaultActions.NAV_RESET.value
    op = "wm.xr_navigation_reset"
    op_mode = 'PRESS'
    haptic_name = VRDefaultActions.HAPTIC.value
    haptic_match_user_paths = True
    haptic_duration = 0.3
    haptic_frequency = 3000.0
    haptic_amplitude = 0.5
    op_properties = [("location", False), ("rotation", False), ("scale", True)]
    included_maps = {VRDefaultActionmaps.DEFAULT.value}


class VRActionNavResetGamepad(VRActionNavReset):
    haptic_name = VRDefaultActions.HAPTIC_RIGHT.value
    map_name = VRDefaultActionmaps.GAMEPAD.value
    path_type = VRActionPathType.GAMEPAD
    included_maps = {VRDefaultActionmaps.GAMEPAD.value}

