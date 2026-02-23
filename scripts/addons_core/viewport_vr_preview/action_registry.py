import importlib

if "bpy" in locals():
    importlib.reload(actions)
    importlib.reload(profiles)
else:
    from . import actions, profiles

import bpy
import inspect
import pkgutil

from .action import VRAction
from .action_profile import (
    VRDefaultActions,
    VRDefaultActionmaps,
    VRActionProfile
)

# A singletone class representing the mapping between generalized actions in Blender
# and device-specific action profiles.
class VRActionRegistry:
    _instance = None
    _initialized = False

    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
        return cls._instance

    def __init__(self):
        if self._initialized:
            return
        self._initialized = True
        self.reset()

    def reset(self):
        self.actions = {}
        self.profiles = {}

    def register_action(self, action):
        bucket = self.actions.get(action.name)
        if bucket is None:
            self.actions[action.name] = [action]
        else:
            bucket.append(action)

    def register_profile(self, profile):
        if profile.name in self.profiles:
            return
        self.profiles[profile.name] = profile

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
            item = slot.vr_action_map_add(am_default, VRDefaultActionmaps.DEFAULT.value)
            if item is None:
                continue
            for profile_name in default_profiles:
                profile = self.profiles[profile_name]
                slot.vr_action_map_item_add(item, profile)

        if gamepad_profiles:
            am_gamepad = session_state.actionmaps.new(
                session_state, VRDefaultActionmaps.GAMEPAD.value, True
            )
            gamepad_action_names = self._collect_action_names(gamepad_profiles)
            for action_name in gamepad_action_names:
                slot = self._select_action_for_map(action_name, VRDefaultActionmaps.GAMEPAD.value)
                if slot is None:
                    continue
                item = slot.vr_action_map_add(am_gamepad, VRDefaultActionmaps.GAMEPAD.value)
                if item is None:
                    continue
                for profile_name in gamepad_profiles:
                    profile = self.profiles[profile_name]
                    slot.vr_action_map_item_add(item, profile)

    def get_profile_setting_name(profile_name):
        return f"vr_actions_enable_{profile_name}"

    def destroy_profile_settings(self):
        for profile in self.profiles.values():
            setting_name = VRActionRegistry.get_profile_setting_name(profile.name)
            if hasattr(bpy.types.Scene, setting_name):
                delattr(bpy.types.Scene, setting_name)

    def get_opt_in_profiles(self):
        return [
            profile for profile in self.profiles.values()
            if profile.requires_opt_in
        ]
    
    def get_profile_enabled(self, profile_name):
        if profile_name not in self.profiles:
            return False
        
        # Check if profile requires opt-in
        profile = self.profiles[profile_name]
        if not profile.requires_opt_in:
            return True

        # If profile opt-in is required, then return the current setting value
        setting_name = VRActionRegistry.get_profile_setting_name(profile_name)
        assert hasattr(bpy.context.scene, setting_name)
        return getattr(bpy.context.scene, setting_name)

    def build_profile_settings(self):   
        self.destroy_profile_settings()

        opt_in_profiles = self.get_opt_in_profiles()
        for profile in opt_in_profiles:
            setting_name = VRActionRegistry.get_profile_setting_name(profile.name)
            profile_setting = bpy.props.BoolProperty(
                description=(
                    f"Enable bindings for the {profile.ui_label} controllers. "
                    "Note that this may not be supported by all OpenXR runtimes"
                ),
                default=False,
            )

            setattr(bpy.types.Scene, setting_name, profile_setting)

    def _remove_actionmap(self, session_state, name):
        actionmaps = session_state.actionmaps
        idx = actionmaps.find(session_state, name)
        if idx is None or idx < 0:
            return
        
        actionmaps.remove(idx)

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


def register():
    registry = VRActionRegistry()

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

    registry.build_profile_settings()


def unregister():
    #TODO: Destroys data in the action registry.
    registry = VRActionRegistry()
    registry.reset()
