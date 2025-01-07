# SPDX-FileCopyrightText: 2018-2022 The glTF-Blender-IO authors
#
# SPDX-License-Identifier: Apache-2.0

import bpy
import typing
from ......io.exp.user_extensions import export_user_extensions
from ......io.com import gltf2_io
from .....com.extras import generate_extras
from .channels import gather_sk_sampled_channels


def gather_action_sk_sampled(object_uuid: str,
                             blender_action: typing.Optional[bpy.types.Action],
                             slot_handle: int,
                             cache_key: str,
                             export_settings):

    # If no animation in file, no need to bake
    if len(bpy.data.actions) == 0:
        return None

    channels = __gather_channels(object_uuid, blender_action.name if blender_action else cache_key, slot_handle if blender_action else None, export_settings)

    if not channels:
        return None

    # TODOSLOT
    # blender_object = export_settings['vtree'].nodes[object_uuid].blender_object
    # export_user_extensions(
    #     'animation_action_sk_sampled',
    #     export_settings,
    #     animation,
    #     blender_object,
    #     blender_action,
    #     cache_key)

    return channels


# TODOSLOT to move
def __gather_name(object_uuid: str, blender_action: typing.Optional[bpy.types.Action], cache_key: str, export_settings):
    if blender_action:
        return blender_action.name
    elif object_uuid == cache_key:
        return export_settings['vtree'].nodes[object_uuid].blender_object.name
    else:
        return cache_key


def __gather_channels(object_uuid: str, blender_action_name: str, slot_handle: int,
                      export_settings) -> typing.List[gltf2_io.AnimationChannel]:
    return gather_sk_sampled_channels(object_uuid, blender_action_name, slot_handle, export_settings)


# TODOSLOT to move
def __gather_extras(blender_action, export_settings):
    if export_settings['gltf_extras']:
        return generate_extras(blender_action) if blender_action else None
    return None
