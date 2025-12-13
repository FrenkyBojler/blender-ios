from ..action_profile import VRActionProfile, VRDefaultActions, VRDefaultActionprofiles, VRDefaultActionbindings

class VRActionProfileWMR(VRActionProfile):
    def __init__(self):
        super().__init__(VRDefaultActionbindings.WMR.value)
        self.profile = VRDefaultActionprofiles.WMR.value

        self.action_map[VRDefaultActions.NAV_GRAB.value].update({
            "component_paths": ["/input/squeeze/click", "/input/squeeze/click"],
        })

        self.action_map[VRDefaultActions.NAV_RESET.value].update({
            "component_paths": ["/input/menu/click", "/input/menu/click"],
        })
