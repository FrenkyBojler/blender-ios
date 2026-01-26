from ..action import VRActionFloatLeftHanded, VRActionFloatRightHanded, VRActionPathType
from ..action_profile import VRDefaultActions, VRDefaultActionmaps


class VRActionFly(VRActionFloatLeftHanded):
    name = VRDefaultActions.FLY.value
    op = "wm.xr_navigation_fly"
    map_name = VRDefaultActionmaps.GAMEPAD.value
    path_type = VRActionPathType.GAMEPAD
    included_maps = {VRDefaultActionmaps.GAMEPAD.value}


class VRActionFlyForward(VRActionFloatLeftHanded):
    name = VRDefaultActions.FLY_FORWARD.value
    op = "wm.xr_navigation_fly"
    op_properties = [("mode", 'VIEWER_FORWARD'), ("lock_location_z", True)]
    included_maps = {
        VRDefaultActionmaps.DEFAULT.value,
        VRDefaultActionmaps.GAMEPAD.value,
    }


class VRActionFlyBack(VRActionFloatLeftHanded):
    name = VRDefaultActions.FLY_BACK.value
    op = "wm.xr_navigation_fly"
    op_properties = [("mode", 'VIEWER_BACK'), ("lock_location_z", True)]
    included_maps = {
        VRDefaultActionmaps.DEFAULT.value,
        VRDefaultActionmaps.GAMEPAD.value,
    }


class VRActionFlyLeft(VRActionFloatLeftHanded):
    name = VRDefaultActions.FLY_LEFT.value
    op = "wm.xr_navigation_fly"
    op_properties = [("mode", 'VIEWER_LEFT'), ("lock_location_z", True)]
    included_maps = {
        VRDefaultActionmaps.DEFAULT.value,
        VRDefaultActionmaps.GAMEPAD.value,
    }


class VRActionFlyRight(VRActionFloatLeftHanded):
    name = VRDefaultActions.FLY_RIGHT.value
    op = "wm.xr_navigation_fly"
    op_properties = [("mode", 'VIEWER_RIGHT'), ("lock_location_z", True)]
    included_maps = {
        VRDefaultActionmaps.DEFAULT.value,
        VRDefaultActionmaps.GAMEPAD.value,
    }


class VRActionFlyUp(VRActionFloatRightHanded):
    name = VRDefaultActions.FLY_UP.value
    op = "wm.xr_navigation_fly"
    op_properties = [("mode", 'UP')]
    included_maps = {
        VRDefaultActionmaps.DEFAULT.value,
        VRDefaultActionmaps.GAMEPAD.value,
    }


class VRActionFlyDown(VRActionFloatRightHanded):
    name = VRDefaultActions.FLY_DOWN.value
    op = "wm.xr_navigation_fly"
    op_properties = [("mode", 'DOWN')]
    included_maps = {
        VRDefaultActionmaps.DEFAULT.value,
        VRDefaultActionmaps.GAMEPAD.value,
    }


class VRActionFlyTurnLeft(VRActionFloatRightHanded):
    name = VRDefaultActions.FLY_TURNLEFT.value
    op = "wm.xr_navigation_fly"
    op_properties = [("mode", 'TURNLEFT')]
    included_maps = {
        VRDefaultActionmaps.DEFAULT.value,
        VRDefaultActionmaps.GAMEPAD.value,
    }


class VRActionFlyTurnRight(VRActionFloatRightHanded):
    name = VRDefaultActions.FLY_TURNRIGHT.value
    op = "wm.xr_navigation_fly"
    op_properties = [("mode", 'TURNRIGHT')]
    included_maps = {
        VRDefaultActionmaps.DEFAULT.value,
        VRDefaultActionmaps.GAMEPAD.value,
    }

