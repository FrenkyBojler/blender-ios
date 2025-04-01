# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pathlib
import re
import sys
import unittest

import bpy
from mathutils import Matrix


"""
blender -b --factory-startup --python tests/python/bl_animation_copy_global_transform.py -- --testdir tests/data/animation
"""

C = bpy.context
D = bpy.data

import copy_global_transform


class TestableClipboard(copy_global_transform.Clipboard):
    # Stored on a class attribute, as it's shared across instances.
    payload = ""

    def write(self, payload: str) -> None:
        self.__class__.payload = payload

    def read(self) -> str:
        return self.__class__.payload


class CopyGlobalTransformTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.testdir = args.testdir

        cls._orig_clipboard_class = copy_global_transform.ClipboardClass
        copy_global_transform.ClipboardClass = TestableClipboard

    @classmethod
    def tearDownClass(cls):
        copy_global_transform.ClipboardClass = cls._orig_clipboard_class

    def setUp(self):
        bpy.ops.wm.open_mainfile(filepath=str(self.testdir / "copy-global-transform.blend"))

    def run_operator(self):
        # The operator has to run in a screen context.
        result = bpy.ops.object.copy_global_transform()
        self.assertEqual({'FINISHED'}, result)

    def test_copy_object(self):
        # Suzanne should be at the origin on frame 1.
        suzanne = D.objects['Suzanne']
        C.view_layer.objects.active = suzanne
        C.scene.frame_set(1)
        self.assertEqual(Matrix.Identity(4), suzanne.matrix_world)

        self.run_operator()
        expect = """Matrix((
            (1.0, 0.0, 0.0, 0.0),
            (0.0, 1.0, 0.0, 0.0),
            (0.0, 0.0, 1.0, 0.0),
            (0.0, 0.0, 0.0, 1.0),
        ))"""
        self.assertEqualString(expect, TestableClipboard.payload)

    def assertEqualString(self, expect: str, actual: str, msg: str | None = None) -> None:
        """String comparison that collapses whitespace.

        This makes the testing code a bit easier to read, as it's independent of
        indentation and line wrapping.
        """
        self.assertEqual(
            re.sub(r"\s+", " ", expect),
            re.sub(r"\s+", " ", actual),
            msg,
        )


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

    # Enable the Copy Global Transform add-on before starting the tests.
    bpy.ops.preferences.addon_enable(module="copy_global_transform")

    unittest.main(argv=remaining)


if __name__ == "__main__":
    main()
