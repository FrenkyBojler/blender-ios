from ..action_profile import VRActionProfile, VRDefaultActions

class VRActionProfileOculus(VRActionProfile):
    def __init__(self):
        super().__init__("oculus")
        self.profile = "/interaction_profiles/oculus/touch_controller"

        self.action_map[VRDefaultActions.NAV_GRAB.value].update({
            "component_paths": ["/input/squeeze/value", "/input/squeeze/value"],
        })

        self.action_map[VRDefaultActions.NAV_RESET.value].update({
            "component_paths": ["/input/x/click", "/input/a/click"],
        })
