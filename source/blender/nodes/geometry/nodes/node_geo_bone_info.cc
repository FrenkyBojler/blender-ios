/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_matrix.hh"

#include "BKE_action.hh"
#include "BKE_armature.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_bone_info_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Armature")
      .optional_label()
      .description("Armature object to retrieve the bone information from");
  b.add_input<decl::String>("Bone Name")
      .optional_label()
      .description("Name of the bone to retrieve");

  b.add_output<decl::Matrix>("Pose").description(
      "Evaluated final transform of the bone in armature space");
  b.add_output<decl::Matrix>("Local Pose")
      .description("Difference between the pose and rest pose relative to the parent bone");
  b.add_output<decl::Matrix>("Transform Pose")
      .description("Matrix representing the bone's location, rotation, and scale properties");
  b.add_output<decl::Matrix>("Rest Pose")
      .description("Original transform of the bone in armature space, defined in edit mode");
  b.add_output<decl::Float>("Rest Length").description("Original length of the bone");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Object *object = params.extract_input<Object *>("Armature");
  if (!object) {
    params.set_default_remaining_outputs();
    return;
  }
  if (object->type != OB_ARMATURE) {
    params.set_default_remaining_outputs();
    params.error_message_add(NodeWarningType::Error, TIP_("Object is not an armature"));
    return;
  }
  const std::string bone_name = params.extract_input<std::string>("Bone Name");
  if (bone_name.empty()) {
    params.set_default_remaining_outputs();
    return;
  }
  if (!object->pose) {
    params.set_default_remaining_outputs();
    params.error_message_add(NodeWarningType::Error, TIP_("Object has no pose"));
    return;
  }

  bPoseChannel *pchan = BKE_pose_channel_find_name(object->pose, bone_name.c_str());
  if (!pchan) {
    params.set_default_remaining_outputs();
    params.error_message_add(NodeWarningType::Error, TIP_("Bone not found"));
    return;
  }
  bPoseChannel *parent_pchan = BKE_pose_channel_find_name(object->pose, bone_name.c_str());
  Bone *bone = pchan->bone;
  const float4x4 pose = float4x4(pchan->pose_mat);
  const float4x4 rest_pose = float4x4(bone->arm_mat);

  const float4x4 parent_pose = pchan->parent ? float4x4(pchan->parent->pose_mat) :
                                               float4x4::identity();
  const float4x4 parent_rest_pose = bone->parent ? float4x4(bone->parent->arm_mat) :
                                                   float4x4::identity();
  const float4x4 local_pose = math::invert(rest_pose) * parent_rest_pose *
                              math::invert(parent_pose) * pose;

  float4x4 transform_pose;
  BKE_pchan_to_mat4(pchan, transform_pose.ptr());

  params.set_output("Pose", pose);
  params.set_output("Local Pose", local_pose);
  params.set_output("Transform Pose", transform_pose);
  params.set_output("Rest Pose", rest_pose);
  params.set_output("Rest Length", bone->length);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeBoneInfo");
  ntype.ui_name = "Bone Info";
  ntype.ui_description = "Retrieve information of armature bones";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_bone_info_cc
