/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_armature.hh"
#include "DNA_armature_types.h"
#include "DEG_depsgraph_query.hh"
#include "RNA_prototypes.hh"

#include "BKE_action.hh"
#include "node_geometry_util.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"
#include "NOD_rna_define.hh"

#include "RNA_access.hh"

namespace blender::nodes::node_geo_armature_info_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object").hide_label();
  b.add_input<decl::String>("Bone Name").hide_label();
  b.add_input<decl::Bool>("Use Parent").default_value(true);
  b.add_input<decl::Bool>("Local Axis").default_value(false); // Переименовать и по умолчанию выключен
  
  
  b.add_output<decl::Vector>("Location");
  b.add_output<decl::Rotation>("Rotation");
  b.add_output<decl::Vector>("Scale");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Object *object = params.extract_input<Object *>("Object");
  const std::string bone_name = params.extract_input<std::string>("Bone Name");
  const bool use_parent = params.extract_input<bool>("Use Parent");     // Учитывать родительские кости
  const bool local_axis = params.extract_input<bool>("Local Axis");     // Использовать локальные оси кости
  
  const bool is_armature = (object && object->type == OB_ARMATURE);
  if (!is_armature) {
    params.set_output("Location", float3(0.0f));
    params.set_output("Rotation", math::Quaternion::identity());
    params.set_output("Scale", float3(1.0f, 1.0f, 1.0f));
    return;
  }

  if (bone_name.empty()) {
    params.set_output("Location", float3(0.0f));
    params.set_output("Rotation", math::Quaternion::identity());
    params.set_output("Scale", float3(1.0f, 1.0f, 1.0f));
    return;
  }

  const Depsgraph *depsgraph = params.depsgraph();
  Object *object_eval = DEG_get_evaluated(depsgraph, object);
  
  if (!object_eval || !object_eval->pose) {
    params.set_output("Location", float3(0.0f));
    params.set_output("Rotation", math::Quaternion::identity());
    params.set_output("Scale", float3(1.0f, 1.0f, 1.0f));
    return;
  }

  bPoseChannel *pchan = BKE_pose_channel_find_name(object_eval->pose, bone_name.c_str());
  if (pchan) {
    float3 location, scale;
    math::Quaternion rotation;
    
    if (use_parent) {
      // Учитываем родительские кости - мировые координаты
      float4x4 mat = float4x4(pchan->pose_mat);
      math::to_loc_rot_scale_safe<true>(mat, location, rotation, scale);
    } else {
      if (local_axis) {
        // НЕ учитываем родителей + локальные оси кости
        location = float3(pchan->loc);
        scale = float3(pchan->scale);
        rotation = math::Quaternion(pchan->quat);
      } else {
        // НЕ учитываем родителей + мировые оси
        float4x4 bone_mat = float4x4(pchan->bone->arm_mat);
        
        float4x4 loc_mat = math::from_location<float4x4>(float3(pchan->loc));
        float4x4 rot_mat = math::from_rotation<float4x4>(math::normalize(math::Quaternion(pchan->quat)));
        float4x4 scale_mat = math::from_scale<float4x4>(float3(pchan->scale));
        
        float4x4 local_transform = loc_mat * rot_mat * scale_mat;
        float4x4 world_transform = bone_mat * local_transform;
        
        math::to_loc_rot_scale_safe<true>(world_transform, location, rotation, scale);
      }
    }
    
    params.set_output("Location", location);
    params.set_output("Rotation", rotation);
    params.set_output("Scale", scale);
  } else {
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