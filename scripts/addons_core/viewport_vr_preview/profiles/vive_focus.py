from ..action_profile import VRActionProfile, VRDefaultActions

class VRActionProfileViveFocus(VRActionProfile):
    requires_opt_in = True
    ui_label = "HTC Vive Focus"
    name = "vive_focus"
    profile = "/interaction_profiles/htc/vive_focus3_controller"
    
    def __init__(self):
        super().__init__()

        self.action_map[VRDefaultActions.NAV_GRAB.value].update({
            "component_paths": ["/input/squeeze/click", "/input/squeeze/click"],
        })
        
        self.action_map[VRDefaultActions.NAV_RESET.value].update({
            "component_paths": ["/input/x/click", "/input/a/click"],
        })
