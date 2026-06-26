# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later */
"""
blender -b --factory-startup --python tests/python/sculpt_paint/grease_pencil_paint_tests.py -- --testdir tests/files/mesh_paint/
"""

__all__ = (
    "main",
)

import math
import numpy as np
import os
import pathlib
import sys
import unittest

import bpy

sys.path.append(os.path.dirname(os.path.realpath(__file__)))
from modules.test_helpers import set_view3d_context_override, generate_stroke

args = None


@staticmethod
def compare_drawing(evaluated_drawing, expected_drawing):

    if len(evaluated_drawing.attributes.items()) != len(expected_drawing.attributes.items()):
        print("Attribute count doesn't match")
        return False

    for a_idx, attribute in evaluated_drawing.attributes.items():
        expected_attribute = expected_drawing.attributes[a_idx]
    
        if len(attribute.data.items()) != len(expected_attribute.data.items()):
            print("Attribute data length doesn't match")
            return False
    
        value_attr_name = (
            'vector' if attribute.data_type == 'FLOAT_VECTOR' or
            attribute.data_type == 'FLOAT2' else
            'color' if attribute.data_type == 'FLOAT_COLOR' else 'value'
        )
    
        for v_idx, attribute_value in attribute.data.items():
            if getattr(
                    attribute_value,
                    value_attr_name) != getattr(
                    expected_attribute.data[v_idx],
                    value_attr_name):
                print("Attribute '{}' values do not match".format(attribute.name))
                print(getattr(attribute_value, value_attr_name))
                print(getattr(expected_attribute.data[v_idx], value_attr_name))
                return False

    return True


@staticmethod
def compare_layer(evaluated_layer, expected_layer):
    if len(evaluated_layer.frames.items()) != len(expected_layer.frames.items()):
        print("Number of frames doesn't match")
        return false

    for a_idx, frame in evaluated_layer.frames.items():
        expected_frame = expected_layer.frames[a_idx]

        if False == compare_drawing(frame.drawing, expected_frame.drawing):
            return False

    return True


@staticmethod
def compare_greasepencil(evaluated_greasepencil, expected_greasepencil):
    if len(evaluated_greasepencil.layers.items()) != len(expected_greasepencil.layers.items()):
        print("Number of layers does not match")
        return False;

    for a_idx, layer in evaluated_greasepencil.layers.items():
        expected_layer = expected_greasepencil.layers[a_idx]

        if False == compare_layer(layer, expected_layer):
            return False

    return True




class GreasePencilPaintTests(unittest.TestCase):
    """
    Test that painting some predefined test strokes result in the expected drawing
    """

    def setUp(self):
        bpy.ops.wm.open_mainfile(filepath=str(args.testdir / "grease_pencil_paint_tests.blend"), load_ui=False)
        bpy.ops.ed.undo_push()


    def prepare(self):
        self.data = bpy.data.grease_pencils.new("empty_test")
        obj = bpy.data.objects.new("empty_test", self.data)
        bpy.context.collection.objects.link(obj)
        layer = self.data.layers.new("layer")
        frame = layer.frames.new(0)
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj

    def cleanup(self):
        bpy.ops.object.mode_set(mode='OBJECT')
        bpy.ops.object.delete()

    def test_stroke_generates_correct_drawing(self):
        self.prepare()
        bpy.ops.object.mode_set(mode='PAINT_GREASE_PENCIL')
        context_override = bpy.context.copy()
        set_view3d_context_override(context_override)
        with bpy.context.temp_override(**context_override):
            bpy.ops.grease_pencil.brush_stroke(stroke=generate_stroke(context_override))
        #compare with expected
        bpy.ops.object.mode_set(mode='OBJECT')

        result = compare_greasepencil(self.data, bpy.data.objects['Expected'].data)
        self.assertTrue(result, "Drawing doesn't match")
        self.cleanup()

        #self._activate_brush("Add Weight")
        #self._check_stroke()





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
