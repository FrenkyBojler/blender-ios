/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <queue>

#include "atomic_ops.h"

#include "BKE_mesh.hh"
// #include "BKE_mesh_mapping.hh"

#include "BLI_array.hh"
#include "BLI_offset_indices.hh"
#include "BLI_virtual_array.hh"
// #include "BLI_atomic_disjoint_set.hh"
// #include "BLI_math_vector_types.hh"
// #include "BLI_stack.hh"
// #include "BLI_vector.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_maximum_flow_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Int>("Source Index")
      .description("The index of the source element in the graph");
  b.add_input<decl::Int>("Sink Index").description("The index of the sink element in the graph");
  b.add_input<decl::Float>("Edge Capacity").default_value(1.0f).hide_value().supports_field();
  b.add_output<decl::Float>("Maximum Flow")
      .field_source()
      .description(
          "The maximum amount of possible flow for a given edge. This is always less than or "
          "equal to "
          "edge capacity.");
}

struct Edge {
  int from, to;
};

static Array<int> create_reverse_offsets(const Span<int> indices, const int items_num)
{
  Array<int> offsets(items_num + 1, 0);
  offset_indices::build_reverse_offsets(indices, offsets);
  return offsets;
}

GroupedSpan<int> build_directed_vert_to_edge_map(const Span<Edge> edges,
                                                 const int verts_num,
                                                 Array<int> &r_offsets,
                                                 Array<int> &r_indices)
{
  r_offsets = create_reverse_offsets(edges.cast<int>(), verts_num);
  const OffsetIndices<int> offsets(r_offsets);
  r_indices.reinitialize(offsets.total_size());

  /* Version of #reverse_indices_in_groups that accounts for storing two indices for each edge. */
  Array<int> counts(offsets.size(), 0);
  threading::parallel_for(edges.index_range(), 1024, [&](const IndexRange range) {
    for (const int64_t edge : range) {
      const int vert = edges[edge].from;
      const int index_in_group = atomic_fetch_and_add_int32(&counts[vert], 1);
      r_indices[offsets[vert][index_in_group]] = int(edge);
    }
  });
  offset_indices::sort_small_groups(offsets, r_indices);
  return {offsets, r_indices};
}

class MaximumFlowFieldInput final : public bke::MeshFieldInput {
 private:
  int source_;
  int sink_;
  Field<float> capacities_;

 public:
  MaximumFlowFieldInput(const int source_index, const int sink_index, Field<float> capacities)
      : bke::MeshFieldInput(CPPType::get<float>(), "Minimum Cut Field"),
        source_(source_index),
        sink_(sink_index),
        capacities_(capacities)
  {
    category_ = Category::Generated;
  }

  GVArray get_varray_for_context(const Mesh &mesh,
                                 const AttrDomain domain,
                                 const IndexMask & /*mask*/) const final
  {
    const IndexRange verts(mesh.verts_num);
    if (!verts.contains(sink_)) {
      return mesh.attributes().adapt_domain<float>(
          VArray<float>::from_single(0.0f, mesh.edges_num), AttrDomain::Edge, domain);
    }
    if (!verts.contains(source_)) {
      return mesh.attributes().adapt_domain<float>(
          VArray<float>::from_single(0.0f, mesh.edges_num), AttrDomain::Edge, domain);
    }
    const bke::MeshFieldContext edge_context{mesh, AttrDomain::Edge};
    fn::FieldEvaluator edge_evaluator{edge_context, mesh.edges_num};
    edge_evaluator.add(capacities_);
    edge_evaluator.evaluate();
    const VArray<float> input_capacities = edge_evaluator.get_evaluated<float>(0);

    const Span<int2> mesh_edges = mesh.edges();

    /* Compute the maximum flow. Code based on
     * https://en.wikipedia.org/wiki/Edmonds%E2%80%93Karp_algorithm#Pseudocode. */
    float total_flow = 0.0f;
    /* Forward and backwards edges. */
    Array<Edge> edges(mesh.edges_num * 2);
    Array<int> reverse_edge(edges.size());
    Array<float> capacities(edges.size());
    for (const int i : IndexRange(mesh.edges_num)) {
      const int2 edge = mesh_edges[i];

      reverse_edge[i] = i + mesh.edges_num;
      reverse_edge[i + mesh.edges_num] = i;

      edges[i] = {edge.x, edge.y};
      edges[reverse_edge[i]] = {edge.y, edge.x};

      capacities[i] = input_capacities[i];
      capacities[reverse_edge[i]] = capacities[i];
    }
    Array<int> vert_to_outgoing_edge_offset_data;
    Array<int> vert_to_outgoing_edge_indices;
    const GroupedSpan<int> vert_outgoing_edge_map = build_directed_vert_to_edge_map(
        edges, mesh.verts_num, vert_to_outgoing_edge_offset_data, vert_to_outgoing_edge_indices);

    Array<float> flow(edges.size(), 0.0f);
    Array<int> edge_taken_to(mesh.verts_num);
    do {
      std::queue<int> vert_queue;
      vert_queue.push(source_);
      edge_taken_to.fill(-1);
      while (vert_queue.size() > 0 && edge_taken_to[sink_] == -1) {
        const int vert = vert_queue.front();
        vert_queue.pop();
        for (const int edge : vert_outgoing_edge_map[vert]) {
          if (edge == -1) {
            continue;
          }
          const int next_vert = edges[edge].to;
          if (edge_taken_to[next_vert] == -1 && next_vert != source_ &&
              capacities[edge] > flow[edge])
          {
            edge_taken_to[next_vert] = edge;
            vert_queue.push(next_vert);
          }
        }
      }

      if (edge_taken_to[sink_] != -1) {
        float new_flow = FLT_MAX;
        for (int edge = edge_taken_to[sink_]; edge != -1; edge = edge_taken_to[edges[edge].from]) {
          new_flow = std::min(new_flow, capacities[edge] - flow[edge]);
        }
        for (int edge = edge_taken_to[sink_]; edge != -1; edge = edge_taken_to[edges[edge].from]) {
          flow[edge] += new_flow;
          flow[reverse_edge[edge]] -= new_flow;
        }
        total_flow += new_flow;
      }
    } while (edge_taken_to[sink_] != -1);

    Array<float> net_flow(mesh.edges_num, 0.0f);
    for (const int edge : IndexRange(mesh.edges_num)) {
      net_flow[edge] = std::abs(flow[edge] - flow[reverse_edge[edge]]) / 2.0f;
    }

    return mesh.attributes().adapt_domain<float>(
        VArray<float>::from_container(net_flow), AttrDomain::Edge, domain);
  }

  uint64_t hash() const override
  {
    return get_default_hash(source_, sink_, capacities_);
  }

  bool is_equal_to(const fn::FieldNode &other) const override
  {
    if (const MaximumFlowFieldInput *other_field = dynamic_cast<const MaximumFlowFieldInput *>(
            &other))
    {
      return other_field->source_ == source_ && other_field->sink_ == sink_ &&
             other_field->capacities_ == capacities_;
    }
    return false;
  }

  std::optional<AttrDomain> preferred_domain(const Mesh & /*mesh*/) const override
  {
    return AttrDomain::Edge;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  const int source_index = params.extract_input<int>("Source Index");
  const int sink_index = params.extract_input<int>("Sink Index");
  Field<float> capacities = params.extract_input<Field<float>>("Edge Capacity");
  Field<float> maximum_flow_field{
      std::make_shared<MaximumFlowFieldInput>(source_index, sink_index, capacities)};
  params.set_output("Maximum Flow", std::move(maximum_flow_field));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeInputMaximumFlow");
  ntype.ui_name = "Maximum Flow";
  ntype.ui_description =
      "Compute the maximum flow over the edges in the mesh where each edge has a given capacity";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_maximum_flow_cc
