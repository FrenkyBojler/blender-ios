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
#include "UI_interface.hh"
#include "UI_resources.hh"
#include "NOD_rna_define.hh"

#include "RNA_access.hh"

namespace blender::nodes::node_geo_armature_info_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object").hide_label();
  b.add_input<decl::String>("Bone Name").hide_label();
  b.add_input<decl::Bool>("Local Axis").default_value(false);  // True = bone's actual axes (X,Y,Z), False = swapped axes (X,Z,-Y)
  b.add_input<decl::Bool>("Use Child").default_value(true);    // Include constraints in transform calculation
  
  b.add_output<decl::Vector>("Location");
  b.add_output<decl::Rotation>("Rotation");
  b.add_output<decl::Vector>("Scale");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Object *object = params.extract_input<Object *>("Object");
  const std::string bone_name = params.extract_input<std::string>("Bone Name");
  const bool local_axis = params.extract_input<bool>("Local Axis");
  const bool use_child = params.extract_input<bool>("Use Child");
  
  // Validate that we have an armature object
  const bool is_armature = (object && object->type == OB_ARMATURE);
  if (!is_armature) {
    params.set_output("Location", float3(0.0f));
    params.set_output("Rotation", math::Quaternion::identity());
    params.set_output("Scale", float3(1.0f, 1.0f, 1.0f));
    return;
  }

  // Validate that bone name is provided
  if (bone_name.empty()) {
    params.set_output("Location", float3(0.0f));
    params.set_output("Rotation", math::Quaternion::identity());
    params.set_output("Scale", float3(1.0f, 1.0f, 1.0f));
    return;
  }

  // Get the evaluated object from the dependency graph to ensure we have the latest state
  const Depsgraph *depsgraph = params.depsgraph();
  Object *object_eval = DEG_get_evaluated(depsgraph, object);
  
  // Validate that the evaluated object has pose data
  if (!object_eval || !object_eval->pose) {
    params.set_output("Location", float3(0.0f));
    params.set_output("Rotation", math::Quaternion::identity());
    params.set_output("Scale", float3(1.0f, 1.0f, 1.0f));
    return;
  }
  // Find the pose channel (bone) by name
  bPoseChannel *pchan = BKE_pose_channel_find_name(object_eval->pose, bone_name.c_str());
  if (pchan) {
    float3 location, scale;
    math::Quaternion rotation;

    if (use_child) {
      // When Use Child is ON: include constraints in the transform calculation
      // This approach follows the same logic as Blender's drivers for "visual" transform
      
      // Start with pose_mat which contains the bone's transform WITH constraints applied
      float mat[4][4];
      copy_m4_m4(mat, pchan->pose_mat);
      
      // Convert from POSE space to LOCAL space using constraint system
      // This removes parent influence but keeps constraint effects
      // Same conversion used by drivers when getting bone transforms
      BKE_constraint_mat_convertspace(
          object_eval, pchan, nullptr, mat, 
          CONSTRAINT_SPACE_POSE, CONSTRAINT_SPACE_LOCAL, false);
      
      // Decompose the resulting matrix into location, rotation, and scale
      float4x4 constraints_mat = float4x4(mat);
      math::to_loc_rot_scale_safe<true>(constraints_mat, location, rotation, scale);
    } else {
      // When Use Child is OFF: get bone's local transform without constraints
      // Use BKE_pchan_to_mat4 to properly handle all rotation modes (Euler XYZ, XZY, etc., Quaternion, Axis-Angle)
      float mat[4][4];
      BKE_pchan_to_mat4(pchan, mat);
      
      // Decompose the local matrix into components
      float4x4 local_mat = float4x4(mat);
      math::to_loc_rot_scale_safe<true>(local_mat, location, rotation, scale);
    }
    
    // Apply axis swapping when Local Axis is OFF
    // Blender's bone coordinate system uses Y as the bone's length axis and Z as the "up" direction
    // When Local Axis is OFF, we swap to a more intuitive coordinate system where Z is up
    if (!local_axis) {
      // Swap Y and Z axes, and invert the new Y to maintain proper orientation
      location = float3(location.x, -location.z, location.y);
      scale = float3(scale.x, scale.z, scale.y);
      // For quaternions, swap Y and Z components and invert the new Y component
      rotation = math::Quaternion(rotation.w, rotation.x, -rotation.z, rotation.y);
    }
    
    // Output the final transform components
    params.set_output("Location", location);
    params.set_output("Rotation", rotation);
    params.set_output("Scale", scale);
  } else {
    // Bone not found - show error and mark input field as invalid
    params.error_message_add(
        NodeWarningType::Error,
        TIP_("Bone \"") + bone_name + TIP_("\" not found in armature"));
    
    // Mark the "Bone Name" input as having an error (turns it red)
    params.set_input_unused("Bone Name");
    
    // Output default values
    params.set_output("Location", float3(0.0f));
    params.set_output("Rotation", math::Quaternion::identity());
    params.set_output("Scale", float3(1.0f, 1.0f, 1.0f));
  }
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeArmatureInfo", GEO_NODE_ARMATURE_INFO);
  ntype.ui_name = "Bone Info";
  ntype.ui_description = "Get information about armature bones";
  ntype.enum_name_legacy = "ARMATURE_INFO";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_armature_info_cc