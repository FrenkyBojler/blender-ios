/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_sculpt_vertex_color_cc {

class SculptVertexColorFieldInput final : public fn::FieldInput {
 public:
  SculptVertexColorFieldInput() : fn::FieldInput(CPPType::get<ColorGeometry4f>(), "Vertex Color")
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
      const Span<float4> colors = mesh_sculpt_context->colors();
      return VArray<ColorGeometry4f>::ForSpan(Span<ColorGeometry4f>(
          reinterpret_cast<const ColorGeometry4f *>(colors.data()), colors.size()));
    }

    return {};
  }
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Vertex Color").field_source();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  if (params.user_data()->call_data->sculpt_data) {
    Field<ColorGeometry4f> vertex_color_field{std::make_shared<SculptVertexColorFieldInput>()};
    params.set_output("Vertex Color", std::move(vertex_color_field));
  }

  params.set_default_remaining_outputs();
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSculptVertexColor", GEO_NODE_SCULPT_VERTEX_COLOR, NODE_CLASS_INPUT);
  ntype.ui_name = "Vertex Color";
  ntype.ui_description = "Vertex Color";
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_sculpt_vertex_color_cc
