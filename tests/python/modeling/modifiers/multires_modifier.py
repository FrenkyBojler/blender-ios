# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

__all__ = (
    "main",
)

import pathlib
import sys

import bpy

"""
blender -b --factory-startup tests/files/modeling/modifiers/multires_modifier --python tests/python/modeling/modifiers/multires_modifier.py -- --run-all-tests
"""

sys.path.append(str(pathlib.Path(__file__).parents[2].absolute()))
from modules.mesh_test import RunTest, ModifierSpec, MultiModifierSpec, SpecMeshTest, OperatorSpecObjectMode


def main():
    tests = [
        SpecMeshTest("CubeMultires", "testCubeMultires", "expectedCubeMultires",
                     [
                         ModifierSpec('multires', 'MULTIRES', {}),
                         OperatorSpecObjectMode('multires_subdivide', {'modifier': 'multires'}),
                         OperatorSpecObjectMode('modifier_apply', {'modifier': 'multires'})
                     ], apply_modifier=False),
    ]

    modifiers_test = RunTest(tests)
    modifiers_test.main()


if __name__ == "__main__":
    main()
