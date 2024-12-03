/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_position_cc {

class SculptPositionFieldInput final : public fn::FieldInput {
public:
  SculptPositionFieldInput() : fn::FieldInput(CPPType::get<float3>(), "Position")
  {
    category_ = Category::Generated;
  }

  GVArray get_varray_for_context(const FieldContext &context,
    const IndexMask & /* mask */,
    ResourceScope & /* scope */) const final
  {
    if (const bke::SculptFieldContext* sculpt_context =
      dynamic_cast<const bke::SculptFieldContext*>(&context)) {
      return VArray<float3>::ForSpan(sculpt_context->positions());
    }

    return {};
  }
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Vector>("Position").field_source();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  if (params.user_data()->call_data->sculpt_data) {
    Field<float3> position_field{ std::make_shared<SculptPositionFieldInput>() };
    params.set_output("Position", std::move(position_field));
  }
  else {
    Field<float3> position_field = AttributeFieldInput::Create<float3>("position");
    params.set_output("Position", std::move(position_field));
  }
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, GEO_NODE_INPUT_POSITION, "Position", NODE_CLASS_INPUT);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_position_cc
