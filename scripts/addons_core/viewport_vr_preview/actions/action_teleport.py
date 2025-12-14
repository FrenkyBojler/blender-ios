from ..action import VRAction
from ..action_profile import VRDefaultActions


class VRActionTeleport(VRAction):
    def __init__(self):
        super().__init__()
        self.name = VRDefaultActions.TELEPORT.value
        self.op = "wm.xr_navigation_teleport"

