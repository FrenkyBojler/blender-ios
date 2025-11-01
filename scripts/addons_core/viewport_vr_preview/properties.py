# SPDX-FileCopyrightText: 2021-2022 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import (
    PropertyGroup,
)
from bpy.app.handlers import persistent


# Landmarks.
@persistent
def vr_ensure_default_landmark(context: bpy.context):
    # Ensure there's a default landmark (scene camera by default).
    landmarks = bpy.context.scene.vr_landmarks
    if not landmarks:
        landmarks.add()
        landmarks[0].type = 'SCENE_CAMERA'


def vr_landmark_active_type_update(self, context):
    wm = context.window_manager
    session_settings = wm.xr_session_settings
    landmark_active = VRLandmark.get_active_landmark(context)

    # Update session's base pose type to the matching type.
    if landmark_active.type == 'SCENE_CAMERA':
        session_settings.base_pose_type = 'SCENE_CAMERA'
    elif landmark_active.type == 'OBJECT':
        session_settings.base_pose_type = 'OBJECT'
    elif landmark_active.type == 'CUSTOM':
        session_settings.base_pose_type = 'CUSTOM'


def vr_landmark_active_base_pose_object_update(self, context):
    session_settings = context.window_manager.xr_session_settings
    landmark_active = VRLandmark.get_active_landmark(context)

    # Update the anchor object to the (new) camera of this landmark.
    session_settings.base_pose_object = landmark_active.base_pose_object


def vr_landmark_active_base_pose_location_update(self, context):
    session_settings = context.window_manager.xr_session_settings
    landmark_active = VRLandmark.get_active_landmark(context)

    session_settings.base_pose_location = landmark_active.base_pose_location


def vr_landmark_active_base_pose_angle_update(self, context):
    session_settings = context.window_manager.xr_session_settings
    landmark_active = VRLandmark.get_active_landmark(context)

    session_settings.base_pose_angle = landmark_active.base_pose_angle


def vr_landmark_active_base_scale_update(self, context):
    session_settings = context.window_manager.xr_session_settings
    landmark_active = VRLandmark.get_active_landmark(context)

    session_settings.base_scale = landmark_active.base_scale


def vr_landmark_type_update(self, context):
    landmark_selected = VRLandmark.get_selected_landmark(context)
    landmark_active = VRLandmark.get_active_landmark(context)

    # Don't allow non-trivial base scale for scene camera landmarks.
    if landmark_selected.type == 'SCENE_CAMERA':
        landmark_selected.base_scale = 1.0

    # Only update session settings data if the changed landmark is actually
    # the active one.
    if landmark_active == landmark_selected:
        vr_landmark_active_type_update(self, context)


def vr_landmark_base_pose_object_update(self, context):
    landmark_selected = VRLandmark.get_selected_landmark(context)
    landmark_active = VRLandmark.get_active_landmark(context)

    # Only update session settings data if the changed landmark is actually
    # the active one.
    if landmark_active == landmark_selected:
        vr_landmark_active_base_pose_object_update(self, context)


def vr_landmark_base_pose_location_update(self, context):
    landmark_selected = VRLandmark.get_selected_landmark(context)
    landmark_active = VRLandmark.get_active_landmark(context)

    # Only update session settings data if the changed landmark is actually
    # the active one.
    if landmark_active == landmark_selected:
        vr_landmark_active_base_pose_location_update(self, context)


def vr_landmark_base_pose_angle_update(self, context):
    landmark_selected = VRLandmark.get_selected_landmark(context)
    landmark_active = VRLandmark.get_active_landmark(context)

    # Only update session settings data if the changed landmark is actually
    # the active one.
    if landmark_active == landmark_selected:
        vr_landmark_active_base_pose_angle_update(self, context)


def vr_landmark_base_scale_update(self, context):
    landmark_selected = VRLandmark.get_selected_landmark(context)
    landmark_active = VRLandmark.get_active_landmark(context)

    # Only update session settings data if the changed landmark is actually
    # the active one.
    if landmark_active == landmark_selected:
        vr_landmark_active_base_scale_update(self, context)


def vr_landmark_active_update(self, context):
    wm = context.window_manager

    vr_landmark_active_type_update(self, context)
    vr_landmark_active_base_pose_object_update(self, context)
    vr_landmark_active_base_pose_location_update(self, context)
    vr_landmark_active_base_pose_angle_update(self, context)
    vr_landmark_active_base_scale_update(self, context)

    if wm.xr_session_state:
        wm.xr_session_state.reset_to_base_pose(context)


class VRLandmark(PropertyGroup):
    name: bpy.props.StringProperty(
        name="VR Landmark",
        default="Landmark"
    )
    type: bpy.props.EnumProperty(
        name="Type",
        items=[
            ('SCENE_CAMERA', "Scene Camera",
             "Use scene's currently active camera to define the VR view base "
             "location and rotation"),
            ('OBJECT', "Custom Object",
             "Use an existing object to define the VR view base location and "
             "rotation"),
            ('CUSTOM', "Custom Pose",
             "Allow a manually defined position and rotation to be used as "
             "the VR view base pose"),
            ('VIEWFINDER', "Viewfinder",
             "Camera viewpoint captured with the VR director viewfinder"),
        ],
        default='SCENE_CAMERA',
        update=vr_landmark_type_update,
    )
    base_pose_object: bpy.props.PointerProperty(
        name="Object",
        type=bpy.types.Object,
        update=vr_landmark_base_pose_object_update,
    )
    base_pose_location: bpy.props.FloatVectorProperty(
        name="Base Pose Location",
        subtype='TRANSLATION',
        update=vr_landmark_base_pose_location_update,
    )
    base_pose_angle: bpy.props.FloatProperty(
        name="Base Pose Angle",
        subtype='ANGLE',
        update=vr_landmark_base_pose_angle_update,
    )
    base_scale: bpy.props.FloatProperty(
        name="Base Scale",
        description="Viewer reference scale associated with this landmark",
        default=1.0,
        min=0.000001,
        update=vr_landmark_base_scale_update,
    )
    
    # Director Viewfinder properties
    camera_lens: bpy.props.FloatProperty(
        name="Camera Lens",
        description="Focal length in millimeters",
        default=50.0,
        min=1.0,
        max=5000.0,
    )
    camera_aperture: bpy.props.FloatProperty(
        name="Camera Aperture",
        description="F-Stop value for depth of field",
        default=2.8,
        min=0.1,
        max=128.0,
    )
    camera_focus_distance: bpy.props.FloatProperty(
        name="Focus Distance",
        description="Distance to focus point",
        default=10.0,
        min=0.0,
    )
    camera_rotation: bpy.props.FloatVectorProperty(
        name="Camera Rotation",
        description="Camera orientation as Euler angles",
        subtype='EULER',
        size=3,
    )
    use_dof: bpy.props.BoolProperty(
        name="Use Depth of Field",
        description="Enable depth of field for this viewfinder capture",
        default=False,
    )

    @staticmethod
    def get_selected_landmark(context):
        scene = context.scene
        landmarks = scene.vr_landmarks

        return (
            None if (len(landmarks) <
                     1) else landmarks[scene.vr_landmarks_selected]
        )

    @staticmethod
    def get_active_landmark(context):
        scene = context.scene
        landmarks = scene.vr_landmarks

        return (
            None if (len(landmarks) <
                     1) else landmarks[scene.vr_landmarks_active]
        )


classes = (
    VRLandmark,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)

    bpy.types.Scene.vr_landmarks = bpy.props.CollectionProperty(
        name="Landmark",
        type=VRLandmark,
    )
    bpy.types.Scene.vr_landmarks_selected = bpy.props.IntProperty(
        name="Selected Landmark"
    )
    bpy.types.Scene.vr_landmarks_active = bpy.props.IntProperty(
        update=vr_landmark_active_update,
    )
    
    # Viewfinder properties
    bpy.types.Scene.vr_viewfinder_enabled = bpy.props.BoolProperty(
        name="Enable VR Viewfinder",
        description="Enable the director viewfinder on the left controller",
        default=False,
    )
    bpy.types.Scene.vr_viewfinder_mode = bpy.props.EnumProperty(
        name="Viewfinder Mode",
        items=[
            ('LIVE', "Live", "Live viewfinder mode for capturing new shots"),
            ('PLAYBACK', "Playback", "Review captured shots"),
        ],
        default='LIVE',
    )
    bpy.types.Scene.vr_viewfinder_active_button = bpy.props.IntProperty(
        name="Active Viewfinder Button",
        description="Currently active button in the viewfinder UI",
        default=0,
        min=0,
    )
    bpy.types.Scene.vr_viewfinder_playback_index = bpy.props.IntProperty(
        name="Playback Index",
        description="Index of the viewfinder landmark being previewed",
        default=0,
        min=0,
    )
    bpy.types.Scene.vr_viewfinder_size = bpy.props.FloatProperty(
        name="Viewfinder Size",
        description="Size of the viewfinder display",
        default=0.15,
        min=0.01,
        max=1.0,
    )
    bpy.types.Scene.vr_viewfinder_passepartout = bpy.props.FloatProperty(
        name="Viewfinder Passepartout",
        description="Amount of overscan to show around the frame",
        default=0.1,
        min=0.0,
        max=0.5,
    )

    bpy.app.handlers.load_post.append(vr_ensure_default_landmark)


def unregister():
    for cls in classes:
        bpy.utils.unregister_class(cls)

    del bpy.types.Scene.vr_landmarks
    del bpy.types.Scene.vr_landmarks_selected
    del bpy.types.Scene.vr_landmarks_active
    
    # Clean up viewfinder properties
    del bpy.types.Scene.vr_viewfinder_enabled
    del bpy.types.Scene.vr_viewfinder_mode
    del bpy.types.Scene.vr_viewfinder_active_button
    del bpy.types.Scene.vr_viewfinder_playback_index
    del bpy.types.Scene.vr_viewfinder_size
    del bpy.types.Scene.vr_viewfinder_passepartout

    bpy.app.handlers.load_post.remove(vr_ensure_default_landmark)
