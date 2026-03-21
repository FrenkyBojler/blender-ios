/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <queue>

#include "BLI_timeit.hh"

#include "BLI_bounds.hh"
#include "BLI_array_utils.hh"
#include "BLI_disjoint_set.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_math_quaternion.hh"
#include "BLI_math_vector.h"
#include "BLI_ordered_edge.hh"

#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_mesh_edge_angle_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Int>("Connectivity");
  b.add_input<decl::Int>("Step");

  b.add_input<decl::Bool>("Selection").supports_field();

  b.add_output<decl::Float>("Unsigned Angle")
      .field_source()
      .description(
          "The shortest angle in radians between two faces where they meet at an edge. Flat edges "
          "and Non-manifold edges have an angle of zero. Computing this value is faster than the "
          "signed angle");
  b.add_output<decl::Float>("Signed Angle")
      .field_source()
      .description(
          "The signed angle in radians between two faces where they meet at an edge. Flat edges "
          "and Non-manifold edges have an angle of zero. Concave angles are positive and convex "
          "angles are negative. Computing this value is slower than the unsigned angle");

  b.add_output<decl::Bool>("Mask").field_source_reference_all();
}

static Array<int2> create_edge_map(const OffsetIndices<int> faces,
                                   const Span<int> corner_edges,
                                   const int total_edges)
{
  Array<int2> edge_map(total_edges, int2(-1));

  for (const int i_face : faces.index_range()) {
    for (const int edge : corner_edges.slice(faces[i_face])) {
      int2 &entry = edge_map[edge];
      if (entry[0] == -1) {
        entry[0] = i_face;
      }
      else if (entry[1] == -1) {
        entry[1] = i_face;
      }
      else {
        entry = int2(-2);
      }
    }
  }
  return edge_map;
}

class AngleFieldInput final : public bke::MeshFieldInput {
 public:
  AngleFieldInput() : bke::MeshFieldInput(CPPType::get<float>(), "Unsigned Angle Field")
  {
    category_ = Category::Generated;
  }

  GVArray get_varray_for_context(const Mesh &mesh,
                                 const AttrDomain domain,
                                 const IndexMask & /*mask*/) const final
  {
    const Span<float3> positions = mesh.vert_positions();
    const OffsetIndices faces = mesh.faces();
    const Span<int> corner_verts = mesh.corner_verts();
    const Span<int> corner_edges = mesh.corner_edges();
    Array<int2> edge_map = create_edge_map(faces, corner_edges, mesh.edges_num);

    auto angle_fn =
        [edge_map = std::move(edge_map), positions, faces, corner_verts](const int i) -> float {
      if (edge_map[i][0] < 0 || edge_map[i][1] < 0) {
        return 0.0f;
      }
      const IndexRange face_1 = faces[edge_map[i][0]];
      const IndexRange face_2 = faces[edge_map[i][1]];
      const float3 normal_1 = bke::mesh::face_normal_calc(positions, corner_verts.slice(face_1));
      const float3 normal_2 = bke::mesh::face_normal_calc(positions, corner_verts.slice(face_2));
      return angle_normalized_v3v3(normal_1, normal_2);
    };

    VArray<float> angles = VArray<float>::from_func(mesh.edges_num, angle_fn);
    return mesh.attributes().adapt_domain<float>(std::move(angles), AttrDomain::Edge, domain);
  }

  uint64_t hash() const override
  {
    /* Some random constant hash. */
    return 32426725235;
  }

  bool is_equal_to(const fn::FieldNode &other) const override
  {
    return dynamic_cast<const AngleFieldInput *>(&other) != nullptr;
  }

  std::optional<AttrDomain> preferred_domain(const Mesh & /*mesh*/) const override
  {
    return AttrDomain::Edge;
  }
};

static int find_other_vert_of_edge_triangle(const OffsetIndices<int> faces,
                                            const Span<int> corner_verts,
                                            const Span<int3> corner_tris,
                                            const int face_index,
                                            const int2 edge)
{
  const OrderedEdge ordered_edge(edge);
  for (const int tri_index : bke::mesh::face_triangles_range(faces, face_index)) {
    const int3 &tri = corner_tris[tri_index];
    const int3 vert_tri(corner_verts[tri[0]], corner_verts[tri[1]], corner_verts[tri[2]]);
    if (ordered_edge == OrderedEdge(vert_tri[0], vert_tri[1])) {
      return vert_tri[2];
    }
    if (ordered_edge == OrderedEdge(vert_tri[1], vert_tri[2])) {
      return vert_tri[0];
    }
    if (ordered_edge == OrderedEdge(vert_tri[2], vert_tri[0])) {
      return vert_tri[1];
    }
  }
  BLI_assert_unreachable();
  return -1;
}

class SignedAngleFieldInput final : public bke::MeshFieldInput {
 public:
  SignedAngleFieldInput() : bke::MeshFieldInput(CPPType::get<float>(), "Signed Angle Field")
  {
    category_ = Category::Generated;
  }

  GVArray get_varray_for_context(const Mesh &mesh,
                                 const AttrDomain domain,
                                 const IndexMask & /*mask*/) const final
  {
    const Span<float3> positions = mesh.vert_positions();
    const Span<int2> edges = mesh.edges();
    const OffsetIndices faces = mesh.faces();
    const Span<int> corner_verts = mesh.corner_verts();
    const Span<int> corner_edges = mesh.corner_edges();
    const Span<int3> corner_tris = mesh.corner_tris();
    Array<int2> edge_map = create_edge_map(faces, corner_edges, mesh.edges_num);

    auto angle_fn =
        [edge_map = std::move(edge_map), positions, edges, faces, corner_verts, corner_tris](
            const int i) -> float {
      if (edge_map[i][0] < 0 || edge_map[i][1] < 0) {
        return 0.0f;
      }
      const int face_index_1 = edge_map[i][0];
      const int face_index_2 = edge_map[i][1];
      const IndexRange face_1 = faces[face_index_1];
      const IndexRange face_2 = faces[face_index_2];

      /* Find the normals of the 2 faces. */
      const float3 face_1_normal = bke::mesh::face_normal_calc(positions,
                                                               corner_verts.slice(face_1));
      const float3 face_2_normal = bke::mesh::face_normal_calc(positions,
                                                               corner_verts.slice(face_2));

      /* Find the centerpoint of the axis edge */
      const float3 edge_centerpoint = math::midpoint(positions[edges[i][0]],
                                                     positions[edges[i][1]]);

      /* Use the third point of the triangle connected to the edge in face 2 to determine a
       * reference point for the concavity test. */
      const int tri_other_vert = find_other_vert_of_edge_triangle(
          faces, corner_verts, corner_tris, face_index_2, edges[i]);
      const float3 face_2_tangent = math::normalize(positions[tri_other_vert] - edge_centerpoint);
      const float concavity = math::dot(face_1_normal, face_2_tangent);

      /* Get the unsigned angle between the two faces */
      const float angle = angle_normalized_v3v3(face_1_normal, face_2_normal);

      if (angle == 0.0f || angle == 2.0f * M_PI || concavity < 0) {
        return angle;
      }
      return -angle;
    };

    VArray<float> angles = VArray<float>::from_func(mesh.edges_num, angle_fn);
    return mesh.attributes().adapt_domain<float>(std::move(angles), AttrDomain::Edge, domain);
  }

  uint64_t hash() const override
  {
    /* Some random constant hash. */
    return 68465416863;
  }

  bool is_equal_to(const fn::FieldNode &other) const override
  {
    return dynamic_cast<const SignedAngleFieldInput *>(&other) != nullptr;
  }

  std::optional<AttrDomain> preferred_domain(const Mesh & /*mesh*/) const override
  {
    return AttrDomain::Edge;
  }
};

using EdgeLookup = VectorSet<OrderedEdge,
                             0,
                             DefaultProbingStrategy,
                             DefaultHash<OrderedEdge>,
                             DefaultEquality<OrderedEdge>,
                             SimpleVectorSetSlot<OrderedEdge, int>,
                             GuardedAllocator>;

using VertPriority = std::pair<int, int>;

#if (0)

struct EdgeInfo {
  Vector<std::shared_ptr<Vector<int>>> known_rings;
};

template<typename FuncT>
static bool shortest_paths(const GroupedSpan<int> vert_to_verts,
                           const int start,
                           const int end,
                           const FuncT edge_func,
                           MutableSpan<bool> r_visited,
                           MutableSpan<int> r_prev_index,
                           MutableSpan<int> r_distance)
{
  std::vector<VertPriority> queue_buffer;
  queue_buffer.reserve(r_visited.size());
  std::priority_queue<VertPriority, std::vector<VertPriority>, std::greater<>> queue(
      std::greater<>{}, std::move(queue_buffer));
  r_distance[start] = 0;
  queue.emplace(0, start);

  while (!queue.empty()) {
    const int cost_i = queue.top().first;
    const int vert_i = queue.top().second;
    queue.pop();
    if (r_visited[vert_i]) {
      continue;
    }
    r_visited[vert_i] = true;

    if (vert_i == end) {
      return true;
    }

    for (const int neighbor_vert_i : vert_to_verts[vert_i]) {
      if (r_visited[neighbor_vert_i]) {
        continue;
      }

      const int new_neighbor_cost = cost_i + 1;
      if (!(new_neighbor_cost < r_distance[neighbor_vert_i])) {
        continue;
      }

      if (!edge_func(vert_i, neighbor_vert_i)) {
        continue;
      }

      r_distance[neighbor_vert_i] = new_neighbor_cost;
      r_prev_index[neighbor_vert_i] = vert_i;
      queue.emplace(new_neighbor_cost, neighbor_vert_i);
    }
  }

  return false;
}

static Vector<Vector<int>> rings_for(const GroupedSpan<int> vert_to_verts,
                                     const Span<int2> edges,
                                     const Span<bool> edge_mask,
                                     const EdgeLookup &edge_lookup,
                                     const int start,
                                     const int end,
                                     MutableSpan<int8_t> edges_flow)
{
  Array<bool> visited(vert_to_verts.size());
  Array<int> prev_verts(vert_to_verts.size());
  Array<int> distance(vert_to_verts.size());

  int total_flows = 0;
  for ([[maybe_unused]] const int flow_i : IndexRange(max_flow_value)) {

    visited.as_mutable_span().fill(false);
    prev_verts.as_mutable_span().fill(-1);
    distance.as_mutable_span().fill(std::numeric_limits<int>::max());

    const bool path_found = shortest_paths(
        vert_to_verts,
        start,
        end,
        [&](const int vert_a, const int vect_b) -> bool {
          const int2 path_edge(vert_a, vect_b);

          const int edge_i = edge_lookup.index_of(OrderedEdge(path_edge));
          if (!edge_mask[edge_i]) {
            return false;
          }

          const int2 edge = edges[edge_i];

          BLI_assert(OrderedEdge(edge) == OrderedEdge(path_edge));
          const int8_t flow_sign = (edge == path_edge) ? 1 : -1;
          return math::abs(flow_sign + edges_flow[edge_i]) <= 1;
        },
        visited,
        prev_verts,
        distance);

    if (!path_found) {
      break;
    }

    total_flows++;

    int iter = end;
#  ifndef NDEBUG
    Vector<int, 32> path;
#  endif
    while (iter != start) {
      BLI_assert(path.size() <= vert_to_verts.size());
#  ifndef NDEBUG
      path.append(iter);
#  endif

      const int next = iter;
      iter = prev_verts[iter];
      const int current = iter;

      const int edge_i = edge_lookup.index_of(OrderedEdge(current, next));
      const int2 edge = edges[edge_i];

      const int2 path_edge(current, next);
      BLI_assert(OrderedEdge(edge) == OrderedEdge(path_edge));
      const int8_t flow_sign = (edge == path_edge) ? 1 : -1;
      edges_flow[edge_i] += flow_sign;
    }

#  ifndef NDEBUG
    path.append(start);
    BLI_assert(VectorSet<int>(path.as_span()).size() == path.size());
#  endif
  }

  return total_flows;
}

static void in_edge_connectivity(const GroupedSpan<int> vert_to_verts,
                                 const Span<int2> edges,
                                 const Span<bool> edge_mask,
                                 const EdgeLookup &edge_lookup)
{
  Array<EdgeInfo> edges_info(edges.size());

  Array<int8_t> max_flows(edges.size());
  Array<int8_t> flows(edges.size(), 0);

  for (const int edge_i : IndexRange(edges_num)) {
    if (!edge_mask[edge_i]) {
      continue;
    }

    const int2 edge = edges[edge_i];

    const Span<int> start_verts = vert_to_verts[edge.x];
    const int start_degree = std::count_if(
        start_verts.begin(), start_verts.end(), [&](const int vert) {
          return edge_mask[edge_lookup.index_of(int2(edge.x, vert))];
        });

    const Span<int> end_verts = vert_to_verts[edge.y];
    const int end_degree = std::count_if(end_verts.begin(), end_verts.end(), [&](const int vert) {
      return edge_mask[edge_lookup.index_of(int2(edge.y, vert))];
    });

    const int max_flow_lim = math::min<int>(start_degree, end_degree);

    const EdgeInfo &info = edges_info[edge_i];
    BLI_assert(info.known_rings.size() <= max_flow_lim - 1);
    if (info.known_rings.size() == max_flow_lim - 1) {
      max_flows[edge_i] = max_flow_lim;
      continue;
    }

    BLI_assert(
        std::all_of(flows.begin(), flows.end(), [&](const int8_t value) { return value == 0; }));

    for (const auto &ring in info.known_rings) {
      const Span<int> ring_verts = ring->as_span();
      for (const int vert_i : ring_verts.index_range()) {
        const int vert_index = ring_verts[vert_i];
        const int next_vert =
            ring_verts[math::pereodic_module<int>(vert_i + 1, ring_verts.size())];
        const OrderedEdge ring_edge(vert_index, next_vert);
        const int edge_index = edge_lookup.index_of(ring_edge);
        const int8_t flow_sign = edges[edge_index] == int2(vert_index, next_vert);
        flows[edge_index] += flow_sign;
      }
    }

    flows[edge_i] = 0;
    BLI_assert(std::all_of(
        flows.begin(), flows.end(), [&](const int8_t value) { return ELEM(value, -1, 0, 1); }));

    Vector<Vector<int>> new_edge_rings = rings_for(edge_i, flows);

    max_flows[edge_i] = 1 + new_edge_rings.size();

    info.known_rings.clear();
    for (Vector<int> &ring : new_edge_rings) {
      info.known_rings.append(std::make_shared(std::move(ring)));
    }
    new_edge_rings.clear();

    for (const auto &ring in info.known_rings) {
      const Span<int> ring_verts = ring->as_span();
      for (const int vert_i : ring_verts.index_range()) {
        const int vert_index = ring_verts[vert_i];
        const int next_vert =
            ring_verts[math::pereodic_module<int>(vert_i + 1, ring_verts.size())];
        const OrderedEdge ring_edge(vert_index, next_vert);
        const int edge_index = edge_lookup.index_of(ring_edge);
        const int8_t flow_sign = edges[edge_index] == int2(vert_index, next_vert);
        flows[edge_index] -= flow_sign;

        if (edges_info[edge_index].known_rings.is_empty()) {
          edges_info[edge_index].known_rings.append(ring);
        }
      }
    }
  }
}

#endif

template<typename T>
class OffsetQueue {  
 public:
  using Storage = Map<int, Vector<T>, 10>;
 private:
  Storage radix_data_;
  std::optional<int> lowest_key_;

 public:
  bool is_empty() const
  {
    return radix_data_.is_empty();
  }

  void push(const int key, T value)
  {
    radix_data_.lookup_or_add(key, {}).append(std::move(value));
    
    if (lowest_key_.has_value()) {
      lowest_key_ = math::min<int>(*lowest_key_, key);
    }
  }

  template<typename Func>
  void try_drop_front(const Func &func)
  {
    BLI_assert(!this->is_empty());
    
    if (lowest_key_.has_value()) {
      Vector<T> &values = radix_data_.lookup(*lowest_key_);

      values.remove_if(func);

      if (values.is_empty()) {
        radix_data_.remove(*lowest_key_);
        lowest_key_ = std::nullopt;
      }

      return;
    }
  }

  std::pair<int, T> pop()
  {
    BLI_assert(!this->is_empty());
    
    if (lowest_key_.has_value()) {
      Vector<T> &values = radix_data_.lookup(*lowest_key_);
      BLI_assert(!values.is_empty());
      
      T value = values.pop_last();
      const int value_key = *lowest_key_;
      if (values.is_empty()) {
        radix_data_.remove(*lowest_key_);
        lowest_key_ = std::nullopt;
      }

      return std::make_pair(value_key, std::move(value));
    }
    
    std::optional<std::pair<int, Vector<T> *>> lowest_key;
    for (const auto item : radix_data_.items()) {
      lowest_key = std::make_pair(item.key, &item.value);
      break;
    }
    for (const auto item : radix_data_.items()) {
      if (lowest_key->first > item.key) {
        lowest_key = std::make_pair(item.key, &item.value);
      }
    }
    
    T value = lowest_key->second->pop_last();
    if (lowest_key->second->is_empty()) {
      radix_data_.remove(lowest_key->first);
    } else {
      lowest_key_ = lowest_key->first;
    }
    return std::make_pair(lowest_key->first, std::move(value));
  }
};

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

template<typename T> static int count_if(const Span<int> indices, const Span<T> values)
{
  return std::count_if(
      indices.begin(), indices.end(), [&](const int index) { return bool(values[index]); });
}

static int max_flow_of(const GroupedSpan<int> vert_to_verts,
                       const GroupedSpan<int> vert_to_edges,
                       const Span<int2> edges,
                       const Span<bool> edge_mask,
                       const int index,
                       const std::optional<int> check_value = std::nullopt,
                       Vector<Vector<int>> *r_edge_pathes = nullptr)
{
  const int start = edges[index].x;
  const int end = edges[index].y;

  const int max_flow_value = math::min<int>(count_if(vert_to_edges[start], edge_mask),
                                            count_if(vert_to_edges[end], edge_mask));
  if (max_flow_value == 0) {
    return 0;
  }

  Array<bool, 0> visited(vert_to_verts.size());
  Array<int, 0> prev_verts(vert_to_verts.size());
  Array<int, 0> distance(vert_to_verts.size());
  Array<int8_t, 0> edges_flow(edges.size(), 0);

  Vector<int, 0> buffer_current_queue;
  Vector<int, 0> buffer_next_queue;

  buffer_current_queue.reserve(1000);
  buffer_next_queue.reserve(1000);

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

static Bounds<int> gather_min_max(const Span<int> indices, const Span<int> values)
{
  Bounds<int> min_max_value(values[indices.first()], values[indices.first()]);
  for (const int index : indices.drop_front(1)) {
    math::min_max(values[index], min_max_value.min, min_max_value.max);
  }
  return min_max_value;
}

template<typename T, typename TConvert>
static int64_t max_element_of(const Span<T> elements,  const int64_t grain_size, const TConvert &convert)
{
  return threading::parallel_reduce(
           elements.index_range().drop_front(1),
           grain_size,
           std::make_pair(convert(elements.first()), 0),
           [&](const IndexRange range, auto value) {
             for (const int index : range) {
               auto item_value = convert(elements[index]);
               if (item_value > value.first) {
                 value.first = std::move(item_value);
                 value.second = index;
               }
               else if (item_value == value.first) {
                 value.second = math::min(value.second, index);
               }
             }
             return value;
           },
           [&](const auto &a, const auto &b) {
             if (a.first < b.first) {
               return b;
             }
             else if (a.first == b.first) {
               return std::make_pair(a.first, math::min(a.second, b.second));
             }
             return a;
           }).second;
}

template<typename T, typename TConvert>
static int64_t min_element_of(const Span<T> elements,  const int64_t grain_size, const TConvert &convert)
{
  return threading::parallel_reduce(
           elements.index_range().drop_front(1),
           grain_size,
           std::make_pair(convert(elements.first()), 0),
           [&](const IndexRange range, auto value) {
             for (const int index : range) {
               auto item_value = convert(elements[index]);
               if (item_value < value.first) {
                 value.first = std::move(item_value);
                 value.second = index;
               }
               else if (item_value == value.first) {
                 value.second = math::min(value.second, index);
               }
             }
             return value;
           },
           [&](const auto &a, const auto &b) {
             if (a.first > b.first) {
               return b;
             }
             else if (a.first == b.first) {
               return std::make_pair(a.first, math::min(a.second, b.second));
             }
             return a;
           }).second;
}

class MaskFieldInput final : public bke::MeshFieldInput {
  int connectivity_;
  int step_;
  Field<bool> selection_field_;

 public:
  MaskFieldInput(const int connectivity, const int step, Field<bool> selection_field)
      : bke::MeshFieldInput(CPPType::get<bool>(), "Mask Field"),
        connectivity_(connectivity),
        step_(step),
        selection_field_(std::move(selection_field))
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
    GroupedSpan<int> vert_to_edges;
    {
      SCOPED_TIMER_AVERAGED("build_vert_to_edge_map");
      vert_to_edges = bke::mesh::build_vert_to_edge_map(
          edges, mesh.verts_num, vert_to_edge_offset_data, vert_to_edge_indices);
    }

    Array<int> other_vertex(vert_to_edges.data.size());
    threading::parallel_for(vert_to_edges.index_range(), 2048, [&](const IndexRange range) {
      for (const int vert_i : range) {
        for (const int edge_i : vert_to_edges.offsets[vert_i]) {
          other_vertex[edge_i] = bke::mesh::edge_other_vert(edges[vert_to_edges.data[edge_i]],
                                                            vert_i);
        }
      }
    });
    const GroupedSpan<int> vert_to_verts(vert_to_edges.offsets, other_vertex.as_span());

    const bke::MeshFieldContext context(mesh, domain);
    FieldEvaluator evaluator(context, mesh.edges_num);
    evaluator.set_selection(selection_field_);
    evaluator.evaluate();
    const IndexMask selection = evaluator.get_evaluated_selection_as_mask();
    printf("Size: %d;\n", int(selection.size()));

    EdgeLookup edge_lookup;
    {
      SCOPED_TIMER_AVERAGED("edge_lookup");
      for (const int2 edge : edges) {
        edge_lookup.add(edge);
      }
    }

    Array<bool> edge_mask(edges.size(), true);
    Array<int> edges_flow(edges.size(), 0);
    {
      SCOPED_TIMER_AVERAGED("initial max_flow");
      threading::parallel_for(edges.index_range(), 20, [&](const IndexRange range) {
        for (const int edge_i : range) {
          const int2 edge = edges[edge_i];
          edges_flow[edge_i] = max_flow_of(vert_to_verts, vert_to_edges, edges, edge_mask, edge_i);
        }
      });
    }

    const Array<int> input_edges_flow = edges_flow.as_span();

    const Bounds<int> connectivity_bounds = *bounds::min_max(edges_flow.as_span());

    if (connectivity_bounds.max <= connectivity_) {
      return VArray<bool>::from_single(true, edges.size());
    }

    Array<Vector<int>> connectivity_to_edges(1);
    // for (const int edge_i : edges.index_range()) {
    //   connectivity_to_edges[edges_flow[edge_i]].append(edge_i);
    // }
    connectivity_to_edges.first().reinitialize(selection.size());
    // array_utils::fill_index_range(connectivity_to_edges.first().as_mutable_span());
    selection.to_indices(connectivity_to_edges.first().as_mutable_span());

    Array<bool> edge_flow_update_state(edges.size());

    int counter = 0;
    [[maybe_unused]] bool stop = false;

    SCOPED_TIMER_AVERAGED("main loop");
    // for (const int r_connectivity_i :
    //      connectivity_to_edges.index_range().drop_back(connectivity_ + 1))
    const int r_connectivity_i = 0;
    {
      const int connectivity_index = connectivity_to_edges.index_range().last(r_connectivity_i);
      SCOPED_TIMER_AVERAGED(" Loop " + std::to_string(connectivity_index) + ";");
      Vector<int> &all_edges_to_drop = connectivity_to_edges[connectivity_index];
      BLI_assert(VectorSet<int>(all_edges_to_drop.as_span()).size() == all_edges_to_drop.size());

      int full_update_size = 0;
      int trace_update_size = 0;
      Map<int, int> changes_per_value;

      while (!all_edges_to_drop.is_empty()) {
        counter++;
        if (step_ <= counter) {
          stop = true;
        }

        if (stop) {
          break;
        }

        BLI_assert(!all_edges_to_drop.is_empty());
        const int next_to_delete_i = min_element_of(
            all_edges_to_drop.as_span(), 900, [&](const int edge_i) {
              BLI_assert(edge_mask[edge_i]);
              // BLI_assert(edges_flow[edge_i] == connectivity_index);

              int smooth_connectivity = 0;
              int total_degree = 0;

              const int2 edge = edges[edge_i];
              for (const Span<int> connected_edges :
                   {vert_to_edges[edge.x], vert_to_edges[edge.y]}) {
                for (const int connected_edge : connected_edges) {
                  total_degree += int(edge_mask[connected_edge]);
                  smooth_connectivity += math::max<int>(0, int(edge_mask[connected_edge]) *
                                         edges_flow[connected_edge]);
                }
              }

              // return smooth_connectivity;
              return float(smooth_connectivity - edges_flow[edge_i] * 2) / (total_degree - 2) * edges_flow[edge_i];
              // return std::make_pair(edges_flow[edge_i], smooth_connectivity / edges_flow[edge_i] / edges_flow[edge_i]);
            });
        const int edge_to_delete = all_edges_to_drop[next_to_delete_i];

        const int edge_flow = edges_flow[edge_to_delete];
        // BLI_assert(edge_flow == connectivity_index);

        if (edge_flow <= connectivity_) {
          all_edges_to_drop.remove_and_reorder(next_to_delete_i);
          continue;
        }

        edge_mask[edge_to_delete] = false;
        all_edges_to_drop.remove_and_reorder(next_to_delete_i);

        full_update_size += all_edges_to_drop.size();

        Vector<Vector<int>> all_pathes;
        max_flow_of(
            vert_to_verts, vert_to_edges, edges, edge_mask, edge_to_delete, -1, &all_pathes);

        VectorSet<int> stack_to_update;
        for (const Span<int> path : all_pathes) {
          for (const int edge_i : path) {
            if (!edge_mask[edge_i]) {
              continue;
            }

            if (edges_flow[edge_i] < edge_flow) {
              continue;
            }

            stack_to_update.add_new(edge_i);
          }
        }

        edge_flow_update_state.fill(false);

        std::atomic_int total;
        while (!stack_to_update.is_empty()) {
          trace_update_size += stack_to_update.size();
          threading::EnumerableThreadSpecific<Vector<int>> next_stacks;
          threading::parallel_for(stack_to_update.index_range(), 2, [&](const IndexRange range) {
            for (const int edge_to_update : stack_to_update.as_span().slice(range)) {
              if (!edge_mask[edge_to_update]) {
                continue;
              }

              if (edges_flow[edge_to_update] < edge_flow) {
                continue;
              }

              if (edge_flow_update_state[edge_to_update]) {
                continue;
              }
              edge_flow_update_state[edge_to_update] = true;

              Vector<Vector<int>> new_pathes;
              const int new_flow = max_flow_of(vert_to_verts,
                                               vert_to_edges,
                                               edges,
                                               edge_mask,
                                               edge_to_update,
                                               edges_flow[edge_to_update],
                                               &new_pathes);
              if (edges_flow[edge_to_update] == new_flow) {
                continue;
              }
              // BLI_assert(ELEM(new_flow, connectivity_index - 1, connectivity_index));

              edges_flow[edge_to_update] = new_flow;

              Vector<int> &local_next_stack = next_stacks.local();
              for (const Span<int> path : new_pathes) {
                for (const int edge_i : path) {
                  if (!edge_mask[edge_i]) {
                    continue;
                  }

                  if (edges_flow[edge_i] < edge_flow) {
                    continue;
                  }

                  if (edge_flow_update_state[edge_i]) {
                    continue;
                  }
                  local_next_stack.append(edge_i);
                }
              }
              total++;
            }
          });

          VectorSet<int> distinct_edges_to_check;
          for (const Span<int> next_stack : next_stacks) {
            distinct_edges_to_check.add_multiple(next_stack);
          }
          stack_to_update = std::move(distinct_edges_to_check);
        }

        // threading::parallel_for(all_edges_to_drop.index_range(), 20, [&](const IndexRange range)
        // {
        //   // int local_total = 0;
        //   for (const int edge_i : all_edges_to_drop.as_span().slice(range)) {
        //     BLI_assert(edge_mask[edge_i]);
        //
        //     if (edge_flow_update_state[edge_i]) {
        //       continue;
        //     }
        //
        //     // const int old_value = edges_flow[edge_i];
        //     BLI_assert(edges_flow[edge_i] == max_flow_of(vert_to_verts, vert_to_edges, edges,
        //     edge_mask, edge_i));
        //     // local_total += int(old_value != edges_flow[edge_i]);
        //   }
        //   // total += local_total;
        // });
        // changes_per_value.add_or_modify(
        //     total, [](int *value) { *value = 1; }, [](int *value) { (*value)++; });
        // 
        // const int prefix_size = std::distance(
        //     all_edges_to_drop.begin(),
        //     std::stable_partition(
        //         all_edges_to_drop.begin(), all_edges_to_drop.end(), [&](const int edge_index) {
        //           BLI_assert(
        //               ELEM(edges_flow[edge_index], connectivity_index - 1, connectivity_index));
        //           return edges_flow[edge_index] == connectivity_index;
        //         }));
        // 
        // connectivity_to_edges[connectivity_index - 1].extend(
        //     all_edges_to_drop.as_span().drop_front(prefix_size));
        // all_edges_to_drop.resize(prefix_size);
      }

      printf("\nLevel: %d;\n", connectivity_index);
      printf("\n Full update: %d; Trace update: %d;\n", full_update_size, trace_update_size);
      Vector<int> keys;
      for (const int key : changes_per_value.keys()) {
        keys.append(key);
      }
      std::sort(keys.begin(), keys.end());
      for (const int key : keys) {
        printf(" %d. Dropped count: %d;\n", changes_per_value.lookup(key), key);
      }
    }

    // threading::parallel_for(edges.index_range(), 20, [&](const IndexRange range) {
    //   for (const int edge_i : range) {
    //     if (!edge_mask[edge_i]) {
    //       continue;
    //     }
    //     if (max_flow_of(vert_to_verts, vert_to_edges, edges, edge_mask, edge_i) ==
    //         math::min<int>(input_edges_flow[edge_i], connectivity_))
    //     {
    //       continue;
    //     }
    //     BLI_assert_unreachable();
    //     throw std::runtime_error("AAAA");
    //   }
    // });

    return VArray<bool>::from_container(std::move(edge_mask));
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  if (params.output_is_required("Unsigned Angle")) {
    Field<float> angle_field{std::make_shared<AngleFieldInput>()};
    params.set_output("Unsigned Angle", std::move(angle_field));
  }
  if (params.output_is_required("Signed Angle")) {
    Field<float> angle_field{std::make_shared<SignedAngleFieldInput>()};
    params.set_output("Signed Angle", std::move(angle_field));
  }

  Field<bool> mask_field{std::make_shared<MaskFieldInput>(params.get_input<int>("Connectivity"),
                                                          params.get_input<int>("Step"),
                                                          params.get_input<Field<bool>>("Selection"))};
  params.set_output("Mask", std::move(mask_field));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeInputMeshEdgeAngle", GEO_NODE_INPUT_MESH_EDGE_ANGLE);
  ntype.ui_name = "Edge Angle";
  ntype.ui_description = "The angle between the normals of connected manifold faces";
  ntype.enum_name_legacy = "MESH_EDGE_ANGLE";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_mesh_edge_angle_cc
