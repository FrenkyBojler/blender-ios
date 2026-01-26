from ..action import VRActionFloat
from ..action_profile import VRDefaultActions, VRDefaultActionmaps


class VRActionTeleport(VRActionFloat):
    def __init__(self):
        super().__init__()
        self.name = VRDefaultActions.TELEPORT.value
        self.op = "wm.xr_navigation_teleport"
        self.included_maps = {
            VRDefaultActionmaps.DEFAULT.value,
            VRDefaultActionmaps.GAMEPAD.value,
        }

