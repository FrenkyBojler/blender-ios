# Viewport VR Preview Development Notes

This addon discovers VR actions and interaction profiles automatically:

- `action_registry.register()` imports every module in `actions/` and registers every class defined there that subclasses `VRAction`.
- `action_registry.register()` imports every module in `profiles/` and registers every class defined there that subclasses `VRActionProfile`.

Because discovery is automatic, adding a new file with one or more concrete subclasses is usually enough. No manual import list needs to be updated.

## Adding A New Action Profile

Create a new module in `profiles/`, then add a subclass of `VRActionProfile`.

Required class attributes:

- `name`: Internal short name used to uniquely identify the profile in the registry and scene settings.
- `profile`: OpenXR interaction profile path, for example `"/interaction_profiles/oculus/touch_controller"`.
- `ui_label`: Human-readable name used to display the profile in the scene settings.

Additional class attributes:

- `requires_opt_in`: Set to `True` when the profile should be disabled unless the user enables it in the scene settings.

Typical implementation:

```python
from ..action_profile import VRActionProfile, VRDefaultActions


class VRActionProfileExample(VRActionProfile):
    name = "example"
    profile = "/interaction_profiles/vendor/example_controller"
    ui_label = "Example Controller"
    requires_opt_in = True

    def __init__(self):
        super().__init__()

        self.action_map[VRDefaultActions.TELEPORT.value].update({
            "component_paths": ["/input/trigger/value", "/input/trigger/value"],
            "threshold": 0.3,
            "axis_region": "ANY",
        })

        self.action_map[VRDefaultActions.NAV_RESET.value].update({
            "component_paths": ["/input/x/click", "/input/a/click"],
        })

        # Set an action to None when the profile does not support it.
        self.action_map[VRDefaultActions.FLY_UP.value] = None
```

### Profile Rules

- Always call `super().__init__()` first so the default bindings are created before you override them.
- Use `VRDefaultActions.<ACTION>.value` as the action-map key for consistency.
- Set an entry to `None` when the interaction profile does not support that action.
- `component_paths` should match the action's user paths:
  - Handed actions usually provide two entries, left then right.
  - Gamepad actions usually provide one entry.
- Pose actions also need `pose_location` and `pose_rotation`.
- Float actions also need `threshold` and `axis_region`.

### `ui_label` Expectations

Concrete profiles should always set `ui_label`.

Reasons:

- Opt-in profiles use it directly in the "Extensions" UI and in the generated scene property description.
- Non-opt-in profiles do not currently expose it in the same UI, but keeping a human-readable label on every profile makes the metadata complete and avoids future guesswork.
- The base class leaves it as `None` only as a placeholder; concrete subclasses should treat it as required metadata.

### When To Opt In

Set `requires_opt_in = True` for profiles that may only work on some runtimes or platforms.

## Adding A New VR Action

There are two parts to a new action:

1. Add a stable action name.
2. Add one or more action classes in `actions/`.

### 1. Add The Action Name

Add the new action to `VRDefaultActions` in `action_profile.py`.

Example:

```python
class VRDefaultActions(Enum):
    ...
    MY_ACTION = "my_action"
```

This value is the key used by action classes and profile bindings.

### 2. Add The Action Class

Create a new module in `actions/`, or add a class to an existing module.

Choose the base class that matches the binding type:

- `VRAction`: Generic action. Set `type` manually.
- `VRActionFloat`: Float input action with operator, thresholds, and optional haptics.
- `VRActionFloatLeftHanded`: Same as `VRActionFloat`, but defaults to the left-hand user path.
- `VRActionFloatRightHanded`: Same as `VRActionFloat`, but defaults to the right-hand user path.
- `VRActionPose`: Pose action with `pose_is_controller_grip` or `pose_is_controller_aim`.

Typical float-action implementation:

```python
from ..action import VRActionFloatLeftHanded
from ..action_profile import VRDefaultActions, VRDefaultActionmaps


class VRActionMyAction(VRActionFloatLeftHanded):
    name = VRDefaultActions.MY_ACTION.value
    op = "wm.xr_navigation_my_action"
    op_mode = "MODAL"
    included_maps = {
        VRDefaultActionmaps.DEFAULT.value,
        VRDefaultActionmaps.GAMEPAD.value,
    }
    op_properties = [
        ("mode", "SOMETHING"),
    ]
```

### Action Rules

- `name` must match the key used in every profile's `action_map`.
- `included_maps` controls which action maps can include this action.
- `map_name` is more specific than `included_maps`. Set it when there are multiple implementations of the same logical action and only one should be used for a given map.
- `path_type` controls the generated OpenXR user paths:
  - Left-handed: `"/user/hand/left"`
  - Right-handed: `"/user/hand/right"`
  - Dual-handed: both left and right hands
  - Gamepad: `"/user/gamepad"`
- For gamepad-only actions, set `map_name = VRDefaultActionmaps.GAMEPAD.value` and use the gamepad path type when needed.

## Connecting Actions To Profiles

Registering the action class alone is not enough. The action only appears in an action map when all of the following are true:

- The action class is discovered in `actions/`.
- At least one profile contains a non-`None` binding for that action in `self.action_map`.

In practice, a new action usually requires updates in both places:

- Add the action name and class.
- Add bindings for that action in every profile that supports it.

If no registered profile binds the new action, the registry skips it when building action maps.

## Verifying Changes

- Reload the addon or restart Blender so the registry re-imports the new modules.
- Start an XR session with controller actions enabled.
- Confirm the action set is created without warnings and that the new bindings behave correctly on the target runtime.
