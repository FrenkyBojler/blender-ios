# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later */

__all__ = (
    "main",
)

import argparse
import unittest
import sys
import pathlib

import bpy

"""
blender -b --factory-startup --python tests/python/bl_paint.py -- --testdir tests/data/sculpting/ --with_gpu_tests
"""

args = argparse.Namespace


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


class SampleColorTest(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.open_mainfile(filepath=str(args.testdir / "multi_texture_mesh.blend"), load_ui=False)

        self.context_override = bpy.context.copy()
        set_view3d_context_override(self.context_override)

    def test_merged_and_non_merged_return_different_values(self):
        """Test that the sampled color is different depending on if we sample the merged value or not."""
        if not args.with_gpu_tests:
            self.skipTest("Requires GPU to read from buffers.")

        original_color = bpy.context.tool_settings.unified_paint_settings.color
        with bpy.context.temp_override(**self.context_override):
            location = (int(self.context_override['area'].width / 2), int(self.context_override['area'].height / 2))
            bpy.ops.paint.sample_color(location=location)

        non_merged_color = bpy.context.tool_settings.unified_paint_settings.color

        with bpy.context.temp_override(**self.context_override):
            location = (int(self.context_override['area'].width / 2), int(self.context_override['area'].height / 2))
            bpy.ops.paint.sample_color(location=location, merged=True)

        merged_color = bpy.context.tool_settings.unified_paint_settings.color

        self.assertNotEquals(original_color, non_merged_color, "Non-merged sample should not be the original color.")
        self.assertNotEquals(original_color, merged_color, "Merged sample should not be the original color.")
        self.assertNotEquals(non_merged_color, merged_color, "Merged and non-merged sample should not be equivalent.")



def main():
    global args
    import argparse

    argv = [sys.argv[0]]
    if '--' in sys.argv:
        argv += sys.argv[sys.argv.index('--') + 1:]

    parser = argparse.ArgumentParser()
    parser.add_argument('--testdir', required=True, type=pathlib.Path)
    parser.add_argument('--with_gpu_tests', required=False, action='store_true')

    args, remaining = parser.parse_known_args(argv)

    unittest.main(argv=remaining)


if __name__ == "__main__":
    main()
