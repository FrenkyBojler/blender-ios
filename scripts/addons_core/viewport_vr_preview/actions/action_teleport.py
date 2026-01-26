from ..action import VRActionFloat
from ..action_profile import VRDefaultActions, VRDefaultActionmaps


class VRActionTeleport(VRActionFloat):
    name = VRDefaultActions.TELEPORT.value
    op = "wm.xr_navigation_teleport"
    included_maps = {
        VRDefaultActionmaps.DEFAULT.value,
        VRDefaultActionmaps.GAMEPAD.value,
    }

