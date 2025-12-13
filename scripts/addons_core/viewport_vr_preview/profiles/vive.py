from ..action_profile import VRActionProfile, VRDefaultActions, VRDefaultActionprofiles, VRDefaultActionbindings

class VRActionProfileVive(VRActionProfile):
    def __init__(self):
        super().__init__(VRDefaultActionbindings.VIVE.value)
        self.profile = VRDefaultActionprofiles.VIVE.value

        self.action_map[VRDefaultActions.NAV_GRAB.value].update({
            "component_paths": ["/input/squeeze/click", "/input/squeeze/click"],
        })

        self.action_map[VRDefaultActions.FLY_FORWARD.value].update({
                "component_paths": ["/input/trackpad/y"],
            })

        self.action_map[VRDefaultActions.FLY_BACK.value].update({
                "component_paths": ["/input/trackpad/y"],
            })
        
        self.action_map[VRDefaultActions.FLY_LEFT.value].update({
                "component_paths": ["/input/trackpad/x"],
            })
        
        self.action_map[VRDefaultActions.FLY_RIGHT.value].update({
                "component_paths": ["/input/trackpad/x"],
            })  
        
        self.action_map[VRDefaultActions.FLY_UP.value].update({
                "component_paths": ["/input/trackpad/y"],
            })
        
        self.action_map[VRDefaultActions.FLY_DOWN.value].update({
                "component_paths": ["/input/trackpad/y"],
            })
        
        self.action_map[VRDefaultActions.FLY_TURNLEFT.value].update({
                "component_paths": ["/input/trackpad/x"],
            })
        
        self.action_map[VRDefaultActions.FLY_TURNRIGHT.value].update({
                "component_paths": ["/input/trackpad/x"],
            })

        self.action_map[VRDefaultActions.NAV_RESET.value].update({
            "component_paths": ["/input/menu/click", "/input/menu/click"],
        })
