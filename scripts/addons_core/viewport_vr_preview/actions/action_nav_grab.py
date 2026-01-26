from ..action import VRActionFloat
from ..action_profile import VRDefaultActions


class VRActionNavGrab(VRActionFloat):
    name = VRDefaultActions.NAV_GRAB.value
    op = "wm.xr_navigation_grab"
    bimanual = True
    op_properties = [("lock_rotation", True)]

