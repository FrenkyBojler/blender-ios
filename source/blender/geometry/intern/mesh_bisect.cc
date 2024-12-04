/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BLI_array_utils.hh"
#include "BLI_bit_vector.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_math_base.hh"
#include "BLI_math_geom.h"
#include "BLI_memarena.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "BKE_attribute.hh"
#include "BKE_attribute_math.hh"
#include "BKE_customdata.hh"
#include "BKE_mesh.hh"
#include "BLI_math_geom.h"

#include "GEO_mesh_bisect.hh"
#include <variant>

namespace blender::geometry {

enum EdgeIntersectType { Discarded = 0, Intersect = 1, Kept = 2, TypeCount = 3 };
// enum PolygonIntersectType { Outside = 0, Intersect = 1, Inside = 2, TypeCount = 3 };

/*
 * Vertex generated from linear interpolation between two
 */
struct MeshVertexGroupCopyMask {
  IndexMask src_indices;
};
/*
 * Vertex generated from linear interpolation between two
 */
struct MeshVertexGroupLinear {
  Span<int2> src_indices;
  Span<float> weights;
};

struct MeshEdgeGroupCopyMask {
  /*
   * Mask for the edges to copy from the data source.
   */
  IndexMask src_indices;
  /*
   * Optional: Table for remapping the src indices. Leave empty if indices are unchanged.
   */
  Span<int> mapping_table;
};
/*
 * Edge generated between the given vertices.
 */
struct MeshEdgeGroupPair {
  /*
   * Indices to the edge connects in the target mesh.
   */
  Span<int2> vertex_indices;
  /*
   * Optional: Edges to inherit values from, support 0, 1, 2 edges to inherit from. Takes the
   * average when possible. Leave invalid entries to -1, or leave it empty if no values are set.
   */
  Span<int2> src_edge_indices;
};

/*
 * Triangles generated from given .
 */
struct MeshTriangleGroup {
  /*
   * Indices to the polygon attributes should be copied from.
   */
  Span<int> src_polygon_indices;
  /*
   * Edge index sets, indices point to edges in the target mesh.
   */
  Span<int3> vertex_indices; /* Optional, can be computed by checking matching edge index pairs! */
  Span<int3> edge_indices;
  Span<float3> src_weights;
  Span<int2> src_corners;

  int64_t num_faces() const
  {
    BLI_assert(src_polygon_indices.size() == edge_indices.size());
    BLI_assert(src_polygon_indices.size() * 3 == src_corners.size());
    BLI_assert(src_polygon_indices.size() == src_weights.size());
    return src_polygon_indices.size();
  }

  int64_t num_corners() const
  {
    return edge_indices.size() * 3;
  }

  void fill_corner_offsets(MutableSpan<int> offset_slice) const
  {
    for (const int64_t index : offset_slice.index_range().drop_front(1)) {
      offset_slice[index] = offset_slice[index - 1] + 3;
    }
  }
};
struct MeshFaceGroupCopyMask {

  int num_corners;
  /*
   * Mask for the faces to copy from the data source.
   */
  IndexMask src_indices;

  Span<int> mapping_table_verts;
  Span<int> mapping_table_edges;

  void fill_corner_table(const OffsetIndices<int> src_corner_offsets,
                         const Span<int> src_corner_verts,
                         const Span<int> src_corner_edges,
                         MutableSpan<int> offset_slice,
                         MutableSpan<int> dst_corner_verts,
                         MutableSpan<int> dst_corner_edges) const
  {
    for (const int64_t local_face_index : offset_slice.index_range().drop_back(1)) {
      const IndexRange src_range = src_corner_offsets[src_indices[local_face_index]];
      const IndexRange dst_range(offset_slice[local_face_index], src_range.size());
      offset_slice[local_face_index + 1] = dst_range.one_after_last();

      for (const int64_t index : src_range.index_range()) {
        dst_corner_verts[dst_range[index]] =
            mapping_table_verts[src_corner_verts[src_range[index]]];
        dst_corner_edges[dst_range[index]] =
            mapping_table_edges[src_corner_edges[src_range[index]]];
        BLI_assert(dst_corner_verts[dst_range[index]] >= 0);
        BLI_assert(dst_corner_edges[dst_range[index]] >= 0);
      }
    }
  }
};

struct LocalData {
  MemArena *pf_arena = nullptr;

  ~LocalData()
  {
    if (pf_arena) {
      BLI_memarena_free(pf_arena);
    }
  }
};

/*
 * Why variants and not inheritance?
 *
 * Pros:
 * - Stack constructible (clean to use, promotes Span<T> over owning data structure).
 * - Restricted usage (enforces standardization, reduce maintainance).
 *
 * Cons:
 * - Unsafe (if allowing 'custom' variants compiler would not complain, mitigatable?).
 *    - Can provide inheritance based impl. as 'custom' through pointer...
 * - Verbose with if / else switches, increased maintainance and error prone (compiler..).
 * - Restricted usage complicates deprication of old / modified variants.
 */

/*
 * Descriptor variants for mesh vertex groups/sets.
 *
 * TODO:
 * Barycentric
 * N-ary
 * Custom
 */
using VariantVertexGroup = std::variant<MeshVertexGroupCopyMask, MeshVertexGroupLinear>;
/*
 * Descriptor variants for mesh edge groups/sets.
 */
using VariantEdgeGroup = std::variant<MeshEdgeGroupCopyMask, MeshEdgeGroupPair>;
/*
 * Descriptor variants for mesh polygon groups/sets.
 */
using VariantPolygonGroup = std::variant<MeshFaceGroupCopyMask, MeshTriangleGroup>;

IndexRange vertex_range(VariantVertexGroup group)
{
  if (auto item = std::get_if<MeshVertexGroupCopyMask>(&group)) {
    return item->src_indices.index_range();
  }
  else if (auto item = std::get_if<MeshVertexGroupLinear>(&group)) {
    return item->src_indices.index_range();
  }
  BLI_assert_msg(false, "Unreachable: Invalid variant implementation");
}

IndexRange edge_range(VariantEdgeGroup group)
{
  if (auto item = std::get_if<MeshEdgeGroupCopyMask>(&group)) {
    return item->src_indices.index_range();
  }
  else if (auto item = std::get_if<MeshEdgeGroupPair>(&group)) {
    return item->vertex_indices.index_range();
  }
  BLI_assert_msg(false, "Unreachable: Invalid variant implementation");
}

int count_num_vertices(const Span<VariantVertexGroup> vertex_groups)
{
  int vertex_count = 0;
  for (int64_t i = 0; i < vertex_groups.size(); i++) {
    vertex_count += vertex_range(vertex_groups[i]).size();
  }
  return vertex_count;
}

int count_num_edges(const Span<VariantEdgeGroup> edge_groups)
{
  int edge_count = 0;
  for (int64_t i = 0; i < edge_groups.size(); i++) {
    edge_count += edge_range(edge_groups[i]).size();
  }
  return edge_count;
}

int2 get_face_shape(const VariantPolygonGroup &group)
{
  if (auto item = std::get_if<MeshTriangleGroup>(&group)) {
    return int2{int(item->num_faces()), int(item->num_corners())};
  }
  else if (auto item = std::get_if<MeshFaceGroupCopyMask>(&group)) {
    return int2{int(item->src_indices.size()), item->num_corners};
  }
  else {
    BLI_assert_msg(false, "Unreachable: Invalid variant implementation");
  }
}

int2 count_num_polygons(const Span<VariantPolygonGroup> poly_groups)
{
  int face_count = 0;
  int offset = 0;
  for (int64_t group_index = 0; group_index < poly_groups.size(); group_index++) {
    const int2 shape = get_face_shape(poly_groups[group_index]);
    face_count += shape.x;
    offset += shape.y;
  }
  return {face_count, offset};
}

void transfer_vertex_data(const Mesh &src_mesh,
                          Mesh &dst_mesh,
                          const Span<VariantVertexGroup> vertex_groups,
                          const bke::AttributeFilter &attribute_filter)
{
  const bke::AttributeAccessor src_attributes = src_mesh.attributes();
  bke::MutableAttributeAccessor dst_attributes = dst_mesh.attributes_for_write();

  Set<std::string> copy_point_skip;
  // copy_point_skip.add("nurbs_weight");

  /* Copy point domain. */
  for (bke::AttributeTransferData &attribute : bke::retrieve_attributes_for_transfer(
           src_attributes,
           dst_attributes,
           ATTR_DOMAIN_MASK_POINT,
           bke::attribute_filter_with_skip_ref(attribute_filter, copy_point_skip)))
  {
    bke::attribute_math::convert_to_static_type(attribute.meta_data.data_type, [&](auto dummy) {
      using T = decltype(dummy);

      const Span<T> src_data = attribute.src.template typed<T>();
      MutableSpan<T> dst_data = attribute.dst.span.typed<T>();

      int64_t group_offset = 0;
      for (int64_t i = 0; i < vertex_groups.size(); i++) {
        const IndexRange group_range = vertex_range(vertex_groups[i]);

        if (auto item = std::get_if<MeshVertexGroupCopyMask>(&vertex_groups[i])) {
          /* Copy */
          const IndexRange dst_range = group_range.shift(group_offset);
          array_utils::gather(src_data, item->src_indices, dst_data.slice(dst_range));
        }
        else if (auto item = std::get_if<MeshVertexGroupLinear>(&vertex_groups[i])) {
          threading::parallel_for(group_range, 512, [&](IndexRange group_slice) {
            const IndexRange dst_range = group_slice.shift(group_offset);

            /* Linear interpolate */
            for (const int64_t i : group_slice.index_range()) {
              const int64_t sample_index = group_slice[i];
              int2 src_vert_indices = item->src_indices[sample_index];
              dst_data[dst_range[i]] = bke::attribute_math::mix2(item->weights[sample_index],
                                                                 src_data[src_vert_indices.x],
                                                                 src_data[src_vert_indices.y]);
            }
          });
        }
        else {
          BLI_assert_msg(false, "Unreachable: Invalid variant implementation");
        }

        group_offset += group_range.size();
      }
    });
  }
}

void transfer_edge_data(const Mesh &src_mesh,
                        Mesh &dst_mesh,
                        const Span<VariantEdgeGroup> edge_groups,
                        const bke::AttributeFilter &attribute_filter)
{
  const bke::AttributeAccessor src_attributes = src_mesh.attributes();
  bke::MutableAttributeAccessor dst_attributes = dst_mesh.attributes_for_write();

  Set<std::string> copy_edge_skip;
  copy_edge_skip.add(".edge_verts");

  /* Assign edge indices */
  {
    Span<int2> src_edges = src_mesh.edges();
    MutableSpan<int2> dst_edges = dst_mesh.edges_for_write();

    int64_t group_offset = 0;
    for (int64_t i = 0; i < edge_groups.size(); i++) {
      const IndexRange dst_range = edge_range(edge_groups[i]).shift(group_offset);
      MutableSpan<int2> dst_edge_slice = dst_edges.slice(dst_range);

      if (auto item = std::get_if<MeshEdgeGroupCopyMask>(&edge_groups[i])) {
        /* Copy using updated mapping table */
        item->src_indices.foreach_index([&](int64_t i, int64_t pos) {
          int2 src_index_pair = src_edges[i];
          int2 mapped_pair = int2(item->mapping_table[src_index_pair.x],
                                  item->mapping_table[src_index_pair.y]);
          dst_edge_slice[pos] = mapped_pair;
        });
      }
      else if (auto item = std::get_if<MeshEdgeGroupPair>(&edge_groups[i])) {
        /* Copy indices */
        dst_edge_slice.copy_from(item->vertex_indices);
      }
      else {
        BLI_assert_msg(false, "Unreachable: Invalid variant implementation");
      }
      group_offset += dst_range.size();
    }
  }

  /* Copy edge domain. */
  for (bke::AttributeTransferData &attribute : bke::retrieve_attributes_for_transfer(
           src_attributes,
           dst_attributes,
           ATTR_DOMAIN_MASK_EDGE,
           bke::attribute_filter_with_skip_ref(attribute_filter, copy_edge_skip)))
  {
    const CPPType &cpp_type = *bke::custom_data_type_to_cpp_type(attribute.meta_data.data_type);
    bke::attribute_math::convert_to_static_type(cpp_type, [&](auto dummy) {
      using T = decltype(dummy);

      Span<T> src_data = attribute.src.template typed<T>();
      MutableSpan<T> dst_data = attribute.dst.span.typed<T>();

      int64_t group_offset = 0;
      for (int64_t i = 0; i < edge_groups.size(); i++) {
        const IndexRange group_range = edge_range(edge_groups[i]);
        const IndexRange dst_range = group_range.shift(group_offset);

        if (auto item = std::get_if<MeshEdgeGroupCopyMask>(&edge_groups[i])) {
          /* Copy */
          array_utils::gather(src_data, item->src_indices, dst_data.slice(dst_range));
        }
        else if (auto item = std::get_if<MeshEdgeGroupPair>(&edge_groups[i])) {
          /* Mix edge data IFF source edges are specified. */
          if (item->src_edge_indices.size() == group_range.size()) {
            threading::parallel_for(group_range, 512, [&](IndexRange group_slice) {
              for (const int64_t i : group_slice.index_range()) {
                const int64_t sample_index = group_slice[i];
                int2 vert_indices = item->src_edge_indices[sample_index];

                T &value = dst_data[dst_range[i]];
                if (vert_indices.x == -1) {
                  cpp_type.default_construct(&value);
                }
                else if (vert_indices.y == -1) {
                  value = src_data[vert_indices.x];
                }
                else {
                  value = bke::attribute_math::mix2(
                      0.5f, src_data[vert_indices.x], src_data[vert_indices.y]);
                }
              }
            });
          }
          else {
            /* Defualt fill. */
            cpp_type.default_construct_n(dst_data.slice(dst_range).data(), dst_range.size());
          }
        }
        else {
          BLI_assert_msg(false, "Unreachable: Invalid variant implementation");
        }
        group_offset += group_range.size();
      }
    });
  }
}

/* Find the sorted common value pair of `a` so that `a.x` is found in b.
 * Assumes the shared value exist!
 *
 */
int2 sort_shared_index(int2 a, int2 b)
{
  if (a.x == b.x || a.x == b.y) {
    return a;
  }
  BLI_assert(a.y == b.x || a.y == b.y);
  return int2(a.y, a.x);
}

/* Find the common value in `a` (assumed the shared value exist).
 */
int get_unshared_index(int2 a, int b)
{
  BLI_assert(b == a.x || b == a.y);
  return b == a.x ? a.x : a.y;
}

Vector<int> compute_polygon_vert_indices_from_edge_data(Span<int> dst_face_offsets_slice,
                                                        Span<int2> dst_edges,
                                                        Span<int> edge_corner_indices)
{
  Vector<int> face_corner_indices(dst_face_offsets_slice.last() - dst_face_offsets_slice.first());
  threading::parallel_for(dst_face_offsets_slice.index_range(), 4092, [&](IndexRange subrange) {
    for (const int64_t local_face_index : subrange) {
      const int local_first_corner = dst_face_offsets_slice[local_face_index];
      const IndexRange src_range(
          local_first_corner, dst_face_offsets_slice[local_face_index + 1] - local_first_corner);
      const IndexRange dst_range(src_range.first() - dst_face_offsets_slice.first(),
                                 src_range.size());
      const Span<int> edgec_inds = edge_corner_indices.slice(src_range);

      /* Transfer corners */
      const int2 initial_sort = sort_shared_index(dst_edges[edgec_inds.last()],
                                                  dst_edges[edgec_inds.first()]);

      int next_common = initial_sort.y;
      face_corner_indices[dst_range.first()] = next_common;
      for (const int64_t i : edgec_inds.drop_front(1)) {
        next_common = get_unshared_index(dst_edges[i], next_common);
        face_corner_indices[dst_range[i]] = next_common;
      }
    }
  });
  return face_corner_indices;
}

void transfer_polygon_data(const Mesh &src_mesh,
                           Mesh &dst_mesh,
                           const Span<VariantPolygonGroup> poly_groups,
                           const bke::AttributeFilter &attribute_filter)
{
  if (dst_mesh.faces_num == 0) {
    return;
  }

  const bke::AttributeAccessor src_attributes = src_mesh.attributes();
  bke::MutableAttributeAccessor dst_attributes = dst_mesh.attributes_for_write();

  Set<std::string> copy_poly_skip;
  copy_poly_skip.add(".corner_vert");
  copy_poly_skip.add(".corner_edge");

  Span<int> src_vert_corners = src_mesh.corner_verts();
  Span<int> src_edge_corners = src_mesh.corner_edges();
  OffsetIndices<int> src_face_offsets(src_mesh.face_offsets());

  /* Assign polygon indices */
  Array<int64_t> group_face_count(poly_groups.size());
  {
    MutableSpan<int> dst_vert_corners = dst_mesh.corner_verts_for_write();
    MutableSpan<int> dst_edge_corners = dst_mesh.corner_edges_for_write();
    MutableSpan<int> dst_face_offsets = dst_mesh.face_offsets_for_write();
    Span<int2> dst_edges = dst_mesh.edges();

    int64_t tot_corner_offset = 0;
    int64_t tot_face_count = 0;

    Vector<int> computed_corner_indices;

    dst_face_offsets[0] = 0;
    for (int64_t group_index = 0; group_index < poly_groups.size(); group_index++) {

      const int2 face_shape = get_face_shape(poly_groups[group_index]);
      const IndexRange dst_corner_range(tot_corner_offset, face_shape.y);
      MutableSpan<int> dst_face_offsets_slice = dst_face_offsets.slice(
          IndexRange(tot_face_count, face_shape.x + 1));

      /* Write face corner offsets */
      if (auto item = std::get_if<MeshTriangleGroup>(&poly_groups[group_index])) {
        /* Write face offsets */
        item->fill_corner_offsets(dst_face_offsets_slice);

        Span<int> group_edge_indices = item->edge_indices.cast<int>();
        dst_edge_corners.slice(dst_corner_range).copy_from(group_edge_indices);

        Span<int> group_vert_indices;
        if (item->vertex_indices.size() == 0) {
          computed_corner_indices = compute_polygon_vert_indices_from_edge_data(
              dst_face_offsets_slice, dst_edges, group_edge_indices);
          group_vert_indices = computed_corner_indices.as_span();
        }
        else {
          group_vert_indices = item->vertex_indices.cast<int>();
        }
        dst_vert_corners.slice(dst_corner_range).copy_from(group_vert_indices);
      }
      else if (auto item = std::get_if<MeshFaceGroupCopyMask>(&poly_groups[group_index])) {

        item->fill_corner_table(src_face_offsets,
                                src_vert_corners,
                                src_edge_corners,
                                dst_face_offsets_slice,
                                dst_vert_corners.slice(dst_corner_range),
                                dst_edge_corners.slice(dst_corner_range));
      }
      else {
        BLI_assert_msg(false, "Unreachable: Invalid variant implementation");
      }

      tot_face_count += face_shape.x;
      tot_corner_offset += face_shape.y;
      group_face_count[group_index] = tot_face_count;
    }
  }

  /* Copy face domain. */
  for (bke::AttributeTransferData &attribute : bke::retrieve_attributes_for_transfer(
           src_attributes,
           dst_attributes,
           ATTR_DOMAIN_MASK_FACE,
           bke::attribute_filter_with_skip_ref(attribute_filter, copy_poly_skip)))
  {
    const CPPType &cpp_type = *bke::custom_data_type_to_cpp_type(attribute.meta_data.data_type);
    bke::attribute_math::convert_to_static_type(cpp_type, [&](auto dummy) {
      using T = decltype(dummy);

      Span<T> src_data = attribute.src.template typed<T>();
      MutableSpan<T> dst_data = attribute.dst.span.typed<T>();

      int64_t face_offset = 0;
      for (int64_t group_index = 0; group_index < poly_groups.size(); group_index++) {
        const IndexRange dst_range(face_offset, group_face_count[group_index] - face_offset);

        if (auto item = std::get_if<MeshTriangleGroup>(&poly_groups[group_index])) {
          array_utils::gather(src_data, item->src_polygon_indices, dst_data.slice(dst_range));
        }
        else if (auto item = std::get_if<MeshFaceGroupCopyMask>(&poly_groups[group_index])) {
          array_utils::gather(src_data, item->src_indices, dst_data.slice(dst_range));
        }
        else {
          BLI_assert_msg(false, "Unreachable: Invalid variant implementation");
        }
        face_offset += group_face_count[group_index];
      }
    });
  }

  /* Copy face corner domain. */
  {
    OffsetIndices dst_face_offsets(dst_mesh.face_offsets());
    for (bke::AttributeTransferData &attribute : bke::retrieve_attributes_for_transfer(
             src_attributes,
             dst_attributes,
             ATTR_DOMAIN_MASK_CORNER,
             bke::attribute_filter_with_skip_ref(attribute_filter, copy_poly_skip)))
    {
      const CPPType &cpp_type = *bke::custom_data_type_to_cpp_type(attribute.meta_data.data_type);
      bke::attribute_math::convert_to_static_type(cpp_type, [&](auto dummy) {
        using T = decltype(dummy);

        Span<T> src_data = attribute.src.template typed<T>();
        MutableSpan<T> dst_data = attribute.dst.span.typed<T>();

        int64_t face_offset = 0;
        for (int64_t group_index = 0; group_index < poly_groups.size(); group_index++) {
          if (auto item = std::get_if<MeshTriangleGroup>(&poly_groups[group_index])) {
            const IndexRange face_range(face_offset, group_face_count[group_index] - face_offset);
            const Span<float> src_weights = item->src_weights.cast<float>();
            const int64_t first_corner = dst_face_offsets[face_range.first()].first();

            threading::parallel_for(face_range, 4092, [&](IndexRange subrange) {
              for (const int64_t face_index : subrange) {
                for (const int64_t dst_corner_index : dst_face_offsets[face_index]) {
                  const int64_t src_index = dst_corner_index - first_corner;
                  const int2 cpair = item->src_corners[src_index];
                  /* Checking if cpair.y == -1 but catching 0.0f weights */
                  BLI_assert(cpair.y >= 0 || src_weights[src_index] == 0.0f);
                  dst_data[dst_corner_index] = src_weights[src_index] == 0.0f ?
                                                   src_data[cpair.x] :
                                                   bke::attribute_math::mix2(
                                                       src_weights[src_index],
                                                       src_data[cpair.x],
                                                       src_data[cpair.y]);
                }
              }
            });
          }
          else if (auto item = std::get_if<MeshFaceGroupCopyMask>(&poly_groups[group_index])) {
            const IndexRange local_face_range(group_face_count[group_index] - face_offset);
            threading::parallel_for(local_face_range, 4092, [&](IndexRange subrange) {
              for (const int64_t face_index : subrange) {
                const int64_t src_face_index = item->src_indices[face_index];
                const IndexRange src_range = src_face_offsets[src_face_index];
                const IndexRange dst_range = dst_face_offsets[face_index + face_offset];

                for (const int64_t index : src_range.index_range()) {
                  dst_data[dst_range[index]] = src_data[src_range[index]];
                }
              }
            });
          }
          else {
            BLI_assert_msg(false, "Unreachable: Invalid variant implementation");
          }
          face_offset += group_face_count[group_index];
        }
      });
    }
  }
}

/* Compute interpolation weight for an edge.
 */
float edge_weight(const float signed_dist_x, const float signed_dist_y)
{
  const float abs_d1 = abs(signed_dist_x);
  const float tot_dist = abs_d1 + abs(signed_dist_y);
  return math::safe_divide<float>(abs_d1, tot_dist);
};

int2 sorted_edge(const int2 edge)
{
  if (edge.y < edge.x) {
    return {edge.y, edge.x};
  }
  return edge;
}

float sorted_weight(const int2 edge, const float weight)
{
  if (edge.y < edge.x) {
    return 1.0f - weight;
  }
  return weight;
}

bool is_sequential_pair(int2 ab, const int tot_size)
{
  return (ab.x + 1) % tot_size == ab.y;
}

Mesh *new_mesh_from_groups(const Mesh &src_mesh,
                           const Span<VariantVertexGroup> vertex_groups,
                           const Span<VariantEdgeGroup> edge_groups,
                           const Span<VariantPolygonGroup> poly_groups)
{
  const int num_vertices = count_num_vertices(vertex_groups);
  const int num_edges = count_num_edges(edge_groups);
  int2 num_poly_corner = count_num_polygons(poly_groups);

  Mesh *result = BKE_mesh_new_nomain_from_template(
      &src_mesh, num_vertices, num_edges, num_poly_corner.x, num_poly_corner.y);
  return result;
}
std::pair<Mesh *, BisectResult> bisect_mesh(const Mesh &mesh,
                                            const BisectArgs &args,
                                            const bke::AttributeFilter &attribute_filter)
{
  const int src_num_vert = mesh.verts_num;
  const int src_num_edges = mesh.edges_num;
  const int src_num_polys = mesh.faces_num;

  /* Compute per-vertex distance to the plane.
   *
   *    *  <- 'Outside' relative to the plane
   *     \
   *  ^^^^ Plane ^^^^^
   *       \
   *        *  <-- 'Inside' relative to plane
   */
  const Span<float3> src_positions = mesh.vert_positions();
  IndexRange src_vert_range = src_positions.index_range();
  BLI_assert(src_num_vert == src_positions.size());

  /* Bit flag property for how vertices relate to the plane.
   *
   * 0: Outside
   * 1 (2^0): Inside
   * 2 (2^2): Intersect
   */
  const int8_t MASK_OUTSIDE = 0x0;
  const int8_t MASK_INSIDE = 0x1;

  // TODO: REMOVE
  const int8_t MASK_KEPT = 0x4;
  const int8_t MASK_IN_PLANE = 0x8;

  /* Classify vertices
   */
  Array<int8_t> vertex_flags(src_num_vert);
  Array<float, 12> dist_buffer(src_num_vert);

  auto fn_is_outside = [](float signed_distance) { return signed_distance >= 0.0f; };

  IndexMaskMemory kept_memory;
  IndexMask kept_vertices = IndexMask::from_predicate(
      IndexMask(src_vert_range), GrainSize(512), kept_memory, [&](const int64_t i) {
        const float dist_to_plane = dist_signed_to_plane_v3(src_positions[i], args.plane);

        const int8_t is_inside = fn_is_outside(dist_to_plane) ? MASK_OUTSIDE : MASK_INSIDE;
        vertex_flags[i] = is_inside;
        dist_buffer[i] = dist_to_plane;
        return is_inside;
      });

  const int num_verts_kept = int(kept_vertices.size());
  if (num_verts_kept == 0) {
    return {nullptr, BisectResult::Discard};
  }

  /* Classify edges to keep, this is necessary to remap edges and edge attributes.
   */
  const Span<int2> src_edges = mesh.edges();
  auto fn_intersected_edge = [&](int64_t i) {
    const int8_t v1_kept = vertex_flags[src_edges[i].x];
    const int8_t v2_kept = vertex_flags[src_edges[i].y];
    return v1_kept + v2_kept;
  };

  Array<float, 12> edge_insertion_factor(src_num_edges);
  IndexMaskMemory intersected_memory;
  Array<IndexMask, EdgeIntersectType::TypeCount> edge_type_selections(
      EdgeIntersectType::TypeCount);
  IndexMask::from_groups<int64_t>(IndexMask(src_edges.index_range()),
                                  intersected_memory,
                                  fn_intersected_edge,
                                  edge_type_selections.as_mutable_span());

  const int64_t num_edges_intersected = edge_type_selections[EdgeIntersectType::Intersect].size();
  const int num_edges_kept = int(edge_type_selections[EdgeIntersectType::Kept].size());

  /* Handle keep/discard all. */
  if (num_edges_intersected == 0) {
    BLI_assert(edge_type_selections[EdgeIntersectType::Discarded].size() == 0);
    return {nullptr, BisectResult::Keep};
  }

  /* Classify faces
   */
  const OffsetIndices src_polys = mesh.faces();
  const Span<int> src_corner_verts = mesh.corner_verts();
  const Span<int> src_corner_edges = mesh.corner_edges();

  /* Group polygons by intersection classification */
  auto fn_polygon_group = [&](int64_t index_poly) {
    const IndexRange src_corner_range = src_polys[index_poly];
    const Span<int> corners = src_corner_verts.slice(src_corner_range);

    /* Determine intersection, loop over all edges except initial, and last (2 edges must be cut).
     */
    const int8_t initial = vertex_flags[corners.first()];
    for (const int64_t index : corners.index_range().drop_front(1)) {
      if (initial != vertex_flags[corners[index]]) {
        return EdgeIntersectType::Intersect;
      }
    }
    return initial >= MASK_INSIDE ? EdgeIntersectType::Kept : EdgeIntersectType::Discarded;
  };
  IndexMaskMemory polygon_sort_memory;
  Array<IndexMask, EdgeIntersectType::TypeCount> poly_type_selections(
      EdgeIntersectType::TypeCount);
  IndexMask::from_groups<int64_t>(IndexMask(src_polys.index_range()),
                                  polygon_sort_memory,
                                  fn_polygon_group,
                                  poly_type_selections.as_mutable_span());

  /* Compute corner offsets for intersected polygons (synchronous) */
  const int64_t num_intersected_polygons =
      poly_type_selections[EdgeIntersectType::Intersect].size();
  Vector<int> tesselation_off_data(num_intersected_polygons + 1);
  MutableSpan<int> tess_off_span = tesselation_off_data.as_mutable_span().slice(
      1, num_intersected_polygons);
  tesselation_off_data[0] = 0;

  const int max_face_size = poly_type_selections[EdgeIntersectType::Intersect].parallel_reduce(
      GrainSize(4096),
      0,
      [&](const int64_t index, const int64_t index_pos, const int &identity) {
        const int face_size = src_polys[index].size();
        const int tri_count = bke::mesh::face_triangles_num(face_size);
        tess_off_span[index_pos] = tri_count;
        return face_size;
      },
      [](int a, int b) { return std::max<int>(a, b); });

  std::partial_sum(
      tesselation_off_data.begin(), tesselation_off_data.end(), tesselation_off_data.begin());
  OffsetIndices<int> tesselation_offsets(tesselation_off_data);

  /* Compute triangulation (asynchronous) */
  threading::EnumerableThreadSpecific<LocalData> all_local_data;

  Vector<int3> tesselation(tesselation_offsets.last());

  Vector<int> tess_dst_offsets(num_intersected_polygons + 1);
  MutableSpan<int> tess_dst_off_span = tess_dst_offsets.as_mutable_span().slice(
      1, num_intersected_polygons);
  tess_dst_offsets[0] = 0;

  Vector<int> edge_split_dst_offset_buffer(num_intersected_polygons + 1);
  MutableSpan<int> edge_split_dst_off_span = edge_split_dst_offset_buffer.as_mutable_span().slice(
      1, num_intersected_polygons);
  edge_split_dst_offset_buffer[0] = num_edges_intersected;

  Vector<int> edge_formed_dst_offset_buffer(num_intersected_polygons + 1);
  MutableSpan<int> edge_formed_dst_off_span =
      edge_formed_dst_offset_buffer.as_mutable_span().slice(1, num_intersected_polygons);
  edge_formed_dst_offset_buffer[0] = 0;

  Vector<int> edge_mask_offset_buffer(num_intersected_polygons + 1);
  MutableSpan<int> edge_mask_offset_span = edge_mask_offset_buffer.as_mutable_span().slice(
      1, num_intersected_polygons);
  edge_mask_offset_buffer[0] = 0;

  Vector<int> local_corner_indices(max_face_size);
  std::iota(local_corner_indices.begin(), local_corner_indices.end(), 0);

  /* Tesselate
   */
  poly_type_selections[EdgeIntersectType::Intersect].foreach_index(
      GrainSize(512), [&](const int64_t index_poly, const int64_t index_pos) {
        LocalData &local_data = all_local_data.local();

        const IndexRange src_corner_range = src_polys[index_poly];
        const IndexRange local_corner_range(src_corner_range.size());
        const Span<int> local_corner_span = local_corner_indices.as_span().slice(
            0, src_corner_range.size());

        /* Count number of intersections on the boundary. */
        int prev_vert = src_corner_verts[src_corner_range.last()];
        int boundary_intersect_count = 0;
        for (const int64_t corner : src_corner_range) {
          const int next_vert = src_corner_verts[corner];
          boundary_intersect_count += int(vertex_flags[prev_vert] != vertex_flags[next_vert]);
          prev_vert = next_vert;
        }
        BLI_assert(boundary_intersect_count % 2 == 0);

        // Make thread local allocated memory
        Vector<float3> local_corner_pos(local_corner_range.size());
        array_utils::gather(src_positions,
                            src_corner_verts.slice(src_corner_range),
                            local_corner_pos.as_mutable_span());
        Vector<int8_t> local_flags(local_corner_range.size());
        array_utils::gather(vertex_flags.as_span(),
                            src_corner_verts.slice(src_corner_range),
                            local_flags.as_mutable_span());

        /* Tesselate */
        MutableSpan<int3> corner_tess_span = tesselation.as_mutable_span().slice(
            tesselation_offsets[index_pos]);
        bke::mesh::mesh_calc_tessellation_for_face(local_corner_span,
                                                   src_positions,
                                                   0,
                                                   src_corner_range.size(),
                                                   corner_tess_span.data(),
                                                   &local_data.pf_arena);

        /* Count intersections within the tesselated triangle set. */
        int count_tris_with_cut = 0;
        int count_tris_inside = 0; /* Number of tess tris inside the plane (split or kept). */
        int count_tris_internal_split = 0; /* Number of tess tris outputs two tris (== quad). */
        for (int3 &tri : corner_tess_span) {
          const int num_inner = local_flags[tri.x] + local_flags[tri.y] + local_flags[tri.z];
          if (num_inner == 0) {
            continue;
          }
          const int8_t cut_ie1 = int8_t(local_flags[tri.x] != local_flags[tri.y]);
          const int8_t cut_ie2 = int8_t(local_flags[tri.y] != local_flags[tri.z]);
          const int8_t cut_ie3 = int8_t(local_flags[tri.z] != local_flags[tri.x]);

          const int8_t num_cuts = cut_ie1 + cut_ie2 + cut_ie3;
          if (num_cuts) {
            BLI_assert(num_cuts == 2);
            count_tris_with_cut++; /* Total num_cuts / 2 */
            /* Adjust tri order to align edges as: cut, cut, in plane edge.
             * Order simplifies logic when generating the tri.
             */
            if (!cut_ie1) {
              const int x = tri.x;
              tri.x = tri.y;
              tri.y = tri.z;
              tri.z = x;
            }
            else if (!cut_ie2) {
              const int y = tri.y;
              tri.y = tri.x;
              tri.x = tri.z;
              tri.z = y;
            }
          }
          /* Note that case for 'num_inner == 0' is already skipped.
           */
          count_tris_inside++;                              /* num_inner == 3 ? 1 : num_inner */
          count_tris_internal_split += (num_inner - 1) % 2; /* num_inner == 2 */
        }

        /* Store counts
         */
        const int num_tris_inner = count_tris_inside - count_tris_with_cut;
        tess_dst_off_span[index_pos] = count_tris_inside + count_tris_internal_split;
        edge_split_dst_off_span[index_pos] = count_tris_with_cut - boundary_intersect_count / 2;
        edge_formed_dst_off_span[index_pos] = count_tris_with_cut + count_tris_internal_split +
                                              num_tris_inner;
        edge_mask_offset_span[index_pos] = count_tris_with_cut; /* any cut => has edge on border */
      });

  /* Accumulate dst offsets */
  std::partial_sum(tess_dst_offsets.begin(), tess_dst_offsets.end(), tess_dst_offsets.begin());
  OffsetIndices<int> tesselation_dst_offsets(tess_dst_offsets);

  std::partial_sum(edge_split_dst_offset_buffer.begin(),
                   edge_split_dst_offset_buffer.end(),
                   edge_split_dst_offset_buffer.begin());
  OffsetIndices<int> edge_split_dst_offsets(edge_split_dst_offset_buffer);

  std::partial_sum(edge_formed_dst_offset_buffer.begin(),
                   edge_formed_dst_offset_buffer.end(),
                   edge_formed_dst_offset_buffer.begin());
  OffsetIndices<int> edge_formed_dst_offsets(edge_formed_dst_offset_buffer);

  std::partial_sum(edge_mask_offset_buffer.begin(),
                   edge_mask_offset_buffer.end(),
                   edge_mask_offset_buffer.begin());
  OffsetIndices<int> index_mask_offsets(edge_mask_offset_buffer);
  BLI_assert(index_mask_offsets.total_size() == index_mask_offsets.last());

  /* Create vertex index map */
  Array<int, 12> old_to_new_vertex_map(src_num_vert);
  kept_vertices.foreach_index([&](const int64_t index_vert, const int64_t index_pos) {
    old_to_new_vertex_map[index_vert] = index_pos;
  });
  /* Maps index of kept edges from the original mesh to their index in the new mesh */
  Array<int, 12> old_to_new_edge_map(src_num_edges);
  old_to_new_edge_map.fill(-1);
  edge_type_selections[EdgeIntersectType::Kept].foreach_index(
      [&](const int64_t index_edge, const int64_t index_pos) {
        old_to_new_edge_map[index_edge] = index_pos;
      });

  /* Buffers for vertices formed from edge splits, two types either:
   * 1. Vertices formed from existing edges in the input/source
   * 2. Vertices formed splitting virtual/tesselated edges
   */
  const int64_t tot_split_edges = edge_split_dst_offsets.last();

  Array<int, 12> tesselation_polygon_indices(tesselation_dst_offsets.last());
  Array<int3, 12> tesselation_vertex_indices(tesselation_dst_offsets.last());
  Array<int3, 12> tesselation_edge_indices(tesselation_vertex_indices.size());
  Array<float3, 12> tesselation_corner_weights(tesselation_vertex_indices.size());
  Array<int2, 12> tesselation_corner_vertices(tesselation_vertex_indices.size() * 3);

  Array<float, 12> vertex_split_weights(tot_split_edges);
  Array<int2, 12> vertex_split_vertices(tot_split_edges);

  const int num_total_edge_created = tot_split_edges + edge_formed_dst_offsets.last();
  Array<int2, 12> edge_output_vertex_pairs(num_total_edge_created);
  Array<int2, 12> edge_output_src_edges(num_total_edge_created);

  /* Index mask for tracking edges formed inside the plane. */
  Vector<int> edge_border_index_mask(index_mask_offsets.last());

  auto split_edge = [&](const int2 src_vert_indices,
                        const int64_t index_offset,
                        const int2 src_edge_indices,
                        float &r_edge_weight) {
    const float signed_dist_x = dist_buffer[src_vert_indices.x];
    const float signed_dist_y = dist_buffer[src_vert_indices.y];
    const int other_vertex = fn_is_outside(signed_dist_x) ? src_vert_indices.y :
                                                            src_vert_indices.x;

    const int new_vert_index = num_verts_kept + index_offset;
    const int new_edge_index = num_edges_kept + index_offset;

    r_edge_weight = edge_weight(signed_dist_x, signed_dist_y);
    vertex_split_weights[index_offset] = r_edge_weight;
    vertex_split_vertices[index_offset] = src_vert_indices;

    edge_output_vertex_pairs[index_offset] = int2(old_to_new_vertex_map[other_vertex],
                                                  new_vert_index);
    edge_output_src_edges[index_offset] = src_edge_indices;

    return int2{new_vert_index, new_edge_index};
  };

  /* Split existing edges (synchronous)
   */
  edge_type_selections[EdgeIntersectType::Intersect].foreach_index(
      [&](const int64_t index_src_edge, const int64_t index_pos) {
        float edge_weight;
        const int2 vert_indices = sorted_edge(src_edges[index_src_edge]);
        const int2 ve_index = split_edge(
            vert_indices, index_pos, int2{int(index_src_edge), -1}, edge_weight);
        old_to_new_edge_map[index_src_edge] = ve_index.y;
      });

  /* Generate output triangulation
   */
  poly_type_selections[EdgeIntersectType::Intersect].foreach_index(
      GrainSize(512), [&](const int64_t index_poly, const int64_t index_pos) {
        const IndexRange src_corner_range = src_polys[index_poly];
        const IndexRange local_corner_range(src_corner_range.size());

        const IndexRange edge_split_dst_offset = edge_split_dst_offsets[index_pos];
        const IndexRange edge_formed_dst_offset = edge_formed_dst_offsets[index_pos];
        const IndexRange tess_dst_offset = tesselation_dst_offsets[index_pos];
        const IndexRange index_mask_offset = index_mask_offsets[index_pos];

        // = tess_dst_offset * 3
        const IndexRange tess_dst_offset_corner(tess_dst_offset.first() * 3,
                                                tess_dst_offset.size() * 3);

        // Todo: allocate to these buffers..
        MutableSpan<int> local_tess_poly_src = tesselation_polygon_indices.as_mutable_span().slice(
            tess_dst_offset);
        MutableSpan<int3> local_tess_inds = tesselation_vertex_indices.as_mutable_span().slice(
            tess_dst_offset);
        MutableSpan<int3> local_tess_edges = tesselation_edge_indices.as_mutable_span().slice(
            tess_dst_offset);

        MutableSpan<float3> local_tess_corner_weights =
            tesselation_corner_weights.as_mutable_span().slice(tess_dst_offset);
        MutableSpan<int2> local_tess_corner_vertices =
            tesselation_corner_vertices.as_mutable_span().slice(tess_dst_offset_corner);

        const Span<int> local_corner_verts = src_corner_verts.slice(src_corner_range);
        const Span<int> local_corner_edges = src_corner_edges.slice(src_corner_range);

        // Make thread local allocated memory
        Vector<float3> local_corner_pos(local_corner_range.size());
        array_utils::gather(src_positions,
                            src_corner_verts.slice(src_corner_range),
                            local_corner_pos.as_mutable_span());
        Vector<int8_t> local_flags(local_corner_range.size());
        array_utils::gather(vertex_flags.as_span(),
                            src_corner_verts.slice(src_corner_range),
                            local_flags.as_mutable_span());

        Vector<int2> edge_split_pairs(local_corner_range.size());
        Vector<int2> index_vert_edge(local_corner_range.size());
        Vector<float> edge_split_weight(local_corner_range.size());
        int count_lookup_edges = 0; /* Counts the number of entries in the lookup table. */

        auto append_outer_edge = [&](int2 pair) {
          /* Append an edge that lies on the boundary to the cutting plane.
           */
          const int edge_index = old_to_new_edge_map[local_corner_edges[pair.x]];
          const int offset_virtual_split = edge_index - num_edges_kept;
          const int vertex_index = num_verts_kept + offset_virtual_split;

          const int2 sorted_pair = sorted_edge(pair);
          index_vert_edge[count_lookup_edges] = int2{vertex_index, edge_index};
          edge_split_pairs[count_lookup_edges] = sorted_pair;
          /* Ensure weight is sorted in local order, this may invert an inverted value */
          edge_split_weight[count_lookup_edges] = sorted_weight(
              int2{local_corner_verts[sorted_pair.x], local_corner_verts[sorted_pair.y]},
              vertex_split_weights[offset_virtual_split]);
          count_lookup_edges++;
        };

        /* Fetch split boundary edges */
        int prev_corner = local_corner_range.last();
        for (const int corner : local_corner_range) {
          if (local_flags[corner] != local_flags[prev_corner]) {
            append_outer_edge(int2{prev_corner, corner});
          }
          prev_corner = corner;
        }

        int count_edges_formed = 0; /* Counts formed edges. */
        auto new_formed_edge_index = [&]() {
          const int offset_virtual_formed = edge_split_dst_offsets.last() +
                                            edge_formed_dst_offset.first() + count_edges_formed;
          count_edges_formed++;
          return int2{offset_virtual_formed, num_edges_kept + offset_virtual_formed};
        };

        int count_edges_split = 0;
        auto find_or_split_edge = [&](int2 corners) {
          /*
           */
          int2 pair = sorted_edge(corners);
          auto it = std::find(edge_split_pairs.begin(), edge_split_pairs.end(), pair);

          if (it == edge_split_pairs.end()) {
            /* Split the edge */
            const int2 vert_indices{local_corner_verts[pair.x], local_corner_verts[pair.y]};
            const int64_t offset_virtual_split = edge_split_dst_offset.first() + count_edges_split;

            edge_split_pairs[count_lookup_edges] = pair;
            index_vert_edge[count_lookup_edges] = split_edge(
                vert_indices,
                offset_virtual_split,
                int2{-1, -1},
                edge_split_weight[count_lookup_edges]);

            count_edges_split++;
            return count_lookup_edges++;
          }
          return int(std::distance(edge_split_pairs.begin(), it));
        };

        auto find_or_create_inner_edge = [&](const int2 corners) {
          /* Form a new edge inside the N-gon that isn't intersected by the plane.
           */

          int2 pair = sorted_edge(corners);
          auto it = std::find(edge_split_pairs.begin(), edge_split_pairs.end(), pair);

          if (it == edge_split_pairs.end()) {
            /* Create a new edge entry! */
            const int2 offset_index = new_formed_edge_index();
            edge_split_pairs[count_lookup_edges] = pair;
            index_vert_edge[count_lookup_edges] = int2(-1, offset_index.y);

            edge_output_vertex_pairs[offset_index.x] = int2{
                old_to_new_vertex_map[local_corner_verts[corners.x]],
                old_to_new_vertex_map[local_corner_verts[corners.y]]};
            edge_output_src_edges[offset_index.x] = int2{
                old_to_new_edge_map[local_corner_edges[corners.x]],
                old_to_new_edge_map[local_corner_verts[corners.y]]};

            count_lookup_edges++;
            return offset_index.y;
          }
          const int64_t local_split_index = std::distance(edge_split_pairs.begin(), it);
          return index_vert_edge[local_split_index].y;
        };

        int count_edges_formed_at_cut = 0;
        auto form_edge_at_cut = [&](int2 new_vert_indices, int2 src_edge_indices) {
          /* Form a new edge inside the N-gon between two intersections (at the plane cut).
           */
          const int2 offset_index = new_formed_edge_index();
          edge_output_vertex_pairs[offset_index.x] = new_vert_indices;
          edge_output_src_edges[offset_index.x] = src_edge_indices;

          edge_border_index_mask[index_mask_offset[count_edges_formed_at_cut]] = offset_index.y;

          count_edges_formed_at_cut++;
          return offset_index.y;
        };

        auto form_edge_diagonal =
            [&](int split_vert_index, int corner_index, int split_edge_source_index) {
              /* Form a new edge inside the triangle itself, between a cut edge and interior vertex
               * (occurs when output from spliting the tesselated tri forms a quad).
               */
              const int2 offset_index = new_formed_edge_index();
              edge_output_vertex_pairs[offset_index.x] = int2(
                  split_vert_index, old_to_new_vertex_map[local_corner_verts[corner_index]]);
              edge_output_src_edges[offset_index.x] = int2(
                  old_to_new_edge_map[local_corner_edges[corner_index]], split_edge_source_index);

              return offset_index.y;
            };

        int tri_count = 0;
        for (const int3 tri : tesselation.as_span().slice(tesselation_offsets[index_pos])) {
          const int num_inner = local_flags[tri.x] + local_flags[tri.y] + local_flags[tri.z];
          if (num_inner == 0) {
            continue;
          }
          local_tess_poly_src[tri_count] = index_poly;

          if (num_inner == 3) {
            /* No intersection... Keep whole and form inner edge(s) */
            const int2 xy = int2{tri.x, tri.y};
            const int2 yz = int2{tri.y, tri.z};
            const int2 zx = int2{tri.y, tri.z};
            const int e0 = is_sequential_pair(xy, local_corner_range.size()) ?
                               old_to_new_edge_map[local_corner_edges[tri.x]] :
                               find_or_create_inner_edge(xy);
            const int e1 = is_sequential_pair(yz, local_corner_range.size()) ?
                               old_to_new_edge_map[local_corner_edges[tri.y]] :
                               find_or_create_inner_edge(yz);
            const int e2 = is_sequential_pair(zx, local_corner_range.size()) ?
                               old_to_new_edge_map[local_corner_edges[tri.z]] :
                               find_or_create_inner_edge(zx);
            local_tess_edges[tri_count] = int3{e0, e1, e2};
            local_tess_inds[tri_count] = int3{old_to_new_vertex_map[local_corner_verts[tri.x]],
                                              old_to_new_vertex_map[local_corner_verts[tri.y]],
                                              old_to_new_vertex_map[local_corner_verts[tri.z]]};

            const int coffset = tri_count * 3;
            local_tess_corner_vertices[coffset + 0] = int2{int(src_corner_range[tri.x]), -1};
            local_tess_corner_vertices[coffset + 1] = int2{int(src_corner_range[tri.y]), -1};
            local_tess_corner_vertices[coffset + 2] = int2{int(src_corner_range[tri.z]), -1};
            local_tess_corner_weights[tri_count] = float3{0.0f, 0.0f, 0.0f};
            tri_count++;
          }
          else if (num_inner == 1) {

            /*
             * Case
             *
             * 0          2
             *  *---------*    Outside
             *   \       /
             *  ^ x ^ ^ x ^ <- Plane
             *   b \   / a
             *      \ /        Inside
             *       * 1, c
             */
            const int split_index_b = find_or_split_edge(int2{tri.x, tri.y});
            const int split_index_a = find_or_split_edge(int2{tri.y, tri.z});

            const int2 ve_cut_a = index_vert_edge[split_index_a];
            const int2 ve_cut_b = index_vert_edge[split_index_b];

            /* Sorted xy, yz index pairs */
            const int2 pair_a = edge_split_pairs[split_index_a];
            const int2 pair_b = edge_split_pairs[split_index_b];

            const int e0 = form_edge_at_cut(int2{ve_cut_b.x, ve_cut_a.x}, int2{-1, -1});
            const int v1 = local_corner_verts[tri.y];
            const int vc = old_to_new_vertex_map[v1];
            BLI_assert(vertex_flags[v1] & MASK_INSIDE);
            BLI_assert(vc >= 0);

            local_tess_inds[tri_count] = int3{ve_cut_a.x, ve_cut_b.x, vc};
            local_tess_edges[tri_count] = int3{e0, ve_cut_b.y, ve_cut_a.y};

            const int coffset = tri_count * 3;
            local_tess_corner_vertices[coffset + 0] = int2{int(src_corner_range[pair_a.x]),
                                                           int(src_corner_range[pair_a.y])};
            local_tess_corner_vertices[coffset + 1] = int2{int(src_corner_range[pair_b.x]),
                                                           int(src_corner_range[pair_b.y])};
            local_tess_corner_vertices[coffset + 2] = int2{int(src_corner_range[tri.y]), -1};
            local_tess_corner_weights[tri_count] = float3{
                edge_split_weight[split_index_a], edge_split_weight[split_index_b], 0.0f};
            tri_count++;
          }
          else {
            /*
             * Case
             *
             *         1
             *          *              Outside
             *         / \
             *        /   \
             *   ^ ^ x ^ ^ x ^ ^ ^  <- Plane
             *      / II c  \
             *     /   c     \       c == Inner triangulation edge
             *    / c     I   \
             * 2 *-------------*  0    Inside
             *
             *      ^ Inner edge
             */
            const int split_index0 = find_or_split_edge(int2{tri.x, tri.y});
            const int split_index1 = find_or_split_edge(int2{tri.y, tri.z});

            const int2 ve_cut0 = index_vert_edge[split_index0];
            const int2 ve_cut1 = index_vert_edge[split_index1];

            /* Sorted xy, yz index pairs */
            const int2 pair_01 = edge_split_pairs[split_index0];
            const int2 pair_12 = edge_split_pairs[split_index1];

            const int2 zx = int2{tri.z, tri.x};
            const int einner = is_sequential_pair(zx, local_corner_range.size()) ?
                                   old_to_new_edge_map[local_corner_edges[tri.z]] :
                                   find_or_create_inner_edge(zx);

            const int ecut = form_edge_at_cut(int2{ve_cut0.x, ve_cut1.x}, int2{-1, -1});
            const int ediag = form_edge_diagonal(ve_cut0.x, tri.z, -1);
            const int v0 = old_to_new_vertex_map[local_corner_verts[tri.x]];
            const int v2 = old_to_new_vertex_map[local_corner_verts[tri.z]];

            local_tess_inds[tri_count] = int3{v0, ve_cut0.x, v2};
            local_tess_inds[tri_count + 1] = int3{v2, ve_cut0.x, ve_cut1.x};

            local_tess_edges[tri_count] = int3{ve_cut0.y, ediag, einner};
            local_tess_edges[tri_count + 1] = int3{ediag, ecut, ve_cut1.y};

            const int coffset = tri_count * 3;
            local_tess_corner_vertices[coffset + 0] = int2{int(src_corner_range[tri.x]), -1};
            local_tess_corner_vertices[coffset + 1] = int2{int(src_corner_range[pair_01.x]),
                                                           int(src_corner_range[pair_01.y])};
            local_tess_corner_vertices[coffset + 2] = int2{int(src_corner_range[tri.z]), -1};

            local_tess_corner_vertices[coffset + 3] = int2{int(src_corner_range[tri.z]), -1};
            local_tess_corner_vertices[coffset + 4] = int2{int(src_corner_range[pair_01.x]),
                                                           int(src_corner_range[pair_01.y])};
            local_tess_corner_vertices[coffset + 5] = int2{int(src_corner_range[pair_12.x]),
                                                           int(src_corner_range[pair_12.y])};

            local_tess_corner_weights[tri_count] = float3{
                0.0f, edge_split_weight[split_index0], 0.0f};
            local_tess_corner_weights[tri_count + 1] = float3{
                0.0f, edge_split_weight[split_index0], edge_split_weight[split_index1]};

            local_tess_poly_src[tri_count + 1] = index_poly;
            tri_count += 2;
          }
        }
      });

  const int num_copied_corners = poly_type_selections[EdgeIntersectType::Kept].parallel_reduce(
      GrainSize(4096),
      0,
      [&](const int64_t index_face, const int64_t index_pos, const int &identity) {
        return int(src_polys[index_face].size());
      },
      [](int a, int b) { return a + b; });

  /* Group Descriptors */
  std::array<VariantVertexGroup, 2> vertex_groups = {
      MeshVertexGroupCopyMask{kept_vertices},
      MeshVertexGroupLinear{vertex_split_vertices.as_span(), vertex_split_weights.as_span()}};

  std::array<VariantEdgeGroup, 2> edge_groups = {
      MeshEdgeGroupCopyMask{edge_type_selections[EdgeIntersectType::Kept],
                            old_to_new_vertex_map.as_span()},
      MeshEdgeGroupPair{edge_output_vertex_pairs.as_span(), edge_output_src_edges.as_span()}};

  std::array<VariantPolygonGroup, 2> poly_groups = {
      MeshFaceGroupCopyMask{num_copied_corners,
                            poly_type_selections[EdgeIntersectType::Kept],
                            old_to_new_vertex_map.as_span(),
                            old_to_new_edge_map.as_span()},
      MeshTriangleGroup{tesselation_polygon_indices,
                        tesselation_vertex_indices,
                        tesselation_edge_indices,
                        tesselation_corner_weights,
                        tesselation_corner_vertices}};

  /* Create new mesh */
  Mesh *result = new_mesh_from_groups(mesh, vertex_groups, edge_groups, poly_groups);
  transfer_vertex_data(mesh, *result, vertex_groups, attribute_filter);
  transfer_edge_data(mesh, *result, edge_groups, attribute_filter);
  transfer_polygon_data(mesh, *result, poly_groups, attribute_filter);

  /* Copy kept data */

  // TODO

  /* Handle intersected data */

  // TODO

  return {result, BisectResult::Bisect};
}  // namespace blender::geometry

}  // namespace blender::geometry
