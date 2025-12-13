from ..action_profile import VRActionProfile, VRDefaultActions, VRDefaultActionprofiles, VRDefaultActionbindings

# Huawei controller action profile
class VRActionProfileHuawei(VRActionProfile):
    def __init__(self):
        super().__init__(VRDefaultActionbindings.HUAWEI.value)
        self.profile = VRDefaultActionprofiles.HUAWEI.value
        
        self.action_map[VRDefaultActions.NAV_GRAB.value].update({
                "component_paths": ["/input/trackpad/click", "/input/trackpad/click"],
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
                "component_paths": ["/input/back/click", "/input/back/click"],
            })
