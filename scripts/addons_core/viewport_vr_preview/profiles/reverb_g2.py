from ..action_profile import VRActionProfile, VRDefaultActions

class VRActionProfileReverbG2(VRActionProfile):
    requires_opt_in = True
    ui_label = "HP Reverb G2"
    name = "reverb_g2"
    profile = "/interaction_profiles/hp/mixed_reality_controller"

    def __init__(self):
        super().__init__()

        self.action_map[VRDefaultActions.NAV_GRAB.value].update({
            "component_paths": ["/input/squeeze/value", "/input/squeeze/value"],
        })

        self.action_map[VRDefaultActions.NAV_RESET.value].update({
            "component_paths": ["/input/x/click", "/input/a/click"],
        })
