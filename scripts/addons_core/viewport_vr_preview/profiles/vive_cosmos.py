from ..action_profile import VRActionProfile, VRDefaultActions, VRDefaultActionprofiles, VRDefaultActionbindings

class VRActionProfileViveCosmos(VRActionProfile):
    def __init__(self):
        super().__init__(VRDefaultActionbindings.VIVE_COSMOS.value)
        self.profile = VRDefaultActionprofiles.VIVE_COSMOS.value

        self.action_map[VRDefaultActions.NAV_GRAB.value].update({
            "component_paths": ["/input/squeeze/click", "/input/squeeze/click"],
        })
        
        self.action_map[VRDefaultActions.NAV_RESET.value].update({
            "component_paths": ["/input/x/click", "/input/a/click"],
        })
