/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

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
      "Evaluated transformation of the bone on armature space");
  b.add_output<decl::Matrix>("Rest Pose")
      .description("Original transformation of the bone as set in edit mode in armature space");
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
  Bone *bone = pchan->bone;
  params.set_output("Pose", float4x4(pchan->pose_mat));
  params.set_output("Rest Pose", float4x4(bone->arm_mat));
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
