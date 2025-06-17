/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_listbase.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_string.h"

#include "BKE_action.hh"
#include "BKE_armature.hh"
#include "BKE_deform.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"

#include "CLG_log.h"

#include "DNA_armature_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"

#include "testing/testing.h"

namespace blender::bke::tests {

/* This happens usually in BKE_pose_bone_done. Update here to avoid creating a full depsgraph. */
static void update_pose_matrices(bPoseChannel &pchan)
{
  BKE_pchan_calc_mat(&pchan);
  if (!(pchan.bone->flag & BONE_NO_DEFORM)) {
    mat4_to_dquat(&pchan.runtime.deform_dual_quat, pchan.bone->arm_mat, pchan.chan_mat);
  }
}

class ArmatureDeformTest : public testing::Test {
 public:
  Main *bmain;

  /* Translation for bones.
   * Rotation is omitted here for simplicity, the goal is to test all the code paths rather than
   * the details of the bone transformation. Other tests are more suitable for comparing
   * deformation results. */
  static float3 offset_bone1()
  {
    return float3(5, 0, 1);
  }
  static float3 offset_bone2()
  {
    return float3(0, -2, 0);
  }

  Object *create_test_armature() const
  {
    Object *ob = BKE_object_add_only_object(bmain, OB_ARMATURE, "Test Armature Object");
    bArmature *arm = BKE_id_new<bArmature>(bmain, "Test Armature");
    ob->data = arm;

    Bone *bone1 = MEM_callocN<Bone>("Bone1");
    STRNCPY(bone1->name, "Bone1");
    copy_v3_v3(bone1->tail, float3(0, 0, 0));
    copy_v3_v3(bone1->head, float3(0, 0, 1));
    BLI_addtail(&arm->bonebase, bone1);
    BKE_armature_where_is_bone(bone1, nullptr, false);
    bone1->weight = 1.0f;
    /* Bone envelope large enough to include all vertices.
     * Falloff math isn't tested here, just have to make sure vertices are included. */
    bone1->rad_head = 2.0f;
    bone1->rad_tail = 2.0f;

    Bone *bone2 = MEM_callocN<Bone>("Bone2");
    STRNCPY(bone2->name, "Bone2");
    copy_v3_v3(bone2->tail, float3(0, 0, 0));
    copy_v3_v3(bone2->head, float3(0, 0, 1));
    BLI_addtail(&arm->bonebase, bone2);
    BKE_armature_where_is_bone(bone2, nullptr, false);
    bone2->weight = 1.0f;
    bone2->rad_head = 2.0f;
    bone2->rad_tail = 2.0f;

    BKE_pose_ensure(bmain, ob, arm, false);

    bPoseChannel *pchan1 = BKE_pose_channel_find_name(ob->pose, "Bone1");
    bPoseChannel *pchan2 = BKE_pose_channel_find_name(ob->pose, "Bone2");
    copy_v3_v3(pchan1->loc, offset_bone1());
    copy_v3_v3(pchan2->loc, offset_bone2());
    update_pose_matrices(*pchan1);
    update_pose_matrices(*pchan2);

    return ob;
  }

  static Span<float3> mesh_positions()
  {
    static Array<float3> data = {float3(-1, -1, -1),
                                 float3(1, -1, -1),
                                 float3(-1, 1, -1),
                                 float3(1, 1, -1),
                                 float3(-1, -1, 1),
                                 float3(1, -1, 1),
                                 float3(-1, 1, 1),
                                 float3(1, 1, 1)};
    return data;
  }

  static Span<float> mesh_weights_bone1()
  {
    static Array<float> data = {1, 1, 1, 1, 1, 1, 1, 1};
    return data;
  }

  static Span<float> mesh_weights_bone2()
  {
    static Array<float> data = {0, 0, 0, 0, 1, 1, 1, 1};
    return data;
  }

  /* Creates a cube with all vertices in "Bone1" group and the top face in "Bone2" group. */
  Object *create_test_mesh() const
  {
    Object *ob = BKE_object_add_only_object(bmain, OB_MESH, "Test Mesh Object");
    Mesh *mesh_in_main = BKE_mesh_add(bmain, "Test Mesh");
    ob->data = mesh_in_main;

    Mesh *mesh = BKE_mesh_new_nomain(mesh_positions().size(), 0, 0, 0);
    mesh->vert_positions_for_write().copy_from(mesh_positions());
    MutableSpan<MDeformVert> dverts = mesh->deform_verts_for_write();
    for (const int i : dverts.index_range()) {
      const float weight0 = mesh_weights_bone1()[i];
      const float weight1 = mesh_weights_bone2()[i];

      if (weight0 > 0.0f) {
        BKE_defvert_add_index_notest(&dverts[i], 0, weight0);
      }
      if (weight1 > 0.0f) {
        BKE_defvert_add_index_notest(&dverts[i], 1, weight1);
      }
    }
    mesh->tag_positions_changed();

    BKE_mesh_nomain_to_mesh(mesh, mesh_in_main, ob);
    BLI_assert(!mesh_in_main->deform_verts().is_empty());

    BKE_object_defgroup_new(ob, "Bone1");
    BKE_object_defgroup_new(ob, "Bone2");

    return ob;
  }

  enum class InterpolationTest {
    /* Linear interpolation. */
    Linear,
    /* Dual-quaternion method, aka. "Preserve Volume" (ARM_DEF_QUATERNION). */
    DualQuaternion,
  };

  enum class WeightingTest {
    /* Disabled (no deform). */
    None,
    /* Falloff from closest envelope point. */
    Envelope,
    /* Vertex group weight. */
    VertexGroups,
    /* Single vertex group weight. */
    SingleVertexGroup,
  };

  static Span<float3> expected_mesh_positions(const WeightingTest weighting)
  {
    /* Both bones weighted equally. */
    static Array<float3> data_envelope = {float3(1.5f, -2, -0.5f),
                                          float3(3.5f, -2, -0.5f),
                                          float3(1.5f, 0, -0.5f),
                                          float3(3.5f, 0, -0.5f),
                                          float3(1.5f, -2, 1.5f),
                                          float3(3.5f, -2, 1.5f),
                                          float3(1.5f, 0, 1.5f),
                                          float3(3.5f, 0, 1.5f)};
    /* Bottom verts deformed only by Bone1, top group deformed equally by both bones. */
    static Array<float3> data_vgroups = {float3(4, -1, 0),
                                         float3(6, -1, 0),
                                         float3(4, 1, 0),
                                         float3(6, 1, 0),
                                         float3(1.5f, -2, 1.5f),
                                         float3(3.5f, -2, 1.5f),
                                         float3(1.5f, 0, 1.5f),
                                         float3(3.5f, 0, 1.5f)};
    /* Only the "Bone2" vertex group is affected (same relative weights). */
    static Array<float3> data_single = {float3(-1, -1, -1),
                                        float3(1, -1, -1),
                                        float3(-1, 1, -1),
                                        float3(1, 1, -1),
                                        float3(1.5f, -2, 1.5f),
                                        float3(3.5f, -2, 1.5f),
                                        float3(1.5f, 0, 1.5f),
                                        float3(3.5f, 0, 1.5f)};

    switch (weighting) {
      case WeightingTest::None:
        return mesh_positions();
      case WeightingTest::Envelope:
        return data_envelope;
      case WeightingTest::VertexGroups:
        return data_vgroups;
      case WeightingTest::SingleVertexGroup:
        return data_single;
    }
    BLI_assert_unreachable();
    return {};
  }

  void mesh_test(const InterpolationTest interpolation, const WeightingTest weighting)
  {
    Object *ob_arm = this->create_test_armature();
    Object *ob_target = this->create_test_mesh();

    Mesh *mesh = static_cast<Mesh *>(ob_target->data);
    float(*vert_positions)[3] = mesh->vert_positions_for_write().cast<float[3]>().data();

    int deform_flag = 0;
    const char *defgrp_name = nullptr;

    switch (interpolation) {
      case InterpolationTest::Linear:
        /* Nothing to change, default mode. */
        break;
      case InterpolationTest::DualQuaternion:
        deform_flag |= ARM_DEF_QUATERNION;
        break;
    }

    switch (weighting) {
      case WeightingTest::None:
        /* Nothing to do. */
        break;
      case WeightingTest::Envelope:
        deform_flag |= ARM_DEF_ENVELOPE;
        break;
      case WeightingTest::VertexGroups:
        deform_flag |= ARM_DEF_VGROUP;
        break;
      case WeightingTest::SingleVertexGroup:
        deform_flag |= ARM_DEF_VGROUP;
        defgrp_name = "Bone2";
        break;
    }

    BKE_armature_deform_coords_with_mesh(ob_arm,
                                         ob_target,
                                         vert_positions,
                                         nullptr,
                                         mesh->verts_num,
                                         deform_flag,
                                         nullptr,
                                         defgrp_name,
                                         nullptr);

    EXPECT_EQ_SPAN(expected_mesh_positions(weighting), mesh->vert_positions());

    BKE_id_delete(bmain, ob_arm);
    BKE_id_delete(bmain, ob_target);
  }

  void SetUp() override
  {
    CLG_init();
    BKE_idtype_init();
    bmain = BKE_main_new();
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
    CLG_exit();
  }
};

/**
 * TODO
 * - Interpolation:
 *    * Linear
 *    * Dual-Quaternion ("Preserve Volume", ARM_DEF_QUATERNION)
 * - Bone weighting:
 *    * disabled (no ARM_DEF_* flags)
 *    * envelopes (ARM_DEF_ENVELOPE)
 *    * vertex groups (ARM_DEF_VGROUP)
 *    * single vertex group (defgrp_name parameter)
 * - Outputs:
 *    * Position-only
 *    * "Full" (deform matrix, for crazyspace)
 * - Target object types:
 *    * mesh
 *    * edit-mesh (bmesh)
 *    * lattice
 *    * curves
 *    * legacy curves (uses mesh proxy?)
 *    * unsupported ID type (should pass through)
 * - explicit me_target parameter (where/how is this case invoked?)
 * - inverted vertex group (ARM_DEF_INVERT_VGROUP)
 * - multi-modifier feature, roughly:
 *    1. Some deform modifier before
 *    2. Followed by 2 armature modifiers
 *    3. test that `vert_coords_prev` has original data
 *    4. mixed result based on vertex groups
 * - relative armature/target object transform (non-identity "premat"/"postmat" matrices)
 */

TEST_F(ArmatureDeformTest, MeshDeform)
{
  mesh_test(InterpolationTest::Linear, WeightingTest::None);
  mesh_test(InterpolationTest::Linear, WeightingTest::Envelope);
  mesh_test(InterpolationTest::Linear, WeightingTest::VertexGroups);
  mesh_test(InterpolationTest::Linear, WeightingTest::SingleVertexGroup);
  mesh_test(InterpolationTest::DualQuaternion, WeightingTest::None);
  mesh_test(InterpolationTest::DualQuaternion, WeightingTest::Envelope);
  mesh_test(InterpolationTest::DualQuaternion, WeightingTest::VertexGroups);
  mesh_test(InterpolationTest::DualQuaternion, WeightingTest::SingleVertexGroup);
  // Object *ob_arm = this->create_test_armature();
  // Object *ob_target = this->create_test_mesh();

  // Mesh *mesh = static_cast<Mesh *>(ob_target->data);
  // float(*vert_positions)[3] = mesh->vert_positions_for_write().cast<float[3]>().data();
  // const int deform_flag = ARM_DEF_VGROUP;

  // BKE_armature_deform_coords_with_mesh(ob_arm,
  //                                      ob_target,
  //                                      vert_positions,
  //                                      nullptr,
  //                                      mesh->verts_num,
  //                                      deform_flag,
  //                                      nullptr,
  //                                      nullptr,
  //                                      nullptr);

  // EXPECT_EQ_SPAN(expected_mesh_positions(), mesh->vert_positions());

  // BKE_id_delete(bmain, ob_arm);
  // BKE_id_delete(bmain, ob_target);
}

TEST_F(ArmatureDeformTest, LatticeDeform) {}

}  // namespace blender::bke::tests
