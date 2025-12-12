from ..action_profile import VRActionProfile
from ..defaults import VRDefaultActions, VRDefaultActionprofiles

class VRActionProfileWMR(VRActionProfile):
    def __init__(self):
        super().__init__("Windows MR")
        self.profile = VRDefaultActionprofiles.WMR.value

        self.action_map[VRDefaultActions.NAV_GRAB.value].update({
            "component_paths": ["/input/squeeze/click", "/input/squeeze/click"],
        })

        self.action_map[VRDefaultActions.NAV_RESET.value].update({
            "component_paths": ["/input/menu/click", "/input/menu/click"],
        })
