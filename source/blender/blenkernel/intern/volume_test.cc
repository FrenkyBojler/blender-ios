/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#ifdef WITH_OPENVDB

#  include "testing/testing.h"

#  include "DNA_volume_types.h"

#  include "BKE_idtype.hh"
#  include "BKE_lib_id.hh"
#  include "BKE_main.hh"
#  include "BKE_volume.hh"
#  include "BKE_volume_grid.hh"

namespace blender::bke::tests {

class VolumeTest : public ::testing::Test {
 public:
  Main *bmain;

  static void SetUpTestSuite()
  {
    BKE_idtype_init();
  }

  static void TearDownTestSuite() {}

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

TEST_F(VolumeTest, add_new_grid_with_type)
{
  Volume *volume = BKE_id_new<Volume>(bmain, nullptr);

  EXPECT_TRUE(BKE_volume_grid_add_new(volume, "float_grid", VOLUME_GRID_FLOAT));
  EXPECT_EQ(BKE_volume_num_grids(volume), 1);

  const VolumeGridData *grid = BKE_volume_grid_find(volume, "float_grid");
  EXPECT_NE(grid, nullptr);
  EXPECT_EQ(volume_grid::get_type(*grid), VOLUME_GRID_FLOAT);
  EXPECT_EQ(volume_grid::get_name(*grid), "float_grid");

  EXPECT_TRUE(BKE_volume_grid_add_new(volume, "vector_grid", VOLUME_GRID_VECTOR_FLOAT));
  EXPECT_EQ(BKE_volume_num_grids(volume), 2);

  const VolumeGridData *vector_grid = BKE_volume_grid_find(volume, "vector_grid");
  EXPECT_NE(vector_grid, nullptr);
  EXPECT_EQ(volume_grid::get_type(*vector_grid), VOLUME_GRID_VECTOR_FLOAT);

  EXPECT_EQ(BKE_volume_grid_find(volume, "nonexistent_grid"), nullptr);

  BKE_id_free(bmain, volume);
}

TEST_F(VolumeTest, add_new_grid_with_invalid_input)
{
  Volume *volume = BKE_id_new<Volume>(bmain, nullptr);

  EXPECT_FALSE(BKE_volume_grid_add_new(volume, nullptr, VOLUME_GRID_FLOAT));
  EXPECT_EQ(BKE_volume_num_grids(volume), 0);

  EXPECT_FALSE(BKE_volume_grid_add_new(volume, "", VOLUME_GRID_FLOAT));
  EXPECT_EQ(BKE_volume_num_grids(volume), 0);

  EXPECT_FALSE(BKE_volume_grid_add_new(volume, "points_grid", VOLUME_GRID_POINTS));
  EXPECT_EQ(BKE_volume_num_grids(volume), 0);

  BKE_id_free(bmain, volume);
}

TEST_F(VolumeTest, clear_all_grids)
{
  Volume *volume = BKE_id_new<Volume>(bmain, nullptr);

  EXPECT_TRUE(BKE_volume_grid_add_new(volume, "grid1", VOLUME_GRID_FLOAT));
  EXPECT_TRUE(BKE_volume_grid_add_new(volume, "grid2", VOLUME_GRID_DOUBLE));
  EXPECT_TRUE(BKE_volume_grid_add_new(volume, "grid3", VOLUME_GRID_VECTOR_FLOAT));
  EXPECT_EQ(BKE_volume_num_grids(volume), 3);

  EXPECT_NE(BKE_volume_grid_find(volume, "grid1"), nullptr);
  EXPECT_NE(BKE_volume_grid_find(volume, "grid2"), nullptr);
  EXPECT_NE(BKE_volume_grid_find(volume, "grid3"), nullptr);

  BKE_volume_clear_all_grids(volume);

  EXPECT_EQ(BKE_volume_num_grids(volume), 0);
  EXPECT_EQ(BKE_volume_grid_find(volume, "grid1"), nullptr);
  EXPECT_EQ(BKE_volume_grid_find(volume, "grid2"), nullptr);
  EXPECT_EQ(BKE_volume_grid_find(volume, "grid3"), nullptr);

  BKE_id_free(bmain, volume);
}

TEST_F(VolumeTest, clear_all_grids_empty_volume)
{
  Volume *volume = BKE_id_new<Volume>(bmain, nullptr);

  EXPECT_EQ(BKE_volume_num_grids(volume), 0);

  BKE_volume_clear_all_grids(volume);

  EXPECT_EQ(BKE_volume_num_grids(volume), 0);

  BKE_id_free(bmain, volume);
}

}  // namespace blender::bke::tests

#endif /* WITH_OPENVDB */
