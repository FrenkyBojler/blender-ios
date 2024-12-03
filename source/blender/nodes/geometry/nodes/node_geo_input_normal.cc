/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "BKE_paint_bvh.hh"

#include "bmesh.hh"

struct BMVert;

namespace blender::nodes::node_geo_input_normal_cc {

class SculptNormalFieldInput final : public fn::FieldInput {
 public:
  SculptNormalFieldInput() : fn::FieldInput(CPPType::get<float3>(), "Normal")
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

      const Depsgraph &depsgraph = mesh_sculpt_context->depsgraph();
      const Object &object = mesh_sculpt_context->object();
      const Span<int> indices = mesh_sculpt_context->indices();

      const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, object);
      Array<float3> normals(indices.size());

      for (const int i : normals.index_range()) {
        normals[i] = vert_normals[indices[i]];
      }

      return VArray<float3>::ForContainer(normals);
    }

    if (const bke::GridsSculptFieldContext *grids_sculpt_context =
            dynamic_cast<const bke::GridsSculptFieldContext *>(&context))
    {

      const SubdivCCG &subdiv_ccg = grids_sculpt_context->subdiv_ccg();
      const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
      const Span<int> grids = grids_sculpt_context->grids();
      const Span<float3> ccg_normals = subdiv_ccg.normals;

      const int total_vertices = grids.size() * key.grid_area;
      Array<float3> normals_array(total_vertices);
      MutableSpan<float3> normals(normals_array);

      for (const int i : grids.index_range()) {
        const Span<float3> grid_normals = ccg_normals.slice(bke::ccg::grid_range(key, grids[i]));
        const MutableSpan<float3> node_normals = normals.slice(bke::ccg::grid_range(key, i));
        node_normals.copy_from(grid_normals);
      }
      return VArray<float3>::ForSpan(normals);
    }

    if (const bke::BMeshSculptFieldContext *bmesh_sculpt_context =
            dynamic_cast<const bke::BMeshSculptFieldContext *>(&context))
    {

      const Set<BMVert *, 0> &verts = bmesh_sculpt_context->verts();
      Array<float3> normals(verts.size());

      int i = 0;
      for (const BMVert *vert : verts) {
        normals[i] = float3(vert->no);
        i++;
      }

      return VArray<float3>::ForContainer(normals);
    }

    return {};
  }
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Vector>("Normal").field_source();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  if (params.user_data()->call_data->sculpt_data) {
    Field<float3> normal_field{std::make_shared<SculptNormalFieldInput>()};
    params.set_output("Normal", std::move(normal_field));
  }
  else {
    Field<float3> normal_field{std::make_shared<bke::NormalFieldInput>()};
    params.set_output("Normal", std::move(normal_field));
  }
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, GEO_NODE_INPUT_NORMAL, "Normal", NODE_CLASS_INPUT);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_normal_cc
