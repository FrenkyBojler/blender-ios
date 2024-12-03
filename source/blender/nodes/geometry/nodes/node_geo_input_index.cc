/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_index_cc {

class SculptIndexFieldInput final : public fn::FieldInput {
 public:
  SculptIndexFieldInput() : fn::FieldInput(CPPType::get<int>(), "Index")
  {
    category_ = Category::Generated;
  }

  GVArray get_varray_for_context(const FieldContext &context,
                                 const IndexMask & /* mask */,
                                 ResourceScope & /* scope */) const final
  {
    if (const bke::MeshSculptFieldContext *mesh_sculpt_context =
            dynamic_cast<const bke::MeshSculptFieldContext *>(&context))
    {
      return VArray<int>::ForSpan(mesh_sculpt_context->indices());
    }

    return {};
  }
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Int>("Index").field_source();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  if (params.user_data()->call_data->sculpt_data) {
    Field<int> index_field = {std::make_shared<SculptIndexFieldInput>()};
  }
  else {
    Field<int> index_field{std::make_shared<fn::IndexFieldInput>()};
    params.set_output("Index", std::move(index_field));
  }
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, GEO_NODE_INPUT_INDEX, "Index", NODE_CLASS_INPUT);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_index_cc
