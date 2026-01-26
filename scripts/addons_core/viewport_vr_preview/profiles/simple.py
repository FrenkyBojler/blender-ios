from ..action_profile import VRActionProfile, VRDefaultActions

class VRActionProfileSimple(VRActionProfile):
    def __init__(self):
        super().__init__("simple")
        self.profile = "/interaction_profiles/khr/simple_controller"

        self.action_map[VRDefaultActions.TELEPORT.value].update({
            "component_paths": ["/input/select/click", "/input/select/click"],
        })

        self.action_map[VRDefaultActions.NAV_RESET.value].update({
            "component_paths": ["/input/menu/click", "/input/menu/click"],
        })

        # Khronos simple profile does not have these actions
        self.action_map[VRDefaultActions.NAV_GRAB.value] = None
        self.action_map[VRDefaultActions.FLY_FORWARD.value] = None
        self.action_map[VRDefaultActions.FLY_BACK.value] = None
        self.action_map[VRDefaultActions.FLY_LEFT.value] = None
        self.action_map[VRDefaultActions.FLY_RIGHT.value] = None
        self.action_map[VRDefaultActions.FLY_UP.value] = None
        self.action_map[VRDefaultActions.FLY_DOWN.value] = None
        self.action_map[VRDefaultActions.FLY_TURNLEFT.value] = None
        self.action_map[VRDefaultActions.FLY_TURNRIGHT.value] = None
