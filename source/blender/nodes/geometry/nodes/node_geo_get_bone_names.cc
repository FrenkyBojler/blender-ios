/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_action.hh"
#include "BKE_armature.hh"

#include "BLI_sort.hh"

#include "NOD_geometry_nodes_list.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_get_bone_names_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_output<decl::String>("Names"_ustr).structure_type(StructureType::List);

  b.add_input<decl::Object>("Armature"_ustr)
      .optional_label()
      .description("Armature object to retrieve the bone information from");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Object *object = params.extract_input<Object *>("Armature"_ustr);
  if (!object) {
    params.set_default_remaining_outputs();
    return;
  }
  if (object->type != OB_ARMATURE) {
    params.set_default_remaining_outputs();
    params.error_message_add(NodeWarningType::Error, TIP_("Object is not an armature"));
    return;
  }

  const bArmature &armature = *id_cast<const bArmature *>(object->data);
  VectorSet<std::string> names_set;

  BKE_armature_foreach_bone(
      armature, [&](const int bone_index, const Bone &bone) { names_set.add(bone.name); });

  Vector<std::string> names = names_set.extract_vector();
  parallel_sort(
      names.begin(), names.end(), [](const StringRef a, const StringRef b) { return a < b; });
  params.set_output("Names"_ustr, GList::from_container(names));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeGetBoneNames"_ustr);
  ntype.ui_name = "Get Bone Names";
  ntype.ui_description = "Retrieves armature bone names as a list of strings";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_get_bone_names_cc
