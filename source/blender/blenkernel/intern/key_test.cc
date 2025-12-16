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

  static void SetUpTestSuite()
  {
    BKE_idtype_init();
  }

  void SetUp() override
  {
    bmain = BKE_main_new();
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
  }
};

namespace blender::bke::tests {
TEST_F(ShapekeyTest, evaluation)
{
  Object *ob = BKE_object_add_only_object(bmain, OB_MESH, "Object");
  Mesh *mesh = BKE_mesh_add(bmain, "Test Mesh");
  mesh->verts_num = 4;
  blender::bke::mesh_ensure_required_data_layers(*mesh);
  /* Shapekeys don't affect edges and faces so they can be ignored here. */
  Vector<float3> verts = {
      {0, 0, 0},
      {1, 0, 0},
      {1, 1, 0},
      {0, 1, 0},
  };
  mesh->vert_positions_for_write().copy_from(verts);
  ob->data = mesh;

  Key *base = BKE_key_add(bmain, &ob->id);
  Key *key1 = BKE_key_add(bmain, &ob->id);
  Key *key2 = BKE_key_add(bmain, &ob->id);
}

TEST_F(ShapekeyTest, evaluate_different_element_count)
{
  // BKE_key_evaluate_object_ex
}
}  // namespace blender::bke::tests
