from ..action_profile import VRDefaultActions
from ..action import VRAction


class VRActionPose(VRAction):
    type = 'POSE'
    pose_is_controller_grip = False
    pose_is_controller_aim = False
    
    def vr_action_map_add(self, action_map):
        action_map_item = super().vr_action_map_add(action_map)
        if action_map_item is None:
            return None

        action_map_item.pose_is_controller_grip = self.pose_is_controller_grip
        action_map_item.pose_is_controller_aim = self.pose_is_controller_aim
        return action_map_item

    def vr_action_map_item_add(self, action_map_item, action_profile):
        action_map_binding = super().vr_action_map_item_add(action_map_item, action_profile)
        
        if action_map_binding is None:
            return None
        
        action_properties = action_profile.action_map[self.name]
        action_map_binding.pose_location = action_properties["pose_location"]
        action_map_binding.pose_rotation = action_properties["pose_rotation"]
        return action_map_binding


class VRActionControllerGrip(VRActionPose):
    name = VRDefaultActions.CONTROLLER_GRIP.value
    pose_is_controller_grip = True


class VRActionControllerAim(VRActionPose):
    name = VRDefaultActions.CONTROLLER_AIM.value
    pose_is_controller_aim = True

