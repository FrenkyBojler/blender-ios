# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later */

__all__ = (
    "main",
)

import math
import unittest
import sys
import pathlib
import numpy as np

import bpy

"""
blender -b --factory-startup --python tests/python/sculpt_paint/brush_strength_curves_test.py -- --testdir tests/files/sculpting/
"""

args = None


def set_view3d_context_override(context_override):
    """
    Set context override to become the first viewport in the active workspace

    The ``context_override`` is expected to be a copy of an actual current context
    obtained by `context.copy()`
    """

    for area in context_override["screen"].areas:
        if area.type != 'VIEW_3D':
            continue
        for space in area.spaces:
            if space.type != 'VIEW_3D':
                continue
            for region in area.regions:
                if region.type != 'WINDOW':
                    continue
                context_override["area"] = area
                context_override["region"] = region


def generate_stroke(context):
    """
    Generate stroke for the bpy.ops.sculpt.brush_stroke operator

    The generated stroke coves the full plane diagonal.
    """
    import bpy
    from mathutils import Vector

    template = {
        "name": "stroke",
        "mouse": (0.0, 0.0),
        "mouse_event": (0, 0),
        "is_start": True,
        "location": (0, 0, 0),
        "pressure": 1.0,
        "time": 1.0,
        "size": 1.0,
        "x_tilt": 0,
        "y_tilt": 0
    }

    num_steps = 100
    start = Vector((context['area'].width, context['area'].height))
    end = Vector((0, 0))
    delta = (end - start) / (num_steps - 1)

    stroke = []
    for i in range(num_steps):
        step = template.copy()
        step["mouse_event"] = start + delta * i
        stroke.append(step)

    return stroke


class MeshBrushTests(unittest.TestCase):
    """
    Test that none of the included brushes create NaN or inf valued vertices
    """

    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.ed.undo_push()

        bpy.ops.mesh.primitive_monkey_add()
        bpy.ops.sculpt.sculptmode_toggle()

    def _activate_brush(self, brush):
        result = bpy.ops.brush.asset_activate(
            asset_library_type='ESSENTIALS',
            relative_asset_identifier='brushes/essentials_brushes-mesh_sculpt.blend/Brush/{}'.format(brush))
        self.assertEqual({'FINISHED'}, result)

    @staticmethod
    def _get_attribute_data():
        mesh = bpy.context.object.data
        position_attr = mesh.attributes['position']
        num_vertices = mesh.attributes.domain_size('POINT')
        position_data = np.zeros((num_vertices * 3), dtype=np.float32)
        position_attr.data.foreach_get('vector', np.ravel(position_data))

        return position_data

    def _check_stroke(self):
        # Ideally, we would use something like pytest and parameterized tests here, but this helper function is an
        # alright solution for now...

        initial_data = self._get_attribute_data()

        context_override = bpy.context.copy()
        set_view3d_context_override(context_override)
        with bpy.context.temp_override(**context_override):
            bpy.ops.sculpt.brush_stroke(stroke=generate_stroke(context_override), override_location=True)

        new_data = self._get_attribute_data()

        # Note, depending on if the tests are run with asserts enabled or not, the test may fail before this point
        # inside blender itself.
        all_valid = all([not math.isinf(pos) and not math.isnan(pos) for pos in new_data])
        any_different = any([orig != new for (orig, new) in zip(initial_data, new_data)])
        self.assertTrue(all_valid, "All position components should be rational values")
        self.assertTrue(any_different, "At least one position should be different from its original value")

    def test_blob_brush_creates_valid_data(self):
        self._activate_brush("Blob")
        self._check_stroke()

    def test_clay_brush_creates_valid_data(self):
        self._activate_brush("Clay")
        self._check_stroke()

    def test_clay_strips_brush_creates_valid_data(self):
        self._activate_brush("Clay Strips")
        self._check_stroke()

    def test_clay_thumb_brush_creates_valid_data(self):
        self._activate_brush("Clay Thumb")
        self._check_stroke()

    def test_crease_polish_brush_creates_valid_data(self):
        self._activate_brush("Crease Polish")
        self._check_stroke()

    def test_crease_sharp_brush_creates_valid_data(self):
        self._activate_brush("Crease Sharp")
        self._check_stroke()

    def test_draw_brush_creates_valid_data(self):
        self._activate_brush("Draw")
        self._check_stroke()

    def test_draw_sharp_brush_creates_valid_data(self):
        self._activate_brush("Draw Sharp")
        self._check_stroke()

    def test_inflate_deflate_brush_creates_valid_data(self):
        self._activate_brush("Inflate/Deflate")
        self._check_stroke()

    def test_fill_deepen_brush_creates_valid_data(self):
        self._activate_brush("Fill/Deepen")
        self._check_stroke()

    def test_flatten_contrast_brush_creates_valid_data(self):
        self._activate_brush("Flatten/Contrast")
        self._check_stroke()

    def test_plateau_brush_creates_valid_data(self):
        self._activate_brush("Plateau")
        self._check_stroke()

    def test_scrape_multiplane_brush_creates_valid_data(self):
        self._activate_brush("Scrape Multiplane")
        self._check_stroke()

    def test_scrape_fill_brush_creates_valid_data(self):
        self._activate_brush("Scrape/Fill")
        self._check_stroke()

    def test_smooth_brush_creates_valid_data(self):
        self._activate_brush("Smooth")
        self._check_stroke()

    def test_trim_brush_creates_valid_data(self):
        self._activate_brush("Trim")
        self._check_stroke()

    # We don't test the boundary brush here due to more specific mouse positioning requirements

    def test_elastic_grab_brush_creates_valid_data(self):
        self._activate_brush("Elastic Grab")
        self._check_stroke()

    def test_elastic_snake_hook_brush_creates_valid_data(self):
        self._activate_brush("Elastic Snake Hook")
        self._check_stroke()

    def test_grab_brush_creates_valid_data(self):
        self._activate_brush("Grab")
        self._check_stroke()

    def test_grab_2d_brush_creates_valid_data(self):
        self._activate_brush("Grab 2D")
        self._check_stroke()

    def test_grab_silhouette_brush_creates_valid_data(self):
        self._activate_brush("Grab Silhouette")
        self._check_stroke()

    def test_nudge_brush_creates_valid_data(self):
        self._activate_brush("Nudge")
        self._check_stroke()

    def test_pinch_magnify_brush_creates_valid_data(self):
        self._activate_brush("Pinch/Magnify")
        self._check_stroke()

    @unittest.skip("Debug assert")
    def test_pose_brush_creates_valid_data(self):
        self._activate_brush("Pose")
        self._check_stroke()

    def test_pull_brush_creates_valid_data(self):
        self._activate_brush("Pull")
        self._check_stroke()

    def test_relax_pinch_brush_creates_valid_data(self):
        self._activate_brush("Relax Pinch")
        self._check_stroke()

    def test_relax_slide_brush_creates_valid_data(self):
        self._activate_brush("Relax Slide")
        self._check_stroke()

    def test_snake_hook_brush_creates_valid_data(self):
        self._activate_brush("Snake Hook")
        self._check_stroke()

    def test_thumb_brush_creates_valid_data(self):
        self._activate_brush("Thumb")
        self._check_stroke()

    @unittest.skip("Needs specific positioning")
    def test_twist_brush_creates_valid_data(self):
        self._activate_brush("Twist")
        self._check_stroke()


def main():
    global args
    import argparse

    argv = [sys.argv[0]]
    if '--' in sys.argv:
        argv += sys.argv[sys.argv.index('--') + 1:]

    parser = argparse.ArgumentParser()
    parser.add_argument('--testdir', required=True, type=pathlib.Path)

    args, remaining = parser.parse_known_args(argv)

    unittest.main(argv=remaining)


if __name__ == "__main__":
    main()
