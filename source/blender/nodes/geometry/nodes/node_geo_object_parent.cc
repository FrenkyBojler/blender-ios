/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_object_types.h"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_object_parent_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object").optional_label();
  b.add_output<decl::Object>("Parent").description(
      "The parent of the input object, if one exists");
  b.add_output<decl::Bool>("Exists").description("Whether the input object has a parent");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Object *object = params.extract_input<Object *>("Object");

  if (object == nullptr) {
    params.set_default_remaining_outputs();
    return;
  }

  params.set_output("Parent", object->parent);
  params.set_output("Exists", object->parent != nullptr);
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeObjectParent");
  ntype.ui_name = "Object Parent";
  ntype.ui_description = "Retrieve the parent of an object";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_object_parent_cc
