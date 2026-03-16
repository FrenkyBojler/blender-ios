/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "ANIM_action.hh"
#include "ANIM_evaluation.hh"
#include "evaluation_internal.hh"

#include "BKE_action.hh"
#include "BKE_animsys.h"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_object.hh"

#include "DNA_object_types.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "BLI_math_base.h"

#include <optional>

#include "CLG_log.h"
#include "testing/testing.h"

namespace blender::animrig::tests {

using namespace blender::animrig::internal;

class AnimationEvaluationTest : public testing::Test {
 protected:
  Main *bmain;
  Action *action;
  Object *cube;
  Slot *slot;
  Layer *layer;

  KeyframeSettings settings = get_keyframe_settings(false);
  AnimationEvalContext anim_eval_context = {};
  PointerRNA cube_rna_ptr;

 public:
  static void SetUpTestSuite()
  {
    /* BKE_id_free() hits a code path that uses CLOG, which crashes if not initialized properly. */
    CLG_init();

    /* To make id_can_have_animdata() and friends work, the `id_types` array needs to be set up. */
    BKE_idtype_init();

    RNA_init();
  }

  static void TearDownTestSuite()
  {
    CLG_exit();
    RNA_exit();
  }

  void SetUp() override
  {
    bmain = BKE_main_new();
    action = BKE_id_new<Action>(bmain, "ACÄnimåtië");

    cube = BKE_object_add_only_object(bmain, OB_EMPTY, "Küüübus");

    slot = &action->slot_add();
    ASSERT_EQ(assign_action_and_slot(action, slot, cube->id), ActionSlotAssignmentResult::OK);

    layer = &action->layer_add("Kübus layer");

    /* Make it easier to predict test values. */
    settings.interpolation = BEZT_IPO_LIN;

    cube_rna_ptr = RNA_pointer_create_discrete(&cube->id, RNA_Object, &cube->id);
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
  }

  /** Evaluate the layer, and return result for the given property. */
  std::optional<float> evaluate_single_property(const StringRefNull rna_path,
                                                const int array_index,
                                                const float eval_time)
  {
    anim_eval_context.eval_time = eval_time;
    EvaluationResult result = evaluate_layer(
        cube_rna_ptr, *action, *layer, slot->handle, anim_eval_context);

    const AnimatedProperty *loc0_result = result.lookup_ptr(PropIdentifier(rna_path, array_index));
    if (!loc0_result) {
      return {};
    }
    return loc0_result->value;
  }

  void key_quaternion(Main *bmain, Slot &slot, StripKeyframeData &strip_data, const float4 &values)
  {
    for (int i = 0; i < 4; i++) {
      strip_data.keyframe_insert(
          bmain, slot, {"rotation_quaternion", i}, {1, values[i]}, settings);
    }
  }

  /** Evaluate the layer, and test that the given property evaluates to the expected value. */
  testing::AssertionResult test_evaluate_layer(const StringRefNull rna_path,
                                               const int array_index,
                                               const float2 eval_time__expect_value)
  {
    const float eval_time = eval_time__expect_value[0];
    const float expect_value = eval_time__expect_value[1];

    const std::optional<float> opt_eval_value = evaluate_single_property(
        rna_path, array_index, eval_time);
    if (!opt_eval_value) {
      return testing::AssertionFailure()
             << rna_path << "[" << array_index << "] should have been animated";
    }

    const float eval_value = *opt_eval_value;
    const uint diff_ulps = ulp_diff_ff(expect_value, eval_value);
    if (diff_ulps >= 4) {
      return testing::AssertionFailure()
             << std::endl
             << "    " << rna_path << "[" << array_index
             << "] evaluation did not produce the expected result:" << std::endl
             << "      evaluated to: " << testing::PrintToString(eval_value) << std::endl
             << "      expected    : " << testing::PrintToString(expect_value) << std::endl;
    }

    return testing::AssertionSuccess();
  };

  /** Evaluate the layer, and test that the given property is not part of the result. */
  testing::AssertionResult test_evaluate_layer_no_result(const StringRefNull rna_path,
                                                         const int array_index,
                                                         const float eval_time)
  {
    const std::optional<float> eval_value = evaluate_single_property(
        rna_path, array_index, eval_time);
    if (eval_value) {
      return testing::AssertionFailure()
             << std::endl
             << "    " << rna_path << "[" << array_index
             << "] evaluation should NOT produce a value:" << std::endl
             << "      evaluated to: " << testing::PrintToString(*eval_value) << std::endl;
    }

    return testing::AssertionSuccess();
  }
};

TEST_F(AnimationEvaluationTest, evaluate_layer__keyframes)
{
  Strip &strip = layer->strip_add(*action, Strip::Type::Keyframe);
  StripKeyframeData &strip_data = strip.data<StripKeyframeData>(*action);

  /* Set some keys. */
  strip_data.keyframe_insert(bmain, *slot, {"location", 0}, {1.0f, 47.1f}, settings);
  strip_data.keyframe_insert(bmain, *slot, {"location", 0}, {5.0f, 47.5f}, settings);
  strip_data.keyframe_insert(bmain, *slot, {"rotation_euler", 1}, {1.0f, 0.0f}, settings);
  strip_data.keyframe_insert(bmain, *slot, {"rotation_euler", 1}, {5.0f, 3.14f}, settings);

  /* Set the animated properties to some values. These should not be overwritten
   * by the evaluation itself. */
  cube->loc[0] = 3.0f;
  cube->loc[1] = 2.0f;
  cube->loc[2] = 7.0f;
  cube->rot[0] = 3.0f;
  cube->rot[1] = 2.0f;
  cube->rot[2] = 7.0f;

  /* Evaluate. */
  anim_eval_context.eval_time = 3.0f;
  EvaluationResult result = evaluate_layer(
      cube_rna_ptr, *action, *layer, slot->handle, anim_eval_context);

  /* Check the result. */
  ASSERT_FALSE(result.is_empty());
  AnimatedProperty *loc0_result = result.lookup_ptr(PropIdentifier("location", 0));
  ASSERT_NE(nullptr, loc0_result) << "location[0] should have been animated";
  EXPECT_EQ(47.3f, loc0_result->value);

  EXPECT_EQ(3.0f, cube->loc[0]) << "Evaluation should not modify the animated ID";
  EXPECT_EQ(2.0f, cube->loc[1]) << "Evaluation should not modify the animated ID";
  EXPECT_EQ(7.0f, cube->loc[2]) << "Evaluation should not modify the animated ID";
  EXPECT_EQ(3.0f, cube->rot[0]) << "Evaluation should not modify the animated ID";
  EXPECT_EQ(2.0f, cube->rot[1]) << "Evaluation should not modify the animated ID";
  EXPECT_EQ(7.0f, cube->rot[2]) << "Evaluation should not modify the animated ID";
}

TEST_F(AnimationEvaluationTest, evaluate_replace_layer)
{
  Strip &strip = layer->strip_add(*action, Strip::Type::Keyframe);
  StripKeyframeData &layer_1_strip_data = strip.data<StripKeyframeData>(*action);

  Layer &layer_2 = action->layer_add("layer_2");
  layer_2.layer_mix_mode = int8_t(Layer::MixMode::Replace);
  Strip &strip_2 = layer_2.strip_add(*action, Strip::Type::Keyframe);
  StripKeyframeData &layer_2_strip_data = strip_2.data<StripKeyframeData>(*action);

  layer_1_strip_data.keyframe_insert(bmain, *slot, {"location", 0}, {1, 10}, settings);
  layer_2_strip_data.keyframe_insert(bmain, *slot, {"location", 0}, {1, 20}, settings);

  layer_1_strip_data.keyframe_insert(bmain, *slot, {"rotation_euler", 0}, {1, 0.5f}, settings);
  layer_2_strip_data.keyframe_insert(bmain, *slot, {"rotation_euler", 0}, {1, 1.0f}, settings);

  layer_1_strip_data.keyframe_insert(bmain, *slot, {"scale", 0}, {1, 1.0f}, settings);
  layer_2_strip_data.keyframe_insert(bmain, *slot, {"scale", 0}, {1, 0.5f}, settings);

  EvaluationResult result = evaluate_action(
      cube_rna_ptr, *action, slot->handle, anim_eval_context);

  ASSERT_FALSE(result.is_empty());

  AnimatedProperty *loc_result = result.lookup_ptr(PropIdentifier("location", 0));
  AnimatedProperty *rot_result = result.lookup_ptr(PropIdentifier("rotation_euler", 0));
  AnimatedProperty *scale_result = result.lookup_ptr(PropIdentifier("scale", 0));

  EXPECT_FLOAT_EQ(loc_result->value, 20.0f);
  EXPECT_FLOAT_EQ(rot_result->value, 1.0f);
  EXPECT_FLOAT_EQ(scale_result->value, 0.5f);

  /* 50% influence. */
  layer_2.influence = 0.5f;
  result = evaluate_action(cube_rna_ptr, *action, slot->handle, anim_eval_context);

  loc_result = result.lookup_ptr(PropIdentifier("location", 0));
  rot_result = result.lookup_ptr(PropIdentifier("rotation_euler", 0));
  scale_result = result.lookup_ptr(PropIdentifier("scale", 0));

  EXPECT_FLOAT_EQ(loc_result->value, 15.0f);
  EXPECT_FLOAT_EQ(rot_result->value, 0.75f);
  EXPECT_FLOAT_EQ(scale_result->value, 0.75f);
}

TEST_F(AnimationEvaluationTest, evaluate_combine_layer)
{
  Strip &strip = layer->strip_add(*action, Strip::Type::Keyframe);
  StripKeyframeData &layer_1_strip_data = strip.data<StripKeyframeData>(*action);

  Layer &layer_2 = action->layer_add("layer_2");
  layer_2.layer_mix_mode = int8_t(Layer::MixMode::Combine);
  Strip &strip_2 = layer_2.strip_add(*action, Strip::Type::Keyframe);
  StripKeyframeData &layer_2_strip_data = strip_2.data<StripKeyframeData>(*action);

  layer_1_strip_data.keyframe_insert(bmain, *slot, {"location", 0}, {1, 10}, settings);
  layer_2_strip_data.keyframe_insert(bmain, *slot, {"location", 0}, {1, 20}, settings);

  layer_1_strip_data.keyframe_insert(bmain, *slot, {"rotation_euler", 0}, {1, 0.5f}, settings);
  layer_2_strip_data.keyframe_insert(bmain, *slot, {"rotation_euler", 0}, {1, 1.0f}, settings);

  layer_1_strip_data.keyframe_insert(bmain, *slot, {"scale", 0}, {1, 1.0f}, settings);
  layer_2_strip_data.keyframe_insert(bmain, *slot, {"scale", 0}, {1, 0.5f}, settings);

  EvaluationResult result = evaluate_action(
      cube_rna_ptr, *action, slot->handle, anim_eval_context);

  ASSERT_FALSE(result.is_empty());

  AnimatedProperty *loc_result = result.lookup_ptr(PropIdentifier("location", 0));
  AnimatedProperty *rot_result = result.lookup_ptr(PropIdentifier("rotation_euler", 0));
  AnimatedProperty *scale_result = result.lookup_ptr(PropIdentifier("scale", 0));

  EXPECT_FLOAT_EQ(loc_result->value, 30.0f);
  EXPECT_FLOAT_EQ(rot_result->value, 1.5f);
  /* Scales are not combined additively. For scale, 1 is considered the default so combining 1 and
   * 1 should still result in 1. */
  EXPECT_FLOAT_EQ(scale_result->value, 0.5f);

  /* 50% influence. */
  layer_2.influence = 0.5f;
  result = evaluate_action(cube_rna_ptr, *action, slot->handle, anim_eval_context);

  loc_result = result.lookup_ptr(PropIdentifier("location", 0));
  rot_result = result.lookup_ptr(PropIdentifier("rotation_euler", 0));
  scale_result = result.lookup_ptr(PropIdentifier("scale", 0));

  EXPECT_FLOAT_EQ(loc_result->value, 20.0f);
  EXPECT_FLOAT_EQ(rot_result->value, 1.0f);
  EXPECT_FLOAT_EQ(scale_result->value, 0.75f);
}

TEST_F(AnimationEvaluationTest, evaluate_single_layer_50_percent)
{
  /* For the first layer, the influence is ignored. See `evaluate_action` in
   * `animrig/intern/evaluation.cc`. This could be changed in the future by implementing an
   * explicit rest pose. */
  Strip &strip = layer->strip_add(*action, Strip::Type::Keyframe);
  StripKeyframeData &layer_1_strip_data = strip.data<StripKeyframeData>(*action);

  layer_1_strip_data.keyframe_insert(bmain, *slot, {"location", 0}, {1, 10}, settings);
  layer_1_strip_data.keyframe_insert(bmain, *slot, {"rotation_euler", 0}, {1, 0.5f}, settings);
  layer_1_strip_data.keyframe_insert(bmain, *slot, {"scale", 0}, {1, 2.0f}, settings);

  layer->influence = 0.5f;
  EvaluationResult result = evaluate_action(
      cube_rna_ptr, *action, slot->handle, anim_eval_context);

  AnimatedProperty *loc_result = result.lookup_ptr(PropIdentifier("location", 0));
  AnimatedProperty *rot_result = result.lookup_ptr(PropIdentifier("rotation_euler", 0));
  AnimatedProperty *scale_result = result.lookup_ptr(PropIdentifier("scale", 0));

  /* Reducing the influence slider has no effect. */
  EXPECT_FLOAT_EQ(loc_result->value, 10.0f);
  EXPECT_FLOAT_EQ(rot_result->value, 0.5f);
  EXPECT_FLOAT_EQ(scale_result->value, 2.0f);
}

TEST_F(AnimationEvaluationTest, evaluate_quaternion_replace_layer)
{
  /* Quaternions are a special case because they not blended per channel but always as a complete
   * float[4]. */
  cube->rotmode = ROT_MODE_QUAT;

  Strip &strip = layer->strip_add(*action, Strip::Type::Keyframe);
  StripKeyframeData &layer_1_strip_data = strip.data<StripKeyframeData>(*action);

  Layer &layer_2 = action->layer_add("layer_2");
  layer_2.layer_mix_mode = int8_t(Layer::MixMode::Replace);
  Strip &strip_2 = layer_2.strip_add(*action, Strip::Type::Keyframe);
  StripKeyframeData &layer_2_strip_data = strip_2.data<StripKeyframeData>(*action);

  /* Equal to 90 deg clockwise rotation. */
  key_quaternion(bmain, *slot, layer_1_strip_data, {0.707f, 0.0f, 0.707f, 0.0f});
  /* Equal to 90 deg counter clockwise rotation. */
  key_quaternion(bmain, *slot, layer_2_strip_data, {0.707f, 0.0f, -0.707f, 0.0f});

  EvaluationResult result = evaluate_action(
      cube_rna_ptr, *action, slot->handle, anim_eval_context);
  ASSERT_FALSE(result.is_empty());

  float4 expected_result = {0.707f, 0.0f, -0.707f, 0.0f};
  for (int i = 0; i < 4; i++) {
    AnimatedProperty *eval_result = result.lookup_ptr(PropIdentifier("rotation_quaternion", i));
    if (!eval_result) {
      continue;
    }
    EXPECT_NEAR(eval_result->value, expected_result[i], 0.001f) << "Failed at index: " << i;
  }

  /* 50% influence. */
  layer_2.influence = 0.5;

  result = evaluate_action(cube_rna_ptr, *action, slot->handle, anim_eval_context);
  ASSERT_FALSE(result.is_empty());

  /* Should result in no rotation. */
  expected_result = {1.0f, 0.0f, 0.0f, 0.0f};
  for (int i = 0; i < 4; i++) {
    AnimatedProperty *eval_result = result.lookup_ptr(PropIdentifier("rotation_quaternion", i));
    if (!eval_result) {
      continue;
    }
    EXPECT_NEAR(eval_result->value, expected_result[i], 0.001f) << "Failed at index: " << i;
  }
}

TEST_F(AnimationEvaluationTest, evaluate_quaternion_combine_layer)
{
  cube->rotmode = ROT_MODE_QUAT;

  Strip &strip = layer->strip_add(*action, Strip::Type::Keyframe);
  StripKeyframeData &layer_1_strip_data = strip.data<StripKeyframeData>(*action);

  Layer &layer_2 = action->layer_add("layer_2");
  layer_2.layer_mix_mode = int8_t(Layer::MixMode::Combine);
  Strip &strip_2 = layer_2.strip_add(*action, Strip::Type::Keyframe);
  StripKeyframeData &layer_2_strip_data = strip_2.data<StripKeyframeData>(*action);

  /* Equal to 90 deg clockwise rotation. */
  key_quaternion(bmain, *slot, layer_1_strip_data, {0.707f, 0.0f, 0.707f, 0.0f});
  /* Equal to 90 deg counter clockwise rotation. */
  key_quaternion(bmain, *slot, layer_2_strip_data, {0.707f, 0.0f, -0.707f, 0.0f});

  EvaluationResult result = evaluate_action(
      cube_rna_ptr, *action, slot->handle, anim_eval_context);
  ASSERT_FALSE(result.is_empty());

  /* Should result in no rotation. */
  float4 expected_result = {1.0f, 0.0f, 0.0f, 0.0f};
  for (int i = 0; i < 4; i++) {
    AnimatedProperty *eval_result = result.lookup_ptr(PropIdentifier("rotation_quaternion", i));
    if (!eval_result) {
      continue;
    }
    EXPECT_NEAR(eval_result->value, expected_result[i], 0.001f) << "Failed at index: " << i;
  }

  /* 50% influence. */
  layer_2.influence = 0.5;

  result = evaluate_action(cube_rna_ptr, *action, slot->handle, anim_eval_context);
  ASSERT_FALSE(result.is_empty());

  /* This is a 45 degree rotation. */
  expected_result = {0.924f, 0.0f, 0.383f, 0.0f};
  for (int i = 0; i < 4; i++) {
    AnimatedProperty *eval_result = result.lookup_ptr(PropIdentifier("rotation_quaternion", i));
    if (!eval_result) {
      continue;
    }
    EXPECT_NEAR(eval_result->value, expected_result[i], 0.001f) << "Failed at index: " << i;
  }
}

TEST_F(AnimationEvaluationTest, evaluate_sparse_quaternion_blending)
{
  /* When blending quaternions we always build a complete float[4]. This has to work even if not
   * all quaternion channels are keyed. */
  cube->rotmode = ROT_MODE_QUAT;

  Strip &strip = layer->strip_add(*action, Strip::Type::Keyframe);
  StripKeyframeData &layer_1_strip_data = strip.data<StripKeyframeData>(*action);

  Layer &layer_2 = action->layer_add("layer_2");
  layer_2.layer_mix_mode = int8_t(Layer::MixMode::Combine);
  Strip &strip_2 = layer_2.strip_add(*action, Strip::Type::Keyframe);
  StripKeyframeData &layer_2_strip_data = strip_2.data<StripKeyframeData>(*action);

  /* Equal to 90 deg clockwise rotation. Also note that the quaternion does not have to be
   * normalized.*/
  layer_1_strip_data.keyframe_insert(
      bmain, *slot, {"rotation_quaternion", 2}, {1, 1.0f}, settings);

  /* Equal to 90 deg counter clockwise rotation. */
  layer_2_strip_data.keyframe_insert(
      bmain, *slot, {"rotation_quaternion", 2}, {1, -1.0f}, settings);

  EvaluationResult result = evaluate_action(
      cube_rna_ptr, *action, slot->handle, anim_eval_context);
  ASSERT_FALSE(result.is_empty());

  /* Should result in no rotation. */
  float4 expected_result = {1.0f, 0.0f, 0.0f, 0.0f};
  for (int i = 0; i < 4; i++) {
    AnimatedProperty *eval_result = result.lookup_ptr(PropIdentifier("rotation_quaternion", i));
    if (!eval_result) {
      continue;
    }
    EXPECT_NEAR(eval_result->value, expected_result[i], 0.001f) << "Failed at index: " << i;
  }
}

TEST_F(AnimationEvaluationTest, strip_boundaries__single_strip)
{
  /* Single finite strip, check first, middle, and last frame. */
  Strip &strip = layer->strip_add(*action, Strip::Type::Keyframe);
  strip.resize(1.0f, 10.0f);

  /* Set some keys. */
  StripKeyframeData &strip_data = strip.data<StripKeyframeData>(*action);
  strip_data.keyframe_insert(bmain, *slot, {"location", 0}, {1.0f, 47.0f}, settings);
  strip_data.keyframe_insert(bmain, *slot, {"location", 0}, {5.0f, 327.0f}, settings);
  strip_data.keyframe_insert(bmain, *slot, {"location", 0}, {10.0f, 48.0f}, settings);

  /* Evaluate the layer to see how it handles the boundaries + something in between. */
  EXPECT_TRUE(test_evaluate_layer("location", 0, {1.0f, 47.0f}));
  EXPECT_TRUE(test_evaluate_layer("location", 0, {3.0f, 187.0f}));
  EXPECT_TRUE(test_evaluate_layer("location", 0, {10.0f, 48.0f}));

  EXPECT_TRUE(test_evaluate_layer_no_result("location", 0, 10.001f));
}

TEST_F(AnimationEvaluationTest, strip_boundaries__nonoverlapping)
{
  /* Two finite strips that are strictly distinct. */
  Strip &strip1 = layer->strip_add(*action, Strip::Type::Keyframe);
  Strip &strip2 = layer->strip_add(*action, Strip::Type::Keyframe);
  strip1.resize(1.0f, 10.0f);
  strip2.resize(11.0f, 20.0f);
  strip2.frame_offset = 10;

  /* Set some keys. */
  {
    StripKeyframeData &strip_data1 = strip1.data<StripKeyframeData>(*action);
    strip_data1.keyframe_insert(bmain, *slot, {"location", 0}, {1.0f, 47.0f}, settings);
    strip_data1.keyframe_insert(bmain, *slot, {"location", 0}, {5.0f, 327.0f}, settings);
    strip_data1.keyframe_insert(bmain, *slot, {"location", 0}, {10.0f, 48.0f}, settings);
  }
  {
    StripKeyframeData &strip_data2 = strip2.data<StripKeyframeData>(*action);
    strip_data2.keyframe_insert(bmain, *slot, {"location", 0}, {1.0f, 47.0f}, settings);
    strip_data2.keyframe_insert(bmain, *slot, {"location", 0}, {5.0f, 327.0f}, settings);
    strip_data2.keyframe_insert(bmain, *slot, {"location", 0}, {10.0f, 48.0f}, settings);
  }

  /* Check Strip 1. */
  EXPECT_TRUE(test_evaluate_layer("location", 0, {1.0f, 47.0f}));
  EXPECT_TRUE(test_evaluate_layer("location", 0, {3.0f, 187.0f}));
  EXPECT_TRUE(test_evaluate_layer("location", 0, {10.0f, 48.0f}));

  /* Check Strip 2. */
  EXPECT_TRUE(test_evaluate_layer("location", 0, {11.0f, 47.0f}));
  EXPECT_TRUE(test_evaluate_layer("location", 0, {13.0f, 187.0f}));
  EXPECT_TRUE(test_evaluate_layer("location", 0, {20.0f, 48.0f}));

  /* Check outside the range of the strips. */
  EXPECT_TRUE(test_evaluate_layer_no_result("location", 0, 0.999f));
  EXPECT_TRUE(test_evaluate_layer_no_result("location", 0, 10.001f));
  EXPECT_TRUE(test_evaluate_layer_no_result("location", 0, 10.999f));
  EXPECT_TRUE(test_evaluate_layer_no_result("location", 0, 20.001f));
}

TEST_F(AnimationEvaluationTest, strip_boundaries__overlapping_edge)
{
  /* Two finite strips that are overlapping on their edge. */
  Strip &strip1 = layer->strip_add(*action, Strip::Type::Keyframe);
  Strip &strip2 = layer->strip_add(*action, Strip::Type::Keyframe);
  strip1.resize(1.0f, 10.0f);
  strip2.resize(10.0f, 19.0f);
  strip2.frame_offset = 9;

  /* Set some keys. */
  {
    StripKeyframeData &strip_data1 = strip1.data<StripKeyframeData>(*action);
    strip_data1.keyframe_insert(bmain, *slot, {"location", 0}, {1.0f, 47.0f}, settings);
    strip_data1.keyframe_insert(bmain, *slot, {"location", 0}, {5.0f, 327.0f}, settings);
    strip_data1.keyframe_insert(bmain, *slot, {"location", 0}, {10.0f, 48.0f}, settings);
  }
  {
    StripKeyframeData &strip_data2 = strip2.data<StripKeyframeData>(*action);
    strip_data2.keyframe_insert(bmain, *slot, {"location", 0}, {1.0f, 47.0f}, settings);
    strip_data2.keyframe_insert(bmain, *slot, {"location", 0}, {5.0f, 327.0f}, settings);
    strip_data2.keyframe_insert(bmain, *slot, {"location", 0}, {10.0f, 48.0f}, settings);
  }

  /* Check Strip 1. */
  EXPECT_TRUE(test_evaluate_layer("location", 0, {1.0f, 47.0f}));
  EXPECT_TRUE(test_evaluate_layer("location", 0, {3.0f, 187.0f}));

  /* Check overlapping frame. */
  EXPECT_TRUE(test_evaluate_layer("location", 0, {10.0f, 47.0f}))
      << "On the overlapping frame, only Strip 2 should be evaluated.";

  /* Check Strip 2. */
  EXPECT_TRUE(test_evaluate_layer("location", 0, {12.0f, 187.0f}));
  EXPECT_TRUE(test_evaluate_layer("location", 0, {19.0f, 48.0f}));

  /* Check outside the range of the strips. */
  EXPECT_TRUE(test_evaluate_layer_no_result("location", 0, 0.999f));
  EXPECT_TRUE(test_evaluate_layer_no_result("location", 0, 19.001f));
}

class AccessibleEvaluationResult : public EvaluationResult {
 public:
  EvaluationMap &get_map()
  {
    return result_;
  }
};

TEST(AnimationEvaluationResultTest, prop_identifier_hashing)
{
  AccessibleEvaluationResult result;

  /* Test storing the same result twice, with different memory locations of the RNA paths. This
   * tests that the mapping uses the actual string, and not just pointer comparison. */
  const char *rna_path_1 = "pose.bones['Root'].location";
  const std::string rna_path_2(rna_path_1);
  ASSERT_NE(rna_path_1, rna_path_2.c_str())
      << "This test requires different addresses for the RNA path strings";

  PathResolvedRNA fake_resolved_rna;
  result.store(rna_path_1, 0, 1.0f, fake_resolved_rna);
  result.store(rna_path_2, 0, 2.0f, fake_resolved_rna);
  EXPECT_EQ(1, result.get_map().size())
      << "Storing a result for the same property twice should just overwrite the previous value";

  {
    PropIdentifier key(rna_path_1, 0);
    AnimatedProperty *anim_prop = result.lookup_ptr(key);
    EXPECT_EQ(2.0f, anim_prop->value) << "The last-stored result should survive.";
  }
  {
    PropIdentifier key(rna_path_2, 0);
    AnimatedProperty *anim_prop = result.lookup_ptr(key);
    EXPECT_EQ(2.0f, anim_prop->value) << "The last-stored result should survive.";
  }
}

}  // namespace blender::animrig::tests
