

from ..action_profile import VRActionProfile, VRDefaultActions

class VRActionProfileGamepad(VRActionProfile):
    def __init__(self):
        super().__init__("gamepad")
        self.profile = "/interaction_profiles/microsoft/xbox_controller"

        self.action_map[VRDefaultActions.CONTROLLER_GRIP] = None
        self.action_map[VRDefaultActions.CONTROLLER_AIM] = None
        self.action_map[VRDefaultActions.NAV_GRAB] = None
        self.action_map[VRDefaultActions.HAPTIC] = None

        self.action_map[VRDefaultActions.TELEPORT.value].update({
            "component_paths": ["/input/trigger_right/value"],
        })

        self.action_map[VRDefaultActions.FLY.value] = {
            "component_paths": ["/input/trigger_left/value"],
            "threshold": 0.3,
            "axis_region": "ANY",
        }

        self.action_map[VRDefaultActions.FLY_FORWARD.value].update({
            "component_paths": ["/input/thumbstick_left/y"],
        })

        self.action_map[VRDefaultActions.FLY_BACK.value].update({
            "component_paths": ["/input/thumbstick_left/y"],
        })
    
        self.action_map[VRDefaultActions.FLY_LEFT.value].update({
            "component_paths": ["/input/thumbstick_left/x"],
        })
        
        self.action_map[VRDefaultActions.FLY_RIGHT.value].update({
            "component_paths": ["/input/thumbstick_left/x"],
        })  
        
        self.action_map[VRDefaultActions.FLY_UP.value].update({
            "component_paths": ["/input/thumbstick_right/y"],
        })
        
        self.action_map[VRDefaultActions.FLY_DOWN.value].update({
            "component_paths": ["/input/thumbstick_right/y"],
        })
        
        self.action_map[VRDefaultActions.FLY_TURNLEFT.value].update({
            "component_paths": ["/input/thumbstick_right/x"],
        })
        
        self.action_map[VRDefaultActions.FLY_TURNRIGHT.value].update({
            "component_paths": ["/input/thumbstick_right/x"],
        })

        self.action_map[VRDefaultActions.NAV_RESET.value].update({
            "component_paths": ["/input/a/click"],
        })

        self.action_map[VRDefaultActions.HAPTIC_LEFT.value] = {
            "component_paths": ["/output/haptic_left"],
        }

        self.action_map[VRDefaultActions.HAPTIC_RIGHT.value] = {
            "component_paths": ["/output/haptic_right"],
        }

        self.action_map[VRDefaultActions.HAPTIC_LEFTTRIGGER.value] = {
            "component_paths": ["/output/haptic_left_trigger"],
        }

        self.action_map[VRDefaultActions.HAPTIC_RIGHTTRIGGER.value] = {
            "component_paths": ["/output/haptic_right_trigger"],
        }


