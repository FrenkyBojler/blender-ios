import enum
from .action_profile import VRDefaultActions, VRDefaultActionmaps
from enum import Enum

class VRActionPathType(Enum):
    LEFT_HANDED = ["/user/hand/left"]
    RIGHT_HANDED = ["/user/hand/right"]
    DUAL_HANDED = ["/user/hand/left", "/user/hand/right"]
    GAMEPAD = ["/user/gamepad"]    

class VRAction():
    name = VRDefaultActions.EMPTY.value
    type = ''
    path_type = VRActionPathType.DUAL_HANDED
    map_name = None
    included_maps = {VRDefaultActionmaps.DEFAULT.value}

    def enable_gamepad(self):
        self.path_type = VRActionPathType.GAMEPAD

    def vr_action_map_add(self, action_map, actionmap_name=None):
        action_map_item = action_map.actionmap_items.new(self.name, True)

        if action_map_item is None:
            return None
        
        action_map_item.type = self.type
        path_type = self.path_type
        if actionmap_name == VRDefaultActionmaps.GAMEPAD.value:
            path_type = VRActionPathType.GAMEPAD
        for path in path_type.value:
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
    type = 'FLOAT'
    op = None
    op_mode = 'MODAL'
    bimanual = False
    haptic_name = ""
    haptic_match_user_paths = False
    haptic_duration = 0.0
    haptic_frequency = 0.0
    haptic_amplitude = 0.0
    haptic_mode = 'PRESS'
    op_properties = None

    def vr_action_map_add(self, action_map, actionmap_name=None):
        action_map_item = super().vr_action_map_add(action_map, actionmap_name)
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


class VRActionFloatLeftHanded(VRActionFloat):
    path_type = VRActionPathType.LEFT_HANDED


class VRActionFloatRightHanded(VRActionFloat):
    path_type = VRActionPathType.RIGHT_HANDED

