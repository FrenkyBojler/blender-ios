/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_listbase.hh"
#include "BLI_math_vector_c.hh"
#include "BLI_string.hh"

#include "BKE_armature.hh"
#include "BKE_gtest_base.hh"
#include "BKE_main.hh"
#include "BKE_object.hh"

#include "DNA_object_types.h"

#include "ED_object.hh"

#include "MEM_guardedalloc.h"

#include "testing/testing.h"

namespace blender::ed::object::tests {

class ObjectActiveCenterTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;
  Object *armature_object = nullptr;
  bArmature *armature = nullptr;
  EditBone *ebone = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();

    armature = BKE_armature_add(bmain, "Armature");
    armature->edbo = MEM_new_zeroed<ListBaseT<EditBone>>("edbo test");

    ebone = MEM_new<EditBone>("eBone test");
    STRNCPY(ebone->name, "Bone");
    zero_v3(ebone->head);
    ebone->tail[0] = 0.0f;
    ebone->tail[1] = -0.2f;
    ebone->tail[2] = 1.9f;
    BLI_addtail(armature->edbo, ebone);
    armature->act_edbone = ebone;

    armature_object = BKE_object_add_only_object(bmain, OB_ARMATURE, "Armature");
    armature_object->data = id_cast<ID *>(armature);
    armature_object->mode = OB_MODE_EDIT;
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
  }

  void clear_bone_selection()
  {
    ebone->flag &= ~(BONE_SELECTED | BONE_ROOTSEL | BONE_TIPSEL);
  }

  bool calc_center(const bool select_only, float r_center[3])
  {
    return calc_active_center_for_editmode(armature_object, select_only, r_center);
  }
};

/* Tip-only selection must use the tip as the active center. */
TEST_F(ObjectActiveCenterTest, ArmatureTipOnlyUsesTail)
{
  clear_bone_selection();
  ebone->flag |= BONE_TIPSEL;

  float center[3];
  ASSERT_TRUE(calc_center(false, center));
  EXPECT_V3_NEAR(center, ebone->tail, 1e-6f);
}

/* Root-only selection keeps the head/root center. */
TEST_F(ObjectActiveCenterTest, ArmatureRootOnlyUsesHead)
{
  clear_bone_selection();
  ebone->flag |= BONE_ROOTSEL;

  float center[3];
  ASSERT_TRUE(calc_center(false, center));
  EXPECT_V3_NEAR(center, ebone->head, 1e-6f);
}

/* When both endpoints are selected, prefer the head over the tip. */
TEST_F(ObjectActiveCenterTest, ArmatureBothEndsUsesHead)
{
  clear_bone_selection();
  ebone->flag |= BONE_ROOTSEL | BONE_TIPSEL;

  float center[3];
  ASSERT_TRUE(calc_center(false, center));
  EXPECT_V3_NEAR(center, ebone->head, 1e-6f);
}

/* Whole-bone selection (no exclusive tip) also uses the head. */
TEST_F(ObjectActiveCenterTest, ArmatureBoneSelectedUsesHead)
{
  clear_bone_selection();
  ebone->flag |= BONE_SELECTED;

  float center[3];
  ASSERT_TRUE(calc_center(false, center));
  EXPECT_V3_NEAR(center, ebone->head, 1e-6f);
}

/* Tip-only counts as selected for select_only callers (e.g. Snap With: Active). */
TEST_F(ObjectActiveCenterTest, ArmatureTipOnlySelectOnlySucceeds)
{
  clear_bone_selection();
  ebone->flag |= BONE_TIPSEL;

  float center[3];
  ASSERT_TRUE(calc_center(true, center));
  EXPECT_V3_NEAR(center, ebone->tail, 1e-6f);
}

/* select_only must fail when the active bone has no selected endpoints. */
TEST_F(ObjectActiveCenterTest, ArmatureUnselectedSelectOnlyFails)
{
  clear_bone_selection();

  float center[3] = {1.0f, 2.0f, 3.0f};
  EXPECT_FALSE(calc_center(true, center));
}

/* Without an active edit bone there is no center to return. */
TEST_F(ObjectActiveCenterTest, ArmatureNoActiveBoneFails)
{
  armature->act_edbone = nullptr;
  clear_bone_selection();
  ebone->flag |= BONE_TIPSEL;

  float center[3];
  EXPECT_FALSE(calc_center(false, center));
}

}  // namespace blender::ed::object::tests
