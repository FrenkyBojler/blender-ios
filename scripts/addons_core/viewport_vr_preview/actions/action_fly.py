from ..action import VRActionFloatLeftHanded, VRActionFloatRightHanded
from ..action_profile import VRDefaultActions


class VRActionFlyForward(VRActionFloatLeftHanded):
    def __init__(self):
        super().__init__()
        self.name = VRDefaultActions.FLY_FORWARD.value
        self.op = "wm.xr_navigation_fly"
        self.op_properties = [("mode", 'VIEWER_FORWARD'), ("lock_location_z", True)]


class VRActionFlyBack(VRActionFloatLeftHanded):
    def __init__(self):
        super().__init__()
        self.name = VRDefaultActions.FLY_BACK.value
        self.op = "wm.xr_navigation_fly"
        self.op_properties = [("mode", 'VIEWER_BACK'), ("lock_location_z", True)]


class VRActionFlyLeft(VRActionFloatLeftHanded):
    def __init__(self):
        super().__init__()
        self.name = VRDefaultActions.FLY_LEFT.value
        self.op = "wm.xr_navigation_fly"
        self.op_properties = [("mode", 'VIEWER_LEFT'), ("lock_location_z", True)]


class VRActionFlyRight(VRActionFloatLeftHanded):
    def __init__(self):
        super().__init__()
        self.name = VRDefaultActions.FLY_RIGHT.value
        self.op = "wm.xr_navigation_fly"
        self.op_properties = [("mode", 'VIEWER_RIGHT'), ("lock_location_z", True)]


class VRActionFlyUp(VRActionFloatRightHanded):
    def __init__(self):
        super().__init__()
        self.name = VRDefaultActions.FLY_UP.value
        self.op = "wm.xr_navigation_fly"
        self.op_properties = [("mode", 'UP')]


class VRActionFlyDown(VRActionFloatRightHanded):
    def __init__(self):
        super().__init__()
        self.name = VRDefaultActions.FLY_DOWN.value
        self.op = "wm.xr_navigation_fly"
        self.op_properties = [("mode", 'DOWN')]


class VRActionFlyTurnLeft(VRActionFloatRightHanded):
    def __init__(self):
        super().__init__()
        self.name = VRDefaultActions.FLY_TURNLEFT.value
        self.op = "wm.xr_navigation_fly"
        self.op_properties = [("mode", 'TURNLEFT')]


class VRActionFlyTurnRight(VRActionFloatRightHanded):
    def __init__(self):
        super().__init__()
        self.name = VRDefaultActions.FLY_TURNRIGHT.value
        self.op = "wm.xr_navigation_fly"
        self.op_properties = [("mode", 'TURNRIGHT')]

