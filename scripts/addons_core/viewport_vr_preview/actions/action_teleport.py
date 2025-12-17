from ..action import VRActionFloat
from ..action_profile import VRDefaultActions


class VRActionTeleport(VRActionFloat):
    def __init__(self):
        super().__init__()
        self.name = VRDefaultActions.TELEPORT.value
        self.op = "wm.xr_navigation_teleport"

