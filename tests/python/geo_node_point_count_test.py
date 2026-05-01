# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Geometry node test that verifies point count rather than exact geometry.

Used for merge-by-distance tests whose winner-selection is intentionally
order-dependent: the blend file is designed so that the output count is
invariant regardless of which point wins each merge cluster.
"""

import bpy
import os
import sys

# The evaluated test_object must produce exactly this many points.
EXPECTED_POINT_COUNT = 12500


def main():
    test_name = bpy.path.display_name_from_filepath(bpy.data.filepath)
    print("\nSTART {} test.".format(test_name))

    depsgraph = bpy.context.evaluated_depsgraph_get()
    test_obj = bpy.data.objects["test_object"]
    evaluated = test_obj.evaluated_get(depsgraph)

    data = evaluated.data
    if hasattr(data, 'points'):
        actual = len(data.points)
    elif hasattr(data, 'vertices'):
        actual = len(data.vertices)
    else:
        raise Exception("test_object data has neither points nor vertices: {}".format(type(data).__name__))

    if actual != EXPECTED_POINT_COUNT:
        raise Exception(
            "FAILED {}: expected {} points, got {}".format(
                test_name, EXPECTED_POINT_COUNT, actual
            )
        )

    print("PASSED {}: {} points as expected.".format(test_name, actual))


main()
