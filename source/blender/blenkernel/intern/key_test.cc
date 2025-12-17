/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_idtype.hh"
#include "BKE_key.hh"
#include "BKE_main.hh"
#include "BKE_mesh.h"
#include "BKE_mesh.hh"
#include "BKE_object.hh"

#include "DNA_object_types.h"

#include "testing/testing.h"

#include "MEM_guardedalloc.h"

class ShapekeyTest : public testing::Test {
 public:
  Main *bmain = nullptr;
  Object *ob = nullptr;
  Mesh *mesh = nullptr;

  static void SetUpTestSuite()
  {
    BKE_idtype_init();
  }

  void SetUp() override
  {
    bmain = BKE_main_new();
    ob = BKE_object_add_only_object(bmain, OB_MESH, "Object");
    mesh = BKE_mesh_add(bmain, "Test Mesh");
    ob->data = mesh;

    mesh->verts_num = 4;
    blender::bke::mesh_ensure_required_data_layers(*mesh);
    /* Shapekeys don't affect edges and faces so they can be ignored here. */
    blender::Vector<blender::float3> verts = {
        {0, 0, 0},
        {1, 0, 0},
        {1, 1, 0},
        {0, 1, 0},
    };
    mesh->vert_positions_for_write().copy_from(verts);
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
  }
};

namespace blender::bke::tests {

TEST_F(ShapekeyTest, mesh_key_creation)
{
  Key *key = BKE_key_add(bmain, &mesh->id);
  ASSERT_EQ(key->from, &mesh->id);
  /* Assignment to the mesh does not happen automatically by adding it. */
  mesh->key = key;
  ASSERT_EQ(BKE_key_from_object(ob), key);
  KeyBlock *base = BKE_keyblock_add(key, "base");
  /* This should be set automatically after adding the first key. */
  ASSERT_EQ(key->refkey, base);
  /* The elemsize stores how many bytes one element has (vertex in this case). */
  ASSERT_EQ(key->elemsize, 12);
  /* Adding the keyblock does not actually allocate any data for it. */
  ASSERT_EQ(base->data, nullptr);
  BKE_keyblock_convert_from_mesh(mesh, key, base);
  ASSERT_NE(base->data, nullptr);
  ASSERT_EQ(base->totelem, 4);
  float3 *data = reinterpret_cast<float3 *>(base->data);
  Array<float3> expected = {
      {0, 0, 0},
      {1, 0, 0},
      {1, 1, 0},
      {0, 1, 0},
  };
  EXPECT_EQ_ARRAY(&data[0], &expected[0], 4);
}

TEST_F(ShapekeyTest, mesh_key_evaluation_relative)
{
  Key *key = BKE_key_add(bmain, &mesh->id);
  mesh->key = key;
  key->type = KEY_RELATIVE;
  KeyBlock *base = BKE_keyblock_add(key, "base");
  BKE_keyblock_convert_from_mesh(mesh, key, base);

  KeyBlock *key1 = BKE_keyblock_add(key, "one");
  BKE_keyblock_convert_from_mesh(mesh, key, key1);
  float3 *key1_data = reinterpret_cast<float3 *>(key1->data);
  key1_data[0] = {1, 1, 1};

  key1->curval = 1.0;
  int totelem = 0;
  float3 *ob_eval = reinterpret_cast<float3 *>(BKE_key_evaluate_object(ob, &totelem));
  ASSERT_NE(ob_eval, nullptr);
  ASSERT_EQ(totelem, mesh->verts_num);
  Array<float3> expected = {
      {1, 1, 1},
      {1, 0, 0},
      {1, 1, 0},
      {0, 1, 0},
  };
  EXPECT_EQ_ARRAY(&ob_eval[0], &expected[0], 4);
  MEM_freeN(ob_eval);

  KeyBlock *key2 = BKE_keyblock_add(key, "two");
  BKE_keyblock_convert_from_mesh(mesh, key, key2);
  float3 *key2_data = reinterpret_cast<float3 *>(key2->data);
  key2_data[0] = {2, 2, 2};
  key2_data[1] = {-1, -1, -1};

  key2->curval = 1.0;
  ob_eval = reinterpret_cast<float3 *>(BKE_key_evaluate_object(ob, &totelem));
  expected = {
      {3, 3, 3},
      {-1, -1, -1},
      {1, 1, 0},
      {0, 1, 0},
  };
  EXPECT_EQ_ARRAY(&ob_eval[0], &expected[0], 4);
  MEM_freeN(ob_eval);

  /* Blend in halfway. */
  key2->curval = 0.5;
  ob_eval = reinterpret_cast<float3 *>(BKE_key_evaluate_object(ob, &totelem));
  expected = {
      {2, 2, 2},
      {0, -0.5, -0.5},
      {1, 1, 0},
      {0, 1, 0},
  };
  EXPECT_EQ_ARRAY(&ob_eval[0], &expected[0], 4);
  MEM_freeN(ob_eval);

  /* Blend in double. */
  key2->curval = 2.0;
  ob_eval = reinterpret_cast<float3 *>(BKE_key_evaluate_object(ob, &totelem));
  expected = {
      {5, 5, 5},
      {-3, -2, -2},
      {1, 1, 0},
      {0, 1, 0},
  };
  EXPECT_EQ_ARRAY(&ob_eval[0], &expected[0], 4);
  MEM_freeN(ob_eval);
}

TEST_F(ShapekeyTest, mesh_key_evaluation_absolute)
{
  Key *key = BKE_key_add(bmain, &mesh->id);
  mesh->key = key;
  key->type = KEY_NORMAL;
  KeyBlock *base = BKE_keyblock_add(key, "base");
  BKE_keyblock_convert_from_mesh(mesh, key, base);

  KeyBlock *key1 = BKE_keyblock_add(key, "one");
  key1->pos = 1.0;
  BKE_keyblock_convert_from_mesh(mesh, key, key1);
  float3 *key1_data = reinterpret_cast<float3 *>(key1->data);
  key1_data[1] = {1, 1, 1};

  KeyBlock *key2 = BKE_keyblock_add(key, "two");
  key2->pos = 2.0;
  BKE_keyblock_convert_from_mesh(mesh, key, key2);
  float3 *key2_data = reinterpret_cast<float3 *>(key2->data);
  key2_data[1] = {0, 0, 0};
  key2_data[2] = {5, 5, 5};

  /* At 0 this should be the base. */
  key->ctime = 0.0;
  int totelem = 0;
  float3 *ob_eval = reinterpret_cast<float3 *>(BKE_key_evaluate_object(ob, &totelem));
  Array<float3> expected = {
      {0, 0, 0},
      {1, 0, 0},
      {1, 1, 0},
      {0, 1, 0},
  };
  EXPECT_EQ_ARRAY(&ob_eval[0], &expected[0], 4);
  MEM_freeN(ob_eval);

  /* At 1 this should be at key1. */
  key->ctime = 100.0;
  ob_eval = reinterpret_cast<float3 *>(BKE_key_evaluate_object(ob, &totelem));
  expected = {
      {0, 0, 0},
      {1, 1, 1},
      {1, 1, 0},
      {0, 1, 0},
  };
  EXPECT_EQ_ARRAY(&ob_eval[0], &expected[0], 4);
  MEM_freeN(ob_eval);

  /* At 2 this should be at key2. */
  key->ctime = 200.0;
  ob_eval = reinterpret_cast<float3 *>(BKE_key_evaluate_object(ob, &totelem));
  expected = {
      {0, 0, 0},
      {0, 0, 0},
      {5, 5, 5},
      {0, 1, 0},
  };
  EXPECT_EQ_ARRAY(&ob_eval[0], &expected[0], 4);
  MEM_freeN(ob_eval);

  /* This should be a linear blend between key1 and key2; */
  key->ctime = 150.0;
  ob_eval = reinterpret_cast<float3 *>(BKE_key_evaluate_object(ob, &totelem));
  expected = {
      {0, 0, 0},
      {0.5, 0.5, 0.5},
      {3, 3, 2.5},
      {0, 1, 0},
  };
  EXPECT_EQ_ARRAY(&ob_eval[0], &expected[0], 4);
  MEM_freeN(ob_eval);
}

/* For historical reasons, keyblocks can end up having an element count that does not match the
 * element count of the source data (unequal vertex count in the case of meshes). This is not
 * supported in relative evaluation though. */
TEST_F(ShapekeyTest, mesh_key_evaluation_relative_uneqal_element_count)
{
  Key *key = BKE_key_add(bmain, &mesh->id);
  mesh->key = key;
  key->type = KEY_NORMAL;
  KeyBlock *base = BKE_keyblock_add(key, "base");
  BKE_keyblock_convert_from_mesh(mesh, key, base);

  KeyBlock *key1 = BKE_keyblock_add(key, "one");
  ASSERT_EQ(mesh->verts_num, 4);
  ASSERT_EQ(key->elemsize, 12);
  /* The mesh has 4 vertices, but this shapekey will only have 3. */
  float3 *key1_data = reinterpret_cast<float3 *>(
      MEM_malloc_arrayN(size_t(3), size_t(key->elemsize), __func__));
  key1->data = key1_data;
  key1->totelem = 3;
  key1_data[1] = {5, 5, 5};

  key1->curval = 1.0;
  int totelem = 0;
  float3 *ob_eval = reinterpret_cast<float3 *>(BKE_key_evaluate_object(ob, &totelem));
  /* Despite having the key at full influence, it should do nothing since the element count does
   * not match. */
  Array<float3> expected = {
      {0, 0, 0},
      {1, 0, 0},
      {1, 1, 0},
      {0, 1, 0},
  };
  EXPECT_EQ_ARRAY(&ob_eval[0], &expected[0], 4);
  MEM_freeN(ob_eval);
}

}  // namespace blender::bke::tests
