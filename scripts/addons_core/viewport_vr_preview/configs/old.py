# SPDX-FileCopyrightText: 2021-2022 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later
try:
    from deepdiff import deepdiff
except ImportError:
    DeepDiff = None

from collections.abc import Mapping, Sequence

_MISSING = object()


actionconfig_data_old = \
    [("blender_default",
      {"items":
       [("controller_grip", {"type": 'POSE', "user_paths": ['/user/hand/left', '/user/hand/right'], "pose_is_controller_grip": 'True', "pose_is_controller_aim": 'False'}, None,
         {"bindings":
           [("huawei", {"profile": '/interaction_profiles/huawei/controller', "component_paths": ['/input/grip/pose', '/input/grip/pose'], "pose_location": '(0.0, 0.0, 0.0)', "pose_rotation": '(0.0, 0.0, 0.0)'}),
            ("index", {"profile": '/interaction_profiles/valve/index_controller', "component_paths": ['/input/grip/pose', '/input/grip/pose'], "pose_location": '(0.0, 0.0, 0.0)', "pose_rotation": '(0.0, 0.0, 0.0)'}),
               ("oculus", {"profile": '/interaction_profiles/oculus/touch_controller', "component_paths": ['/input/grip/pose', '/input/grip/pose'], "pose_location": '(0.0, 0.0, 0.0)', "pose_rotation": '(0.0, 0.0, 0.0)'}),
               ("reverb_g2", {"profile": '/interaction_profiles/hp/mixed_reality_controller', "component_paths": ['/input/grip/pose', '/input/grip/pose'], "pose_location": '(0.0, 0.0, 0.0)', "pose_rotation": '(0.0, 0.0, 0.0)'}),
               ("simple", {"profile": '/interaction_profiles/khr/simple_controller', "component_paths": ['/input/grip/pose', '/input/grip/pose'], "pose_location": '(0.0, 0.0, 0.0)', "pose_rotation": '(0.0, 0.0, 0.0)'}),
               ("vive", {"profile": '/interaction_profiles/htc/vive_controller', "component_paths": ['/input/grip/pose', '/input/grip/pose'], "pose_location": '(0.0, 0.0, 0.0)', "pose_rotation": '(0.0, 0.0, 0.0)'}),
               ("vive_cosmos", {"profile": '/interaction_profiles/htc/vive_cosmos_controller', "component_paths": ['/input/grip/pose', '/input/grip/pose'], "pose_location": '(0.0, 0.0, 0.0)', "pose_rotation": '(0.0, 0.0, 0.0)'}),
               ("vive_focus", {"profile": '/interaction_profiles/htc/vive_focus3_controller', "component_paths": ['/input/grip/pose', '/input/grip/pose'], "pose_location": '(0.0, 0.0, 0.0)', "pose_rotation": '(0.0, 0.0, 0.0)'}),
               ("wmr", {"profile": '/interaction_profiles/microsoft/motion_controller', "component_paths": ['/input/grip/pose', '/input/grip/pose'], "pose_location": '(0.0, 0.0, 0.0)', "pose_rotation": '(0.0, 0.0, 0.0)'}),
            ],
          },
         ),
           ("controller_aim", {"type": 'POSE', "user_paths": ['/user/hand/left', '/user/hand/right'], "pose_is_controller_grip": 'False', "pose_is_controller_aim": 'True'}, None,
            {"bindings":
            [("huawei", {"profile": '/interaction_profiles/huawei/controller', "component_paths": ['/input/aim/pose', '/input/aim/pose'], "pose_location": '(0.0, 0.0, 0.0)', "pose_rotation": '(0.0, 0.0, 0.0)'}),
             ("index", {"profile": '/interaction_profiles/valve/index_controller', "component_paths": ['/input/aim/pose', '/input/aim/pose'], "pose_location": '(0.0, 0.0, 0.0)', "pose_rotation": '(0.0, 0.0, 0.0)'}),
                ("oculus", {"profile": '/interaction_profiles/oculus/touch_controller', "component_paths": ['/input/aim/pose', '/input/aim/pose'], "pose_location": '(0.0, 0.0, 0.0)', "pose_rotation": '(0.0, 0.0, 0.0)'}),
                ("reverb_g2", {"profile": '/interaction_profiles/hp/mixed_reality_controller', "component_paths": ['/input/aim/pose', '/input/aim/pose'], "pose_location": '(0.0, 0.0, 0.0)', "pose_rotation": '(0.0, 0.0, 0.0)'}),
                ("simple", {"profile": '/interaction_profiles/khr/simple_controller', "component_paths": ['/input/aim/pose', '/input/aim/pose'], "pose_location": '(0.0, 0.0, 0.0)', "pose_rotation": '(0.0, 0.0, 0.0)'}),
                ("vive", {"profile": '/interaction_profiles/htc/vive_controller', "component_paths": ['/input/aim/pose', '/input/aim/pose'], "pose_location": '(0.0, 0.0, 0.0)', "pose_rotation": '(0.0, 0.0, 0.0)'}),
                ("vive_cosmos", {"profile": '/interaction_profiles/htc/vive_cosmos_controller', "component_paths": ['/input/aim/pose', '/input/aim/pose'], "pose_location": '(0.0, 0.0, 0.0)', "pose_rotation": '(0.0, 0.0, 0.0)'}),
                ("vive_focus", {"profile": '/interaction_profiles/htc/vive_focus3_controller', "component_paths": ['/input/aim/pose', '/input/aim/pose'], "pose_location": '(0.0, 0.0, 0.0)', "pose_rotation": '(0.0, 0.0, 0.0)'}),
                ("wmr", {"profile": '/interaction_profiles/microsoft/motion_controller', "component_paths": ['/input/aim/pose', '/input/aim/pose'], "pose_location": '(0.0, 0.0, 0.0)', "pose_rotation": '(0.0, 0.0, 0.0)'}),
             ],
             },
            ),
           ("teleport", {"type": 'FLOAT', "user_paths": ['/user/hand/left', '/user/hand/right'], "op": 'wm.xr_navigation_teleport', "op_mode": 'MODAL', "bimanual": 'False', "haptic_name": '', "haptic_match_user_paths": 'False', "haptic_duration": '0.0', "haptic_frequency": '0.0', "haptic_amplitude": '0.0', "haptic_mode": 'PRESS'}, None,
            {"bindings":
            [("huawei", {"profile": '/interaction_profiles/huawei/controller', "component_paths": ['/input/trigger/value', '/input/trigger/value'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
             ("index", {"profile": '/interaction_profiles/valve/index_controller', "component_paths": ['/input/trigger/value', '/input/trigger/value'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
                ("oculus", {"profile": '/interaction_profiles/oculus/touch_controller', "component_paths": ['/input/trigger/value', '/input/trigger/value'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
                ("reverb_g2", {"profile": '/interaction_profiles/hp/mixed_reality_controller', "component_paths": ['/input/trigger/value', '/input/trigger/value'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
                ("simple", {"profile": '/interaction_profiles/khr/simple_controller', "component_paths": ['/input/select/click', '/input/select/click'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
                ("vive", {"profile": '/interaction_profiles/htc/vive_controller', "component_paths": ['/input/trigger/value', '/input/trigger/value'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
                ("vive_cosmos", {"profile": '/interaction_profiles/htc/vive_cosmos_controller', "component_paths": ['/input/trigger/value', '/input/trigger/value'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
                ("vive_focus", {"profile": '/interaction_profiles/htc/vive_focus3_controller', "component_paths": ['/input/trigger/value', '/input/trigger/value'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
                ("wmr", {"profile": '/interaction_profiles/microsoft/motion_controller', "component_paths": ['/input/trigger/value', '/input/trigger/value'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
             ],
             },
            ),
           ("nav_grab", {"type": 'FLOAT', "user_paths": ['/user/hand/left', '/user/hand/right'], "op": 'wm.xr_navigation_grab', "op_mode": 'MODAL', "bimanual": 'True', "haptic_name": '', "haptic_match_user_paths": 'False', "haptic_duration": '0.0', "haptic_frequency": '0.0', "haptic_amplitude": '0.0', "haptic_mode": 'PRESS'},
            {"op_properties":
            [("lock_rotation", True),
             ],
             },
            {"bindings":
            [("huawei", {"profile": '/interaction_profiles/huawei/controller', "component_paths": ['/input/trackpad/click', '/input/trackpad/click'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
             ("index", {"profile": '/interaction_profiles/valve/index_controller', "component_paths": ['/input/squeeze/force', '/input/squeeze/force'], "threshold": '0.5', "axis_region": 'ANY'}),
                ("oculus", {"profile": '/interaction_profiles/oculus/touch_controller', "component_paths": ['/input/squeeze/value', '/input/squeeze/value'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
                ("reverb_g2", {"profile": '/interaction_profiles/hp/mixed_reality_controller', "component_paths": ['/input/squeeze/value', '/input/squeeze/value'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
                ("simple", {"profile": '/interaction_profiles/khr/simple_controller', "component_paths": ['/input/menu/click', '/input/menu/click'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
                ("vive", {"profile": '/interaction_profiles/htc/vive_controller', "component_paths": ['/input/squeeze/click', '/input/squeeze/click'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
                ("vive_cosmos", {"profile": '/interaction_profiles/htc/vive_cosmos_controller', "component_paths": ['/input/squeeze/click', '/input/squeeze/click'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
                ("vive_focus", {"profile": '/interaction_profiles/htc/vive_focus3_controller', "component_paths": ['/input/squeeze/click', '/input/squeeze/click'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
                ("wmr", {"profile": '/interaction_profiles/microsoft/motion_controller', "component_paths": ['/input/squeeze/click', '/input/squeeze/click'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
             ],
             },
            ),
           ("fly_forward", {"type": 'FLOAT', "user_paths": ['/user/hand/left'], "op": 'wm.xr_navigation_fly', "op_mode": 'MODAL', "bimanual": 'False', "haptic_name": '', "haptic_match_user_paths": 'False', "haptic_duration": '0.0', "haptic_frequency": '0.0', "haptic_amplitude": '0.0', "haptic_mode": 'PRESS'},
            {"op_properties":
            [("mode", 'VIEWER_FORWARD'),
             ("alt_mode", 'UP'),
             ("lock_location_z", True),
             ],
             },
            {"bindings":
            [("huawei", {"profile": '/interaction_profiles/huawei/controller', "component_paths": ['/input/trackpad/y'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
             ("index", {"profile": '/interaction_profiles/valve/index_controller', "component_paths": ['/input/thumbstick/y'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ("oculus", {"profile": '/interaction_profiles/oculus/touch_controller', "component_paths": ['/input/thumbstick/y'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ("reverb_g2", {"profile": '/interaction_profiles/hp/mixed_reality_controller', "component_paths": ['/input/thumbstick/y'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ("vive", {"profile": '/interaction_profiles/htc/vive_controller', "component_paths": ['/input/trackpad/y'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ("vive_cosmos", {"profile": '/interaction_profiles/htc/vive_cosmos_controller', "component_paths": ['/input/thumbstick/y'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ("vive_focus", {"profile": '/interaction_profiles/htc/vive_focus3_controller', "component_paths": ['/input/thumbstick/y'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ("wmr", {"profile": '/interaction_profiles/microsoft/motion_controller', "component_paths": ['/input/thumbstick/y'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
             ],
             },
            ),
           ("fly_back", {"type": 'FLOAT', "user_paths": ['/user/hand/left'], "op": 'wm.xr_navigation_fly', "op_mode": 'MODAL', "bimanual": 'False', "haptic_name": '', "haptic_match_user_paths": 'False', "haptic_duration": '0.0', "haptic_frequency": '0.0', "haptic_amplitude": '0.0', "haptic_mode": 'PRESS'},
            {"op_properties":
            [("mode", 'VIEWER_BACK'),
             ("alt_mode", 'DOWN'),
             ("lock_location_z", True),
             ],
             },
            {"bindings":
            [("huawei", {"profile": '/interaction_profiles/huawei/controller', "component_paths": ['/input/trackpad/y'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
             ("index", {"profile": '/interaction_profiles/valve/index_controller', "component_paths": ['/input/thumbstick/y'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ("oculus", {"profile": '/interaction_profiles/oculus/touch_controller', "component_paths": ['/input/thumbstick/y'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ("reverb_g2", {"profile": '/interaction_profiles/hp/mixed_reality_controller', "component_paths": ['/input/thumbstick/y'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ("vive", {"profile": '/interaction_profiles/htc/vive_controller', "component_paths": ['/input/trackpad/y'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ("vive_cosmos", {"profile": '/interaction_profiles/htc/vive_cosmos_controller', "component_paths": ['/input/thumbstick/y'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ("vive_focus", {"profile": '/interaction_profiles/htc/vive_focus3_controller', "component_paths": ['/input/thumbstick/y'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ("wmr", {"profile": '/interaction_profiles/microsoft/motion_controller', "component_paths": ['/input/thumbstick/y'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
             ],
             },
            ),
           ("fly_left", {"type": 'FLOAT', "user_paths": ['/user/hand/left'], "op": 'wm.xr_navigation_fly', "op_mode": 'MODAL', "bimanual": 'False', "haptic_name": '', "haptic_match_user_paths": 'False', "haptic_duration": '0.0', "haptic_frequency": '0.0', "haptic_amplitude": '0.0', "haptic_mode": 'PRESS'},
            {"op_properties":
            [("mode", 'VIEWER_LEFT'),
             ("alt_mode", 'TURNLEFT'),
             ("lock_location_z", True),
             ],
             },
            {"bindings":
            [("huawei", {"profile": '/interaction_profiles/huawei/controller', "component_paths": ['/input/trackpad/x'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
             ("index", {"profile": '/interaction_profiles/valve/index_controller', "component_paths": ['/input/thumbstick/x'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ("oculus", {"profile": '/interaction_profiles/oculus/touch_controller', "component_paths": ['/input/thumbstick/x'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ("reverb_g2", {"profile": '/interaction_profiles/hp/mixed_reality_controller', "component_paths": ['/input/thumbstick/x'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ("vive", {"profile": '/interaction_profiles/htc/vive_controller', "component_paths": ['/input/trackpad/x'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ("vive_cosmos", {"profile": '/interaction_profiles/htc/vive_cosmos_controller', "component_paths": ['/input/thumbstick/x'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ("vive_focus", {"profile": '/interaction_profiles/htc/vive_focus3_controller', "component_paths": ['/input/thumbstick/x'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ("wmr", {"profile": '/interaction_profiles/microsoft/motion_controller', "component_paths": ['/input/thumbstick/x'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
             ],
             },
            ),
           ("fly_right", {"type": 'FLOAT', "user_paths": ['/user/hand/left'], "op": 'wm.xr_navigation_fly', "op_mode": 'MODAL', "bimanual": 'False', "haptic_name": '', "haptic_match_user_paths": 'False', "haptic_duration": '0.0', "haptic_frequency": '0.0', "haptic_amplitude": '0.0', "haptic_mode": 'PRESS'},
            {"op_properties":
            [("mode", 'VIEWER_RIGHT'),
             ("alt_mode", 'TURNRIGHT'),
             ("lock_location_z", True),
             ],
             },
            {"bindings":
            [("huawei", {"profile": '/interaction_profiles/huawei/controller', "component_paths": ['/input/trackpad/x'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
             ("index", {"profile": '/interaction_profiles/valve/index_controller', "component_paths": ['/input/thumbstick/x'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ("oculus", {"profile": '/interaction_profiles/oculus/touch_controller', "component_paths": ['/input/thumbstick/x'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ("reverb_g2", {"profile": '/interaction_profiles/hp/mixed_reality_controller', "component_paths": ['/input/thumbstick/x'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ("vive", {"profile": '/interaction_profiles/htc/vive_controller', "component_paths": ['/input/trackpad/x'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ("vive_cosmos", {"profile": '/interaction_profiles/htc/vive_cosmos_controller', "component_paths": ['/input/thumbstick/x'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ("vive_focus", {"profile": '/interaction_profiles/htc/vive_focus3_controller', "component_paths": ['/input/thumbstick/x'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ("wmr", {"profile": '/interaction_profiles/microsoft/motion_controller', "component_paths": ['/input/thumbstick/x'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
             ],
             },
            ),
           ("fly_up", {"type": 'FLOAT', "user_paths": ['/user/hand/right'], "op": 'wm.xr_navigation_fly', "op_mode": 'MODAL', "bimanual": 'False', "haptic_name": '', "haptic_match_user_paths": 'False', "haptic_duration": '0.0', "haptic_frequency": '0.0', "haptic_amplitude": '0.0', "haptic_mode": 'PRESS'},
            {"op_properties":
            [("mode", 'UP'),
             ("alt_mode", 'VIEWER_FORWARD'),
             ("alt_lock_location_z", True),
             ],
             },
            {"bindings":
            [("huawei", {"profile": '/interaction_profiles/huawei/controller', "component_paths": ['/input/trackpad/y'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
             ("index", {"profile": '/interaction_profiles/valve/index_controller', "component_paths": ['/input/thumbstick/y'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ("oculus", {"profile": '/interaction_profiles/oculus/touch_controller', "component_paths": ['/input/thumbstick/y'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ("reverb_g2", {"profile": '/interaction_profiles/hp/mixed_reality_controller', "component_paths": ['/input/thumbstick/y'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ("vive", {"profile": '/interaction_profiles/htc/vive_controller', "component_paths": ['/input/trackpad/y'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ("vive_cosmos", {"profile": '/interaction_profiles/htc/vive_cosmos_controller', "component_paths": ['/input/thumbstick/y'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ("vive_focus", {"profile": '/interaction_profiles/htc/vive_focus3_controller', "component_paths": ['/input/thumbstick/y'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ("wmr", {"profile": '/interaction_profiles/microsoft/motion_controller', "component_paths": ['/input/thumbstick/y'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
             ],
             },
            ),
           ("fly_down", {"type": 'FLOAT', "user_paths": ['/user/hand/right'], "op": 'wm.xr_navigation_fly', "op_mode": 'MODAL', "bimanual": 'False', "haptic_name": '', "haptic_match_user_paths": 'False', "haptic_duration": '0.0', "haptic_frequency": '0.0', "haptic_amplitude": '0.0', "haptic_mode": 'PRESS'},
            {"op_properties":
            [("mode", 'DOWN'),
             ("alt_mode", 'VIEWER_BACK'),
             ("alt_lock_location_z", True),
             ],
             },
            {"bindings":
            [("huawei", {"profile": '/interaction_profiles/huawei/controller', "component_paths": ['/input/trackpad/y'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
             ("index", {"profile": '/interaction_profiles/valve/index_controller', "component_paths": ['/input/thumbstick/y'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ("oculus", {"profile": '/interaction_profiles/oculus/touch_controller', "component_paths": ['/input/thumbstick/y'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ("reverb_g2", {"profile": '/interaction_profiles/hp/mixed_reality_controller', "component_paths": ['/input/thumbstick/y'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ("vive", {"profile": '/interaction_profiles/htc/vive_controller', "component_paths": ['/input/trackpad/y'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ("vive_cosmos", {"profile": '/interaction_profiles/htc/vive_cosmos_controller', "component_paths": ['/input/thumbstick/y'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ("vive_focus", {"profile": '/interaction_profiles/htc/vive_focus3_controller', "component_paths": ['/input/thumbstick/y'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ("wmr", {"profile": '/interaction_profiles/microsoft/motion_controller', "component_paths": ['/input/thumbstick/y'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
             ],
             },
            ),
           ("fly_turnleft", {"type": 'FLOAT', "user_paths": ['/user/hand/right'], "op": 'wm.xr_navigation_fly', "op_mode": 'MODAL', "bimanual": 'False', "haptic_name": '', "haptic_match_user_paths": 'False', "haptic_duration": '0.0', "haptic_frequency": '0.0', "haptic_amplitude": '0.0', "haptic_mode": 'PRESS'},
            {"op_properties":
            [("mode", 'TURNLEFT'),
             ("alt_mode", 'VIEWER_LEFT'),
             ("alt_lock_location_z", True),
             ],
             },
            {"bindings":
            [("huawei", {"profile": '/interaction_profiles/huawei/controller', "component_paths": ['/input/trackpad/x'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
             ("index", {"profile": '/interaction_profiles/valve/index_controller', "component_paths": ['/input/thumbstick/x'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ("oculus", {"profile": '/interaction_profiles/oculus/touch_controller', "component_paths": ['/input/thumbstick/x'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ("reverb_g2", {"profile": '/interaction_profiles/hp/mixed_reality_controller', "component_paths": ['/input/thumbstick/x'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ("vive", {"profile": '/interaction_profiles/htc/vive_controller', "component_paths": ['/input/trackpad/x'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ("vive_cosmos", {"profile": '/interaction_profiles/htc/vive_cosmos_controller', "component_paths": ['/input/thumbstick/x'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ("vive_focus", {"profile": '/interaction_profiles/htc/vive_focus3_controller', "component_paths": ['/input/thumbstick/x'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ("wmr", {"profile": '/interaction_profiles/microsoft/motion_controller', "component_paths": ['/input/thumbstick/x'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
             ],
             },
            ),
           ("fly_turnright", {"type": 'FLOAT', "user_paths": ['/user/hand/right'], "op": 'wm.xr_navigation_fly', "op_mode": 'MODAL', "bimanual": 'False', "haptic_name": '', "haptic_match_user_paths": 'False', "haptic_duration": '0.0', "haptic_frequency": '0.0', "haptic_amplitude": '0.0', "haptic_mode": 'PRESS'},
            {"op_properties":
            [("mode", 'TURNRIGHT'),
             ("alt_mode", 'VIEWER_RIGHT'),
             ("alt_lock_location_z", True),
             ],
             },
            {"bindings":
            [("huawei", {"profile": '/interaction_profiles/huawei/controller', "component_paths": ['/input/trackpad/x'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
             ("index", {"profile": '/interaction_profiles/valve/index_controller', "component_paths": ['/input/thumbstick/x'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ("oculus", {"profile": '/interaction_profiles/oculus/touch_controller', "component_paths": ['/input/thumbstick/x'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ("reverb_g2", {"profile": '/interaction_profiles/hp/mixed_reality_controller', "component_paths": ['/input/thumbstick/x'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ("vive", {"profile": '/interaction_profiles/htc/vive_controller', "component_paths": ['/input/trackpad/x'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ("vive_cosmos", {"profile": '/interaction_profiles/htc/vive_cosmos_controller', "component_paths": ['/input/thumbstick/x'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ("vive_focus", {"profile": '/interaction_profiles/htc/vive_focus3_controller', "component_paths": ['/input/thumbstick/x'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ("wmr", {"profile": '/interaction_profiles/microsoft/motion_controller', "component_paths": ['/input/thumbstick/x'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
             ],
             },
            ),
           ("nav_reset", {"type": 'FLOAT', "user_paths": ['/user/hand/left', '/user/hand/right'], "op": 'wm.xr_navigation_reset', "op_mode": 'PRESS', "bimanual": 'False', "haptic_name": 'haptic', "haptic_match_user_paths": 'True', "haptic_duration": '0.30000001192092896', "haptic_frequency": '3000.0', "haptic_amplitude": '0.5', "haptic_mode": 'PRESS'},
            {"op_properties":
            [("location", False),
             ("rotation", False),
                ("scale", True),
             ],
             },
            {"bindings":
            [("huawei", {"profile": '/interaction_profiles/huawei/controller', "component_paths": ['/input/back/click', '/input/back/click'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
             ("index", {"profile": '/interaction_profiles/valve/index_controller', "component_paths": ['/input/a/click', '/input/a/click'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
                ("oculus", {"profile": '/interaction_profiles/oculus/touch_controller', "component_paths": ['/input/x/click', '/input/a/click'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
                ("reverb_g2", {"profile": '/interaction_profiles/hp/mixed_reality_controller', "component_paths": ['/input/x/click', '/input/a/click'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
                ("vive", {"profile": '/interaction_profiles/htc/vive_controller', "component_paths": ['/input/menu/click', '/input/menu/click'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
                ("vive_cosmos", {"profile": '/interaction_profiles/htc/vive_cosmos_controller', "component_paths": ['/input/x/click', '/input/a/click'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
                ("vive_focus", {"profile": '/interaction_profiles/htc/vive_focus3_controller', "component_paths": ['/input/x/click', '/input/a/click'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
                ("wmr", {"profile": '/interaction_profiles/microsoft/motion_controller', "component_paths": ['/input/menu/click', '/input/menu/click'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
             ],
             },
            ),
           ("haptic", {"type": 'VIBRATION', "user_paths": ['/user/hand/left', '/user/hand/right']}, None,
            {"bindings":
            [("huawei", {"profile": '/interaction_profiles/huawei/controller', "component_paths": ['/output/haptic', '/output/haptic']}),
             ("index", {"profile": '/interaction_profiles/valve/index_controller', "component_paths": ['/output/haptic', '/output/haptic']}),
                ("oculus", {"profile": '/interaction_profiles/oculus/touch_controller', "component_paths": ['/output/haptic', '/output/haptic']}),
                ("reverb_g2", {"profile": '/interaction_profiles/hp/mixed_reality_controller', "component_paths": ['/output/haptic', '/output/haptic']}),
                ("simple", {"profile": '/interaction_profiles/khr/simple_controller', "component_paths": ['/output/haptic', '/output/haptic']}),
                ("vive", {"profile": '/interaction_profiles/htc/vive_controller', "component_paths": ['/output/haptic', '/output/haptic']}),
                ("vive_cosmos", {"profile": '/interaction_profiles/htc/vive_cosmos_controller', "component_paths": ['/output/haptic', '/output/haptic']}),
                ("vive_focus", {"profile": '/interaction_profiles/htc/vive_focus3_controller', "component_paths": ['/output/haptic', '/output/haptic']}),
                ("wmr", {"profile": '/interaction_profiles/microsoft/motion_controller', "component_paths": ['/output/haptic', '/output/haptic']}),
             ],
             },
            ),
        ],
       },
      ),
        ("blender_default_gamepad",
         {"items":
          [("teleport", {"type": 'FLOAT', "user_paths": ['/user/gamepad'], "op": 'wm.xr_navigation_teleport', "op_mode": 'MODAL', "bimanual": 'False', "haptic_name": '', "haptic_match_user_paths": 'False', "haptic_duration": '0.0', "haptic_frequency": '0.0', "haptic_amplitude": '0.0', "haptic_mode": 'PRESS'}, None,
              {"bindings":
               [("gamepad", {"profile": '/interaction_profiles/microsoft/xbox_controller', "component_paths": ['/input/trigger_right/value'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
                ],
               },
            ),
              ("fly", {"type": 'FLOAT', "user_paths": ['/user/gamepad'], "op": 'wm.xr_navigation_fly', "op_mode": 'MODAL', "bimanual": 'False', "haptic_name": '', "haptic_match_user_paths": 'False', "haptic_duration": '0.0', "haptic_frequency": '0.0', "haptic_amplitude": '0.0', "haptic_mode": 'PRESS'}, None,
               {"bindings":
               [("gamepad", {"profile": '/interaction_profiles/microsoft/xbox_controller', "component_paths": ['/input/trigger_left/value'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
                ],
                },
               ),
           ("fly_forward", {"type": 'FLOAT', "user_paths": ['/user/gamepad'], "op": 'wm.xr_navigation_fly', "op_mode": 'MODAL', "bimanual": 'False', "haptic_name": '', "haptic_match_user_paths": 'False', "haptic_duration": '0.0', "haptic_frequency": '0.0', "haptic_amplitude": '0.0', "haptic_mode": 'PRESS'},
              {"op_properties":
               [("mode", 'VIEWER_FORWARD'),
                ("alt_mode", 'UP'),
                ("lock_location_z", True),
                ],
               },
              {"bindings":
               [("gamepad", {"profile": '/interaction_profiles/microsoft/xbox_controller', "component_paths": ['/input/thumbstick_left/y'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ],
               },
            ),
              ("fly_back", {"type": 'FLOAT', "user_paths": ['/user/gamepad'], "op": 'wm.xr_navigation_fly', "op_mode": 'MODAL', "bimanual": 'False', "haptic_name": '', "haptic_match_user_paths": 'False', "haptic_duration": '0.0', "haptic_frequency": '0.0', "haptic_amplitude": '0.0', "haptic_mode": 'PRESS'},
               {"op_properties":
               [("mode", 'VIEWER_BACK'),
                ("alt_mode", 'DOWN'),
                ("lock_location_z", True),
                ],
                },
               {"bindings":
               [("gamepad", {"profile": '/interaction_profiles/microsoft/xbox_controller', "component_paths": ['/input/thumbstick_left/y'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ],
                },
               ),
              ("fly_left", {"type": 'FLOAT', "user_paths": ['/user/gamepad'], "op": 'wm.xr_navigation_fly', "op_mode": 'MODAL', "bimanual": 'False', "haptic_name": '', "haptic_match_user_paths": 'False', "haptic_duration": '0.0', "haptic_frequency": '0.0', "haptic_amplitude": '0.0', "haptic_mode": 'PRESS'},
               {"op_properties":
               [("mode", 'VIEWER_LEFT'),
                ("alt_mode", 'TURNLEFT'),
                ("lock_location_z", True),
                ],
                },
               {"bindings":
               [("gamepad", {"profile": '/interaction_profiles/microsoft/xbox_controller', "component_paths": ['/input/thumbstick_left/x'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ],
                },
               ),
              ("fly_right", {"type": 'FLOAT', "user_paths": ['/user/gamepad'], "op": 'wm.xr_navigation_fly', "op_mode": 'MODAL', "bimanual": 'False', "haptic_name": '', "haptic_match_user_paths": 'False', "haptic_duration": '0.0', "haptic_frequency": '0.0', "haptic_amplitude": '0.0', "haptic_mode": 'PRESS'},
               {"op_properties":
               [("mode", 'VIEWER_RIGHT'),
                ("alt_mode", 'TURNRIGHT'),
                ("lock_location_z", True),
                ],
                },
               {"bindings":
               [("gamepad", {"profile": '/interaction_profiles/microsoft/xbox_controller', "component_paths": ['/input/thumbstick_left/x'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ],
                },
               ),
              ("fly_up", {"type": 'FLOAT', "user_paths": ['/user/gamepad'], "op": 'wm.xr_navigation_fly', "op_mode": 'MODAL', "bimanual": 'False', "haptic_name": '', "haptic_match_user_paths": 'False', "haptic_duration": '0.0', "haptic_frequency": '0.0', "haptic_amplitude": '0.0', "haptic_mode": 'PRESS'},
               {"op_properties":
               [("mode", 'UP'),
                ("alt_mode", 'VIEWER_FORWARD'),
                ("alt_lock_location_z", True),
                ]
                },
               {"bindings":
               [("gamepad", {"profile": '/interaction_profiles/microsoft/xbox_controller', "component_paths": ['/input/thumbstick_right/y'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ],
                },
               ),
              ("fly_down", {"type": 'FLOAT', "user_paths": ['/user/gamepad'], "op": 'wm.xr_navigation_fly', "op_mode": 'MODAL', "bimanual": 'False', "haptic_name": '', "haptic_match_user_paths": 'False', "haptic_duration": '0.0', "haptic_frequency": '0.0', "haptic_amplitude": '0.0', "haptic_mode": 'PRESS'},
               {"op_properties":
               [("mode", 'DOWN'),
                ("alt_mode", 'VIEWER_BACK'),
                ("alt_lock_location_z", True),
                ]
                },
               {"bindings":
               [("gamepad", {"profile": '/interaction_profiles/microsoft/xbox_controller', "component_paths": ['/input/thumbstick_right/y'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ],
                },
               ),
              ("fly_turnleft", {"type": 'FLOAT', "user_paths": ['/user/gamepad'], "op": 'wm.xr_navigation_fly', "op_mode": 'MODAL', "bimanual": 'False', "haptic_name": '', "haptic_match_user_paths": 'False', "haptic_duration": '0.0', "haptic_frequency": '0.0', "haptic_amplitude": '0.0', "haptic_mode": 'PRESS'},
               {"op_properties":
               [("mode", 'TURNLEFT'),
                ("alt_mode", 'VIEWER_LEFT'),
                ("alt_lock_location_z", True),
                ]
                },
               {"bindings":
               [("gamepad", {"profile": '/interaction_profiles/microsoft/xbox_controller', "component_paths": ['/input/thumbstick_right/x'], "threshold": '0.30000001192092896', "axis_region": 'NEGATIVE'}),
                ],
                },
               ),
              ("fly_turnright", {"type": 'FLOAT', "user_paths": ['/user/gamepad'], "op": 'wm.xr_navigation_fly', "op_mode": 'MODAL', "bimanual": 'False', "haptic_name": '', "haptic_match_user_paths": 'False', "haptic_duration": '0.0', "haptic_frequency": '0.0', "haptic_amplitude": '0.0', "haptic_mode": 'PRESS'},
               {"op_properties":
               [("mode", 'TURNRIGHT'),
                ("alt_mode", 'VIEWER_RIGHT'),
                ("alt_lock_location_z", True),
                ]
                },
               {"bindings":
               [("gamepad", {"profile": '/interaction_profiles/microsoft/xbox_controller', "component_paths": ['/input/thumbstick_right/x'], "threshold": '0.30000001192092896', "axis_region": 'POSITIVE'}),
                ],
                },
               ),
              ("nav_reset", {"type": 'FLOAT', "user_paths": ['/user/gamepad'], "op": 'wm.xr_navigation_reset', "op_mode": 'PRESS', "bimanual": 'False', "haptic_name": 'haptic_right', "haptic_match_user_paths": 'True', "haptic_duration": '0.30000001192092896', "haptic_frequency": '3000.0', "haptic_amplitude": '0.5', "haptic_mode": 'PRESS'},
               {"op_properties":
               [("location", False),
                ("rotation", False),
                ("scale", True),
                ],
                },
               {"bindings":
               [("gamepad", {"profile": '/interaction_profiles/microsoft/xbox_controller', "component_paths": ['/input/a/click'], "threshold": '0.30000001192092896', "axis_region": 'ANY'}),
                ],
                },
               ),
              ("haptic_left", {"type": 'VIBRATION', "user_paths": ['/user/gamepad']}, None,
               {"bindings":
               [("gamepad", {"profile": '/interaction_profiles/microsoft/xbox_controller', "component_paths": ['/output/haptic_left']}),
                ],
                },
               ),
              ("haptic_right", {"type": 'VIBRATION', "user_paths": ['/user/gamepad']}, None,
               {"bindings":
               [("gamepad", {"profile": '/interaction_profiles/microsoft/xbox_controller', "component_paths": ['/output/haptic_right']}),
                ],
                },
               ),
              ("haptic_lefttrigger", {"type": 'VIBRATION', "user_paths": ['/user/gamepad']}, None,
               {"bindings":
               [("gamepad", {"profile": '/interaction_profiles/microsoft/xbox_controller', "component_paths": ['/output/haptic_left_trigger']}),
                ],
                },
               ),
              ("haptic_righttrigger", {"type": 'VIBRATION', "user_paths": ['/user/gamepad']}, None,
               {"bindings":
               [("gamepad", {"profile": '/interaction_profiles/microsoft/xbox_controller', "component_paths": ['/output/haptic_right_trigger']}),
                ],
                },
               ),
           ],
          },
         ),
     ]


def deep_diff(a, b, path="root", diffs=None):
   """
   Recursively diff two Python objects.
   Returns a list of human-readable diff strings.
   """
   if diffs is None:
      diffs = []

   # Type mismatch
   if type(a) != type(b):
      diffs.append(
         f"{path}: type mismatch {type(a).__name__} != {type(b).__name__}"
      )
      return diffs
   
   # Dicts
   if isinstance(a, Mapping):
      keys = set(a) | set(b)
      for k in keys:
         av = a.get(k, _MISSING)
         bv = b.get(k, _MISSING)

         if av is _MISSING:
               diffs.append(f"{path}[{k!r}]: missing in first")
         elif bv is _MISSING:
               diffs.append(f"{path}[{k!r}]: missing in second")
         else:
               deep_diff(av, bv, f"{path}[{k!r}]", diffs)

   # Lists / Tuples (ordered)
   elif isinstance(a, Sequence) and not isinstance(a, (str, bytes, bytearray)):
      if len(a) != len(b):
         diffs.append(
               f"{path}: length mismatch {len(a)} != {len(b)}"
         )

      for i, (av, bv) in enumerate(zip(a, b)):
         deep_diff(av, bv, f"{path}[{i}]", diffs)

   # Scalars
   else:
      if a != b:
         diffs.append(f"{path}: {a!r} != {b!r}")

   return diffs


def test_actionconfig(actionconfig_data):
   if DeepDiff is not None:
      diff = DeepDiff(
         actionconfig_data,
         actionconfig_data_old,
         ignore_order=True,
         verbose_level=2
      )
      if diff:
         raise AssertionError(f"Objects differ:\n{diff}")
      
   else:
      diff = deep_diff(actionconfig_data, actionconfig_data_old)
      if diff:
         raise AssertionError("Objects differ")
   
   print("No differences found between actionconfig_data and actionconfig_data_old...")
