/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "BLI_kdtree.hh"

#include <cmath>

namespace blender {

/* -------------------------------------------------------------------- */
/* Tests */

static void standard_test()
{
  for (int tree_size = 30; tree_size < 500; tree_size++) {
    int tree_index = 0;
    KDTree<float> *tree = kdtree_new<float>(tree_size);
    int mask = tree_size & 31;
    bool occupied[32] = {false};

    for (int i = 0; i < tree_size; i++) {
      int index = i & mask;
      occupied[index] = true;
      float value = fmodf(index * 7.121f, 0.6037f); /* Co-prime. */
      kdtree_insert<float>(tree, tree_index++, value);
    }
    int expected = 0;
    for (int j = 0; j < 32; j++) {
      if (occupied[j]) {
        expected++;
      }
    }

    int dedup_count = kdtree_deduplicate<float>(tree);
    EXPECT_EQ(dedup_count, expected);
    kdtree_free<float>(tree);
  }
}

static void deduplicate_test()
{
  for (int tree_size = 1; tree_size < 40; tree_size++) {
    int tree_index = 0;
    KDTree<float> *tree = kdtree_new<float>(tree_size);
    for (int i = 0; i < tree_size; i++) {
      kdtree_insert<float>(tree, tree_index++, 1.0f);
    }
    int dedup_count = kdtree_deduplicate<float>(tree);
    EXPECT_EQ(dedup_count, 1);
    kdtree_free<float>(tree);
  }
}

static void grid_large_test()
{
  const int grid = static_cast<int>(std::ceil(std::sqrt(detail::kd_balance_parallel_threshold)));
  const int total = grid * grid;
  KDTree<float2> *tree = kdtree_new<float2>(total);

  int tree_index = 0;
  for (int i = 0; i < grid; i++) {
    for (int j = 0; j < grid; j++) {
      float2 key = {float(i), float(j)};
      kdtree_insert<float2>(tree, tree_index++, key);
    }
  }

  int dedup_count = kdtree_deduplicate<float2>(tree);
  EXPECT_EQ(dedup_count, total);

  kdtree_balance<float2>(tree);

  int check_index = 0;
  for (int i = 0; i < grid; i++) {
    for (int j = 0; j < grid; j++) {
      float2 key = {float(i), float(j)};
      KDTreeNearest<float2> nearest;
      const int found = kdtree_find_nearest<float2>(tree, key, &nearest);
      EXPECT_EQ(found, check_index);
      EXPECT_FLOAT_EQ(nearest.co[0], key[0]);
      EXPECT_FLOAT_EQ(nearest.co[1], key[1]);
      check_index++;
    }
  }

  kdtree_free<float2>(tree);
}

TEST(kdtree, Standard)
{
  standard_test();
}

TEST(kdtree, Deduplicate)
{
  deduplicate_test();
}

TEST(kdtree, GridLarge)
{
  grid_large_test();
}

}  // namespace blender
