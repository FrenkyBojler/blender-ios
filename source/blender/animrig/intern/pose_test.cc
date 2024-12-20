/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_string.h"

#include "BKE_action.hh"
#include "BKE_anim_data.hh"
#include "BKE_animsys.h"
#include "BKE_armature.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_object.hh"

#include "DEG_depsgraph.hh"

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
  Object *obj_armature_a;
  Object *obj_armature_b;
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
    obj_armature_a = BKE_object_add_only_object(bmain, OB_ARMATURE, "obj_armature_a");
    obj_armature_b = BKE_object_add_only_object(bmain, OB_ARMATURE, "obj_armature_b");

    bArmature *armature = BKE_armature_add(bmain, "ArmatureA");
    obj_armature_a->data = armature;

    Bone *bone = static_cast<Bone *>(MEM_mallocN(sizeof(Bone), "BONE"));
    memset(bone, 0, sizeof(Bone));
    STRNCPY(bone->name, "BoneA");
    BLI_addtail(&armature->bonebase, bone);

    bone = static_cast<Bone *>(MEM_mallocN(sizeof(Bone), "BONE"));
    memset(bone, 0, sizeof(Bone));
    STRNCPY(bone->name, "BoneB");
    BLI_addtail(&armature->bonebase, bone);

    BKE_pose_ensure(bmain, obj_armature_a, armature, false);

    armature = BKE_armature_add(bmain, "ArmatureB");
    obj_armature_b->data = armature;

    bone = static_cast<Bone *>(MEM_mallocN(sizeof(Bone), "BONE"));
    memset(bone, 0, sizeof(Bone));
    STRNCPY(bone->name, "BoneA");
    BLI_addtail(&armature->bonebase, bone);

    bone = static_cast<Bone *>(MEM_mallocN(sizeof(Bone), "BONE"));
    memset(bone, 0, sizeof(Bone));
    STRNCPY(bone->name, "BoneB");
    BLI_addtail(&armature->bonebase, bone);

    BKE_pose_ensure(bmain, obj_armature_b, armature, false);
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
  EXPECT_EQ(&get_best_pose_slot_for_id(obj_armature_a->id, *pose_data), &first_slot);
}

TEST_F(PoseTest, apply_action_object)
{
  /* Since pose bones live on the object, the code is already set up to handle objects
   * transforms, even though the name suggests it only applies to bones. */
  Slot &first_slot = pose_data->slot_add();
  EXPECT_EQ(obj_empty->loc[0], 0.0f);
  keyframe_data->keyframe_insert(
      bmain, first_slot, {"location", 0}, {1, 10}, {BEZT_KEYTYPE_KEYFRAME, HD_AUTO, BEZT_IPO_BEZ});
  AnimationEvalContext eval_context = {nullptr, 1.0f};
  blender::animrig::pose_apply_action_all_bones(
      obj_empty, pose_data, first_slot.handle, &eval_context);
  EXPECT_EQ(obj_empty->loc[0], 10.0f);
}

TEST_F(PoseTest, apply_action_all_bones_single_armature)
{
  Slot &first_slot = pose_data->slot_add();
  keyframe_data->keyframe_insert(bmain,
                                 first_slot,
                                 {"pose.bones[\"BoneA\"].location", 0},
                                 {1, 10},
                                 {BEZT_KEYTYPE_KEYFRAME, HD_AUTO, BEZT_IPO_BEZ});
  keyframe_data->keyframe_insert(bmain,
                                 first_slot,
                                 {"pose.bones[\"BoneB\"].location", 1},
                                 {1, 5},
                                 {BEZT_KEYTYPE_KEYFRAME, HD_AUTO, BEZT_IPO_BEZ});

  bPoseChannel *bone_a = BKE_pose_channel_find_name(obj_armature_a->pose, "BoneA");
  bPoseChannel *bone_b = BKE_pose_channel_find_name(obj_armature_a->pose, "BoneB");

  bone_a->loc[1] = 1.0;
  bone_a->loc[2] = 2.0;

  AnimationEvalContext eval_context = {nullptr, 1.0f};
  blender::animrig::pose_apply_action_all_bones(
      obj_armature_a, pose_data, first_slot.handle, &eval_context);
  EXPECT_EQ(bone_a->loc[0], 10.0);
  EXPECT_EQ(bone_b->loc[1], 5.0);

  EXPECT_EQ(bone_a->loc[1], 1.0)
      << "Properties not stored in the pose are expected to not be modified.";
  EXPECT_EQ(bone_a->loc[2], 2.0);
}

TEST_F(PoseTest, apply_action_selected_bones_single_armature)
{
  Slot &first_slot = pose_data->slot_add();
  keyframe_data->keyframe_insert(bmain,
                                 first_slot,
                                 {"pose.bones[\"BoneA\"].location", 0},
                                 {1, 10},
                                 {BEZT_KEYTYPE_KEYFRAME, HD_AUTO, BEZT_IPO_BEZ});
  keyframe_data->keyframe_insert(bmain,
                                 first_slot,
                                 {"pose.bones[\"BoneB\"].location", 1},
                                 {1, 5},
                                 {BEZT_KEYTYPE_KEYFRAME, HD_AUTO, BEZT_IPO_BEZ});

  bPoseChannel *bone_a = BKE_pose_channel_find_name(obj_armature_a->pose, "BoneA");
  bPoseChannel *bone_b = BKE_pose_channel_find_name(obj_armature_a->pose, "BoneB");

  bone_a->loc[1] = 1.0;
  bone_a->loc[2] = 2.0;
  bone_b->loc[1] = 0.0;

  /* The algorithm code uses the bone flag instead of the pose bone flag. */
  bone_a->bone->flag |= BONE_SELECTED;
  bone_b->bone->flag &= ~BONE_SELECTED;

  AnimationEvalContext eval_context = {nullptr, 1.0f};
  blender::animrig::pose_apply_action_selected_bones(
      obj_armature_a, pose_data, first_slot.handle, &eval_context);

  EXPECT_EQ(bone_a->loc[0], 10.0);
  EXPECT_EQ(bone_b->loc[1], 0.0) << "Unselected bones should not be affected.";

  EXPECT_EQ(bone_a->loc[1], 1.0)
      << "Properties not stored in the pose are expected to not be modified.";
  EXPECT_EQ(bone_a->loc[2], 2.0);
}

TEST_F(PoseTest, apply_action_blend_single_armature)
{
  Slot &first_slot = pose_data->slot_add();
  keyframe_data->keyframe_insert(bmain,
                                 first_slot,
                                 {"pose.bones[\"BoneA\"].location", 0},
                                 {1, 10},
                                 {BEZT_KEYTYPE_KEYFRAME, HD_AUTO, BEZT_IPO_BEZ});
  keyframe_data->keyframe_insert(bmain,
                                 first_slot,
                                 {"pose.bones[\"BoneB\"].location", 1},
                                 {1, 5},
                                 {BEZT_KEYTYPE_KEYFRAME, HD_AUTO, BEZT_IPO_BEZ});

  bPoseChannel *bone_a = BKE_pose_channel_find_name(obj_armature_a->pose, "BoneA");
  bPoseChannel *bone_b = BKE_pose_channel_find_name(obj_armature_a->pose, "BoneB");

  bone_a->loc[0] = 0.0;
  bone_b->loc[1] = 0.0;

  AnimationEvalContext eval_context = {nullptr, 1.0f};
  blender::animrig::pose_apply_action_blend(
      obj_armature_a, pose_data, first_slot.handle, &eval_context, 1.0);

  EXPECT_NEAR(bone_a->loc[0], 10.0, 0.001);
  EXPECT_NEAR(bone_b->loc[1], 5.0, 0.001);

  bone_a->loc[0] = 0.0;
  bone_b->loc[1] = 0.0;

  blender::animrig::pose_apply_action_blend(
      obj_armature_a, pose_data, first_slot.handle, &eval_context, 0.5);

  EXPECT_NEAR(bone_a->loc[0], 5.0, 0.001);
  EXPECT_NEAR(bone_b->loc[1], 2.5, 0.001);
}

}  // namespace blender::animrig::tests
