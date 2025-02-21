# SPDX-FileCopyrightText: 2021-2022 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Pose Library - usage functions.
"""

from typing import Set
import re
import bpy

from bpy.types import (
    Action,
    Object,
    ActionSlot,
)


def _find_best_slot(action: Action, object: Object) -> ActionSlot:
    if not action.slots:
        return None
    assigned_slot = None
    # For the selection code, the object doesn't need to be animated yet.
    if object.animation_data and object.animation_data.action_slot:
        assigned_slot = object.animation_data.action_slot

    if assigned_slot and assigned_slot.identifier in action.slots:
        return action.slots[assigned_slot.identifier]

    return action.slots[0]


def select_bones(arm_object: Object, action: Action, *, select: bool, flipped: bool) -> None:
    pose_bone_re = re.compile(r'pose.bones\["([^"]+)"\]')
    pose = arm_object.pose
    if not pose:
        return

    slot = _find_best_slot(action, arm_object)
    if not slot:
        return

    seen_bone_names: Set[str] = set()
    for layer in action.layers:
        for strip in layer.strips:
            channelbag = strip.channelbag(slot)
            if not channelbag:
                continue
            for fcurve in channelbag.fcurves:
                data_path: str = fcurve.data_path
                match = pose_bone_re.match(data_path)
                if not match:
                    continue

                bone_name = match.group(1)

                if bone_name in seen_bone_names:
                    continue
                seen_bone_names.add(bone_name)

                if flipped:
                    bone_name = bpy.utils.flip_name(bone_name)

                try:
                    pose_bone = pose.bones[bone_name]
                except KeyError:
                    continue

                pose_bone.bone.select = select


if __name__ == '__main__':
    import doctest

    print(f"Test result: {doctest.testmod()}")
