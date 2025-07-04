/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_armature.hh"
#include "BKE_constraint.h"
#include "DNA_armature_types.h"
#include "DEG_depsgraph_query.hh"
#include "RNA_prototypes.hh"
#include "DNA_constraint_types.h"
#include "BKE_action.hh"
#include "node_geometry_util.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "UI_interface.hh"
#include "UI_resources.hh"
#include "NOD_rna_define.hh"

#include "RNA_access.hh"

namespace blender::nodes::node_geo_armature_info_cc {

static void node_declare(NodeDeclarationBuilder &b)
{

  b.use_custom_socket_order();
  // Matrix outputs
  b.add_output<decl::Matrix>("Transform");      // Current pose transform matrix
  b.add_output<decl::Matrix>("Rest Transform"); // Rest pose transform matrix
  b.add_output<decl::Float>("Roll");     // Bone roll angle
  // b.add_output<decl::Float>("Use parent");
  // Bone properties outputs
  PanelDeclarationBuilder &column_length = b.add_panel("Length");
  column_length.add_output<decl::Float>("Length");
  column_length.add_output<decl::Float>("Rest Length");

  // Inputs
  b.add_input<decl::Object>("Object").hide_label();
  b.add_input<decl::String>("Bone Name").hide_label();
  b.add_input<decl::Bool>("Eval Constraints").default_value(true);
  
  
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Object *object = params.extract_input<Object *>("Object");
  const std::string bone_name = params.extract_input<std::string>("Bone Name");
  const bool eval_constraints = params.extract_input<bool>("Eval Constraints");
  
  // Default outputs for error cases
  const float4x4 default_matrix = float4x4::identity();
  const float default_length = 1.0f;
  const float default_roll = 0.0f;
  
  // Helper function to set all outputs to defaults
  auto set_default_outputs = [&]() {
    params.set_output("Transform", default_matrix);
    params.set_output("Rest Transform", default_matrix);
    params.set_output("Length", default_length);
    params.set_output("Rest Length", default_length);
    params.set_output("Roll", default_roll);
  };
  
  // Validate that we have an armature object
  if (!object || object->type != OB_ARMATURE) {
    set_default_outputs();
    return;
  }

  // Validate that bone name is provided
  if (bone_name.empty()) {
    set_default_outputs();
    return;
  }

  // Get the evaluated object from the dependency graph
  const Depsgraph *depsgraph = params.depsgraph();
  Object *object_eval = DEG_get_evaluated(depsgraph, object);
  
  // Validate that the evaluated object has armature and pose data
  if (!object_eval || !object_eval->data || !object_eval->pose) {
    set_default_outputs();
    return;
  }

  bArmature *armature = static_cast<bArmature *>(object_eval->data);
  
  // Find the bone in the armature
  Bone *bone = BKE_armature_find_bone_name(armature, bone_name.c_str());
  if (!bone) {
    // Bone not found - show error
    params.error_message_add(
        NodeWarningType::Error,
        TIP_("Bone \"") + bone_name + TIP_("\" not found in armature"));
    params.set_input_unused("Bone Name");
    set_default_outputs();
    return;
  }

  // Find the pose channel for animated data
  bPoseChannel *pchan = BKE_pose_channel_find_name(object_eval->pose, bone_name.c_str());
  if (!pchan) {
    params.error_message_add(
        NodeWarningType::Error,
        TIP_("Pose channel for bone \"") + bone_name + TIP_("\" not found"));
    set_default_outputs();
    return;
  }

  // ======= GET TRANSFORM MATRIX =======
  float transform_mat[4][4];
  
  if (eval_constraints) {
    // Include constraints - use pose_mat and convert to local space
    copy_m4_m4(transform_mat, pchan->pose_mat);
    
    // Convert from POSE space to LOCAL space (removes parent influence, keeps constraints)
    BKE_constraint_mat_convertspace(
        object_eval, pchan, nullptr, transform_mat, 
        CONSTRAINT_SPACE_POSE, CONSTRAINT_SPACE_LOCAL, false);
  } else {
    // No constraints - get bone's local transform
    BKE_pchan_to_mat4(pchan, transform_mat);
  }
  
  // ======= GET REST TRANSFORM MATRIX =======
  float rest_mat[4][4];
  copy_m4_m4(rest_mat, bone->arm_mat);
  
  // ======= CALCULATE LENGTHS =======
  // Rest length - from bone data
  float rest_length = bone->length;
  
  // Animated length - calculate from current transform
  float head_local[3] = {0.0f, 0.0f, 0.0f};
  float tail_local[3] = {0.0f, bone->length, 0.0f};
  
  float head_world[3], tail_world[3];
  mul_v3_m4v3(head_world, transform_mat, head_local);
  mul_v3_m4v3(tail_world, transform_mat, tail_local);
  
  float animated_length = len_v3v3(head_world, tail_world);
  
  // Convert to float4x4
  float4x4 final_transform = float4x4(transform_mat);
  float4x4 final_rest_transform = float4x4(rest_mat);
  
  // Output values
  params.set_output("Transform", final_transform);
  params.set_output("Rest Transform", final_rest_transform);
  params.set_output("Length", animated_length);        // Current pose length
  params.set_output("Rest Length", rest_length);       // Rest pose length
  params.set_output("Roll", bone->arm_roll);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeBoneInfo", GEO_NODE_BONE_INFO);
  ntype.ui_name = "Bone Info";
  ntype.ui_description = "Get bone transform matrices and properties";
  ntype.enum_name_legacy = "ARMATURE_INFO";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_armature_info_cc