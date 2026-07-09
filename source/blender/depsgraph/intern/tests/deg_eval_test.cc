/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gtest/gtest.h"

#include "BKE_appdir.hh"
#include "BKE_context.hh"
#include "BKE_global.hh"
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

#include "testing/testing.h"

namespace blender::deg::tests {

struct FrameSample {
  int frame = 0;
  float matrix_world[4][4];
};

// Sample world matrices for a set of objects across [frame_start, frame_end]
// inclusive, evaluating sequentially. Returns one vector of samples per
// object, indexed the same order as `objects`.
Array<Vector<FrameSample>> sample_motion(const Span<Object *> &objects,
                                         Depsgraph *depsgraph,
                                         const int frame_start,
                                         const int frame_end)
{
  Array<Vector<FrameSample>> results(objects.size());
  for (Vector<FrameSample> &v : results) {
    v.reserve(frame_end - frame_start + 1);
  }

  for (int frame = frame_start; frame <= frame_end; ++frame) {
    DEG_evaluate_on_framechange(depsgraph, frame);

    for (size_t i = 0; i < objects.size(); ++i) {
      Object *eval_ob = DEG_get_evaluated(depsgraph, objects[i]);
      BLI_assert_msg(eval_ob != nullptr, "Object not found in evaluated depsgraph");
      FrameSample sample;
      sample.frame = frame;
      memcpy(sample.matrix_world, eval_ob->object_to_world().ptr(), sizeof(float[4][4]));
      results[i].append(sample);
    }
  }
  return results;
}

class DepsgraphTest : public bke::BlenderGTestBase {

 public:
  Main *bmain_ = nullptr;
  Scene *scene_ = nullptr;
  ViewLayer *view_layer_ = nullptr;
  Depsgraph *depsgraph_ = nullptr;

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
    DEG_id_tag_update(&ob->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
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
  Object *ob = add_mesh_object("StaticCube");
  DEG_graph_build_from_view_layer(depsgraph_);
  DEG_graph_relations_update(depsgraph_);

  evaluate_at_frame(0);
  Object *prev_eval_ob = DEG_get_evaluated(depsgraph_, ob);
  ID *prev_eval_mesh = DEG_get_evaluated(depsgraph_, ob->data);

  for (int frame = 1; frame < 4; frame++) {
    evaluate_at_frame(frame);

    Object *eval_ob = DEG_get_evaluated(depsgraph_, ob);
    ASSERT_NE(eval_ob, nullptr);
    ASSERT_NE(eval_ob, ob);
    ASSERT_EQ(eval_ob, prev_eval_ob)
        << "Just re-evaluating the depsgraph should not create a new eval copy.";
    prev_eval_ob = eval_ob;

    ID *eval_mesh = DEG_get_evaluated(depsgraph_, ob->data);
    ASSERT_NE(eval_mesh, nullptr);
    ASSERT_EQ(eval_mesh, prev_eval_mesh) << "The mesh should also not be re-copied.";
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
TEST_F(DepsgraphTest, evaluate_objects_after_changes_in_main) {}

/*  */
TEST_F(DepsgraphTest, evaluate_animated_object) {}

}  // namespace blender::deg::tests
