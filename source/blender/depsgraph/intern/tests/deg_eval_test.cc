/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gtest/gtest.h"

#include "BKE_action.hh"
#include "BKE_fcurve.hh"
#include "BKE_gtest_base.hh"
#include "BKE_idtype.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_scene.hh"

#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "ANIM_action.hh"
#include "ANIM_fcurve.hh"
#include "ANIM_keyframing.hh"

#include "testing/testing.h"

#include "intern/depsgraph.hh"
#include "intern/node/deg_node.hh"
#include "intern/node/deg_node_operation.hh"

namespace blender::deg::tests {

/* Returns true if any node of type NodeType::COPY_ON_EVAL is tagged for evaluation. */
static bool has_to_read_from_main(blender::Depsgraph *depsgraph)
{
  Depsgraph *dg = reinterpret_cast<Depsgraph *>(depsgraph);
  for (OperationNode *node : dg->operations) {
    /* Assuming that only COPY_ON_EVAL nodes read from Main. */
    if (node->opcode != OperationCode::COPY_ON_EVAL) {
      continue;
    }
    if (node->flag & DEPSOP_FLAG_NEEDS_UPDATE) {
      return true;
    }
  }
  return false;
}

class DepsgraphTest : public bke::BlenderGTestBase {

 public:
  Main *bmain_ = nullptr;
  Scene *scene_ = nullptr;
  ViewLayer *view_layer_ = nullptr;
  blender::Depsgraph *depsgraph_ = nullptr;

  void SetUp() override
  {
    bmain_ = BKE_main_new();
    G_MAIN = bmain_;

    scene_ = BKE_scene_add(bmain_, "DEG_TEST_Scene");
    view_layer_ = BKE_view_layer_default_view(scene_);

    depsgraph_ = DEG_graph_new(bmain_, scene_, view_layer_, DAG_EVAL_VIEWPORT);
  }

  void TearDown() override
  {
    if (depsgraph_) {
      DEG_graph_free(depsgraph_);
      depsgraph_ = nullptr;
    }
    if (bmain_) {
      BKE_main_free(bmain_);
      bmain_ = nullptr;
    }
    G_MAIN = nullptr;
  }

  /* Adds mesh object and tags the depsgraph for an update. */
  Object *add_mesh_object(const char *name)
  {
    Object *ob = BKE_object_add_only_object(bmain_, OB_MESH, name);
    Mesh *cube_mesh = BKE_mesh_add(bmain_, "cube_mesh");
    BKE_mesh_assign_object(bmain_, ob, cube_mesh);
    BKE_collection_object_add(bmain_, scene_->master_collection, ob);
    return ob;
  }

  /* Evaluates the depsgraph at the given frame. Convenience function. */
  void evaluate_at_frame(int frame)
  {
    DEG_evaluate_on_framechange(depsgraph_, frame);
  }
};

/* Basic sanity: graph builds and evaluates without objects. */
TEST_F(DepsgraphTest, build_empty_scene_graph)
{
  DEG_graph_build_from_view_layer(depsgraph_);
  DEG_graph_relations_update(depsgraph_);

  EXPECT_NO_FATAL_FAILURE(evaluate_at_frame(1));
}

TEST_F(DepsgraphTest, evaluate_static_object)
{
  Object *ob = add_mesh_object("static_cube");
  DEG_graph_build_from_view_layer(depsgraph_);
  DEG_graph_relations_update(depsgraph_);

  evaluate_at_frame(0);
  Object *prev_eval_ob = DEG_get_evaluated(depsgraph_, ob);
  ID *prev_eval_mesh = DEG_get_evaluated(depsgraph_, ob->data);

  for (int frame = 1; frame < 4; frame++) {
    evaluate_at_frame(frame);
    EXPECT_TRUE(DEG_id_is_fully_evaluated(depsgraph_, &ob->id));
    Object *eval_ob = DEG_get_evaluated(depsgraph_, ob);
    ASSERT_NE(eval_ob, nullptr);
    EXPECT_NE(eval_ob, ob);
    EXPECT_EQ(eval_ob, prev_eval_ob)
        << "Just re-evaluating the depsgraph should not create a new eval copy.";
    prev_eval_ob = eval_ob;

    EXPECT_TRUE(DEG_id_is_fully_evaluated(depsgraph_, ob->data));
    ID *eval_mesh = DEG_get_evaluated(depsgraph_, ob->data);
    ASSERT_NE(eval_mesh, nullptr);
    EXPECT_EQ(eval_mesh, prev_eval_mesh) << "The mesh should also not be re-copied.";
    prev_eval_mesh = eval_mesh;

    /* No animation, so evaluated location should match original. */
    for (int i = 0; i < 3; ++i) {
      EXPECT_FLOAT_EQ(eval_ob->object_to_world().location()[i],
                      ob->object_to_world().location()[i]);
    }
  }
}

/* When an object is transformed, the copy on eval data has to be updated, meaning the depsgraph
 * reads from main. */
TEST_F(DepsgraphTest, evaluate_objects_after_transforms)
{
  Object *ob = add_mesh_object("static_cube");
  DEG_graph_build_from_view_layer(depsgraph_);
  DEG_graph_relations_update(depsgraph_);

  evaluate_at_frame(0);
  Object *eval_ob = DEG_get_evaluated(depsgraph_, ob);

  ob->loc[0] = 1;
  /* To get the depsgraph to anything we need to tag the ID for evaluation, only then is the data
   * copied from Main. */
  DEG_id_tag_update(&ob->id, ID_RECALC_TRANSFORM);
  EXPECT_FALSE(DEG_id_is_fully_evaluated(depsgraph_, &ob->id));
  EXPECT_GT(abs(ob->loc[0] - eval_ob->loc[0]), 0.0001)
      << "Modifying data in Main should not affect the depsgraph automatically.";
  EXPECT_TRUE(has_to_read_from_main(depsgraph_));

  evaluate_at_frame(0);
  EXPECT_FALSE(has_to_read_from_main(depsgraph_));

  EXPECT_TRUE(DEG_id_is_fully_evaluated(depsgraph_, &ob->id));
  EXPECT_EQ(eval_ob, DEG_get_evaluated(depsgraph_, ob))
      << "Even though the object was recalculated, the pointer is the same because the depsgraph "
         "copies into existing memory where possible.";
  EXPECT_FLOAT_EQ(eval_ob->loc[0], ob->loc[0])
      << "The location value should have been copied from Main.";
}

/* When the object is animated, a frame change does not cause a read from Main
 * because the motion comes from evaluating the action. */
TEST_F(DepsgraphTest, evaluate_animated_object)
{
  Object *ob = add_mesh_object("animated_cube");
  bAction *action = BKE_action_add(bmain_, "test_action");
  const bool success = animrig::assign_action(action, ob->id);
  EXPECT_TRUE(success);
  animrig::Channelbag &channelbag = animrig::action_channelbag_ensure(*action, ob->id);
  FCurve *fcu = channelbag.fcurve_create_unique(bmain_, {"location", 0});
  animrig::insert_vert_fcurve(fcu, {0, 0}, {}, INSERTKEY_NOFLAGS);
  animrig::insert_vert_fcurve(fcu, {1, 1}, {}, INSERTKEY_NOFLAGS);

  DEG_graph_build_from_view_layer(depsgraph_);
  DEG_graph_relations_update(depsgraph_);

  evaluate_at_frame(0);
  Object *eval_ob = DEG_get_evaluated(depsgraph_, ob);
  EXPECT_FLOAT_EQ(eval_ob->loc[0], 0);

  Depsgraph *dg = reinterpret_cast<Depsgraph *>(depsgraph_);
  /* This is what DEG_evaluate_on_framechange calls internally. */
  dg->tag_time_source();
  /* Changing the time should not trigger a read from Main. */
  EXPECT_FALSE(has_to_read_from_main(depsgraph_));

  evaluate_at_frame(1);
  /* The evaluation updates the evaluated object in place. */
  EXPECT_FLOAT_EQ(eval_ob->loc[0], 1);
}

}  // namespace blender::deg::tests
