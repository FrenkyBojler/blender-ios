# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

import unittest

import bpy
from mathutils import Vector


class VolumeCubeBoundsTest(unittest.TestCase):
    def assert_vector_almost_equal(self, actual, expected):
        for actual_value, expected_value in zip(actual, expected):
            self.assertAlmostEqual(actual_value, expected_value, places=5)

    def check_case(self, name, bounds_min, bounds_max, resolution, expected_bounds):
        mesh = bpy.data.meshes.new(name + "Mesh")
        obj = bpy.data.objects.new(name, mesh)
        bpy.context.scene.collection.objects.link(obj)

        group = bpy.data.node_groups.new(name + "Nodes", "GeometryNodeTree")
        try:
            group.interface.new_socket(
                name="Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry"
            )
            output = group.nodes.new("NodeGroupOutput")
            cube = group.nodes.new("GeometryNodeVolumeCube")
            cube.inputs["Density"].default_value = 1.0
            cube.inputs["Background"].default_value = 0.0
            cube.inputs["Min"].default_value = bounds_min
            cube.inputs["Max"].default_value = bounds_max
            cube.inputs["Resolution X"].default_value = resolution[0]
            cube.inputs["Resolution Y"].default_value = resolution[1]
            cube.inputs["Resolution Z"].default_value = resolution[2]
            volume_link = group.links.new(cube.outputs["Volume"], output.inputs["Geometry"])

            modifier = obj.modifiers.new(name="GeometryNodes", type="NODES")
            modifier.node_group = group
            bpy.context.view_layer.update()

            depsgraph = bpy.context.evaluated_depsgraph_get()
            evaluated = obj.evaluated_get(depsgraph)
            geometry = bpy.types.GeometrySet.from_evaluated_object(evaluated)
            volume = geometry.volume
            self.assertIsNotNone(volume)
            self.assertEqual(len(volume.grids), 1)

            transform = volume.grids[0].matrix_object
            expected_voxel_size = tuple(
                (bounds_max[axis] - bounds_min[axis]) / (resolution[axis] - 1)
                for axis in range(3)
            )
            self.assert_vector_almost_equal(
                (transform[0][0], transform[1][1], transform[2][2]), expected_voxel_size
            )
            self.assert_vector_almost_equal(transform.translation, bounds_min)
            active_max = tuple(
                transform.translation[axis] + expected_voxel_size[axis] * (resolution[axis] - 1)
                for axis in range(3)
            )
            self.assert_vector_almost_equal(active_max, bounds_max)

            # The Bounding Box node uses the evaluated volume geometry bounds.
            group.links.remove(volume_link)
            bounding_box = group.nodes.new("GeometryNodeBoundBox")
            group.links.new(cube.outputs["Volume"], bounding_box.inputs["Geometry"])
            group.links.new(bounding_box.outputs["Bounding Box"], output.inputs["Geometry"])
            bpy.context.view_layer.update()

            evaluated = obj.evaluated_get(depsgraph)
            corners = [Vector(corner) for corner in evaluated.bound_box]
            evaluated_min = tuple(min(corner[axis] for corner in corners) for axis in range(3))
            evaluated_max = tuple(max(corner[axis] for corner in corners) for axis in range(3))
            self.assert_vector_almost_equal(evaluated_min, expected_bounds[0])
            self.assert_vector_almost_equal(evaluated_max, expected_bounds[1])
        finally:
            bpy.data.objects.remove(obj, do_unlink=True)
            bpy.data.node_groups.remove(group)
            bpy.data.meshes.remove(mesh)

    def test_bounds(self):
        cases = (
            (
                "symmetric_low",
                (-1.0, -1.0, -1.0),
                (1.0, 1.0, 1.0),
                (2, 2, 2),
                ((-2.0, -2.0, -2.0), (2.0, 2.0, 2.0)),
            ),
            (
                "leaf_below",
                (-1.0, -1.0, -1.0),
                (1.0, 1.0, 1.0),
                (8, 8, 8),
                ((-8.0 / 7.0,) * 3, (8.0 / 7.0,) * 3),
            ),
            (
                "leaf_above",
                (-1.0, -1.0, -1.0),
                (1.0, 1.0, 1.0),
                (9, 9, 9),
                ((-1.125,) * 3, (1.125,) * 3),
            ),
            (
                "symmetric_high_x",
                (-1.0, -1.0, -1.0),
                (1.0, 1.0, 1.0),
                (1025, 2, 2),
                ((-1.0009765625, -2.0, -2.0), (1.0009765625, 2.0, 2.0)),
            ),
            (
                "shifted_low",
                (2.0, -3.0, 5.0),
                (5.0, 1.0, 9.0),
                (2, 3, 5),
                ((0.5, -4.0, 4.5), (6.5, 2.0, 9.5)),
            ),
            (
                "shifted_normal",
                (2.0, -3.0, 5.0),
                (5.0, 1.0, 9.0),
                (17, 11, 9),
                ((1.90625, -3.2, 4.75), (5.09375, 1.2, 9.25)),
            ),
        )
        for case in cases:
            with self.subTest(case=case[0]):
                self.check_case(*case)


def main():
    import sys
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    unittest.main()


if __name__ == "__main__":
    main()
