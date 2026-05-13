# SPDX-FileCopyrightText: 2020-2022 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# A collection of tests that cover a wide range of modeling related modifiers
#
# Note: Please avoid adding new tests to this file and to the associated .blend file, ideally, a single
# file per modifier is preferred to avoid the headache of merge conflicts that comes with a single large file
# See `multires_modifier.py` for an example of splitting these out.

import math
import os
import sys
from random import seed

import bpy

BASE_DIR = os.path.dirname(os.path.realpath(__file__))
sys.path.append(os.path.join(BASE_DIR, "..", ".."))
from modules.mesh_test import RunTest, ModifierSpec, MultiModifierSpec, SpecMeshTest, OperatorSpec


seed(0)


def main():

    tests = [
        #############################################
        # Grease Pencil Modifiers
        #############################################
        # 0
        SpecMeshTest("Line Art Basic", "testObjLineartBasic", "expObjLineartBasic",
                     [ModifierSpec('lineart_basic', 'LINEART',
                                   {
                                       'source_object': bpy.data.objects['LineartBasic'],
                                       'source_type': 'OBJECT',
                                       'radius': 0.02,
                                       'target_layer': bpy.data.objects['testObjLineartBasic'].data.layers[0].name,
                                       'target_material': bpy.data.objects['testObjLineartBasic'].material_slots[0].material,
                                   },
                                   frame_end=1)]),
    ]

    modifiers_test = RunTest(tests)
    modifiers_test.main()


if __name__ == "__main__":
    main()
