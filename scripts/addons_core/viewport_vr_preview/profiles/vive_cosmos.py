from ..action_profile import VRActionProfile, VRDefaultActions

class VRActionProfileViveCosmos(VRActionProfile):
    requires_opt_in = True
    ui_label = "HTC Vive Cosmos"
    name = "vive_cosmos"
    profile = "/interaction_profiles/htc/vive_cosmos_controller"
    
    def __init__(self):
        super().__init__()

        self.action_map[VRDefaultActions.NAV_GRAB.value].update({
            "component_paths": ["/input/squeeze/click", "/input/squeeze/click"],
        })
        
        self.action_map[VRDefaultActions.NAV_RESET.value].update({
            "component_paths": ["/input/x/click", "/input/a/click"],
        })
