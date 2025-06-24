import bpy
import unittest


class TestMeshSpatialOrganization(unittest.TestCase):

    def setUp(self):
        bpy.ops.object.select_all(action='SELECT')
        bpy.ops.object.delete(use_global=False)
        if bpy.context.mode != 'OBJECT':
            bpy.ops.object.mode_set(mode='OBJECT')

    def tearDown(self):
        bpy.ops.object.select_all(action='SELECT')
        bpy.ops.object.delete(use_global=False)

    def create_subdivided_plane(self, subdivisions):
        bpy.ops.mesh.primitive_plane_add(size=2, location=(0, 0, 0))
        plane = bpy.context.active_object
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.mesh.select_all(action='SELECT')
        bpy.ops.mesh.subdivide(number_cuts=subdivisions, smoothness=0.0)
        bpy.ops.object.mode_set(mode='OBJECT')
        return plane

    def get_vertex_data(self, obj):
        mesh = obj.data
        vertices = [(v.co.x, v.co.y, v.co.z) for v in mesh.vertices]
        return {
            'vertices': vertices,
            'vertex_count': len(vertices)
        }

    def test_spatial_organization_changes_vertex_order(self):
        plane = self.create_subdivided_plane(subdivisions=50)
        initial_data = self.get_vertex_data(plane)
        bpy.ops.mesh.reorder_vertices_spatial()
        final_data = self.get_vertex_data(plane)
        self.assertEqual(initial_data['vertex_count'], final_data['vertex_count'])
        vertices_changed = initial_data['vertices'] != final_data['vertices']
        self.assertTrue(vertices_changed)


def run_tests():
    loader = unittest.TestLoader()
    suite = loader.loadTestsFromTestCase(TestMeshSpatialOrganization)
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)
    return result.wasSuccessful()


if __name__ == "__main__":
    success = run_tests()
    print(f"\nTests {'PASSED' if success else 'FAILED'}")
