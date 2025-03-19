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
blender -b --factory-startup --python tests/python/bl_sculpt.py -- --testdir tests/data/sculpting/
"""

args = argparse.Namespace

class MaskByColorTest(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.open_mainfile(filepath=str(args.testdir / "plane_with_red_circle.blend"), load_ui=False)

        #self.context_override = bpy.context.copy()
        #set_view3d_context_override(self.context_override)
        #bpy.ops.ed.undo_push()

    @unittest.skipIf(!args.with_gpu_tests, "Built without gpu test support.")
    def test_off_grid_returns_cancelled(self):
        """Test that operator does not run when the cursor is not on the mesh."""

        with bpy.context.temp_override(**self.context_override):
            location = (0, 0)
            ret_val = bpy.ops.sculpt.mask_by_color(location=location)

            self.assertEqual({'CANCELLED'}, ret_val)

        mesh = bpy.context.object.data
        self.assertFalse('.sculpt_mask' in mesh.attributes.keys(), "Mesh should not have the .sculpt_mask attribute!")


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
