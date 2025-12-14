from .action_profile import VRDefaultActions


class VRAction():
    def __init__(self):
        self.name = VRDefaultActions.EMPTY.value
        self.user_paths = ["/user/hand/left", "/user/hand/right"]
        self.op = None
        self.op_mode = 'MODAL'
        self.bimanual = False
        self.haptic_name = ""
        self.haptic_match_user_paths = False
        self.haptic_duration = 0.0
        self.haptic_frequency = 0.0
        self.haptic_amplitude = 0.0
        self.haptic_mode = 'PRESS'
        self.op_properties = None

    def vr_action_map_add(self, action_map):
        action_map_item = action_map.actionmap_items.new(self.name, True)

        if action_map_item is None:
            return
        
        action_map_item.type = 'FLOAT'
        for path in self.user_paths:
            action_map_item.user_paths.new(path)
        action_map_item.op = self.op
        action_map_item.op_mode = self.op_mode
        action_map_item.bimanual = self.bimanual
        action_map_item.haptic_name = self.haptic_name
        action_map_item.haptic_match_user_paths = self.haptic_match_user_paths
        action_map_item.haptic_duration = self.haptic_duration
        action_map_item.haptic_frequency = self.haptic_frequency
        action_map_item.haptic_amplitude = self.haptic_amplitude
        action_map_item.haptic_mode = self.haptic_mode

        if self.op_properties:
            for attr, value in self.op_properties:
                try:
                    setattr(action_map_item.op_properties, attr, value)
                except AttributeError:
                    print(f"Warning: property '{attr}' not found in action '{self.name}'")
                except Exception as ex:
                    print(f"Warning: {ex!r}")

        return action_map_item


class VRActionLeftHanded(VRAction):
    def __init__(self):
        super().__init__()
        self.user_paths = ["/user/hand/left"]


class VRActionRightHanded(VRAction):
    def __init__(self):
        super().__init__()
        self.user_paths = ["/user/hand/right"]

