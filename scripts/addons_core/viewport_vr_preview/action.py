from .action_profile import VRDefaultActions


class VRAction():
    def __init__(self):
        self.name = VRDefaultActions.EMPTY.value
        self.type = ''
        self.user_paths = ["/user/hand/left", "/user/hand/right"]

    def vr_action_map_add(self, action_map):
        action_map_item = action_map.actionmap_items.new(self.name, True)

        if action_map_item is None:
            return None
        
        action_map_item.type = self.type
        for path in self.user_paths:
            action_map_item.user_paths.new(path)
        
        return action_map_item

    def vr_action_map_item_add(self, action_map_item, action_profile):
        if action_profile.action_map[self.name] is None:
            return None

        action_map_binding = action_map_item.bindings.new(action_profile.name, True)
        if not action_map_binding:
            return None

        action_map_binding.profile = action_profile.profile
        action_properties = action_profile.action_map[self.name]

        for path in action_properties["component_paths"]:
            action_map_binding.component_paths.new(path)
        
        return action_map_binding


class VRActionFloat(VRAction):
    def __init__(self):
        super().__init__()
        self.type = 'FLOAT'
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
        action_map_item = super().vr_action_map_add(action_map)
        if action_map_item is None:
            return None
        
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
    
    def vr_action_map_item_add(self, action_map_item, action_profile):
        action_map_binding = super().vr_action_map_item_add(action_map_item, action_profile)

        if action_map_binding is None:
            return None
        
        action_properties = action_profile.action_map[self.name]
        action_map_binding.threshold = action_properties["threshold"]
        action_map_binding.axis0_region = action_properties["axis_region"]
        action_map_binding.axis1_region = "ANY"
        return action_map_binding


class VRActionHaptic(VRAction):
    def __init__(self):
        super().__init__()
        self.type = 'VIBRATION'
        self.name = VRDefaultActions.HAPTIC.value


class VRActionFloatLeftHanded(VRActionFloat):
    def __init__(self):
        super().__init__()
        self.user_paths = ["/user/hand/left"]


class VRActionFloatRightHanded(VRActionFloat):
    def __init__(self):
        super().__init__()
        self.user_paths = ["/user/hand/right"]

