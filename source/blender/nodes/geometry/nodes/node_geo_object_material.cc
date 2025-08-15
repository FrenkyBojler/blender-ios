/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_material.hh"

#include "DNA_object_types.h"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_object_material_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object").hide_label();
  b.add_input<decl::Int>("Material Index").supports_field();
  b.add_output<decl::Material>("Material");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Object *object = params.extract_input<Object *>("Object");

  if (object == nullptr) {
    params.set_default_remaining_outputs();
    return;
  }

  const int material_index = params.extract_input<int>("Material Index");
  if (material_index < 0 || material_index >= *BKE_object_material_len_p(object)) {
    params.set_default_remaining_outputs();
    return;
  }

  Material *material = BKE_object_material_get(object, material_index + 1);
  params.set_output("Material", material);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeObjectMaterial");
  ntype.ui_name = "Object Material";
  ntype.ui_description = "Retrieve the material from an object";
  ntype.nclass = NODE_CLASS_INPUT;
  blender::bke::node_type_storage(
      ntype, "NodeGeometryObjectMaterial", node_free_standard_storage, node_copy_standard_storage);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_object_material_cc
