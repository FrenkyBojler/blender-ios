# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pathlib
import sys

import bpy

sys.path.append(str(pathlib.Path(__file__).parents[1].absolute()))
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

    command = list(sys.argv)
    for i, cmd in enumerate(command):
        if cmd == "--run-all-tests":
            modifiers_test.do_compare = True
            modifiers_test.run_all_tests()
            break
        elif cmd == "--run-test":
            modifiers_test.do_compare = False
            name = command[i + 1]
            modifiers_test.run_test(name)
            break


if __name__ == "__main__":
    main()
