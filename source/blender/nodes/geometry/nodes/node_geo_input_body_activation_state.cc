/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_geometry_fields.hh"
#include "BKE_physics_geometry.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_body_activation_state_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Bool>("Active").field_source();
  b.add_output<decl::Bool>("Allow Sleep").field_source();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const StringRef is_active_id = bke::PhysicsGeometry::attribute_name(
      bke::PhysicsGeometry::BodyAttribute::is_active);
  const StringRef allow_sleep_id = bke::PhysicsGeometry::attribute_name(
      bke::PhysicsGeometry::BodyAttribute::is_active);
  Field<bool> is_active_field = bke::AttributeFieldInput::Create<bool>(is_active_id);
  Field<bool> allow_sleep_field = bke::AttributeFieldInput::Create<bool>(allow_sleep_id);

  params.set_output("Active", std::move(is_active_field));
  params.set_output("Allow Sleep", std::move(allow_sleep_field));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(
      &ntype, "GeometryNodeInputBodyActivationState", GEO_NODE_INPUT_BODY_ACTIVATION_STATE);
  ntype.ui_name = "Body Activation State";
  ntype.ui_description = "Current activation state of a body in the simulation";
  ntype.enum_name_legacy = "INPUT_BODY_ACTIVATION_STATE";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_body_activation_state_cc
