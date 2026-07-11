# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
This file does not run anything; its methods are accessed by ``run_blender_setup.py``.
"""

import modules.ui_test_utils as ui


def view3d_select_armature_in_front_of_grease_pencil():
    """
    Regression test for #156983.

    In orthographic Front view, an armature clearly in front of a Grease Pencil
    object must be selectable on the first click. GP selection used a clip-space
    depth bias that made GP win picking against nearby objects in ortho.
    """
    import bpy
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    e, t, window = ui.test_window()

    # Clear the default startup scene.
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)

    # Match UI Add → Grease Pencil → Monkey (In Front off).
    bpy.ops.object.grease_pencil_add(type='MONKEY', use_in_front=False)
    gp = bpy.context.object
    gp.name = "Suzanne"
    gp.location = (0.0, 0.0, 0.0)
    t.assertFalse(gp.show_in_front)

    # Match UI Add → Armature.
    bpy.ops.object.armature_add()
    arm = bpy.context.object
    arm.name = "Armature"
    # Front view looks along +Y from the -Y side; negative Y is toward camera.
    arm.location = (0.0, -0.22, 0.0)

    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    t.assertIsNotNone(area, "Expected a 3D View")
    region = next(region for region in area.regions if region.type == 'WINDOW')

    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.view3d.view_axis(type='FRONT')
        region.data.view_perspective = 'ORTHO'
    # Fixed framing so the bone stays large enough to rasterize for GPU picking.
    region.data.view_distance = 4.0
    region.data.view_location = (0.0, 0.0, 0.25)
    yield

    bpy.ops.object.select_all(action='DESELECT')

    # Mid-bone pick; the object origin alone is a weak target over large GP fills.
    click_world = arm.matrix_world @ Vector((0.0, 0.0, 0.5))
    region_co = location_3d_to_region_2d(region, region.data, click_world)
    mid_bone_win_co = (int(region.x + region_co[0]), int(region.y + region_co[1]))
    e.cursor_position_set(*mid_bone_win_co, move=True)
    yield
    e.leftmouse.tap()
    yield

    t.assertEqual(
        window.view_layer.objects.active,
        arm,
        "Armature in front of Grease Pencil should be selected on first click in ortho front view",
    )
    t.assertTrue(arm.select_get())
