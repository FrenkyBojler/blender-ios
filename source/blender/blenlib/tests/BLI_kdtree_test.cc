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
    KDTree_1d *tree = kdtree_1d_new(tree_size);
    int mask = tree_size & 31;
    bool occupied[32] = {false};

    for (int i = 0; i < tree_size; i++) {
      int index = i & mask;
      occupied[index] = true;
      float value = fmodf(index * 7.121f, 0.6037f); /* Co-prime. */
      float key[1] = {value};
      kdtree_1d_insert(tree, tree_index++, key);
    }
    int expected = 0;
    for (int j = 0; j < 32; j++) {
      if (occupied[j]) {
        expected++;
      }
    }

    int dedup_count = kdtree_1d_deduplicate(tree);
    EXPECT_EQ(dedup_count, expected);
    kdtree_1d_free(tree);
  }
}

static void deduplicate_test()
{
  for (int tree_size = 1; tree_size < 40; tree_size++) {
    int tree_index = 0;
    KDTree_1d *tree = kdtree_1d_new(tree_size);
    for (int i = 0; i < tree_size; i++) {
      float key[1] = {1.0f};
      kdtree_1d_insert(tree, tree_index++, key);
    }
    int dedup_count = kdtree_1d_deduplicate(tree);
    EXPECT_EQ(dedup_count, 1);
    kdtree_1d_free(tree);
  }
}

TEST(kdtree, Standard)
{
  standard_test();
}

TEST(kdtree, Deduplicate)
{
  deduplicate_test();
}

TEST(kdtree, Balance2d)
{
  KDTree<float2> *tree = kdtree_new<float2>(7);
  kdtree_insert(tree, 0, float2(-1, -1));
  kdtree_insert(tree, 1, float2(-1, 1));
  kdtree_insert(tree, 2, float2(1, 1));
  kdtree_insert(tree, 3, float2(1, 1));
  kdtree_insert(tree, 4, float2(0, 0));
  kdtree_insert(tree, 5, float2(0.5, 0));
  kdtree_insert(tree, 6, float2(1, 1));

  kdtree_balance(tree);

  EXPECT_EQ(tree->nodes[0].co, float2(-1, -1));
  EXPECT_EQ(tree->nodes[1].co, float2(0, 0));
  EXPECT_EQ(tree->nodes[2].co, float2(-1, 1));
  EXPECT_EQ(tree->nodes[3].co, float2(0.5, 0));
  EXPECT_EQ(tree->nodes[4].co, float2(1, 1));
  EXPECT_EQ(tree->nodes[5].co, float2(1, 1));
  EXPECT_EQ(tree->nodes[6].co, float2(1, 1));

  kdtree_free(tree);
}

}  // namespace blender
