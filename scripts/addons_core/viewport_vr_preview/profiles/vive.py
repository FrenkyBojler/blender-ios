from ..action_profile import VRActionProfile
from ..defaults import VRDefaultActions, VRDefaultActionprofiles

class VRActionProfileVive(VRActionProfile):
    def __init__(self):
        super().__init__(VRDefaultActionbindings.VIVE.value)
        self.profile = VRDefaultActionprofiles.VIVE.value

        self.action_map[VRDefaultActions.NAV_GRAB.value].update({
            "component_paths": ["/input/squeeze/click", "/input/squeeze/click"],
        })

        self.action_map[VRDefaultActions.NAV_RESET.value].update({
            "component_paths": ["/input/menu/click", "/input/menu/click"],
        })
