# SPDX-FileCopyrightText: 2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
blender -b --factory-startup --python tests/python/grease_pencil_join_objects.py -- --testdir tests/data/grease_pencil
"""

import pathlib
import sys
import unittest

import bpy


class GreasePencilJoinObjects(unittest.TestCase):
    ob1: bpy.types.Object
    ob2: bpy.types.Object
    gp1: bpy.types.GreasePencil
    gp2: bpy.types.GreasePencil

    @classmethod
    def setUpClass(cls):
        cls.testdir = args.testdir

    def setUp(self):
        bpy.ops.wm.open_mainfile(filepath=str(self.testdir / "grease_pencil_join_objects.blend"))
        self.ob1 = bpy.data.objects["GPencil1"]
        self.ob2 = bpy.data.objects["GPencil1"]
        self.assertEqual(self.ob1.type, "GREASEPENCIL")
        self.assertEqual(self.ob2.type, "GREASEPENCIL")
        self.gp1 = self.ob1.data
        self.gp2 = self.ob2.data

    def test_join_grease_pencil_objects(self):
        self.ob1.select_set(True)
        self.ob2.select_set(True)
        bpy.context.view_layer.objects.active = self.ob1

        bpy.ops.object.join()


def main():
    global args
    import argparse

    if '--' in sys.argv:
        argv = [sys.argv[0]] + sys.argv[sys.argv.index('--') + 1:]
    else:
        argv = sys.argv

    parser = argparse.ArgumentParser()
    parser.add_argument('--testdir', required=True, type=pathlib.Path)
    args, remaining = parser.parse_known_args(argv)

    unittest.main(argv=remaining)


if __name__ == "__main__":
    main()
