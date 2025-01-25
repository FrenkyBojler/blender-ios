/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "BKE_mesh_mapping.hh"

#include "BLI_array_utils.hh"
#include "BLI_atomic_disjoint_set.hh"
#include "BLI_vector_set.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_mesh_edge_rings_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Int>("Parallel Group Index")
      .field_source()
      .description("Edge group index of the edge ring from parallel edges");
  b.add_output<decl::Int>("Linear Group Index").field_source();
}

static void make_ordered(const int2 prev, int2 &current)
{
  if (ELEM(current[1], prev[0], prev[1])) {
    std::swap(current[0], current[1]);
  }
}

template<typename T, typename Func> static T &find_if(MutableSpan<T> span, const Func &&func)
{
  return *std::find_if(span.begin(), span.end(), func);
}

template<typename T, typename Func> static const T &find_if(const Span<T> span, const Func &&func)
{
  return *std::find_if(span.begin(), span.end(), func);
}

static Array<int2> corner_edge_pairs(const OffsetIndices<int> faces,
                                     const Span<int2> edges,
                                     const Span<int> corner_edges,
                                     const Span<int> corner_verts,
                                     const int totloop,
                                     const int faces_num)
{
  Array<int2> pairs(totloop);
  threading::parallel_for(IndexRange(faces_num), 1024, [&](const IndexRange range) {
    for (const int face_index : range) {
      const IndexRange face = faces[face_index];
      const Span<int> face_corner_edges = corner_edges.slice(face);
      const Span<int> face_corner_verts = corner_verts.slice(face);
      for (const int corner_i : face.index_range()) {
        const int corner_index = face[corner_i];
        pairs[corner_index][0] = face_corner_edges[corner_i];
        pairs[corner_index][1] = bke::mesh::other_corner_edge(
            edges, face_corner_edges, face_corner_verts, corner_i);
      }
    }
  });
  return pairs;
}

static void sort_corner_edge_pairs(const IndexMask mask,
                                   const OffsetIndices<int> offset,
                                   MutableSpan<int2> r_pairs,
                                   MutableSpan<int> r_indices)
{
  mask.foreach_index(GrainSize(1024), [&](const int vert_index) {
    if (offset[vert_index].is_empty()) {
      return;
    }
    MutableSpan<int2> vertex_stars = r_pairs.slice(offset[vert_index]);
    MutableSpan<int> vertex_star_index = r_indices.slice(offset[vert_index]);
    vertex_star_index.first() = 0;
    for (const int index : vertex_stars.index_range().drop_back(1)) {
      int2 &current = vertex_stars[index];
      MutableSpan<int2> next_range = vertex_stars.drop_front(index + 1);
      vertex_star_index[index + 1] = vertex_star_index[index];
      int2 &next = find_if(next_range,
                           [&](const int2 next) { return ELEM(current[1], next[0], next[1]); });
      if (&next != next_range.end()) {
        make_ordered(current, next);
        std::swap(next_range.first(), next);
        continue;
      }
      /* This is the first element in a non-cyclic star and looking for unconnected edge was
       * failure. Try again. */
      int2 &other_next = find_if(
          next_range, [&](const int2 next) { return ELEM(current[0], next[0], next[1]); });
      if (&other_next != next_range.end()) {
        std::swap(current[0], current[1]);
        make_ordered(current, other_next);
        std::swap(next_range.first(), other_next);
        continue;
      }
      vertex_star_index[index + 1]++;
    }
  });
}

class EdgesLineGroupFieldInput final : public bke::MeshFieldInput {
 public:
  EdgesLineGroupFieldInput() : bke::MeshFieldInput(CPPType::get<int>(), "Edges Line Rings Field")
  {
  }

  GVArray get_varray_for_context(const Mesh &mesh,
                                 const AttrDomain domain,
                                 const IndexMask & /*mask*/) const final
  {
    const Span<int2> edges = mesh.edges();
    const Span<int> corner_edges = mesh.corner_edges();
    const Span<int> corner_verts = mesh.corner_verts();

    Array<int> vert_to_edge_offsets;
    Array<int> vert_to_edge_indices;
    const GroupedSpan<int> vert_to_edge_map = bke::mesh::build_vert_to_edge_map(
        edges, mesh.verts_num, vert_to_edge_offsets, vert_to_edge_indices);

    Array<int> vert_to_loop_offsets;
    Array<int> vert_to_loop_indices;
    bke::mesh::build_vert_to_corner_map(
        corner_verts, mesh.verts_num, vert_to_loop_offsets, vert_to_loop_indices);
    const OffsetIndices<int> vert_to_loop_offset(vert_to_loop_offsets);

    Array<int> edge_faces_total(mesh.edges_num, 0);
    array_utils::count_indices(corner_edges, edge_faces_total.as_mutable_span());

    IndexMaskMemory memory;
    const IndexMask vert_mask = IndexMask::from_predicate(
        IndexMask(mesh.verts_num), GrainSize(1024), memory, [&](const int vert_index) {
          for (const int edge_index : vert_to_edge_map[vert_index]) {
            if (!ELEM(edge_faces_total[edge_index], 0, 1, 2)) {
              return false;
            }
          }
          return true;
        });

    Array<int2> stars_pairs(mesh.corners_num);
    const Array<int2> corner_pairs = corner_edge_pairs(
        mesh.faces(), edges, corner_edges, corner_verts, mesh.corners_num, mesh.faces_num);
    array_utils::gather(
        corner_pairs.as_span(), vert_to_loop_indices.as_span(), stars_pairs.as_mutable_span());

    Array<int> star_indices(mesh.corners_num);
    sort_corner_edge_pairs(vert_mask, vert_to_loop_offset, stars_pairs, star_indices);

    AtomicDisjointSet linear_edges(mesh.edges_num);

    vert_mask.foreach_index(GrainSize(1024), [&](const int vert_index) {
      for (IndexRange range = vert_to_loop_offset[vert_index]; !range.is_empty();) {
        const Span<int> vertex_star_index = star_indices.as_span().slice(range);
        const int &first = vertex_star_index.first();
        const int &end = find_if(vertex_star_index,
                                 [&](const int other) { return other != first; });
        const int size = int(&end - &first);
        const Span<int2> current_edge_star = stars_pairs.as_span().slice(range.take_front(size));
        range = range.drop_front(size);
        const bool is_cyclic = current_edge_star.first()[0] == current_edge_star.last()[1];
        if (!is_cyclic) {
          linear_edges.join(current_edge_star.first()[0], current_edge_star.last()[1]);
          continue;
        }
        if (size % 2) {
          continue;
        }
        const int semicircle_size = size / 2;
        const Span<int2> north_corners = current_edge_star.take_front(semicircle_size);
        const Span<int2> south_corners = current_edge_star.take_back(semicircle_size);
        for (const int index : IndexRange(semicircle_size)) {
          linear_edges.join(north_corners[index][0], south_corners[index][0]);
        }
      }
    });

    threading::parallel_for(IndexRange(mesh.verts_num), 2048, [&](const IndexRange range) {
      for (const int vert_index : range) {
        if (vert_to_edge_map[vert_index].size() != 2) {
          continue;
        }
        if (edge_faces_total[vert_to_edge_map[vert_index][0]] != 0) {
          continue;
        }
        if (edge_faces_total[vert_to_edge_map[vert_index][1]] != 0) {
          continue;
        }
        linear_edges.join(vert_to_edge_map[vert_index][0], vert_to_edge_map[vert_index][1]);
      }
    });

    vert_mask.foreach_index(GrainSize(1024), [&](const int vert_index) {
      if (vert_to_edge_map[vert_index].size() != 4) {
        return;
      }
      const IndexRange range = vert_to_loop_offset[vert_index];
      if (range.size() != 2) {
        return;
      }
      const Span<int> vertex_star_index = star_indices.as_span().slice(range);
      const int &first = vertex_star_index.first();
      const int &end = find_if(vertex_star_index, [&](const int other) { return other != first; });
      const int size = int(&end - &first);
      if (size != range.size()) {
        return;
      }
      const int central_edge = stars_pairs.as_span().slice(range).first()[1];
      const int loos_edge = [&]() {
        for (const int edge_index : vert_to_edge_map[vert_index]) {
          if (edge_faces_total[edge_index] == 0) {
            return edge_index;
          }
        }
        BLI_assert_unreachable();
        return 0;
      }();
      linear_edges.join(central_edge, loos_edge);
    });

    Array<int> edge_group(mesh.edges_num);
    linear_edges.calc_reduced_ids(edge_group);
    return mesh.attributes().adapt_domain<int>(
        VArray<int>::ForContainer(std::move(edge_group)), AttrDomain::Edge, domain);
  }

  uint64_t hash() const final
  {
    return 736758993181174;
  }

  bool is_equal_to(const fn::FieldNode &other) const final
  {
    return dynamic_cast<const EdgesLineGroupFieldInput *>(&other) != nullptr;
  }

  std::optional<AttrDomain> preferred_domain(const Mesh & /*mesh*/) const final
  {
    return AttrDomain::Edge;
  }
};

class ParallelEdgeGroupFieldInput final : public bke::MeshFieldInput {
 public:
  ParallelEdgeGroupFieldInput()
      : bke::MeshFieldInput(CPPType::get<int>(), "Parallel Edge Rings Field")
  {
  }

  GVArray get_varray_for_context(const Mesh &mesh,
                                 const AttrDomain domain,
                                 const IndexMask & /*mask*/) const final
  {
    AtomicDisjointSet parallel_edges(mesh.edges_num);

    const Span<int> corner_edges = mesh.corner_edges();
    const blender::OffsetIndices<int> faces = mesh.faces();
    threading::parallel_for(IndexRange(mesh.faces_num), 2048, [&](const IndexRange range) {
      for (const int poly_index : range) {
        const IndexRange poly_range = faces[poly_index];
        if (poly_range.size() % 2) {
          continue;
        }
        /* Split corners of polygon to semicircle segments. */
        const int semicircle_size = poly_range.size() / 2;
        const Span<int> north_corners = corner_edges.slice(poly_range).take_front(semicircle_size);
        const Span<int> south_corners = corner_edges.slice(poly_range).take_back(semicircle_size);
        for (const int index : IndexRange(semicircle_size)) {
          const int north_edge = north_corners[index];
          const int south_edge = south_corners[index];
          parallel_edges.join(north_edge, south_edge);
        }
      }
    });

    Array<int> edge_group(mesh.edges_num);
    parallel_edges.calc_reduced_ids(edge_group);

    return mesh.attributes().adapt_domain<int>(
        VArray<int>::ForContainer(std::move(edge_group)), AttrDomain::Edge, domain);
  }

  uint64_t hash() const final
  {
    return 736758776181174;
  }

  bool is_equal_to(const fn::FieldNode &other) const final
  {
    return dynamic_cast<const ParallelEdgeGroupFieldInput *>(&other) != nullptr;
  }

  std::optional<AttrDomain> preferred_domain(const Mesh & /*mesh*/) const final
  {
    return AttrDomain::Edge;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  Field<int> parallel_edges{std::make_shared<ParallelEdgeGroupFieldInput>()};
  params.set_output("Parallel Group Index", std::move(parallel_edges));
  Field<int> linear_edges{std::make_shared<EdgesLineGroupFieldInput>()};
  params.set_output("Linear Group Index", std::move(linear_edges));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeInputMeshEdgeRings");
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.ui_name = "Edge Rings";
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_mesh_edge_rings_cc
