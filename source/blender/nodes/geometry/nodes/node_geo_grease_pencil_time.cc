/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_geometry_fields.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_grease_pencil_time__cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Delta Time"_ustr).field_source();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Field<float> delta_time = AttributeFieldInput::get_field<float, "delta_time">();
  params.set_output("Delta Time"_ustr, std::move(delta_time));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeGreasePencilPointDeltaTime");
  ntype.ui_name = "Point Delta Time";
  ntype.ui_description =
      "Retrieve the difference in time between when each point in a Grease Pencil curve is drawn";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_grease_pencil_time__cc
