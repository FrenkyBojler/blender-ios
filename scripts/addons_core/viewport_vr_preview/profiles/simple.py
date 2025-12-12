from ..action_profile import VRActionProfile
from ..defaults import VRDefaultActions, VRDefaultActionprofiles

class VRActionProfileSimple(VRActionProfile):
    def __init__(self):
        super().__init__("Simple Controller")
        self.profile = VRDefaultActionprofiles.SIMPLE.value

        self.action_map[VRDefaultActions.TELEPORT.value].update({
            "component_paths": ["/input/select/click", "/input/select/click"],
        })

        """
        self.action_map[VRDefaultActions.NAV_GRAB.value].update({
            "component_paths": ["/input/menu/click", "/input/menu/click"],
        })
        """

        # Khronos simple profile does not have navigation grab
        self.action_map[VRDefaultActions.NAV_GRAB.value] = None

        self.action_map[VRDefaultActions.NAV_RESET.value].update({
            "component_paths": ["/input/menu/click", "/input/menu/click"],
        })
