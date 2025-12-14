from ..action import VRAction
from ..action_profile import VRDefaultActions


class VRActionNavGrab(VRAction):
    def __init__(self):
        super().__init__()
        self.name = VRDefaultActions.NAV_GRAB.value
        self.op = "wm.xr_navigation_grab"
        self.bimanual = True
        self.op_properties = [("lock_rotation", True)]

