/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "BLI_array_utils.hh"

#include "BKE_curves.hh"
#include "BKE_geometry_set.hh"
#include "BKE_gtest_base.hh"
#include "BKE_instances.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"

#include "DNA_curves_types.h"

#include "GEO_mesh_primitive_cuboid.hh"
#include "GEO_realize_instances.hh"

#include "testing/testing.h"

namespace blender {

using namespace blender::bke;

namespace geometry::tests {

class RealizeInstancesTest : public bke::BlenderGTestBase {};

static void create_test_curves(bke::CurvesGeometry &curves, Span<int> offsets)
{
  BLI_assert(!offsets.is_empty());
  const int curves_num = offsets.size() - 1;
  const int points_num = offsets.last();

  curves.resize(points_num, curves_num);
  curves.offsets_for_write().copy_from(offsets);
  curves.update_curve_types();

  /* Attribute storing original indices to test point remapping. */
  SpanAttributeWriter<int> test_indices_writer =
      curves.attributes_for_write().lookup_or_add_for_write_span<int>(
          "test_index", bke::AttrDomain::Point, bke::AttributeInitConstruct());
  array_utils::fill_index_range(test_indices_writer.span);
  test_indices_writer.finish();
}

/* Regression test for builtin curve attributes:
 * The attribute can be added with arbitrary type/domain on instances,
 * but is built-in and restricted on curves, which will not allow writing it
 * to the realized curves geometry. #142163 */
TEST_F(RealizeInstancesTest, InstanceAttributeToBuiltinCurvesAttribute)
{
  Curves *curves_id = BKE_id_new_nomain<Curves>("TestCurves");
  create_test_curves(curves_id->geometry.wrap(), {0, 3});
  bke::GeometrySet curves_geometry = GeometrySet::from_curves(curves_id);

  auto instances = std::make_unique<Instances>(2);
  const int handle = instances->add_reference(bke::InstanceReference{curves_geometry});
  /* The issue only occurs with 2 or more instances. In case of a single instance the code takes a
   * special path that does not run cause this problem. */
  instances->reference_handles_for_write().fill(handle);
  instances->transforms_for_write().fill(float4x4::identity());
  /* This attribute will be converted to the point domain, where it is invalid on curves. */
  instances->attributes_for_write().add<float>(
      "curve_type", AttrDomain::Instance, AttributeInitDefaultValue());
  bke::GeometrySet instances_geometry = GeometrySet::from_instances(std::move(instances));

  geometry::RealizeInstancesOptions options;
  options.realize_instance_attributes = true;
  GeometrySet realized_geometry_set =
      geometry::realize_instances(instances_geometry, options).geometry;
}

static void expect_cube_face_normals_outward(const Mesh &mesh, const int expected_faces_num)
{
  EXPECT_EQ(mesh.faces_num, expected_faces_num);
  const Span<float3> positions = mesh.vert_positions();
  const Span<float3> normals = mesh.face_normals();
  const OffsetIndices faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  for (const int face_i : faces.index_range()) {
    float3 center(0.0f);
    for (const int corner : faces[face_i]) {
      center += positions[corner_verts[corner]];
    }
    center /= faces[face_i].size();
    EXPECT_GT(math::dot(center, normals[face_i]), 0.0f);
  }
}

TEST_F(RealizeInstancesTest, NegativeTransformMeshWindingSingleInstance)
{
  Mesh *cube = geometry::create_cuboid_mesh(float3(2.0f), 2, 2, 2);
  bke::GeometrySet cube_geometry = GeometrySet::from_mesh(cube);

  auto instances = std::make_unique<Instances>(1);
  const int handle = instances->add_reference(bke::InstanceReference{cube_geometry});
  instances->reference_handles_for_write().fill(handle);
  instances->transforms_for_write().fill(math::from_scale<float4x4>(float3(-1.0f, 1.0f, 1.0f)));

  bke::GeometrySet instances_geometry = GeometrySet::from_instances(std::move(instances));
  geometry::RealizeInstancesOptions options;
  bke::GeometrySet realized_geometry =
      geometry::realize_instances(instances_geometry, options).geometry;
  const Mesh *realized_mesh = realized_geometry.get_mesh();

  ASSERT_NE(realized_mesh, nullptr);
  expect_cube_face_normals_outward(*realized_mesh, 6);
}

TEST_F(RealizeInstancesTest, NegativeTransformMeshWindingMultipleInstances)
{
  Mesh *cube = geometry::create_cuboid_mesh(float3(2.0f), 2, 2, 2);
  bke::GeometrySet cube_geometry = GeometrySet::from_mesh(cube);

  auto instances = std::make_unique<Instances>(2);
  const int handle = instances->add_reference(bke::InstanceReference{cube_geometry});
  instances->reference_handles_for_write().fill(handle);
  instances->transforms_for_write()[0] = float4x4::identity();
  instances->transforms_for_write()[1] = math::from_scale<float4x4>(float3(-1.0f, 1.0f, 1.0f));

  bke::GeometrySet instances_geometry = GeometrySet::from_instances(std::move(instances));
  geometry::RealizeInstancesOptions options;
  bke::GeometrySet realized_geometry =
      geometry::realize_instances(instances_geometry, options).geometry;
  const Mesh *realized_mesh = realized_geometry.get_mesh();

  ASSERT_NE(realized_mesh, nullptr);
  expect_cube_face_normals_outward(*realized_mesh, 12);
}

}  // namespace geometry::tests
}  // namespace blender
