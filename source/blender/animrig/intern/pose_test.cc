/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_action.hh"
#include "BKE_anim_data.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_object.hh"

#include "ANIM_action.hh"
#include "ANIM_pose.hh"

#include "CLG_log.h"
#include "testing/testing.h"

namespace blender::animrig::tests {

class PoseTest : public testing::Test {
 public:
  Main *bmain;
  Action *pose_data;
  Object *obj_empty;
  Object *obj_armature;
  StripKeyframeData *keyframe_data;

  static void SetUpTestSuite()
  {
    /* BKE_id_free() hits a code path that uses CLOG, which crashes if not initialized properly. */
    CLG_init();

    /* To make id_can_have_animdata() and friends work, the `id_types` array needs to be set up. */
    BKE_idtype_init();
  }

  static void TearDownTestSuite()
  {
    CLG_exit();
  }

  void SetUp() override
  {
    bmain = BKE_main_new();
    pose_data = static_cast<Action *>(BKE_id_new(bmain, ID_AC, "pose_data"));
    Layer &layer = pose_data->layer_add("first_layer");
    Strip &strip = layer.strip_add(*pose_data, Strip::Type::Keyframe);
    keyframe_data = &strip.data<StripKeyframeData>(*pose_data);

    obj_empty = BKE_object_add_only_object(bmain, OB_EMPTY, "obj_empty");
    obj_armature = BKE_object_add_only_object(bmain, OB_ARMATURE, "obj_armature");
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
  }
};

TEST_F(PoseTest, get_best_slot)
{
  Slot &first_slot = pose_data->slot_add();
  Slot &second_slot = pose_data->slot_add_for_id(obj_empty->id);

  EXPECT_EQ(&get_best_pose_slot_for_id(obj_empty->id, *pose_data), &second_slot);
  EXPECT_EQ(&get_best_pose_slot_for_id(obj_armature->id, *pose_data), &first_slot);
}

TEST_F(PoseTest, apply_action_all_bones) {}

TEST_F(PoseTest, apply_action_selected_bones) {}

TEST_F(PoseTest, apply_action_blend) {}

}  // namespace blender::animrig::tests
