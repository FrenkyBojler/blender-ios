/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_mesh.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_mesh_vertex_curvature_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Minimum Curvature"_ustr)
      .structure_type(StructureType::Field)
      .description(
          "The minimum (most concave) curvature value between a vertex normal and its edges. "
          "Zero indicates no curvature. Negative indicates concave curvature. Positive indicates "
          "convex curvature. Saddle curvature will have minimum and maximum curvatures of "
          "opposite sign");
  b.add_output<decl::Float>("Maximum Curvature"_ustr)
      .structure_type(StructureType::Field)
      .description(
          "The maximum (most convex) curvature value between a vertex normal and its edges. "
          "Zero indicates no curvature. Negative indicates concave curvature. Positive indicates "
          "convex curvature. Saddle curvature will have minimum and maximum curvatures of "
          "opposite sign");
  b.add_output<decl::Float>("Average Signed Curvature"_ustr)
      .structure_type(StructureType::Field)
      .description(
          "The average curvature value between a vertex normal and its edges. "
          "Zero indicates no curvature. Negative indicates concave curvature. Positive indicates "
          "convex curvature. Saddle curvature may have an average curvature close to zero even "
          "if the mesh is highly curved");
  b.add_output<decl::Float>("Average Unsigned Curvature"_ustr)
      .structure_type(StructureType::Field)
      .description("The average magnitude of curvature between a vertex normal and its edges");
}

enum class Statistic { Minimum, Maximum, MeanValue, MeanMagnitude };

class CurvatureInput final : public bke::MeshFieldInput {
 private:
  Statistic mode_;

 public:
  CurvatureInput(Statistic mode)
      : bke::MeshFieldInput(CPPType::get<float>(), "Vertex Curvature"), mode_(mode)
  {
  }

  GVArray get_varray_for_context(const Mesh &mesh,
                                 const AttrDomain domain,
                                 const IndexMask & /*mask*/) const final
  {
    Span<int2> edges = mesh.edges();
    Array<int> map_offsets;
    Array<int> map_indices;
    const GroupedSpan<int> vert_to_edge_map = bke::mesh::build_vert_to_edge_map(
        edges, mesh.verts_num, map_offsets, map_indices);
    Span<float3> positions = mesh.vert_positions();
    Span<float3> normals = mesh.vert_normals_true();

    auto curvature_fn = [vert_to_edge_map = std::move(vert_to_edge_map),
                         positions = std::move(positions),
                         normals = std::move(normals),
                         edges = std::move(edges),
                         mode = mode_](const int vert_i) -> float {
      const Span<int> vert_edges = vert_to_edge_map[vert_i];
      if (vert_edges.is_empty()) {
        return 0.0f;
      }

      float3 p = positions[vert_i];
      float3 n = normals[vert_i];

      std::vector<float> data;
      data.reserve(vert_edges.size());

      for (const int edge_i : vert_edges) {
        int2 edge = edges[edge_i];
        int vert_other_i = edge[0] ^ edge[1] ^ vert_i;
        float3 edge_vec = math::normalize(positions[vert_other_i] - p);
        /* invert so concave is negative, convex is positive */
        data.push_back(-math::dot(n, edge_vec));
      }

      switch (mode) {
        case Statistic::Minimum:
          return *std::min_element(data.begin(), data.end());
        case Statistic::Maximum:
          return *std::max_element(data.begin(), data.end());
        case Statistic::MeanMagnitude:
          return std::accumulate(data.begin(),
                                 data.end(),
                                 0.0f,
                                 [](float sum, float val) { return sum + std::abs(val); }) /
                 (float)data.size();
        default:
          return std::accumulate(data.begin(), data.end(), 0.0f) / (float)data.size();
      }
    };

    return VArray<float>::from_func(mesh.verts_num, curvature_fn);
  }

  void hash_unique(UniqueHashBytes &hash, fn::FieldHashDeep & /*deep_hash_cache*/) const override
  {
    static constexpr int8_t id = 0;
    hash.add(&id);
    hash.add(mode_);
  }

  std::optional<AttrDomain> preferred_domain(const Mesh & /*mesh*/) const override
  {
    return AttrDomain::Point;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  if (params.output_is_required("Minimum Curvature"_ustr)) {
    params.set_output("Minimum Curvature"_ustr,
                      Field<int>::from_input<CurvatureInput>(Statistic::Minimum));
  }
  if (params.output_is_required("Maximum Curvature"_ustr)) {
    params.set_output("Maximum Curvature"_ustr,
                      Field<int>::from_input<CurvatureInput>(Statistic::Maximum));
  }
  if (params.output_is_required("Average Signed Curvature"_ustr)) {
    params.set_output("Average Signed Curvature"_ustr,
                      Field<int>::from_input<CurvatureInput>(Statistic::MeanValue));
  }
  if (params.output_is_required("Average Unsigned Curvature"_ustr)) {
    params.set_output("Average Unsigned Curvature"_ustr,
                      Field<int>::from_input<CurvatureInput>(Statistic::MeanMagnitude));
  }
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeInputMeshVertexCurvature"_ustr);
  ntype.ui_name = "Vertex Curvature";
  ntype.ui_description = "The curvature between a vertex normal and its associated edges";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_mesh_vertex_curvature_cc
