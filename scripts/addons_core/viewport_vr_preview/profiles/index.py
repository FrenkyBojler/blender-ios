

from ..action_profile import VRActionProfile, VRDefaultActions

class VRActionProfileIndex(VRActionProfile):
    name = "index"
    profile = "/interaction_profiles/valve/index_controller"
    
    def __init__(self):
        super().__init__()

        self.action_map[VRDefaultActions.NAV_GRAB.value].update({
            "component_paths": ["/input/squeeze/force", "/input/squeeze/force"],
            "threshold": 0.5,
        })

        self.action_map[VRDefaultActions.NAV_RESET.value].update({
            "component_paths": ["/input/a/click", "/input/a/click"],
        })
