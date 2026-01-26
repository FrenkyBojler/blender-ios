import importlib
import inspect
import pkgutil

from .action import VRAction, VRActionPathType
from .action_profile import (
    VRDefaultActions,
    VRDefaultActionmaps,
    VRActionProfile
)
from . import actions, profiles


class ActionRegistry:
    def __init__(self):
        self.actions = {}
        self.profiles = {}
        self.dirty = True

    def register_action(self, action):
        bucket = self.actions.get(action.name)
        if bucket is None:
            self.actions[action.name] = [action]
        else:
            bucket.append(action)
        self.dirty = True

    def register_profile(self, profile):
        if profile.name in self.profiles:
            return
        self.profiles[profile.name] = profile
        self.dirty = True

    def ensure_actionmaps(self, session_state):
        if not session_state:
            return False
        needs_build = self.dirty
        for name in (VRDefaultActionmaps.DEFAULT.value, VRDefaultActionmaps.GAMEPAD.value):
            idx = session_state.actionmaps.find(session_state, name)
            if idx is None or idx < 0:
                needs_build = True
        if needs_build:
            self.build_actionmaps(session_state)
            self.dirty = False
        return True

    def build_actionmaps(self, session_state):
        self._remove_actionmap(session_state, VRDefaultActionmaps.DEFAULT.value)
        self._remove_actionmap(session_state, VRDefaultActionmaps.GAMEPAD.value)

        default_profiles = [
            name for name in sorted(self.profiles.keys())
            if name != "gamepad"
        ]
        gamepad_profiles = [
            "gamepad"
        ] if "gamepad" in self.profiles else []

        am_default = session_state.actionmaps.new(
            session_state, VRDefaultActionmaps.DEFAULT.value, True
        )
        default_action_names = self._collect_action_names(default_profiles)
        for action_name in default_action_names:
            slot = self._select_action_for_map(action_name, VRDefaultActionmaps.DEFAULT.value)
            if slot is None:
                continue
            self._build_actionmap_item(
                am_default, slot, VRDefaultActionmaps.DEFAULT.value, default_profiles
            )

        if gamepad_profiles:
            am_gamepad = session_state.actionmaps.new(
                session_state, VRDefaultActionmaps.GAMEPAD.value, True
            )
            gamepad_action_names = self._collect_action_names(gamepad_profiles)
            for action_name in gamepad_action_names:
                slot = self._select_action_for_map(action_name, VRDefaultActionmaps.GAMEPAD.value)
                if slot is None:
                    continue
                self._build_actionmap_item(
                    am_gamepad,
                    slot,
                    VRDefaultActionmaps.GAMEPAD.value,
                    gamepad_profiles,
                )

    def _remove_actionmap(self, session_state, name):
        actionmaps = session_state.actionmaps
        idx = actionmaps.find(session_state, name)
        if idx is None or idx < 0:
            return
        try:
            actionmaps.remove(idx)
        except Exception:
            try:
                actionmaps.remove(actionmaps[idx])
            except Exception:
                pass

    def _build_actionmap_item(self, actionmap, slot, actionmap_name, profile_names):
        item = actionmap.actionmap_items.new(slot.name, True)
        item.type = slot.type
        path_type = slot.path_type
        if actionmap_name == VRDefaultActionmaps.GAMEPAD.value:
            path_type = VRActionPathType.GAMEPAD
        if hasattr(path_type, "value"):
            path_type = path_type.value
        for path in path_type:
            item.user_paths.new(path)
        item.pose_is_controller_grip = getattr(slot, "pose_is_controller_grip", False)
        item.pose_is_controller_aim = getattr(slot, "pose_is_controller_aim", False)
        op = getattr(slot, "op", None)
        if op is not None:
            item.op = op
            item.op_mode = getattr(slot, "op_mode", "MODAL")
        item.bimanual = getattr(slot, "bimanual", False)
        item.haptic_name = getattr(slot, "haptic_name", "")
        item.haptic_match_user_paths = getattr(slot, "haptic_match_user_paths", False)
        item.haptic_duration = getattr(slot, "haptic_duration", 0.0)
        item.haptic_frequency = getattr(slot, "haptic_frequency", 0.0)
        item.haptic_amplitude = getattr(slot, "haptic_amplitude", 0.0)
        item.haptic_mode = getattr(slot, "haptic_mode", "PRESS")
        op_properties = getattr(slot, "op_properties", None)
        if op_properties:
            for attr, value in op_properties:
                try:
                    setattr(item.op_properties, attr, value)
                except Exception:
                    pass
        for profile_name in profile_names:
            profile = self.profiles[profile_name]
            binding = profile.action_map.get(slot.name)
            if binding is None:
                continue
            self._build_binding(item, profile, binding)

    def _build_binding(self, item, profile, binding):
        action_map_binding = item.bindings.new(profile.name, True)
        if not action_map_binding:
            return
        action_map_binding.profile = profile.profile
        for path in binding.get("component_paths", []):
            action_map_binding.component_paths.new(path)
        if "threshold" in binding:
            action_map_binding.threshold = binding["threshold"]
            action_map_binding.axis0_region = binding["axis_region"]
            action_map_binding.axis1_region = "ANY"
        if "pose_location" in binding:
            action_map_binding.pose_location = binding["pose_location"]
        if "pose_rotation" in binding:
            action_map_binding.pose_rotation = binding["pose_rotation"]

    def _collect_action_names(self, profile_names):
        action_names = []
        seen = set()
        for profile_name in profile_names:
            profile = self.profiles.get(profile_name)
            if not profile:
                continue
            for key, binding in profile.action_map.items():
                if binding is None:
                    continue
                action_name = key.value if hasattr(key, "value") else key
                if action_name in seen:
                    continue
                if action_name not in self.actions:
                    continue
                seen.add(action_name)
                action_names.append(action_name)
        return action_names

    def _select_action_for_map(self, action_name, map_name):
        variants = self.actions.get(action_name)
        if not variants:
            return None
        for a in variants:
            if getattr(a, "map_name", None) == map_name:
                return a
        for a in variants:
            maps = getattr(a, "included_maps", set())
            if map_name in maps:
                return a
        return None

def _iter_action_classes(module):
    for _, value in inspect.getmembers(module, inspect.isclass):
        if value is VRAction:
            continue
        if not issubclass(value, VRAction):
            continue
        if value.__module__ != module.__name__:
            continue
        yield value

def _iter_profile_classes(module):
    for _, value in inspect.getmembers(module, inspect.isclass):
        if value is VRActionProfile:
            continue
        if not issubclass(value, VRActionProfile):
            continue
        if value.__module__ != module.__name__:
            continue
        yield value

def build_default_registry():
    registry = ActionRegistry()

    # Search through the actions module for action classes.
    for module_info in sorted(
        pkgutil.iter_modules(actions.__path__, actions.__name__ + "."),
        key=lambda info: info.name,
    ):
        module = importlib.import_module(module_info.name)
        for action_cls in _iter_action_classes(module):
            action = action_cls()
            if action.name == VRDefaultActions.EMPTY.value:
                continue
            registry.register_action(action)

    # Search through the profiles module for action profile classes.
    for module_info in sorted(
        pkgutil.iter_modules(profiles.__path__, profiles.__name__ + "."),
        key=lambda info: info.name,
    ):
        module = importlib.import_module(module_info.name)
        for profile_cls in _iter_profile_classes(module):
            profile = profile_cls()
            registry.register_profile(profile)

    registry.dirty = True

    return registry


registry = build_default_registry()
