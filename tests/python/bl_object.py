# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import unittest

import bpy
from mathutils import Vector


class ClosestPointOnMeshTest(unittest.TestCase):
    def test_function_finds_closest_point_successfully(self):
        """Test that attempting to find the closest point succeeds and returns the correct location."""

        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.mesh.primitive_cube_add()
        ret_val = bpy.context.scene.objects[0].closest_point_on_mesh(Vector((0.0, 0.0, 2.0)))
        self.assertTrue(ret_val[0])
        self.assertEqual(ret_val[1], Vector((0.0, 0.0, 1.0)))


class LegacyCurveUndoBoundsTest(unittest.TestCase):
    def test_dimensions_after_undo(self):
        """Regression for #148786: undo must not desync legacy curve dimensions."""
        # Circle diameter (2.0) plus bevel depth (0.1) on each side.
        expected_dimensions = (2.2, 2.2, 0.2)

        # Setup: create two identical beveled legacy curves and record undo steps.
        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.curve.primitive_bezier_circle_add(radius=1.0)
        bpy.context.active_object.name = 'CurveA'
        bpy.context.active_object.data.bevel_depth = 0.1
        bpy.ops.ed.undo_push(message='init')
        bpy.ops.object.duplicate()
        bpy.context.active_object.name = 'CurveB'
        bpy.ops.ed.undo_push(message='duplicate')

        # Act: change selection, then undo back to the post-duplicate state.
        bpy.ops.object.select_all(action='DESELECT')
        bpy.data.objects['CurveA'].select_set(True)
        bpy.context.view_layer.objects.active = bpy.data.objects['CurveA']
        bpy.ops.ed.undo_push(message='select a')
        bpy.ops.ed.undo()

        # Assert: curve dimensions still match evaluated geometry (bevel included).
        for name in ('CurveA', 'CurveB'):
            dimensions = tuple(round(value, 3) for value in bpy.data.objects[name].dimensions)
            self.assertEqual(dimensions, expected_dimensions)


if __name__ == '__main__':
    import sys
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    unittest.main()
