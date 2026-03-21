/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <queue>

#include "BLI_array_utils.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_task.hh"

#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_edge_connectivity_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Int>("In-Edge Connectivity").field_source().desciption("Maximum size of cut containing edge");
}

template<typename FuncT>
static bool shortest_paths(const GroupedSpan<int> vert_to_verts,
                           const GroupedSpan<int> vert_to_edges,
                           const int start,
                           const int end,
                           const FuncT edge_func,
                           Vector<int, 0> &buffer_current_queue,
                           Vector<int, 0> &buffer_next_queue,
                           MutableSpan<bool> r_visited,
                           MutableSpan<int> r_prev_index,
                           MutableSpan<int> r_distance)
{
  int current_distance = 0;
  int next_distance = 1;
  
  buffer_current_queue.clear();
  buffer_next_queue.clear();
  
  buffer_current_queue.append(start);

  r_distance[start] = 0;

  const OffsetIndices<int> offsets = vert_to_edges.offsets;

  while (!buffer_current_queue.is_empty()) {
    const int vert_i = buffer_current_queue.pop_last();

    if (r_visited[vert_i]) {
      continue;
    }

    r_visited[vert_i] = true;

    if (vert_i == end) {
      return true;
    }

    for (const int neighbor_vert_pos : offsets[vert_i]) {
      const int neighbor_vert_i = vert_to_verts.data[neighbor_vert_pos];
      const int neighbor_edge_i = vert_to_edges.data[neighbor_vert_pos];
      if (r_visited[neighbor_vert_i]) {
        continue;
      }

      if (!(next_distance < r_distance[neighbor_vert_i])) {
        continue;
      }

      if (!edge_func(vert_i, neighbor_vert_i, neighbor_edge_i)) {
        continue;
      }

      r_distance[neighbor_vert_i] = next_distance;
      r_prev_index[neighbor_vert_i] = vert_i;
      buffer_next_queue.append(neighbor_vert_i);
    }

    if (!buffer_current_queue.is_empty()) {
      continue;
    }

    std::swap(buffer_current_queue, buffer_next_queue);
    
    current_distance = next_distance;
    next_distance++;
  }

  return false;
}

static int edge_max_flow(const GroupedSpan<int> vert_to_verts,
                         const GroupedSpan<int> vert_to_edges,
                         const Span<int2> edges,
                         const int edge_index)
{
  const int start = edges[edge_index].x;
  const int end = edges[edge_index].y;

  Array<bool> visited(vert_to_verts.size());
  Array<int> prev_verts(vert_to_verts.size());
  Array<int> distance(vert_to_verts.size());
  Array<int8_t> edges_flow(edges.size(), 0);

  Vector<int> buffer_current_queue;
  Vector<int> buffer_next_queue;

  buffer_current_queue.reserve(1000);
  buffer_next_queue.reserve(1000);

  const int max_flow_value = math::min(vert_to_edges[start].size(), vert_to_edges[end].size()) - 1;

  int total_flows = 0;
  for ([[maybe_unused]] const int flow_i : IndexRange(max_flow_value)) {

    visited.as_mutable_span().fill(false);
    prev_verts.as_mutable_span().fill(-1);
    distance.as_mutable_span().fill(std::numeric_limits<int>::max());

    const bool path_found = shortest_paths(
        vert_to_verts,
        vert_to_edges,
        start,
        end,
        [&](const int current, const int next, const int edge_i) -> bool {
          const int8_t flow_sign = (current < next) ? 1 : -1;
          return edge_mask[edge_i] & (math::abs(edges_flow[edge_i] + flow_sign) <= 1);
        },
        buffer_current_queue,
        buffer_next_queue,
        visited,
        prev_verts,
        distance);

    if (!path_found) {
      break;
    }

    total_flows++;

    int iter = end;
#ifndef NDEBUG
    Vector<int, 32> path;
#endif
    while (iter != start) {
      BLI_assert(path.size() <= vert_to_verts.size());
#ifndef NDEBUG
      path.append(iter);
#endif

      const int next = iter;
      iter = prev_verts[iter];
      const int current = iter;

      const int edge_pos = vert_to_verts[current].first_index(next);
      const int edge_i = vert_to_edges[current][edge_pos];

      const int2 edge = edges[edge_i];

      const int8_t flow_sign = (next < current) ? 1 : -1;
      edges_flow[edge_i] -= flow_sign;
    }

#ifndef NDEBUG
    path.append(start);
    BLI_assert(VectorSet<int>(path.as_span()).size() == path.size());
#endif
  }

  if ((r_edge_pathes != nullptr) && (*check_value != total_flows)) {
    (*r_edge_pathes).clear();

    for ([[maybe_unused]] const int pass_i : IndexRange(total_flows)) {

      visited.as_mutable_span().fill(false);
      prev_verts.as_mutable_span().fill(-1);
      distance.as_mutable_span().fill(std::numeric_limits<int>::max());

      const bool path_found = shortest_paths(
          vert_to_verts,
          vert_to_edges,
          end,
          start,
          [&](const int current, const int next, const int edge_i) -> bool {
            const int8_t flow_sign = (current > next) ? 1 : -1;
            return edge_mask[edge_i] & (edges_flow[edge_i] == flow_sign);
          },
          buffer_current_queue,
          buffer_next_queue,
          visited,
          prev_verts,
          distance);

      BLI_assert(path_found);

      (*r_edge_pathes).append({});

      int iter = start;
      while (iter != end) {
        const int next = iter;
        iter = prev_verts[iter];
        const int current = iter;

        const int edge_pos = vert_to_verts[current].first_index(next);
        const int edge_i = vert_to_edges[current][edge_pos];
        BLI_assert(OrderedEdge(edges[edge_i]) == OrderedEdge(current, next));

        r_edge_pathes->last().append(edge_i);

        BLI_assert(edges_flow[edge_i] != 0);
        edges_flow[edge_i] = 0;
      }

      BLI_assert(VectorSet<int>(r_edge_pathes->last().as_span()).size() ==
                 r_edge_pathes->last().size());
    }
#ifndef NDEBUG
    VectorSet<int> all_found_edges;
    int total_found = 0;
    for (const Span<int> found_edges : *r_edge_pathes) {
      all_found_edges.add_multiple(found_edges);
      total_found += found_edges.size();
    }
    BLI_assert(total_found == all_found_edges.size());
#endif
  }

  return total_flows;
}

class InEdgeConnectivityFieldInput final : public bke::MeshFieldInput {
 public:
  InEdgeConnectivityFieldInput() : bke::MeshFieldInput(CPPType::get<bool>(), "In-Edge Connectivity field")
  {
    category_ = Category::Generated;
  }

  GVArray get_varray_for_context(const Mesh &mesh,
                                 const AttrDomain domain,
                                 const IndexMask & /*mask*/) const final
  {
    const Span<int2> edges = mesh.edges();

    Array<int> vert_to_edge_offset_data;
    Array<int> vert_to_edge_indices;
    GroupedSpan<int> vert_to_edges = bke::mesh::build_vert_to_edge_map(
        edges, mesh.verts_num, vert_to_edge_offset_data, vert_to_edge_indices);

    Array<int> other_vertex(vert_to_edges.data.size());
    threading::parallel_for(vert_to_edges.index_range(), 2048, [&](const IndexRange range) {
      for (const int vert_i : range) {
        for (const int edge_i : vert_to_edges.offsets[vert_i]) {
          other_vertex[edge_i] = bke::mesh::edge_other_vert(edges[vert_to_edges.data[edge_i]], vert_i);
        }
      }
    });
    const GroupedSpan<int> vert_to_verts(vert_to_edges.offsets, other_vertex.as_span());

    Array<int> edges_flow(edges.size());
    threading::parallel_for(edges.index_range(), 20, [&](const IndexRange range) {
      for (const int edge_i : range) {
        const int2 edge = edges[edge_i];
        edges_flow[edge_i] = max_flow_of(vert_to_verts, vert_to_edges, edges, edge_mask, edge_i);
      }
    });
    
    return mesh.attribute().adapt_domain(VArray<int>::from_container(std::move(edges_flow)), AttrDomain::Point, domain);
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  params.set_output("In-Edge Connectivity", std::make_shared<InEdgeConnectivityFieldInput>());
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(
      &ntype, "GeometryNodeInputEdgeConnectivity");
  ntype.ui_name = "Edge Connectivity";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_edge_connectivity_cc
