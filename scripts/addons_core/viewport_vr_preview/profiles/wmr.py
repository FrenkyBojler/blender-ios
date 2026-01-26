from ..action_profile import VRActionProfile, VRDefaultActions

class VRActionProfileWMR(VRActionProfile):
    name = "wmr"
    profile = "/interaction_profiles/microsoft/motion_controller"
    
    def __init__(self):
        super().__init__()

        self.action_map[VRDefaultActions.NAV_GRAB.value].update({
            "component_paths": ["/input/squeeze/click", "/input/squeeze/click"],
        })

        self.action_map[VRDefaultActions.NAV_RESET.value].update({
            "component_paths": ["/input/menu/click", "/input/menu/click"],
        })
