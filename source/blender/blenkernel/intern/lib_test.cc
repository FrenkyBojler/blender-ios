/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "testing/testing.h"

#include "CLG_log.h"

#include "GHOST_Path-api.hh"

#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"

#include "RNA_define.hh"

#include "BKE_appdir.hh"
#include "BKE_collection.hh"
#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_idprop.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_remap.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.h"
#include "BKE_node.hh"
#include "BKE_object.hh"
#include "BKE_scene.hh"

#include "IMB_imbuf.hh"

#include "ED_node.hh"

using namespace blender::bke::id;

namespace blender::bke::tests {

class TestData {
 public:
  Main *bmain = nullptr;
  bContext *C = nullptr;

  TestData()
  {
    if (bmain == nullptr) {
      bmain = BKE_main_new();
      G.main = bmain;
    }

    if (C == nullptr) {
      C = CTX_create();
      CTX_data_main_set(C, bmain);
    }
  }

  ~TestData()
  {
    if (bmain != nullptr) {
      BKE_main_free(bmain);
      bmain = nullptr;
      G.main = nullptr;
    }

    if (C != nullptr) {
      CTX_free(C);
      C = nullptr;
    }
  }
};

class LibTest : public ::testing::Test {

 protected:
  static void SetUpTestSuite()
  {
    CLG_init();
    BKE_idtype_init();
    RNA_init();
    bke::node_system_init();
    BKE_appdir_init();
    IMB_init();
    BKE_materials_init();
  }

  static void TearDownTestSuite()
  {
    BKE_materials_exit();
    bke::node_system_exit();
    RNA_exit();
    IMB_exit();
    BKE_appdir_exit();
    GHOST_DisposeSystemPaths();
    CLG_exit();
  }
};

class MaterialTestData : public TestData {
 public:
  Material *material = nullptr;
  bNodeTree *material_nodetree = nullptr;
  MaterialTestData()
  {
    material = BKE_material_add(bmain, "Material");
    ED_node_shader_default(C, &material->id);
    material_nodetree = material->nodetree;
  }

  ~MaterialTestData()
  {
    BKE_id_free(bmain, &material->id);
  }
};

class MeshTestData : public TestData {
 public:
  Mesh *mesh = nullptr;

  MeshTestData()
  {
    mesh = BKE_mesh_add(bmain, nullptr);
  }
};

class TwoMeshesTestData : public MeshTestData {
 public:
  Mesh *other_mesh = nullptr;

  TwoMeshesTestData()
  {
    other_mesh = BKE_mesh_add(bmain, nullptr);
  }
};

class MeshObjectTestData : public MeshTestData {
 public:
  Object *object;
  MeshObjectTestData()
  {
    object = BKE_object_add_only_object(bmain, OB_MESH, nullptr);
    object->data = mesh;
  }
};

/* -------------------------------------------------------------------- */
/** \name Embedded IDs
 * \{ */

TEST_F(LibTest, embedded_ids_can_not_be_remapped)
{
  MaterialTestData context;
  bNodeTree *other_tree = static_cast<bNodeTree *>(BKE_id_new_nomain(ID_NT, nullptr));

  ASSERT_NE(context.material, nullptr);
  ASSERT_EQ(context.material_nodetree, context.material->nodetree);

  BKE_libblock_remap(context.bmain, context.material_nodetree, other_tree, 0);

  EXPECT_EQ(context.material_nodetree, context.material->nodetree);
  EXPECT_NE(context.material->nodetree, other_tree);

  BKE_id_free(nullptr, other_tree);
}

TEST_F(LibTest, embedded_ids_can_not_be_deleted)
{
  MaterialTestData context;

  ASSERT_NE(context.material_nodetree, nullptr);
  ASSERT_EQ(context.material_nodetree, context.material->nodetree);

  BKE_libblock_remap(
      context.bmain, context.material_nodetree, nullptr, ID_REMAP_SKIP_NEVER_NULL_USAGE);

  EXPECT_EQ(context.material_nodetree, context.material->nodetree);
  EXPECT_NE(context.material->nodetree, nullptr);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Remap to self
 * \{ */

TEST_F(LibTest, delete_when_remap_to_self_not_allowed)
{
  TwoMeshesTestData context;

  ASSERT_NE(context.mesh, nullptr);
  ASSERT_NE(context.other_mesh, nullptr);
  context.mesh->texcomesh = context.other_mesh;

  BKE_libblock_remap(context.bmain, context.other_mesh, context.mesh, 0);

  EXPECT_EQ(context.mesh->texcomesh, nullptr);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name User Reference Counting
 * \{ */

TEST_F(LibTest, users_are_decreased_when_not_skipping_never_null)
{
  MeshObjectTestData context;

  ASSERT_NE(context.object, nullptr);
  ASSERT_EQ(context.object->data, context.mesh);
  ASSERT_EQ(context.object->id.tag & ID_TAG_DOIT, 0);
  ASSERT_EQ(context.mesh->id.us, 1);

  /* This is an invalid situation, test case tests this in between value until we have a better
   * solution. */
  BKE_libblock_remap(context.bmain, context.mesh, nullptr, 0);
  EXPECT_EQ(context.mesh->id.us, 0);
  EXPECT_EQ(context.object->data, context.mesh);
  EXPECT_NE(context.object->data, nullptr);
  EXPECT_EQ(context.object->id.tag & ID_TAG_DOIT, 0);
}

TEST_F(LibTest, users_are_same_when_skipping_never_null)
{
  MeshObjectTestData context;

  ASSERT_NE(context.object, nullptr);
  ASSERT_EQ(context.object->data, context.mesh);
  ASSERT_EQ(context.object->id.tag & ID_TAG_DOIT, 0);
  ASSERT_EQ(context.mesh->id.us, 1);

  BKE_libblock_remap(context.bmain, context.mesh, nullptr, ID_REMAP_SKIP_NEVER_NULL_USAGE);
  EXPECT_EQ(context.mesh->id.us, 1);
  EXPECT_EQ(context.object->data, context.mesh);
  EXPECT_NE(context.object->data, nullptr);
  EXPECT_EQ(context.object->id.tag & ID_TAG_DOIT, 0);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Never Null
 * \{ */

TEST_F(LibTest, do_not_delete_when_cannot_unset)
{
  MeshObjectTestData context;

  ASSERT_NE(context.object, nullptr);
  ASSERT_EQ(context.object->data, context.mesh);

  BKE_libblock_remap(context.bmain, context.mesh, nullptr, ID_REMAP_SKIP_NEVER_NULL_USAGE);
  EXPECT_EQ(context.object->data, context.mesh);
  EXPECT_NE(context.object->data, nullptr);
}

TEST_F(LibTest, force_never_null_usage)
{
  MeshObjectTestData context;

  ASSERT_NE(context.object, nullptr);
  ASSERT_EQ(context.object->data, context.mesh);

  BKE_libblock_remap(context.bmain, context.mesh, nullptr, ID_REMAP_FORCE_NEVER_NULL_USAGE);
  EXPECT_EQ(context.object->data, nullptr);
}

TEST_F(LibTest, never_null_usage_flag_not_requested_on_delete)
{
  MeshObjectTestData context;

  ASSERT_NE(context.object, nullptr);
  ASSERT_EQ(context.object->data, context.mesh);
  ASSERT_EQ(context.object->id.tag & ID_TAG_DOIT, 0);

  /* Never null usage isn't requested so the flag should not be set. */
  BKE_libblock_remap(context.bmain, context.mesh, nullptr, ID_REMAP_SKIP_NEVER_NULL_USAGE);
  EXPECT_EQ(context.object->data, context.mesh);
  EXPECT_NE(context.object->data, nullptr);
  EXPECT_EQ(context.object->id.tag & ID_TAG_DOIT, 0);
}

TEST_F(LibTest, never_null_usage_storage_requested_on_delete)
{
  MeshObjectTestData context;

  ASSERT_NE(context.object, nullptr);
  ASSERT_EQ(context.object->data, context.mesh);
  ASSERT_EQ(context.object->id.tag & ID_TAG_DOIT, 0);

  /* Never null usage is requested so the owner ID (the Object) should be added to the set. */
  IDRemapper remapper;
  remapper.add(&context.mesh->id, nullptr);
  BKE_libblock_remap_multiple_locked(
      context.bmain, remapper, (ID_REMAP_SKIP_NEVER_NULL_USAGE | ID_REMAP_STORE_NEVER_NULL_USAGE));

  /* Never null usages un-assignment is not enforced (no #ID_REMAP_FORCE_NEVER_NULL_USAGE),
   * so the object-data should still use the original mesh. */
  EXPECT_EQ(context.object->data, context.mesh);
  EXPECT_NE(context.object->data, nullptr);
  EXPECT_TRUE(remapper.never_null_users().contains(&context.object->id));
}

TEST_F(LibTest, never_null_usage_flag_not_requested_on_remap)
{
  MeshObjectTestData context;
  Mesh *other_mesh = BKE_mesh_add(context.bmain, nullptr);

  ASSERT_NE(context.object, nullptr);
  ASSERT_EQ(context.object->data, context.mesh);
  ASSERT_EQ(context.object->id.tag & ID_TAG_DOIT, 0);

  /* Never null usage isn't requested so the flag should not be set. */
  BKE_libblock_remap(context.bmain, context.mesh, other_mesh, ID_REMAP_SKIP_NEVER_NULL_USAGE);
  EXPECT_EQ(context.object->data, other_mesh);
  EXPECT_EQ(context.object->id.tag & ID_TAG_DOIT, 0);
}

TEST_F(LibTest, never_null_usage_storage_requested_on_remap)
{
  MeshObjectTestData context;
  Mesh *other_mesh = BKE_mesh_add(context.bmain, nullptr);

  ASSERT_NE(context.object, nullptr);
  ASSERT_EQ(context.object->data, context.mesh);
  ASSERT_EQ(context.object->id.tag & ID_TAG_DOIT, 0);

  /* Never null usage is requested, but the obdata is remapped to another Mesh, not to `nullptr`,
   * so the `never_null_users` set should remain empty. */
  IDRemapper remapper;
  remapper.add(&context.mesh->id, &other_mesh->id);
  BKE_libblock_remap_multiple_locked(
      context.bmain, remapper, (ID_REMAP_SKIP_NEVER_NULL_USAGE | ID_REMAP_STORE_NEVER_NULL_USAGE));
  EXPECT_EQ(context.object->data, other_mesh);
  EXPECT_TRUE(remapper.never_null_users().is_empty());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Query Tests
 * \{ */

class WholeIDTestData : public TestData {
 public:
  Scene *scene = nullptr;
  Object *object = nullptr;
  Object *target = nullptr;
  Mesh *mesh = nullptr;
  Material *material = nullptr;

  WholeIDTestData()
  {
    this->scene = BKE_scene_add(this->bmain, "IDLibQueryScene");
    CTX_data_scene_set(this->C, this->scene);

    this->object = BKE_object_add_only_object(this->bmain, OB_MESH, "IDLibQueryObject");
    this->target = BKE_object_add_only_object(this->bmain, OB_EMPTY, "IDLibQueryTarget");

    this->mesh = BKE_mesh_add(this->bmain, "IDLibQueryMesh");
    this->object->data = this->mesh;

    BKE_collection_object_add(this->bmain, this->scene->master_collection, this->object);
    BKE_collection_object_add(this->bmain, this->scene->master_collection, this->target);
  }
};

class IDSubDataTestData : public WholeIDTestData {
 public:
  bNode *node = nullptr;

  IDSubDataTestData()
  {
    /* Add a material that contains an embedded nodetree and assign a custom property to one of
     * its nodes. */
    this->material = BKE_material_add(this->bmain, "Material");
    ED_node_shader_default(C, &this->material->id);

    BKE_object_material_assign(
        this->bmain, this->object, this->material, this->object->actcol, BKE_MAT_ASSIGN_OBJECT);

    this->node = static_cast<bNode *>(this->material->nodetree->nodes.first);

    this->node->prop = bke::idprop::create_group("Node Custom Properties").release();
    IDP_AddToGroup(this->node->prop,
                   bke::idprop::create("ID Pointer", &this->target->id).release());
  }
  ~IDSubDataTestData()
  {
    BKE_id_free(bmain, &material->id);
  }
};

TEST_F(LibTest, libquery_basic)
{
  WholeIDTestData context;

  ASSERT_NE(context.scene, nullptr);
  ASSERT_NE(context.object, nullptr);
  ASSERT_NE(context.target, nullptr);
  ASSERT_NE(context.mesh, nullptr);

  /* Reset all ID user-count to 0. */
  ID *id_iter;
  FOREACH_MAIN_ID_BEGIN (context.bmain, id_iter) {
    id_iter->us = 0;
  }
  FOREACH_MAIN_ID_END;

  /* Set an invalid user-count value to IDs directly used by the scene.
   * This includes these used by its embedded IDs, like the master collection, and the scene
   itself
   * (through the loop-back pointers of embedded IDs to their owner). */
  auto set_count = [](LibraryIDLinkCallbackData *cb_data) -> int {
    if (*(cb_data->id_pointer)) {
      (*(cb_data->id_pointer))->us = 42;
    }
    return IDWALK_RET_NOP;
  };
  BKE_library_foreach_ID_link(
      context.bmain, &context.scene->id, set_count, nullptr, IDWALK_READONLY);
  EXPECT_EQ(context.scene->id.us, 42);
  EXPECT_EQ(context.object->id.us, 42);
  EXPECT_EQ(context.target->id.us, 42);
  EXPECT_EQ(context.mesh->id.us, 0);

  /* Clear object's obdata mesh pointer. */
  auto clear_mesh_pointer = [](LibraryIDLinkCallbackData *cb_data) -> int {
    WholeIDTestData *test_data = static_cast<WholeIDTestData *>(cb_data->user_data);
    if (*(cb_data->id_pointer) == &test_data->mesh->id) {
      *(cb_data->id_pointer) = nullptr;
    }
    return IDWALK_RET_NOP;
  };
  BKE_library_foreach_ID_link(
      context.bmain, &context.object->id, clear_mesh_pointer, &context, IDWALK_NOP);
  EXPECT_EQ(context.object->data, nullptr);

#if 0 /* Does not work. */
  /* Modifying data when IDWALK_READONLY is set is forbidden. */
  context.object->data = context.mesh;
  EXPECT_BLI_ASSERT(BKE_library_foreach_ID_link(context.bmain,
                                                &context.scene->id,
                                                clear_mesh_pointer,
                                                &context.test_data,
                                                IDWALK_READONLY),
                    "");
#endif
}

TEST_F(LibTest, libquery_recursive)
{
  IDSubDataTestData context;

  EXPECT_NE(context.scene, nullptr);
  EXPECT_NE(context.object, nullptr);
  EXPECT_NE(context.target, nullptr);
  EXPECT_NE(context.mesh, nullptr);

  /* Reset all ID user-count to 0. */
  ID *id_iter;
  FOREACH_MAIN_ID_BEGIN (context.bmain, id_iter) {
    id_iter->us = 0;
  }
  FOREACH_MAIN_ID_END;

  /* Set an invalid user-count value to all IDs used by the scene, recursively.
   * Here, it should mean all IDs in Main, including the scene itself
   * (because of the loop-back pointer from the embedded master collection to its scene owner). */
  auto set_count = [](LibraryIDLinkCallbackData *cb_data) -> int {
    if (*(cb_data->id_pointer)) {
      (*(cb_data->id_pointer))->us = 42;
    }
    return IDWALK_RET_NOP;
  };
  BKE_library_foreach_ID_link(
      context.bmain, &context.scene->id, set_count, nullptr, IDWALK_RECURSE);
  FOREACH_MAIN_ID_BEGIN (context.bmain, id_iter) {
    EXPECT_EQ(id_iter->us, 42);
  }
  FOREACH_MAIN_ID_END;

  /* Reset all ID user-count to 0. */
  FOREACH_MAIN_ID_BEGIN (context.bmain, id_iter) {
    id_iter->us = 0;
  }
  FOREACH_MAIN_ID_END;

  /* Recompute valid user counts for all IDs used by the scene, recursively. */
  auto compute_count = [](LibraryIDLinkCallbackData *cb_data) -> int {
    if (*(cb_data->id_pointer) && (cb_data->cb_flag & IDWALK_CB_USER) != 0) {
      (*(cb_data->id_pointer))->us++;
    }
    return IDWALK_RET_NOP;
  };
  BKE_library_foreach_ID_link(
      context.bmain, &context.scene->id, compute_count, nullptr, IDWALK_RECURSE);
  EXPECT_EQ(context.scene->id.us, 0);
  EXPECT_EQ(context.object->id.us, 1);
  /* Scene's master collection, and scene's compositor node IDProperty. Note that object constraint
   * is _not_ a reference-counting usage. */
  EXPECT_EQ(context.target->id.us, 2);
  EXPECT_EQ(context.mesh->id.us, 1);
}

TEST_F(LibTest, libquery_subdata)
{
  IDSubDataTestData context;

  ASSERT_NE(context.scene, nullptr);
  ASSERT_NE(context.object, nullptr);
  ASSERT_NE(context.target, nullptr);
  ASSERT_NE(context.mesh, nullptr);
  ASSERT_NE(context.material, nullptr);

  /* Reset all ID user-count to 0. */
  ID *id_iter;
  FOREACH_MAIN_ID_BEGIN (context.bmain, id_iter) {
    id_iter->us = 0;
  }
  FOREACH_MAIN_ID_END;

  /* Set an invalid user-count value to all IDs used by one of the material's nodes. */
  auto set_count = [](LibraryIDLinkCallbackData *cb_data) -> int {
    if (*(cb_data->id_pointer)) {
      (*(cb_data->id_pointer))->us = 42;
    }
    return IDWALK_RET_NOP;
  };
  auto node_foreach_id = [&context](LibraryForeachIDData *data) {
    bke::node_node_foreach_id(context.node, data);
  };

  BKE_library_foreach_subdata_id(context.bmain,
                                 &context.material->id,
                                 &context.material->nodetree->id,
                                 node_foreach_id,
                                 set_count,
                                 nullptr,
                                 IDWALK_NOP);

  EXPECT_EQ(context.scene->id.us, 0);
  EXPECT_EQ(context.object->id.us, 0);
  /* The material's nodetre input node IDProperty uses the target object. */
  EXPECT_EQ(context.target->id.us, 42);
  EXPECT_EQ(context.mesh->id.us, 0);
}

/** \} */

}  // namespace blender::bke::tests
