from ..action_profile import VRActionProfile
from ..defaults import VRDefaultActions, VRDefaultActionprofiles

class VRActionProfileOculus(VRActionProfile):
    def __init__(self):
        super().__init__(VRDefaultActionbindings.OCULUS.value)
        self.profile = VRDefaultActionprofiles.OCULUS.value

        self.action_map[VRDefaultActions.NAV_GRAB.value].update({
            "component_paths": ["/input/squeeze/value", "/input/squeeze/value"],
        })

        self.action_map[VRDefaultActions.NAV_RESET.value].update({
            "component_paths": ["/input/x/click", "/input/a/click"],
        })
