/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#ifdef WITH_OPENVDB

#  include <openvdb/openvdb.h>

#  include "testing/testing.h"

#  include "BLI_math_vector.hh"

#  include "DNA_volume_types.h"

#  include "BKE_gtest_base.hh"
#  include "BKE_idtype.hh"
#  include "BKE_lib_id.hh"
#  include "BKE_main.hh"
#  include "BKE_volume.hh"
#  include "BKE_volume_grid.hh"
#  include "BKE_volume_openvdb.hh"
#  include "BKE_volume_render.hh"

namespace blender::bke::tests {

class VolumeTest : public BlenderGTestBase {
 public:
  Main *bmain;

  void SetUp() override
  {
    bmain = BKE_main_new();
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
  }
};

TEST_F(VolumeTest, add_grid_with_name_and_find)
{
  Volume *volume = BKE_id_new<Volume>(bmain, nullptr);
  GVolumeGrid grid{VOLUME_GRID_FLOAT};
  grid.get_for_write().set_name("My Grid");
  const VolumeGridData *grid_data = grid.release();
  BKE_volume_grid_add(volume, *grid_data);
  EXPECT_EQ(grid_data, BKE_volume_grid_find(volume, "My Grid"));
  EXPECT_TRUE(grid_data->is_mutable());
  BKE_id_free(bmain, volume);
}

TEST_F(VolumeTest, add_grid_in_two_volumes)
{
  Volume *volume_a = BKE_id_new<Volume>(bmain, nullptr);
  Volume *volume_b = BKE_id_new<Volume>(bmain, nullptr);
  GVolumeGrid grid{VOLUME_GRID_FLOAT};
  grid.get_for_write().set_name("My Grid");
  const VolumeGridData *grid_data = grid.release();
  BKE_volume_grid_add(volume_a, *grid_data);
  EXPECT_TRUE(grid_data->is_mutable());
  grid_data->add_user();
  BKE_volume_grid_add(volume_b, *grid_data);
  EXPECT_FALSE(grid_data->is_mutable());

  VolumeGridData *grid_from_a = BKE_volume_grid_get_for_write(volume_a, 0);
  const VolumeGridData *grid_from_b = BKE_volume_grid_get(volume_b, 0);
  EXPECT_NE(grid_data, grid_from_a);
  EXPECT_TRUE(grid_from_a->is_mutable());
  EXPECT_TRUE(grid_from_b->is_mutable());

  BKE_id_free(bmain, volume_a);
  BKE_id_free(bmain, volume_b);
}

static void collect_wireframe_vertices(
    void *userdata, const float (*verts)[3], const int (*)[2], const int verts_num, const int)
{
  Vector<float3> &vertices = *static_cast<Vector<float3> *>(userdata);
  for (const int i : IndexRange(verts_num)) {
    vertices.append(float3(verts[i][0], verts[i][1], verts[i][2]));
  }
}

static GVolumeGrid create_dense_grid(const openvdb::CoordBBox &active_bounds,
                                     const openvdb::Vec3d &voxel_size,
                                     const openvdb::Vec3d &translation)
{
  openvdb::FloatGrid::Ptr grid = openvdb::FloatGrid::create(0.0f);
  grid->denseFill(active_bounds, 1.0f, true);
  grid->transform().postScale(voxel_size);
  grid->transform().postTranslate(translation);
  return GVolumeGrid{std::move(grid)};
}

static GVolumeGrid create_volume_cube_grid(const float3 bounds_min,
                                           const float3 bounds_max,
                                           const int3 resolution)
{
  const float3 voxel_size = (bounds_max - bounds_min) / float3(resolution - 1);
  return create_dense_grid(
      openvdb::CoordBBox({0, 0, 0}, {resolution.x - 1, resolution.y - 1, resolution.z - 1}),
      openvdb::Vec3d(voxel_size.x, voxel_size.y, voxel_size.z),
      openvdb::Vec3d(bounds_min.x, bounds_min.y, bounds_min.z));
}

static Bounds<float3> get_bounds(const Span<float3> vertices)
{
  BLI_assert(!vertices.is_empty());
  Bounds<float3> bounds{vertices.first(), vertices.first()};
  for (const float3 &vertex : vertices.drop_front(1)) {
    bounds.min = math::min(bounds.min, vertex);
    bounds.max = math::max(bounds.max, vertex);
  }
  return bounds;
}

static Vector<float3> get_wireframe_vertices(Volume &volume, const VolumeGridData &grid)
{
  Vector<float3> vertices;
  BKE_volume_grid_wireframe(&volume, &grid, collect_wireframe_vertices, &vertices);
  return vertices;
}

TEST_F(VolumeTest, bounds_use_active_voxels)
{
  Volume *volume = BKE_id_new<Volume>(bmain, nullptr);
  volume->display.wireframe_detail = VOLUME_WIREFRAME_COARSE;

  auto check_case = [&](GVolumeGrid grid, const float3 expected_min, const float3 expected_max) {
    const std::optional<Bounds<float3>> geometry_bounds = BKE_volume_grid_bounds(grid.get());
    ASSERT_TRUE(geometry_bounds.has_value());
    EXPECT_V3_NEAR(geometry_bounds->min, expected_min, 1e-5f);
    EXPECT_V3_NEAR(geometry_bounds->max, expected_max, 1e-5f);

    for (const VolumeWireframeType wireframe_type :
         {VOLUME_WIREFRAME_BOUNDS, VOLUME_WIREFRAME_BOXES})
    {
      volume->display.wireframe_type = wireframe_type;
      const Vector<float3> vertices = get_wireframe_vertices(*volume, grid.get());
      ASSERT_FALSE(vertices.is_empty());
      const Bounds<float3> wireframe_bounds = get_bounds(vertices);
      EXPECT_V3_NEAR(wireframe_bounds.min, expected_min, 1e-5f);
      EXPECT_V3_NEAR(wireframe_bounds.max, expected_max, 1e-5f);
    }
  };

  check_case(
      create_volume_cube_grid(float3(-1.0f), float3(1.0f), int3(2)), float3(-2.0f), float3(2.0f));
  check_case(
      create_volume_cube_grid(float3(2.0f, -3.0f, 5.0f), float3(5.0f, 1.0f, 9.0f), int3(2, 3, 5)),
      float3(0.5f, -4.0f, 4.5f),
      float3(6.5f, 2.0f, 9.5f));
  check_case(create_volume_cube_grid(float3(-1.0f), float3(1.0f), int3(8)),
             float3(-8.0f / 7.0f),
             float3(8.0f / 7.0f));
  check_case(create_volume_cube_grid(float3(-1.0f), float3(1.0f), int3(9)),
             float3(-1.125f),
             float3(1.125f));
  check_case(create_volume_cube_grid(float3(-1.0f), float3(1.0f), int3(1025, 2, 2)),
             float3(-1.0009765625f, -2.0f, -2.0f),
             float3(1.0009765625f, 2.0f, 2.0f));
  check_case(create_dense_grid(openvdb::CoordBBox({-9, -2, 4}, {-2, 3, 6}),
                               openvdb::Vec3d(0.25, 2.0, 1.5),
                               openvdb::Vec3d(10.0, -4.0, 2.0)),
             float3(7.625f, -9.0f, 7.25f),
             float3(9.625f, 3.0f, 11.75f));

  BKE_id_free(bmain, volume);
}

TEST_F(VolumeTest, wireframe_points_use_voxel_box_centers)
{
  Volume *volume = BKE_id_new<Volume>(bmain, nullptr);
  volume->display.wireframe_type = VOLUME_WIREFRAME_POINTS;
  volume->display.wireframe_detail = VOLUME_WIREFRAME_COARSE;
  const GVolumeGrid grid = create_volume_cube_grid(
      float3(2.0f, -3.0f, 5.0f), float3(5.0f, 1.0f, 9.0f), int3(2, 3, 5));

  const Vector<float3> vertices = get_wireframe_vertices(*volume, grid.get());
  ASSERT_EQ(vertices.size(), 1);
  EXPECT_V3_NEAR(vertices.first(), float3(3.5f, -1.0f, 7.0f), 1e-5f);

  BKE_id_free(bmain, volume);
}

TEST_F(VolumeTest, wireframe_bounds_support_nonlinear_transform)
{
  Volume *volume = BKE_id_new<Volume>(bmain, nullptr);
  volume->display.wireframe_type = VOLUME_WIREFRAME_BOUNDS;
  const openvdb::CoordBBox active_bounds({0, 0, 0}, {2, 3, 4});
  openvdb::FloatGrid::Ptr openvdb_grid = openvdb::FloatGrid::create(0.0f);
  openvdb_grid->denseFill(active_bounds, 1.0f, true);
  const openvdb::math::Transform::Ptr transform = openvdb::math::Transform::createFrustumTransform(
      openvdb::BBoxd(openvdb::Vec3d(-10.0), openvdb::Vec3d(10.0)), 0.5, 20.0, 1.0);
  openvdb_grid->setTransform(transform);
  const GVolumeGrid grid{std::move(openvdb_grid)};

  const Vector<float3> vertices = get_wireframe_vertices(*volume, grid.get());
  ASSERT_FALSE(vertices.is_empty());
  const Bounds<float3> actual_bounds = get_bounds(vertices);
  const openvdb::BBoxd expected_bounds = transform->indexToWorld(
      openvdb::BBoxd(active_bounds.min().asVec3d() - openvdb::Vec3d(0.5),
                     active_bounds.max().asVec3d() + openvdb::Vec3d(0.5)));
  EXPECT_V3_NEAR(actual_bounds.min, float3(expected_bounds.min().asPointer()), 1e-5f);
  EXPECT_V3_NEAR(actual_bounds.max, float3(expected_bounds.max().asPointer()), 1e-5f);

  BKE_id_free(bmain, volume);
}

TEST_F(VolumeTest, coarse_wireframe_ignores_inactive_nodes)
{
  Volume *volume = BKE_id_new<Volume>(bmain, nullptr);
  volume->display.wireframe_type = VOLUME_WIREFRAME_BOXES;
  volume->display.wireframe_detail = VOLUME_WIREFRAME_COARSE;
  openvdb::FloatGrid::Ptr openvdb_grid = openvdb::FloatGrid::create(0.0f);
  openvdb_grid->tree().setValueOn(openvdb::Coord(0), 1.0f);
  openvdb_grid->tree().setValueOff(openvdb::Coord(0));
  const GVolumeGrid grid{std::move(openvdb_grid)};

  EXPECT_FALSE(BKE_volume_grid_bounds(grid.get()).has_value());
  EXPECT_TRUE(get_wireframe_vertices(*volume, grid.get()).is_empty());

  BKE_id_free(bmain, volume);
}

TEST_F(VolumeTest, fine_wireframe_keeps_allocated_leaf_bounds)
{
  Volume *volume = BKE_id_new<Volume>(bmain, nullptr);
  volume->display.wireframe_type = VOLUME_WIREFRAME_BOXES;
  volume->display.wireframe_detail = VOLUME_WIREFRAME_FINE;
  const GVolumeGrid grid = create_dense_grid(
      openvdb::CoordBBox(openvdb::Coord(0), openvdb::Coord(0)),
      openvdb::Vec3d(1.0),
      openvdb::Vec3d(0.0));

  const std::optional<Bounds<float3>> geometry_bounds = BKE_volume_grid_bounds(grid.get());
  ASSERT_TRUE(geometry_bounds.has_value());
  EXPECT_V3_NEAR(geometry_bounds->min, float3(-0.5f), 1e-5f);
  EXPECT_V3_NEAR(geometry_bounds->max, float3(0.5f), 1e-5f);

  const Vector<float3> vertices = get_wireframe_vertices(*volume, grid.get());
  ASSERT_FALSE(vertices.is_empty());
  const Bounds<float3> wireframe_bounds = get_bounds(vertices);
  EXPECT_V3_NEAR(wireframe_bounds.min, float3(-0.5f), 1e-5f);
  EXPECT_V3_NEAR(wireframe_bounds.max, float3(7.5f), 1e-5f);

  BKE_id_free(bmain, volume);
}

}  // namespace blender::bke::tests

#endif /* WITH_OPENVDB */
