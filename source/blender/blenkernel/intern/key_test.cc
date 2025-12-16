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

static void compare_float3(const float3 &a, const float3 &b)
{
  ASSERT_EQ(a[0], b[0]);
  ASSERT_EQ(a[1], b[1]);
  ASSERT_EQ(a[2], b[2]);
}

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
  compare_float3(data[0], {0, 0, 0});
  compare_float3(data[1], {1, 0, 0});
  compare_float3(data[2], {1, 1, 0});
  compare_float3(data[3], {0, 1, 0});
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

  compare_float3(ob_eval[0], {1, 1, 1});
  compare_float3(ob_eval[1], {1, 0, 0});
  compare_float3(ob_eval[2], {1, 1, 0});
  compare_float3(ob_eval[3], {0, 1, 0});

  MEM_freeN(ob_eval);
}

TEST_F(ShapekeyTest, evaluate_different_element_count)
{
  // BKE_key_evaluate_object_ex
}
}  // namespace blender::bke::tests
