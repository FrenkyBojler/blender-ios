from ..action_profile import VRActionProfile, VRDefaultActions

class VRActionProfileViveFocus(VRActionProfile):
    def __init__(self):
        super().__init__("vive_focus")
        self.profile = "/interaction_profiles/htc/vive_focus3_controller"
        self.requires_opt_in = True
        self.ui_label = "HTC Vive Focus"

        self.action_map[VRDefaultActions.NAV_GRAB.value].update({
            "component_paths": ["/input/squeeze/click", "/input/squeeze/click"],
        })
        
        self.action_map[VRDefaultActions.NAV_RESET.value].update({
            "component_paths": ["/input/x/click", "/input/a/click"],
        })
