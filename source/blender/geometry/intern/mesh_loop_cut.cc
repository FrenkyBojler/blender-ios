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
#include "BKE_mesh_mapping.hh"
#include "BLI_math_geom.h"

#include "GEO_mesh_copy_selection.hh"
#include "GEO_mesh_loop_cut.hh"
#include <algorithm>
#include <variant>

namespace blender::geometry {

/* Enums */

/* Type polygon faces are filtered by as defined by loop cut traversal over the face.
 */
enum CutFaceType { Kept = 0, Adjacent = 1, SingleLoop = 2, DoubleLoop = 3, Count = 4 };

enum TraverseFlags {
  /* Edge is not associated with a loop cut */
  NO_LOOP = 0,
  /* Edge associated with a loop cut. */
  IS_SPLIT = 1,
  /* The order of the loop cut (factors) is inverted (flipped) relative to the edge vertex order.
   */
  IS_INVERTED = 2
};

/*Forw decl.*/
void compute_polygon_vert_indices_from_edge_data(OffsetIndices<int> dst_face_offsets_slice,
                                                 Span<int2> dst_edges,
                                                 Span<int> edge_corner_indices,
                                                 MutableSpan<int> face_corner_indices);

/* Implementation constants */

const int64_t GRAIN_SIZE = 4096;
const int64_t GRAIN_SIZE_LARGE = 16384;

/* Mesh declaration constructs */

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

  MeshVertexGroupLinear(Span<int2> src_indices, Span<float> weights)
      : src_indices(src_indices), weights(weights), sample_dst_bilinear(false)
  {
  }
  MeshVertexGroupLinear(Span<int2> src_indices, Span<float> weights, bool sample_dst_bilinear)
      : src_indices(src_indices), weights(weights), sample_dst_bilinear(sample_dst_bilinear)
  {
  }
  Span<int2> src_indices;
  Span<float> weights;

  /* Sample data from the target buffer instead of source, effectively sampling bilinearly if both
   * values are linearly interpolated samples. Ensure samples are valid! */
  bool sample_dst_bilinear;
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

  void fill_corner_table(const Span<int2> dst_edges,
                         MutableSpan<int> offset_slice,
                         MutableSpan<int> dst_corner_verts,
                         MutableSpan<int> dst_corner_edges) const
  {

    /* Fill corner offsets */
    for (const int64_t index : offset_slice.index_range().drop_front(1)) {
      offset_slice[index] = offset_slice[index - 1] + 3;
    }

    Span<int> group_edge_indices = edge_indices.cast<int>();
    dst_corner_edges.copy_from(group_edge_indices);

    if (vertex_indices.data() == nullptr) {
      compute_polygon_vert_indices_from_edge_data(
          OffsetIndices<int>(offset_slice), dst_edges, group_edge_indices, dst_corner_verts);
    }
    else {
      dst_corner_verts.copy_from(vertex_indices.cast<int>());
    }
  }
};

class MeshFaceGroupData {
 public:
  Vector<int> src_polygon_indices;
  /*
   * Face to corner offset table.
   */
  Vector<int> corner_offset_data;
  OffsetIndices<int> corner_offsets;

  /*
   * Edge index sets, indices point to edges in the target mesh.
   */
  Vector<int> edge_indices;
  Vector<float> src_weights;
  Vector<int2> src_corners;

  /*
   * Construct from a pre-constructed corner offset table.
   */
  MeshFaceGroupData(OffsetIndices<int> corner_offsets)
      : src_polygon_indices(corner_offsets.size()),
        corner_offsets(corner_offsets),
        edge_indices(corner_offsets.last()),
        src_weights(corner_offsets.last()),
        src_corners(corner_offsets.last())
  {
  }

  /*
   * Construct with no pre-constructed corner offset table.
   */
  MeshFaceGroupData(const int num_faces, const int num_corners)
      : src_polygon_indices(num_faces),
        corner_offset_data(num_faces + 1),
        corner_offsets(corner_offset_data.as_span()),
        edge_indices(num_corners),
        src_weights(num_corners),
        src_corners(num_corners)
  {
    corner_offset_data[0] = 0;
  }

  bool has_content() const
  {
    return src_polygon_indices.size() > 0;
  }

  int64_t num_faces() const
  {
    BLI_assert(src_polygon_indices.size() == corner_offsets.size());
    BLI_assert(corner_offsets.last() == edge_indices.size());
    BLI_assert(edge_indices.size() == src_corners.size());
    BLI_assert(edge_indices.size() == src_weights.size());
    return src_polygon_indices.size();
  }

  int64_t num_corners() const
  {
    return edge_indices.size();
  }
  void fill_corner_table(const Span<int2> dst_edges,
                         MutableSpan<int> offset_slice,
                         MutableSpan<int> dst_corner_verts,
                         MutableSpan<int> dst_corner_edges) const
  {
    for (const int64_t index : offset_slice.index_range().drop_back(1)) {
      offset_slice[index + 1] = offset_slice[index] + corner_offsets[index].size();
    }

    dst_corner_edges.copy_from(edge_indices);

    compute_polygon_vert_indices_from_edge_data(
        OffsetIndices<int>(offset_slice), dst_edges, edge_indices, dst_corner_verts);
  }
};

class MeshFaceBilinearGroupData {
 public:
  Vector<int> src_polygon_indices;
  Vector<int4> src_corners;

  /*
   * Edge index sets, indices point to edges in the target mesh.
   */
  Vector<int> src_face;
  Vector<int> edge_indices;
  Vector<float2> corner_weights;

  /*
   * Face to corner offset table.
   */
  Vector<int> corner_offset_data;
  OffsetIndices<int> corner_offsets;

  /*
   * Construct with no pre-constructed corner offset table.
   */
  MeshFaceBilinearGroupData(const int num_source_faces, const int num_faces, const int num_corners)
      : src_polygon_indices(num_source_faces),
        src_corners(num_source_faces),
        src_face(num_corners),
        edge_indices(num_corners),
        corner_weights(num_corners),
        corner_offset_data(num_faces + 1),
        corner_offsets(corner_offset_data.as_span())
  {
    corner_offset_data[0] = 0;
  }

  bool has_content() const
  {
    return src_polygon_indices.size() > 0;
  }

  virtual int64_t num_faces() const
  {
    BLI_assert(src_polygon_indices.size() == src_corners.size());
    BLI_assert(edge_indices.size() == corner_offsets.last());
    BLI_assert(edge_indices.size() == corner_weights.size());
    BLI_assert(edge_indices.size() == src_face.size());
    return corner_offsets.size();
  }

  virtual int64_t num_corners() const
  {
    return edge_indices.size();
  }
  virtual void fill_corner_table(const Span<int2> dst_edges,
                                 MutableSpan<int> offset_slice,
                                 MutableSpan<int> dst_corner_verts,
                                 MutableSpan<int> dst_corner_edges) const
  {
    for (const int64_t index : offset_slice.index_range().drop_back(1)) {
      offset_slice[index + 1] = offset_slice[index] + corner_offsets[index].size();
    }

    dst_corner_edges.copy_from(edge_indices);

    compute_polygon_vert_indices_from_edge_data(
        OffsetIndices<int>(offset_slice), dst_edges, edge_indices, dst_corner_verts);
  }

  template<typename T> void gather_face_information(Span<T> src_data, MutableSpan<T> dst_data)
  {
    threading::parallel_for(corner_offsets.index_range(), GRAIN_SIZE, [&](IndexRange subrange) {
      for (const int64_t face_index : subrange) {
        dst_data[face_index] = src_data[src_face[face_index]];
      }
    });
  }

  template<typename T>
  void gather_attribute(Span<T> src_data,
                        OffsetIndices<int> dst_face_offsets,
                        IndexRange face_range,
                        MutableSpan<T> dst_data)
  {

    const int64_t first_corner = dst_face_offsets[face_range.first()].first();
    threading::parallel_for(face_range, GRAIN_SIZE, [&](IndexRange subrange) {
      for (const int64_t face_index : subrange) {
        for (const int64_t dst_corner_index : dst_face_offsets[face_index]) {
          const int64_t src_corner_index = dst_corner_index - first_corner;
          const int64_t src_index = src_face[src_corner_index];
          const int4 cpair = src_corners[src_index];
          const float2 weights = corner_weights[src_corner_index];
          BLI_assert(weights.x >= 0.0f && weights.x <= 1.0f);
          BLI_assert(weights.y >= 0.0f && weights.y <= 1.0f);

          T a = bke::attribute_math::mix2(weights.x, src_data[cpair.x], src_data[cpair.y]);
          T b = bke::attribute_math::mix2(weights.x, src_data[cpair.w], src_data[cpair.z]);
          dst_data[dst_corner_index] = bke::attribute_math::mix2(weights.y, a, b);
        }
      }
    });
  }
};

struct MeshFaceGroupCopyMask {

  int num_corners;
  /*
   * Mask for the faces to copy from the data source.
   */
  IndexMask src_indices;

  /* Optional vertex mapping table, if NULL indices are assumed to be identical to source. */
  Span<int> mapping_table_verts;
  Span<int> mapping_table_edges;

  /* Compute the corner count for the copied polygon faces. */
  MeshFaceGroupCopyMask init_num_corners(OffsetIndices<int> src_face_corners)
  {
    this->num_corners = threading::parallel_reduce(
        src_indices.index_range(),
        GRAIN_SIZE_LARGE,
        0,
        [&](const IndexRange range, int identity) {
          for (const int64_t index : range) {
            identity += int(src_face_corners[src_indices[index]].size());
          }
          return identity;
        },
        [](int a, int b) { return a + b; });
    return *this;
  }

  void fill_corner_table(const OffsetIndices<int> src_corner_offsets,
                         const Span<int> src_corner_verts,
                         const Span<int> src_corner_edges,
                         MutableSpan<int> offset_slice,
                         MutableSpan<int> dst_corner_verts,
                         MutableSpan<int> dst_corner_edges) const
  {
    if (mapping_table_verts.data() == nullptr) {
      BLI_assert(mapping_table_edges.data() != nullptr);
      for (const int64_t local_face_index : offset_slice.index_range().drop_back(1)) {
        const IndexRange src_range = src_corner_offsets[src_indices[local_face_index]];
        const IndexRange dst_range(offset_slice[local_face_index], src_range.size());
        offset_slice[local_face_index + 1] = dst_range.one_after_last();

        for (const int64_t index : src_range.index_range()) {
          dst_corner_verts[dst_range[index]] = src_corner_verts[src_range[index]];
          dst_corner_edges[dst_range[index]] =
              mapping_table_edges[src_corner_edges[src_range[index]]];
          BLI_assert(dst_corner_verts[dst_range[index]] >= 0);
          BLI_assert(dst_corner_edges[dst_range[index]] >= 0);
        }
      }
    }
    else {
      BLI_assert(mapping_table_verts.data() != nullptr);
      BLI_assert(mapping_table_edges.data() != nullptr);
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
  }
};

/*
 * Descriptor variants for mesh vertex groups/sets.
 */
using VariantVertexGroup = std::variant<MeshVertexGroupCopyMask, MeshVertexGroupLinear>;
/*
 * Descriptor variants for mesh edge groups/sets.
 */
using VariantEdgeGroup = std::variant<MeshEdgeGroupCopyMask, MeshEdgeGroupPair>;
/*
 * Descriptor variants for mesh polygon groups/sets.
 */
using VariantPolygonGroup = std::variant<MeshFaceGroupCopyMask,
                                         MeshTriangleGroup,
                                         MeshFaceGroupData *,
                                         MeshFaceBilinearGroupData *>;

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

/* Get the face data shape for the group on the form (number of faces, number of corners).
 */
int2 get_face_shape(const VariantPolygonGroup &group)
{
  if (auto item = std::get_if<MeshTriangleGroup>(&group)) {
    return int2{int(item->num_faces()), int(item->num_corners())};
  }
  else if (auto item = std::get_if<MeshFaceGroupCopyMask>(&group)) {
    return int2{int(item->src_indices.size()), item->num_corners};
  }
  else if (auto item = std::get_if<MeshFaceGroupData *>(&group)) {
    return int2{int((*item)->num_faces()), int((*item)->num_corners())};
  }
  else if (auto item = std::get_if<MeshFaceBilinearGroupData *>(&group)) {
    return int2{int((*item)->num_faces()), int((*item)->num_corners())};
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
  return int2{face_count, offset};
}

void transfer_vertex_data(const Mesh &src_mesh,
                          Mesh &dst_mesh,
                          const Span<VariantVertexGroup> vertex_groups,
                          const bke::AttributeFilter &attribute_filter)
{
  const bke::AttributeAccessor src_attributes = src_mesh.attributes();
  bke::MutableAttributeAccessor dst_attributes = dst_mesh.attributes_for_write();

  Set<std::string> copy_point_skip;

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
          const Span<T> sample_data = item->sample_dst_bilinear ? dst_data : src_data;

          threading::parallel_for(group_range, 512, [&](IndexRange group_slice) {
            const IndexRange dst_range = group_slice.shift(group_offset);

            /* Linear interpolate */
            for (const int64_t i : group_slice.index_range()) {
              const int64_t sample_index = group_slice[i];
              int2 src_vert_indices = item->src_indices[sample_index];
              const T value = bke::attribute_math::mix2(item->weights[sample_index],
                                                        sample_data[src_vert_indices.x],
                                                        sample_data[src_vert_indices.y]);
              dst_data[dst_range[i]] = value;
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
        if (item->mapping_table.is_empty()) {
          item->src_indices.foreach_index(
              GrainSize(GRAIN_SIZE_LARGE),
              [&](int64_t i, int64_t pos) { dst_edge_slice[pos] = src_edges[i]; });
        }
        else {
          item->src_indices.foreach_index(
              GrainSize(GRAIN_SIZE_LARGE), [&](int64_t i, int64_t pos) {
                int2 src_index_pair = src_edges[i];
                int2 mapped_pair = int2(item->mapping_table[src_index_pair.x],
                                        item->mapping_table[src_index_pair.y]);
                dst_edge_slice[pos] = mapped_pair;
              });
        }
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
    const CPPType &cpp_type = bke::attribute_type_to_cpp_type(attribute.meta_data.data_type);
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

/* Find the other value from `a` that is not `b`.
 */
int get_unshared_index(int2 a, int b)
{
  BLI_assert(b == a.x || b == a.y);
  return b == a.x ? a.y : a.x;
}

void compute_polygon_vert_indices_from_edge_data(OffsetIndices<int> face_offsets,
                                                 Span<int2> dst_edges,
                                                 Span<int> edge_corner_indices,
                                                 MutableSpan<int> face_corner_indices)
{
  threading::parallel_for(face_offsets.index_range(), GRAIN_SIZE, [&](IndexRange subrange) {
    for (const int64_t face_index : subrange) {
      const IndexRange src_range = face_offsets[face_index];
      const IndexRange dst_range(src_range.first() - face_offsets.first(), src_range.size());
      const Span<int> src_edges = edge_corner_indices.slice(dst_range);
      BLI_assert(src_edges.size() > 2);

      /* Transfer corners */
      const int2 initial_sort = sort_shared_index(dst_edges[src_edges.first()],
                                                  dst_edges[src_edges[1]]);

      const int2 last_sort = sort_shared_index(dst_edges[src_edges.first()],
                                               dst_edges[src_edges.last()]);
      BLI_assert(initial_sort.y == last_sort.x);

      int common = initial_sort.y; /* Initial vert shared with the last edge! */
      for (const int64_t i : src_edges.index_range()) {
        face_corner_indices[dst_range[i]] = common;
        common = get_unshared_index(dst_edges[src_edges[i]], common);
      }
    }
  });
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

    dst_face_offsets[0] = 0;
    for (int64_t group_index = 0; group_index < poly_groups.size(); group_index++) {

      const int2 face_shape = get_face_shape(poly_groups[group_index]);
      const IndexRange dst_corner_range(tot_corner_offset, face_shape.y);
      MutableSpan<int> dst_face_offsets_slice = dst_face_offsets.slice(
          IndexRange(tot_face_count, face_shape.x + 1));

      tot_face_count += face_shape.x;
      tot_corner_offset += face_shape.y;
      group_face_count[group_index] = face_shape.x;

      if (face_shape.x == 0) {
        BLI_assert(face_shape.y == 0);
        continue;
      }
      BLI_assert(face_shape.y > 0);

      /* Write face corner offsets */
      if (auto item = std::get_if<MeshTriangleGroup>(&poly_groups[group_index])) {
        item->fill_corner_table(dst_edges,
                                dst_face_offsets_slice,
                                dst_vert_corners.slice(dst_corner_range),
                                dst_edge_corners.slice(dst_corner_range));
      }
      else if (auto item = std::get_if<MeshFaceBilinearGroupData *>(&poly_groups[group_index])) {
        (*item)->fill_corner_table(dst_edges,
                                   dst_face_offsets_slice,
                                   dst_vert_corners.slice(dst_corner_range),
                                   dst_edge_corners.slice(dst_corner_range));
      }
      else if (auto item = std::get_if<MeshFaceGroupData *>(&poly_groups[group_index])) {
        (*item)->fill_corner_table(dst_edges,
                                   dst_face_offsets_slice,
                                   dst_vert_corners.slice(dst_corner_range),
                                   dst_edge_corners.slice(dst_corner_range));
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
    }
  }

  /* Copy face domain. */
  for (bke::AttributeTransferData &attribute : bke::retrieve_attributes_for_transfer(
           src_attributes,
           dst_attributes,
           ATTR_DOMAIN_MASK_FACE,
           bke::attribute_filter_with_skip_ref(attribute_filter, copy_poly_skip)))
  {
    const CPPType &cpp_type = bke::attribute_type_to_cpp_type(attribute.meta_data.data_type);
    bke::attribute_math::convert_to_static_type(cpp_type, [&](auto dummy) {
      using T = decltype(dummy);

      Span<T> src_data = attribute.src.template typed<T>();
      MutableSpan<T> dst_data = attribute.dst.span.typed<T>();

      int64_t face_offset = 0;
      for (int64_t group_index = 0; group_index < poly_groups.size(); group_index++) {
        const IndexRange dst_range(face_offset, group_face_count[group_index]);

        if (auto item = std::get_if<MeshTriangleGroup>(&poly_groups[group_index])) {
          array_utils::gather(src_data, item->src_polygon_indices, dst_data.slice(dst_range));
        }
        else if (auto item = std::get_if<MeshFaceBilinearGroupData *>(&poly_groups[group_index])) {
          (*item)->gather_face_information<T>(src_data, dst_data.slice(dst_range));
        }
        else if (auto item = std::get_if<MeshFaceGroupData *>(&poly_groups[group_index])) {
          array_utils::gather(
              src_data, (*item)->src_polygon_indices.as_span(), dst_data.slice(dst_range));
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
      const CPPType &cpp_type = bke::attribute_type_to_cpp_type(attribute.meta_data.data_type);
      bke::attribute_math::convert_to_static_type(cpp_type, [&](auto dummy) {
        using T = decltype(dummy);

        Span<T> src_data = attribute.src.template typed<T>();
        MutableSpan<T> dst_data = attribute.dst.span.typed<T>();

        int64_t face_offset = 0;
        for (int64_t group_index = 0; group_index < poly_groups.size(); group_index++) {
          if (auto item = std::get_if<MeshTriangleGroup>(&poly_groups[group_index])) {
            const IndexRange face_range(face_offset, group_face_count[group_index]);
            const Span<float> src_weights = item->src_weights.cast<float>();
            const int64_t first_corner = dst_face_offsets[face_range.first()].first();

            threading::parallel_for(face_range, GRAIN_SIZE, [&](IndexRange subrange) {
              for (const int64_t face_index : subrange) {
                for (const int64_t dst_corner_index : dst_face_offsets[face_index]) {
                  const int64_t src_index = dst_corner_index - first_corner;
                  const int2 cpair = item->src_corners[src_index];
                  /* Checking if cpair.y == -1 but catching 0.0f weights */
                  BLI_assert(cpair.y >= 0 || src_weights[src_index] == 0.0f);
                  BLI_assert(src_weights[src_index] >= 0.0f && src_weights[src_index] <= 1.0f);
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
          else if (auto item = std::get_if<MeshFaceBilinearGroupData *>(&poly_groups[group_index]))
          {
            const IndexRange face_range(face_offset, group_face_count[group_index]);
            (*item)->gather_attribute<T>(src_data, dst_face_offsets, face_range, dst_data);
          }
          else if (auto item = std::get_if<MeshFaceGroupData *>(&poly_groups[group_index])) {
            /* Identical to above...*/
            const IndexRange face_range(face_offset, group_face_count[group_index]);
            const Span<float> src_weights = (*item)->src_weights.as_span().cast<float>();
            const int64_t first_corner = dst_face_offsets[face_range.first()].first();

            threading::parallel_for(face_range, GRAIN_SIZE, [&](IndexRange subrange) {
              for (const int64_t face_index : subrange) {
                for (const int64_t dst_corner_index : dst_face_offsets[face_index]) {
                  const int64_t src_index = dst_corner_index - first_corner;
                  const int2 cpair = (*item)->src_corners[src_index];
                  /* Checking if cpair.y == -1 but catching 0.0f weights */
                  BLI_assert(cpair.y >= 0 || src_weights[src_index] == 0.0f);
                  BLI_assert(src_weights[src_index] >= 0.0f && src_weights[src_index] <= 1.0f);
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
            const IndexRange local_face_range(group_face_count[group_index]);
            threading::parallel_for(local_face_range, GRAIN_SIZE, [&](IndexRange subrange) {
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

/* Sort the indices so that x < y.
 */
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

/*
* Revertible index range, allowing access to index in the reverse order, example:
*
* IndexRange a(0, 3);
* IndexRange b = a.reverted();

* assert(b[2] == a[0]); // == 0
*
*/
class IndexRangeSigned {
 private:
  int start_;
  int end_;
  int sign_;

  const static int XOR_SIGN = int(0xFFFFFFFF);

  IndexRangeSigned(int start, int end, int sign) : start_(start), end_(end), sign_(sign)
  {
    BLI_assert(sign_ != XOR_SIGN || end_ < start_);
    BLI_assert(sign_ == XOR_SIGN || end_ >= start_);
  }

 public:
  IndexRangeSigned() : IndexRangeSigned(0, 0, 0) {}
  IndexRangeSigned(int start, int size) : IndexRangeSigned(start, start + size, 0) {}
  IndexRangeSigned(IndexRange range) : IndexRangeSigned(range.start(), range.one_after_last(), 0)
  {
  }
  IndexRangeSigned(const IndexRangeSigned &other)
      : IndexRangeSigned(other.start_, other.end_, other.sign_)
  {
  }

  IndexRangeSigned &operator=(const IndexRangeSigned &other)
  {
    start_ = other.start_;
    end_ = other.end_;
    sign_ = other.sign_;
    return *this;
  }

  int first() const
  {
    return start_ + sign_;
  }

  int last() const
  {
    return end_ + (sign_ ^ XOR_SIGN);
  }

  int size() const
  {
    /* Note: XOR works only because reverted ranges (negative) have range[0] == start_ - 1, and
     * because (0xFFFFFFFF ^ 0) == -1. This does not work for size!  */
    return std::abs(start_ - end_);
  }

  /*
   * True if the range is in reverse so that end < start.
   */
  bool is_reverse() const
  {
    return sign_ < 0;
  }

  void reverse()
  {
    std::swap(start_, end_);
    sign_ = end_ < start_ ? XOR_SIGN : 0;
  }

  IndexRangeSigned reverted() const
  {
    IndexRangeSigned range = *this;
    range.reverse();
    return range;
  }

  /*
   * Get the range with interval [0, size).
   */
  IndexRange index_range()
  {
    return IndexRange(0, size());
  }

  /*
   * Get the `index_range()` with the signed interval [0, size) or (size, 0] that shares the same
   * sign as `this` range. Increments will be negative if `this` steps with negative increments
   * (end < start).
   */
  IndexRangeSigned signed_range()
  {
    if (is_reverse()) {
      return IndexRangeSigned(start_ - end_, 0, XOR_SIGN);
    }
    else {
      return IndexRangeSigned(0, end_ - start_, 0);
    }
  }

  /*
   * Reduce the size by one, removes the highest index (first index if reversed, last otherwise).
   */
  IndexRangeSigned reduce(int count)
  {
    BLI_assert(count >= 0 && count <= size());
    if (is_reverse()) {
      return IndexRangeSigned(start_ - count, end_, sign_);
    }
    else {
      return IndexRangeSigned(start_, end_ - count, sign_);
    }
  }

  int operator[](int index) const
  {
    BLI_assert(index >= 0 && index < size());
    return start_ + int(sign_ ^ index);
  }
};

bool valid_face(const IndexRange loops, const bool affected_status)
{
  return affected_status && loops.size() == 4;
}

bool has_no_matching_loop(const IndexRange loop_range)
{
  return loop_range.size() != 2;
}

/* Std::merge but avoids duplicates (assuming set a and b does not contain unique elements by
 * themselves). Also deals with order inversion of the factors...
 */
float *merge_no_duplicates(
    const float *a_beg, const float *a_end, const float *b_beg, const float *b_end, float *dst)
{
  const bool b_inverted = b_end < b_beg;
  const int bstep = b_inverted ? -1 : 1;
  if (b_beg == b_end) {
    return std::copy(a_beg, a_end, dst);
  }

  /* Handle initial case, this is done to have a dst value for comparison in the main loop. */
  float b_value = b_inverted ? 1.0f - *b_beg : *b_beg;
  if (a_beg == a_end || b_value < *a_beg) {
    *dst++ = b_value;
    b_beg += bstep;
    b_value = b_inverted ? 1.0f - *b_beg : *b_beg;
    if (b_beg == b_end) {
      return std::copy(a_beg, a_end, dst);
    }
  }
  else {
    *dst++ = *a_beg++;
  }

  while (a_beg != a_end) {
    if (b_value < *a_beg) {
      if (b_value != *(dst - 1)) {
        *dst++ = b_value;
      }
      b_beg += bstep;
      if (b_beg == b_end) {
        /* No duplicate can exist as b < a */
        return std::copy(a_beg, a_end, dst);
      }
      b_value = b_inverted ? 1.0f - *b_beg : *b_beg;
    }
    else {
      /* Assume A has no duplicates in itself. */
      *dst++ = *a_beg++;
    }
  }

  while (b_beg != b_end && (b_inverted ? 1.0f - *b_beg : *b_beg) == *(dst - 1)) {
    b_beg += bstep;
  }
  while (b_beg != b_end) {
    *dst = b_inverted ? 1.0f - *b_beg : *b_beg;
    ++dst;
    b_beg += bstep;
  }
  return dst;
}

void join_factor_sets(const Span<float> factors_a,
                      const Span<float> factors_b,
                      Vector<float> &dst,
                      const bool inverted_b)
{
  BLI_assert(dst.data() != factors_a.data() && dst.data() != factors_b.data());
  dst.resize(factors_a.size() + factors_b.size());

  /* Merge ordered and sorted unique elements */
  float *new_end = merge_no_duplicates(factors_a.begin(),
                                       factors_a.end(),
                                       inverted_b ? factors_b.end() - 1 : factors_b.begin(),
                                       inverted_b ? factors_b.begin() - 1 : factors_b.end(),
                                       dst.begin());
  dst.resize(new_end - dst.begin());
}

int8_t make_visit_flag(bool inverted)
{
  return int8_t(TraverseFlags::IS_SPLIT |
                (inverted ? TraverseFlags::IS_INVERTED : TraverseFlags::NO_LOOP));
}

/* Get the opposite loop on the quad. */
int get_opposite_loop(const IndexRange loop_range, const int64_t loop_index)
{
  BLI_assert(loop_range.size() == 4);
  return int((loop_index + 2) % 4 + loop_range.first());
}

/* Get the other loop associated with the edge if any. */
int get_other_edge_loop(const Span<int> span, const int loop_index)
{
  BLI_assert(span.size() == 2);
  return span[0] == loop_index ? span[1] : span[0];
}

void traverse_loops(const Mesh &src_mesh,
                    Span<bool> affected_faces,
                    OffsetIndices<int> loop_cut_factor_offsets,
                    Span<float> loop_cut_factors,
                    IndexMask loop_cut_from,
                    MutableSpan<int8_t> traverse_flags,
                    MutableSpan<int> traversed_edge_by,
                    MutableSpan<bool> traversed_faces,
                    MutableSpan<Vector<float>> cut_factor_set,
                    MutableSpan<Vector<float>> cut_factors_inverted,
                    MutableSpan<int> loop_id_remap)
{
  const int src_num_vert = src_mesh.verts_num;
  const int src_num_edges = src_mesh.edges_num;

  const Span<int2> src_edge_indices = src_mesh.edges();
  const OffsetIndices src_face_offsets = src_mesh.faces();
  const Span<int> src_corner_edges = src_mesh.corner_edges();
  const Span<int> src_corner_verts = src_mesh.corner_verts();

  Span<int> corner_to_face_map = src_mesh.corner_to_face_map();

  Array<int> map_offsets;
  Array<int> map_indices;
  const GroupedSpan<int> edge_to_loop_map = bke::mesh::build_edge_to_corner_map(
      src_mesh.corner_edges(), src_mesh.edges_num, map_offsets, map_indices);

  loop_cut_from.foreach_index(
      GrainSize(GRAIN_SIZE), [&](const int64_t edge_index, const int64_t index) {
        Vector<float> tmp_factor_data;
        Vector<float> *tmp_factors = &tmp_factor_data;
        Vector<float> *cut_factors_a = &cut_factor_set[index];
        loop_id_remap[index] = index;
        cut_factors_a->extend(loop_cut_factors.slice(loop_cut_factor_offsets[edge_index]));

        auto fn_append_loop_edge_factors = [&](IndexRange factor_range,
                                               const bool inverted_order) {
          if (factor_range.size()) {
            join_factor_sets(*cut_factors_a,
                             loop_cut_factors.slice(factor_range),
                             *tmp_factors,
                             inverted_order);
            std::swap(tmp_factors, cut_factors_a);
          }
        };

        auto fn_traverse = [&](int current_edge_loop,
                               int next_face,
                               const bool aligned_edge_loop) {
          IndexRange face_loops = src_face_offsets[next_face];
          traversed_faces[next_face] = true;

          while (valid_face(face_loops, affected_faces[next_face])) {

            const int opposite_edge_loop = get_opposite_loop(
                face_loops, current_edge_loop - face_loops.first());
            const int next_edge = src_corner_edges[opposite_edge_loop];

            const bool aligned_opposite = src_corner_verts[opposite_edge_loop] ==
                                          src_edge_indices[next_edge].x;
            const bool reversed_edge_order = aligned_edge_loop == aligned_opposite;
            fn_append_loop_edge_factors(loop_cut_factor_offsets[next_edge], reversed_edge_order);

            /* Stop if traversed by self or an earlier loop (with lower index ID).
             * TODO: FIX THIS; SKIPS ON GREATER EQ, NEED TO HANDLE 0 CASE TO FIX
             */
            if (traverse_flags[next_edge] > 0 && traversed_edge_by[next_edge] >= index) {
              /* Edge is already traversed! */
              loop_id_remap[index] = traversed_edge_by[next_edge];
              break;
            }

            traverse_flags[next_edge] = make_visit_flag(reversed_edge_order);
            traversed_edge_by[next_edge] = index;

            const Span<int> edge_loops = edge_to_loop_map[next_edge];
            if (edge_loops.size() < 2) {
              /* No other loop to continue with */
              break;
            }

            current_edge_loop = get_other_edge_loop(edge_loops, opposite_edge_loop);
            next_face = corner_to_face_map[current_edge_loop];
            traversed_faces[next_face] = true;
            if (edge_loops.size() > 2) {
              /* No matching loop to continue with, needs to be checked after as face needs to be
               * tagged (if it exists)! */
              break;
            }

            face_loops = src_face_offsets[next_face];
          }
        };

        /* Forward traverse (direction defined by 'adjacent' loop in the starting face) */
        traverse_flags[edge_index] = make_visit_flag(false);
        traversed_edge_by[edge_index] = index;

        const Span<int> edge_loops = edge_to_loop_map[edge_index];
        if (edge_loops.size() > 0 && edge_loops.size() <= 2) {
          const int loop_index = edge_loops.first();

          bool forward = src_corner_verts[loop_index] == src_edge_indices[edge_index].x;

          fn_traverse(loop_index, corner_to_face_map[loop_index], forward);

          /* Backward traverse */
          if (edge_loops.size() == 2) {
            /* Matching loop to continue with */
            const int other_loop = edge_loops[1];
            fn_traverse(other_loop, corner_to_face_map[other_loop], !forward);
          }
        }

        /* Update the list used if necessary! */
        if (cut_factor_set[index] != *cut_factors_a) {
          cut_factor_set[index] = std::move(*cut_factors_a);
        }
      });

  loop_cut_from.foreach_index([&](const int64_t edge_index, const int64_t index) {
    int prev = loop_id_remap[index];
    while (prev != loop_id_remap[prev]) {
      prev = loop_id_remap[prev];
    }
    if (loop_id_remap[index] != prev) {
      /* No need to process overlapped loop groups */
      loop_id_remap[index] = prev;
      return;
    }
    /*  */
    const int64_t size = cut_factor_set[index].size();
    cut_factors_inverted[index].resize(size);

    for (const int64_t i : cut_factor_set[index].index_range()) {
      cut_factors_inverted[index][size - 1 - i] = clamp_f(
          1.0f - cut_factor_set[index][i], 0.0f, 1.0f);
    }
  });
}

template<typename FetchEdge>
void filter_faces_by_type(const Mesh &src_mesh,
                          FetchEdge &&fn_fetch_edge,
                          MutableSpan<bool> traversed_faces,
                          IndexMaskMemory &memory,
                          MutableSpan<IndexMask> face_type_selections)
{
  const OffsetIndices src_face_offsets = src_mesh.faces();
  const Span<int> src_corner_edges = src_mesh.corner_edges();

  IndexMask::from_groups<int64_t>(
      IndexMask(src_mesh.faces_num),
      memory,
      [&](int64_t face_index) {
        if (!traversed_faces[face_index]) {
          return int(CutFaceType::Kept);
        }

        const IndexRange face_corners = src_face_offsets[face_index];
        if (face_corners.size() != 4) {
          return int(CutFaceType::Adjacent);
        }

        Vector<int, 4> num_loop_cuts;
        for (const int64_t corner_index : face_corners) {
          std::pair<int8_t, Span<float>> info = fn_fetch_edge(src_corner_edges[corner_index]);
          if (info.first > 0) {
            num_loop_cuts.append(info.second.size());
          }
        }
        if (num_loop_cuts.size() == 2 && num_loop_cuts[0] == num_loop_cuts[1]) {
          return int(CutFaceType::SingleLoop);
        }
        if (num_loop_cuts.size() == 4 && num_loop_cuts[0] == num_loop_cuts[2] &&
            num_loop_cuts[1] == num_loop_cuts[3])
        {
          return int(CutFaceType::DoubleLoop);
        }
        return int(CutFaceType::Adjacent);
      },
      face_type_selections);
}

template<typename FetchEdge>
void count_face_split_shapes(const Mesh &src_mesh,
                             FetchEdge &&fn_fetch_edge,
                             const Span<IndexMask> face_type_masks,
                             Vector<int> &vertex_offsets,
                             std::array<Vector<int>, 2> &edge_offsets,
                             std::array<Vector<int>, 3> &face_offsets,
                             std::array<Vector<int>, 3> &corner_offsets)
{
  const Span<int2> src_edge_indices = src_mesh.edges();
  const OffsetIndices src_face_offsets = src_mesh.faces();
  const Span<int> src_corner_edges = src_mesh.corner_edges();
  const Span<int> src_corner_verts = src_mesh.corner_verts();

  /* Count number of elements formed inside split faces (not shared with other faces). */
  edge_offsets[0].resize(face_type_masks[CutFaceType::DoubleLoop].size() + 1);
  edge_offsets[1].resize(face_type_masks[CutFaceType::SingleLoop].size() + 1);
  edge_offsets[0][0] = 0;
  edge_offsets[1][0] = 0;

  vertex_offsets.resize(face_type_masks[CutFaceType::DoubleLoop].size() + 1);
  vertex_offsets[0] = 0;

  face_offsets[0].resize(face_type_masks[CutFaceType::DoubleLoop].size() + 1);
  face_offsets[1].resize(face_type_masks[CutFaceType::SingleLoop].size() + 1);
  face_offsets[0][0] = 0;
  face_offsets[1][0] = 0;

  corner_offsets[0].resize(face_type_masks[CutFaceType::DoubleLoop].size() + 1);
  corner_offsets[1].resize(face_type_masks[CutFaceType::SingleLoop].size() + 1);
  corner_offsets[2].resize(face_type_masks[CutFaceType::Adjacent].size() + 1);
  corner_offsets[0][0] = 0;
  corner_offsets[1][0] = 0;
  corner_offsets[2][0] = 0;

  threading::parallel_invoke(
      src_mesh.faces_num - face_type_masks[CutFaceType::Kept].size() > GRAIN_SIZE,
      [&]() {
        face_type_masks[CutFaceType::DoubleLoop].foreach_index(
            [&](const int64_t face_index, const int64_t index) {
              const IndexRange face_corners = src_face_offsets[face_index];

              Span<float> factors_a = fn_fetch_edge(src_corner_edges[face_corners[0]]).second;
              Span<float> factors_b = fn_fetch_edge(src_corner_edges[face_corners[1]]).second;

              const int num_internal_vertices = factors_a.size() * factors_b.size();
              const int num_internal_edges = (factors_a.size() + 1) * factors_b.size() +
                                             factors_a.size() * (factors_b.size() + 1);
              const int num_internal_faces = (factors_a.size() + 1) * (factors_b.size() + 1);
              const int num_internal_corners = num_internal_faces * 4;

              /* Accumulate */
              vertex_offsets[index + 1] = vertex_offsets[index] + num_internal_vertices;

              edge_offsets[0][index + 1] = edge_offsets[0][index] + num_internal_edges;
              face_offsets[0][index + 1] = face_offsets[0][index] + num_internal_faces;
              corner_offsets[0][index + 1] = corner_offsets[0][index] + num_internal_corners;
            });
      },
      [&]() {
        face_type_masks[CutFaceType::SingleLoop].foreach_index([&](const int64_t face_index,
                                                                   const int64_t index) {
          const IndexRange face_corners = src_face_offsets[face_index];

          Span<float> factors;
          for (const int64_t corner : face_corners) {
            const std::pair<int8_t, Span<float>> loop = fn_fetch_edge(src_corner_edges[corner]);
            if (loop.first > 0) {
              factors = loop.second;
              break;
            }
          }

          const int64_t loop_count_a = factors.size();
          const int num_internal_edges = loop_count_a;
          const int num_internal_faces = num_internal_edges + 1;
          const int num_internal_corners = 4 * num_internal_faces;

          /* Accumulate */
          edge_offsets[1][index + 1] = edge_offsets[1][index] + num_internal_edges;
          face_offsets[1][index + 1] = face_offsets[1][index] + num_internal_faces;
          corner_offsets[1][index + 1] = corner_offsets[1][index] + num_internal_corners;
        });
      },
      [&]() {
        face_type_masks[CutFaceType::Adjacent].foreach_index([&](const int64_t face_index,
                                                                 const int64_t index) {
          const IndexRange face_corners = src_face_offsets[face_index];

          int num_corners = face_corners.size();
          for (const int64_t corner : face_corners) {
            const std::pair<int8_t, Span<float>> loop = fn_fetch_edge(src_corner_edges[corner]);
            if (loop.first > 0) {
              num_corners += loop.second.size();
            }
          }
          /* Accumulate */
          corner_offsets[2][index + 1] = corner_offsets[2][index] + num_corners;
        });
      });
}

template<typename FetchEdge>
void process_shared_edges(const Mesh &src_mesh,
                          FetchEdge &&fn_fetch_edge,
                          IndexMask edge_selection,
                          const Span<Vector<float>> cut_factor_sets,
                          const OffsetIndices<int> split_edge_offsets,
                          MutableSpan<int> edge_selection_reverse_map,
                          MutableSpan<int2> vertex_split_vertices,
                          MutableSpan<float> vertex_split_weights,
                          MutableSpan<int2> edge_output_vertex_pairs,
                          MutableSpan<int2> edge_output_src_edges)
{
  const Span<int2> src_edge_indices = src_mesh.edges();

  MutableSpan<int2> edge_lerp_vertex_pairs = edge_output_vertex_pairs.slice(
      0, split_edge_offsets.last());
  MutableSpan<int2> edge_lerp_src_edges = edge_output_src_edges.slice(0,
                                                                      split_edge_offsets.last());

  /* Split */
  edge_selection.foreach_index(
      GrainSize(GRAIN_SIZE), [&](const int64_t edge_index, const int64_t index) {
        edge_selection_reverse_map[edge_index] = index; /* Reverse map for edge split mask */

        std::pair<int8_t, const Span<float>> loop_traverse = fn_fetch_edge(edge_index);

        int2 edge = src_edge_indices[edge_index];
        if (loop_traverse.first & IS_INVERTED) {
          std::swap(edge.x, edge.y);
        }

        const IndexRange dst_range_edge = split_edge_offsets[index];
        const IndexRange dst_range_vert(dst_range_edge.first() - index, dst_range_edge.size() - 1);

        /* Generate vertices */
        for (int64_t i = 0; i < dst_range_vert.size(); i++) {
          vertex_split_vertices[dst_range_vert[i]] = edge;
          vertex_split_weights[dst_range_vert[i]] = loop_traverse.second[i];
        }

        /* Generate edges */
        const int initial_vertex_index = dst_range_vert[0] + src_mesh.verts_num;
        for (int i = 1; i < dst_range_edge.size() - 1; i++) {
          edge_lerp_vertex_pairs[dst_range_edge[i]] = {initial_vertex_index + i - 1,
                                                       initial_vertex_index + i};
          edge_output_src_edges[dst_range_edge[i]] = {int(edge_index), -1};
        }

        /* First and last edge cases, includes original vertex! */
        edge_lerp_vertex_pairs[dst_range_edge[0]] = {edge.x, initial_vertex_index};
        edge_output_src_edges[dst_range_edge[0]] = {int(edge_index), -1};
        const int2 last_pair = {int(initial_vertex_index + dst_range_vert.size() - 1), edge.y};
        edge_lerp_vertex_pairs[dst_range_edge[dst_range_edge.size() - 1]] = last_pair;
        edge_output_src_edges[dst_range_edge[dst_range_edge.size() - 1]] = {int(edge_index), -1};
      });
}

template<typename FetchLoop>
void process_face_adjacent(const Mesh &src_mesh,
                           FetchLoop &&fn_fetch_loop,
                           const Span<int8_t> traverse_flags,
                           const Span<int> old_to_new_edge_map,
                           const IndexMask adjacent_face_selection,
                           MeshFaceGroupData &face_data)
{
  const OffsetIndices src_face_offsets = src_mesh.faces();
  const Span<int> src_corner_edges = src_mesh.corner_edges();

  /* Split source faces */
  adjacent_face_selection.foreach_index(
      GrainSize(GRAIN_SIZE), [&](const int64_t face_index, const int64_t index) {
        const IndexRange face_corners = src_face_offsets[face_index];

        const IndexRange dst_range = face_data.corner_offsets[index];
        face_data.src_polygon_indices[index] = int(index);

        int counter = 0;
        for (const int64_t i : face_corners.index_range()) {
          const int current_corner = int(face_corners[i]);

          /* Initial corner */
          face_data.src_corners[dst_range.first() + counter] = {current_corner, -1};
          face_data.src_weights[dst_range.first() + counter] = 0.0f;

          if (traverse_flags[src_corner_edges[current_corner]] == 0) {
            /* Edge is not split, initial corner references original edge */
            face_data.edge_indices[dst_range.first() + counter++] =
                old_to_new_edge_map[src_corner_edges[current_corner]];
          }
          else {
            IndexRangeSigned ref_range_edge, ref_range_vert;
            Span<float> split_factors;
            fn_fetch_loop(current_corner, ref_range_edge, ref_range_vert, split_factors);

            /* Edge is split, initial corner references the first edge in the edge split */
            face_data.edge_indices[dst_range.first() + counter++] = ref_range_edge[0];

            /* Create corners for the remaining edges created in the edge split */
            const int next_corner = int(face_corners[(i + 1) % face_corners.size()]);
            for (const int64_t split_index : split_factors.index_range()) {
              face_data.src_corners[dst_range.first() + counter] = int2{current_corner,
                                                                        next_corner};
              face_data.edge_indices[dst_range.first() + counter] =
                  ref_range_edge[split_index + 1];
              face_data.src_weights[dst_range.first() + counter] = split_factors[split_index];
              counter++;
            }
          }
        }
      });
}

template<typename FetchLoop>
void process_face_single_cut(const Mesh &src_mesh,
                             FetchLoop &&fn_fetch_loop,
                             const Span<int8_t> traverse_flags,
                             const Span<int> old_to_new_edge_map,
                             const IndexMask single_cut_face_selection,
                             const int num_edges_kept,
                             const int num_edges_split,
                             const OffsetIndices<int> dst_edge_ranges,
                             const OffsetIndices<int> dst_face_ranges,
                             const OffsetIndices<int> dst_corner_ranges,
                             MeshFaceGroupData &dst_face_data,
                             MutableSpan<int2> dst_vertex_pairs,
                             MutableSpan<int2> dst_src_edges)
{
  const OffsetIndices src_face_offsets = src_mesh.faces();
  const Span<int> src_corner_edges = src_mesh.corner_edges();

  dst_vertex_pairs = dst_vertex_pairs.slice(num_edges_split, dst_edge_ranges.last());
  dst_src_edges = dst_src_edges.slice(num_edges_split, dst_edge_ranges.last());

  /* Split source faces */
  single_cut_face_selection.foreach_index(
      GrainSize(GRAIN_SIZE), [&](const int64_t face_index, const int64_t index) {
        const IndexRange face_corners = src_face_offsets[face_index];

        int loop_corner = -1;
        for (const int64_t i : face_corners.index_range()) {
          if (traverse_flags[src_corner_edges[face_corners[i]]] > 0) {
            loop_corner = int(i);
            break;
          }
        }

        IndexRangeSigned ref_range_edge[2], ref_range_vert[2];
        Span<float> split_factors, unused;
        fn_fetch_loop(
            face_corners[loop_corner], ref_range_edge[0], ref_range_vert[0], split_factors);
        fn_fetch_loop(face_corners[loop_corner + 2], ref_range_edge[1], ref_range_vert[1], unused);

        const int64_t loop_cuts = split_factors.size();

        /* Clockwise order require opposite side to be reverted (again if necessary)! */
        ref_range_edge[1].reverse();
        ref_range_vert[1].reverse();

        IndexRange dst_edge_range = dst_edge_ranges[index];
        const int2 edge_refs = {src_corner_edges[face_corners[0]],
                                src_corner_edges[face_corners[1]]};
        for (const int64_t i : IndexRange(loop_cuts)) {
          const int2 ref_verts = int2{ref_range_vert[0][i], ref_range_vert[1][i]};

          dst_vertex_pairs[dst_edge_range[i]] = {ref_verts.x, ref_verts.y};
          dst_src_edges[dst_edge_range[i]] = edge_refs;
        }

        const IndexRange dst_face_range = dst_face_ranges[index];
        const IndexRange dst_corner_range = dst_corner_ranges[index];
        const IndexRange internal_edge_range = dst_edge_range.shift(num_edges_kept +
                                                                    num_edges_split);
        int64_t dst_face = dst_face_range.first();
        int64_t dst_corner = dst_corner_range.first();

        /* Construct faces, faces will be created in the order of the first loop with cuts.
         * c         b
         *  x---<---x
         *  |       ^
         *  |   1   |
         *  |       |
         *  x---<---x
         *  |       ^
         *  |   0   |
         *  |       |
         *  *--->---*
         * d         a, loop_corner
         */
        const int corner_a = face_corners[loop_corner + 0];
        const int corner_b = face_corners[loop_corner + 1];
        const int corner_c = face_corners[loop_corner + 2];
        const int corner_d = face_corners[(loop_corner + 3) % face_corners.size()];

        /* First face */
        dst_face_data.src_polygon_indices[dst_face] = int(index);
        dst_face_data.corner_offset_data[++dst_face] =
            4; /* Increment first, writing OffsetIndices */

        dst_face_data.src_corners[dst_corner] = {corner_a, -1};
        dst_face_data.src_weights[dst_corner] = 0.0f;
        dst_face_data.edge_indices[dst_corner] = ref_range_edge[0][0];
        dst_corner++;
        dst_face_data.src_corners[dst_corner] = {corner_a, corner_b};
        const float initial_factor = split_factors[0];
        dst_face_data.src_weights[dst_corner] = initial_factor;
        dst_face_data.edge_indices[dst_corner] = internal_edge_range[0];
        dst_corner++;
        dst_face_data.src_corners[dst_corner] = {corner_d, corner_c};
        dst_face_data.src_weights[dst_corner] = initial_factor;
        dst_face_data.edge_indices[dst_corner] = ref_range_edge[1][0];
        dst_corner++;
        dst_face_data.src_corners[dst_corner] = {corner_d, -1};
        dst_face_data.src_weights[dst_corner] = 0.0f;
        dst_face_data.edge_indices[dst_corner] = old_to_new_edge_map[src_corner_edges[corner_d]];
        dst_corner++;

        /* Internal faces */
        for (const int64_t i : split_factors.index_range().drop_back(1)) {
          dst_face_data.src_polygon_indices[dst_face] = int(index);
          dst_face_data.corner_offset_data[++dst_face] = 4;

          const float bottom_factor = split_factors[i];
          const float top_factor = split_factors[i + 1];
          dst_face_data.src_corners[dst_corner] = {corner_a, corner_b};
          dst_face_data.src_weights[dst_corner] = bottom_factor;
          dst_face_data.edge_indices[dst_corner] = ref_range_edge[0][i + 1];
          dst_corner++;
          dst_face_data.src_corners[dst_corner] = {corner_a, corner_b};
          dst_face_data.src_weights[dst_corner] = top_factor;
          dst_face_data.edge_indices[dst_corner] = internal_edge_range[i + 1];
          dst_corner++;
          dst_face_data.src_corners[dst_corner] = {corner_d, corner_c};
          dst_face_data.src_weights[dst_corner] = top_factor;
          dst_face_data.edge_indices[dst_corner] = ref_range_edge[1][i + 1];
          dst_corner++;
          dst_face_data.src_corners[dst_corner] = {corner_d, corner_c};
          dst_face_data.src_weights[dst_corner] = bottom_factor;
          dst_face_data.edge_indices[dst_corner] = internal_edge_range[i];
          dst_corner++;
        }

        /* Last face */
        dst_face_data.src_polygon_indices[dst_face] = int(index);
        dst_face_data.corner_offset_data[++dst_face] = 4;

        dst_face_data.src_corners[dst_corner] = {corner_a, corner_b};
        const float last_factor = split_factors.last();
        dst_face_data.src_weights[dst_corner] = last_factor;
        dst_face_data.edge_indices[dst_corner] = ref_range_edge[0].last();
        dst_corner++;
        dst_face_data.src_corners[dst_corner] = {corner_b, -1};
        dst_face_data.src_weights[dst_corner] = 0.0f;
        dst_face_data.edge_indices[dst_corner] = old_to_new_edge_map[src_corner_edges[corner_b]];
        dst_corner++;
        dst_face_data.src_corners[dst_corner] = {corner_c, -1};
        dst_face_data.src_weights[dst_corner] = 0.0f;
        dst_face_data.edge_indices[dst_corner] = ref_range_edge[1].last();
        dst_corner++;
        dst_face_data.src_corners[dst_corner] = {corner_d, corner_c};
        dst_face_data.src_weights[dst_corner] = last_factor;
        dst_face_data.edge_indices[dst_corner] = internal_edge_range.last();

        BLI_assert(dst_corner + 1 == dst_corner_range.one_after_last());
      });

  /* Update final corner offsets */
  BLI_assert(dst_face_data.corner_offset_data.first() == 0);
  std::partial_sum(dst_face_data.corner_offset_data.begin(),
                   dst_face_data.corner_offset_data.end(),
                   dst_face_data.corner_offset_data.begin());
}

template<typename FetchLoop>
void process_face_double_cut(const Mesh &src_mesh,
                             FetchLoop &&fn_fetch_loop,
                             const IndexMask single_cut_face_selection,
                             const int num_vertices_shared,
                             const int offset_edges_double_cut,
                             const OffsetIndices<int> dst_vert_ranges,
                             const OffsetIndices<int> dst_edge_ranges,
                             const OffsetIndices<int> dst_face_ranges,
                             const OffsetIndices<int> dst_corner_ranges,
                             MeshFaceBilinearGroupData &dst_face_data,
                             MutableSpan<int2> dst_vertex_pairs,
                             MutableSpan<int2> dst_src_edges,
                             MutableSpan<int2> vertex_bilinear_vertices,
                             MutableSpan<float> vertex_bilinear_weights)
{
  const OffsetIndices src_face_offsets = src_mesh.faces();
  const Span<int> src_corner_edges = src_mesh.corner_edges();

  /* Split source faces */
  single_cut_face_selection.foreach_index(
      GrainSize(GRAIN_SIZE), [&](const int64_t face_index, const int64_t index) {
        const IndexRange face_corners = src_face_offsets[face_index];

        IndexRangeSigned ref_range_edge[4], ref_range_vert[4];
        Span<float> split_factors[4];
        for (const int64_t i : IndexRange(4)) {
          fn_fetch_loop(face_corners[i], ref_range_edge[i], ref_range_vert[i], split_factors[i]);
        }

        /* Clockwise order require opposite side to be reverted (again if necessary)! */
        ref_range_edge[2].reverse();
        ref_range_vert[2].reverse();
        ref_range_edge[3].reverse();
        ref_range_vert[3].reverse();

        /* Matching loop sets should belong to the same loop! */
        const int64_t loop_cuts_0 = split_factors[0].size();
        const int64_t loop_cuts_1 = split_factors[1].size();

        /* Bilinear interp...? Linear interp from new elements... */
        const IndexRange dst_vert_range = dst_vert_ranges[index];
        const IndexRange dst_edge_range = dst_edge_ranges[index];
        const IndexRange dst_face_range = dst_face_ranges[index];
        const IndexRange dst_corner_range = dst_corner_ranges[index];

        /* Corner reference info */
        const int2 vertical_edge_refs = {src_corner_edges[face_corners[0]],
                                         src_corner_edges[face_corners[2]]};
        const int2 horizontal_edge_refs = {src_corner_edges[face_corners[1]],
                                           src_corner_edges[face_corners[3]]};
        dst_face_data.src_polygon_indices[index] = int(face_index);
        dst_face_data.src_corners[index] = {int(face_corners[0]),
                                            int(face_corners[1]),
                                            int(face_corners[2]),
                                            int(face_corners[3])};

        const int64_t step = loop_cuts_1 + 1;
        const int64_t step_corner = step * 4;
        const int64_t step_horizonal_edge = loop_cuts_0 + 1;
        const int64_t num_vertical_edges = loop_cuts_0 * step;
        for (const int64_t i0 : IndexRange(loop_cuts_0)) {
          const int2 ref_vertical = int2{ref_range_vert[0][i0], ref_range_vert[2][i0]};
          const float factor_0 = split_factors[0][i0];

          int prev_vertical = ref_vertical.x;
          for (const int64_t i1 : IndexRange(loop_cuts_1)) {
            const int64_t vindex = i0 * loop_cuts_1 + i1;

            /* Vertices */
            const float factor_1 = split_factors[1][i1];
            vertex_bilinear_vertices[dst_vert_range[vindex]] = ref_vertical;
            vertex_bilinear_weights[dst_vert_range[vindex]] = factor_1;

            /* Edges */
            const int64_t dst_vertical = dst_edge_range[i0 * step + i1];
            const int64_t dst_horizontal_edge = dst_edge_range[i1 * step_horizonal_edge + i0];

            dst_src_edges[dst_vertical] = vertical_edge_refs;
            dst_vertex_pairs[dst_vertical].x = prev_vertical;
            prev_vertical = int(num_vertices_shared + dst_vert_range[vindex]);
            dst_vertex_pairs[dst_vertical].y = prev_vertical;

            /* Corners */
            const int vertical_corner = i0 * 4;
            const int64_t corner_left = dst_corner_range[i0 * step_corner + i1 * 4] + 2;
            const int64_t corner_up = corner_left + 3;
            const int64_t corner_down = dst_corner_range[(i0 + 1) * step_corner + i1 * 4] + 3;
            const int64_t corner_right = corner_down + 1;

            dst_face_data.corner_weights[corner_left] = {factor_0, factor_1};
            dst_face_data.corner_weights[corner_up] = {factor_0, factor_1};
            dst_face_data.corner_weights[corner_down] = {factor_0, factor_1};
            dst_face_data.corner_weights[corner_right] = {factor_0, factor_1};

            const int edge_down = dst_vertical + offset_edges_double_cut;
            const int edge_left = dst_horizontal_edge + num_vertical_edges +
                                  offset_edges_double_cut;
            dst_face_data.edge_indices[corner_left] = edge_left;
            dst_face_data.edge_indices[corner_up] = edge_down + 1;
            dst_face_data.edge_indices[corner_down] = edge_down;
            dst_face_data.edge_indices[corner_right] = edge_left + 1;
          }

          /* Last Edge */
          const int64_t last_dst_vertical = dst_edge_range[i0 * step + loop_cuts_1];
          dst_vertex_pairs[last_dst_vertical] = {prev_vertical, ref_vertical.y};
          dst_src_edges[last_dst_vertical] = vertical_edge_refs;

          /* Bottom Side Corners */
          const int64_t bottom_vertical = dst_corner_range[step_corner * i0] + 1;
          const int64_t bottomside = bottom_vertical + step_corner - 1;
          dst_face_data.corner_weights[bottomside] = {factor_0, 0.0f};
          dst_face_data.edge_indices[bottomside] = ref_range_edge[0][i0 + 1];

          dst_face_data.corner_weights[bottom_vertical] = {factor_0, 0.0f};
          dst_face_data.edge_indices[bottom_vertical] = dst_edge_range[i0 * step] +
                                                        offset_edges_double_cut;

          /* Top Side Corners */
          const int64_t topside = dst_corner_range[step_corner * (i0 + 1)] - 2;
          const int64_t top_vertical = topside + step_corner + 1;
          dst_face_data.corner_weights[topside] = {factor_0, 1.0f};
          dst_face_data.edge_indices[topside] = ref_range_edge[2][i0];

          dst_face_data.corner_weights[top_vertical] = {factor_0, 1.0f};
          dst_face_data.edge_indices[top_vertical] = dst_edge_range[i0 * step + loop_cuts_1] +
                                                     offset_edges_double_cut;
        }

        /* Horizontal */
        for (const int64_t i1 : IndexRange(loop_cuts_1)) {
          const int2 ref_horizontal = int2{ref_range_vert[3][i1], ref_range_vert[1][i1]};

          int prev_horizontal = ref_horizontal.x;
          for (const int64_t i0 : IndexRange(loop_cuts_0)) {
            const int64_t vindex = i0 * loop_cuts_1 + i1;

            /* Edges */
            const int64_t dst_horizontal = num_vertical_edges +
                                           dst_edge_range[i1 * step_horizonal_edge + i0];

            dst_src_edges[dst_horizontal] = horizontal_edge_refs;
            dst_vertex_pairs[dst_horizontal].x = prev_horizontal;
            prev_horizontal = num_vertices_shared + int(dst_vert_range[vindex]);
            dst_vertex_pairs[dst_horizontal].y = prev_horizontal;
          }

          /* Last Edge */
          const int64_t first_row_edge = num_vertical_edges +
                                         dst_edge_range[i1 * step_horizonal_edge];
          const int64_t last_row_edge = first_row_edge + loop_cuts_0;
          dst_vertex_pairs[last_row_edge] = {prev_horizontal, ref_horizontal.y};
          dst_src_edges[last_row_edge] = horizontal_edge_refs;

          const float factor_1 = split_factors[1][i1];

          /* Left Side Corners */
          const int64_t leftside = dst_corner_range[i1 * 4 + 3];
          const int64_t left_horizontal = leftside + 1;
          dst_face_data.corner_weights[leftside] = {0.0f, factor_1};
          dst_face_data.edge_indices[leftside] = ref_range_edge[3][i1];

          dst_face_data.corner_weights[left_horizontal] = {0.0f, factor_1};
          dst_face_data.edge_indices[left_horizontal] = first_row_edge + offset_edges_double_cut;

          /* Right Side Corners */
          const int64_t right_horizontal = dst_corner_range[step_corner * loop_cuts_0 + i1 * 4] +
                                           2;
          const int64_t rightside = right_horizontal + 3;
          dst_face_data.corner_weights[rightside] = {1.0f, factor_1};
          dst_face_data.edge_indices[rightside] = ref_range_edge[1][i1 + 1];

          dst_face_data.corner_weights[right_horizontal] = {1.0f, factor_1};
          dst_face_data.edge_indices[right_horizontal] = last_row_edge + offset_edges_double_cut;
        }

        /* Original 4 corners.
         */
        const int64_t bot_left = dst_corner_range[0];
        dst_face_data.corner_weights[bot_left] = {0.0f, 0.0f};
        dst_face_data.edge_indices[bot_left] = ref_range_edge[0].first();

        const int64_t bot_right = dst_corner_range[step_corner * loop_cuts_0 + 1];
        dst_face_data.corner_weights[bot_right] = {1.0f, 0.0f};
        dst_face_data.edge_indices[bot_right] = ref_range_edge[1].first();

        const int64_t top_left = dst_corner_range[step_corner - 1];
        dst_face_data.corner_weights[top_left] = {0.0f, 1.0f};
        dst_face_data.edge_indices[top_left] = ref_range_edge[3].last();

        const int64_t top_right = dst_corner_range[step_corner * (loop_cuts_0 + 1) - 2];
        dst_face_data.corner_weights[top_right] = {1.0f, 1.0f};
        dst_face_data.edge_indices[top_right] = ref_range_edge[2].last();

        dst_face_data.src_face.as_mutable_span().slice(dst_corner_range).fill(index);
        dst_face_data.corner_offset_data.as_mutable_span().slice(dst_face_range.shift(1)).fill(4);
      });
  /* Update final corner offsets */
  std::partial_sum(dst_face_data.corner_offset_data.begin(),
                   dst_face_data.corner_offset_data.end(),
                   dst_face_data.corner_offset_data.begin());
}

std::optional<Mesh *> loop_cut(const Mesh &src_mesh,
                               Span<bool> affected_faces,
                               IndexMask loop_cut_from,
                               OffsetIndices<int> loop_cut_factor_offsets,
                               Span<float> loop_cut_factors,
                               const LoopCutAttributeOutputs &attribute_outputs,
                               const bke::AttributeFilter &attribute_filter)
{
  Array<int> traversed_edge_by(src_mesh.edges_num);
  Array<int8_t> traverse_flags(src_mesh.edges_num);
  Array<bool> traversed_faces(src_mesh.faces_num);
  traverse_flags.fill(0);
  traversed_faces.fill(false);

  Array<Vector<float>> cut_factor_sets(loop_cut_from.size());
  Array<Vector<float>> cut_factors_inverted(loop_cut_from.size());
  Array<int> loop_id_remap(loop_cut_from.size());

  traverse_loops(src_mesh,
                 affected_faces,
                 loop_cut_factor_offsets,
                 loop_cut_factors,
                 loop_cut_from,
                 traverse_flags,
                 traversed_edge_by,
                 traversed_faces,
                 cut_factor_sets,
                 cut_factors_inverted,
                 loop_id_remap);

  /* Compute masks over kept and split/cut elements. */
  IndexMaskMemory split_edge_memory;
  Array<IndexMask, 2> edge_type_selections(2);
  IndexMask::from_groups<int64_t>(
      IndexMask(src_mesh.edges().index_range()),
      split_edge_memory,
      [&](int64_t i) { return traverse_flags[i] > 0; },
      edge_type_selections.as_mutable_span());

  const int64_t num_edges_kept = edge_type_selections[0].size();
  const int64_t num_edges_split = edge_type_selections[1].size();

  /* Create face masks filtering by loop cut type  */
  auto fn_fetch_edge = [&](const int edge_index) {
    const int8_t flag = traverse_flags[edge_index];
    if (flag & TraverseFlags::IS_SPLIT) {
      return std::pair<int8_t, Span<float>>{
          flag, cut_factor_sets[loop_id_remap[traversed_edge_by[edge_index]]].as_span()};
    }
    return std::pair<int8_t, Span<float>>{flag, Span<float>()};
  };

  IndexMaskMemory split_face_memory;
  Array<IndexMask, CutFaceType::Count> face_type_selections(CutFaceType::Count);
  filter_faces_by_type(
      src_mesh, fn_fetch_edge, traversed_faces, split_face_memory, face_type_selections);

  /* Create mapping tables for copied/preserved data */
  Array<int> old_to_new_edge_map(src_mesh.edges_num);
  edge_type_selections[0].foreach_index(GrainSize(GRAIN_SIZE_LARGE),
                                        [&](const int64_t edge_index, const int64_t index) {
                                          old_to_new_edge_map[edge_index] = index;
                                        });

  /* Count number of vertices and edges formed from splitting source edges */
  Array<int> split_edge_count(num_edges_split + 1);
  OffsetIndices<int> split_edge_offsets(split_edge_count);

  split_edge_count[0] = 0;
  edge_type_selections[1].foreach_index([&](const int64_t edge_index, const int64_t index) {
    const int loop_set = traversed_edge_by[edge_index];
    const int num_edge_cuts = cut_factor_sets[loop_set].size() + 1;
    split_edge_count[index + 1] = split_edge_count[index] + num_edge_cuts;
  });

  /* Comput internal shapes */
  Vector<int> face_split_vertex_offsets;
  std::array<Vector<int>, 2> face_split_edge_offsets;
  std::array<Vector<int>, 3> face_split_face_offsets;
  std::array<Vector<int>, 3> face_split_corner_offsets;

  count_face_split_shapes(src_mesh,
                          fn_fetch_edge,
                          face_type_selections,
                          face_split_vertex_offsets,
                          face_split_edge_offsets,
                          face_split_face_offsets,
                          face_split_corner_offsets);

  /* Vertex data */
  const int num_vertices_shared_split = split_edge_offsets.last() - num_edges_split;
  const int num_vertices_shared = src_mesh.verts_num + num_vertices_shared_split;
  Array<int2, 12> vertex_split_vertices(num_vertices_shared_split);
  Array<float, 12> vertex_split_weights(num_vertices_shared_split);

  Array<int2, 12> vertex_bilinear_vertices(face_split_vertex_offsets.last());
  Array<float, 12> vertex_bilinear_weights(face_split_vertex_offsets.last());

  /* Edge data */
  const int total_edges_formed = split_edge_offsets.last() + face_split_edge_offsets[0].last() +
                                 face_split_edge_offsets[1].last();
  Array<int2, 12> edge_output_vertex_pairs(total_edges_formed);
  Array<int2, 12> edge_output_src_edges(total_edges_formed);
  process_shared_edges(src_mesh,
                       fn_fetch_edge,
                       edge_type_selections[1],
                       cut_factor_sets,
                       split_edge_offsets,
                       old_to_new_edge_map,
                       vertex_split_vertices,
                       vertex_split_weights,
                       edge_output_vertex_pairs,
                       edge_output_src_edges);

  /* Utility function for querying the loop cut info for a traversed edge associated with the given
   * loop corner. */
  auto fn_fetch_loop = [&](const int corner_index,
                           IndexRangeSigned &r_range_edge,
                           IndexRangeSigned &r_range_vert,
                           Span<float> &r_cut_factors) {
    const int edge_index = src_mesh.corner_edges()[corner_index];
    const int loop_vert = src_mesh.corner_verts()[corner_index];
    const int2 edge_verts = src_mesh.edges()[edge_index];

    /* Determine if inserted vertices generated for the edge are aligned with the loop.
     * Occurs if edge is flipped and if that flip is not reverted by the face loop ordering!
     */
    const bool loop_unaligned = edge_verts.x != loop_vert;

    const int edge_split_index = old_to_new_edge_map[edge_index];
    const IndexRange offsets = split_edge_offsets[edge_split_index];
    r_range_edge = IndexRangeSigned(offsets.first() + num_edges_kept, offsets.size());
    r_range_vert = IndexRangeSigned(offsets.first() - edge_split_index + src_mesh.verts_num,
                                    offsets.size() - 1);

    const int loop_id = loop_id_remap[traversed_edge_by[edge_index]];
    if (loop_unaligned != bool(traverse_flags[edge_index] & TraverseFlags::IS_INVERTED)) {
      r_cut_factors = cut_factors_inverted[loop_id];
      r_range_edge.reverse();
      r_range_vert.reverse();
    }
    else {
      r_cut_factors = cut_factor_sets[loop_id];
    }
  };

  /* Face data */
  MeshFaceGroupData single_split_faces(face_split_face_offsets[1].last(),
                                       face_split_corner_offsets[1].last());
  process_face_single_cut(src_mesh,
                          fn_fetch_loop,
                          traverse_flags,
                          old_to_new_edge_map,
                          face_type_selections[CutFaceType::SingleLoop],
                          num_edges_kept,
                          split_edge_offsets.last(),
                          OffsetIndices<int>(face_split_edge_offsets[1]),
                          OffsetIndices<int>(face_split_face_offsets[1]),
                          OffsetIndices<int>(face_split_corner_offsets[1]),
                          single_split_faces,
                          edge_output_vertex_pairs,
                          edge_output_src_edges);

  MeshFaceBilinearGroupData double_split_faces(face_split_face_offsets[0].size() - 1,
                                               face_split_face_offsets[0].last(),
                                               face_split_corner_offsets[0].last());
  const int first_edge = split_edge_offsets.last() + face_split_edge_offsets[1].last();
  const int offset_edges_double_cut = num_edges_kept + face_split_edge_offsets[1].last() +
                                      split_edge_offsets.last();

  process_face_double_cut(
      src_mesh,
      fn_fetch_loop,
      face_type_selections[CutFaceType::DoubleLoop],
      num_vertices_shared,
      offset_edges_double_cut,
      OffsetIndices<int>(face_split_vertex_offsets),
      OffsetIndices<int>(face_split_edge_offsets[0]),
      OffsetIndices<int>(face_split_face_offsets[0]),
      OffsetIndices<int>(face_split_corner_offsets[0]),
      double_split_faces,
      edge_output_vertex_pairs.as_mutable_span().slice(first_edge,
                                                       face_split_edge_offsets[0].last()),
      edge_output_src_edges.as_mutable_span().slice(first_edge, face_split_edge_offsets[0].last()),
      vertex_bilinear_vertices.as_mutable_span(),
      vertex_bilinear_weights.as_mutable_span());

  MeshFaceGroupData linear_faces{OffsetIndices<int>(face_split_corner_offsets[2])};
  process_face_adjacent(src_mesh,
                        fn_fetch_loop,
                        traverse_flags,
                        old_to_new_edge_map,
                        face_type_selections[CutFaceType::Adjacent],
                        linear_faces);

  /* Group Descriptors */
  std::array<VariantVertexGroup, 3> vertex_groups = {
      MeshVertexGroupCopyMask{IndexMask(src_mesh.verts_num)},
      MeshVertexGroupLinear{vertex_split_vertices.as_span(), vertex_split_weights.as_span()},
      MeshVertexGroupLinear{
          vertex_bilinear_vertices.as_span(), vertex_bilinear_weights.as_span(), true}};

  std::array<VariantEdgeGroup, 2> edge_groups = {
      MeshEdgeGroupCopyMask{edge_type_selections[0], Span<int>()},
      MeshEdgeGroupPair{edge_output_vertex_pairs.as_span(), edge_output_src_edges.as_span()}};

  Vector<VariantPolygonGroup, 6> poly_groups;
  poly_groups.append(MeshFaceGroupCopyMask{
      -1, face_type_selections[0], Span<int>(), old_to_new_edge_map.as_span()}
                         .init_num_corners(src_mesh.faces()));
  if (linear_faces.has_content()) {
    poly_groups.append(&linear_faces);
  }
  if (single_split_faces.has_content()) {
    poly_groups.append(&single_split_faces);
  }
  if (double_split_faces.has_content()) {
    poly_groups.append(&double_split_faces);
  }

  /* Create new mesh */
  Mesh *result = new_mesh_from_groups(src_mesh, vertex_groups, edge_groups, poly_groups);
  transfer_vertex_data(src_mesh, *result, vertex_groups, attribute_filter);
  transfer_edge_data(src_mesh, *result, edge_groups, attribute_filter);
  transfer_polygon_data(src_mesh, *result, poly_groups, attribute_filter);

  /* Create edge mask */
  bke::MutableAttributeAccessor attributes = result->attributes_for_write();
  if (attribute_outputs.loop_cut_edge_selection) {
    BLI_assert(!attributes.contains(*attribute_outputs.loop_cut_edge_selection));

    bke::SpanAttributeWriter<bool> attribute = attributes.lookup_or_add_for_write_span<bool>(
        *attribute_outputs.loop_cut_edge_selection, bke::AttrDomain::Edge);

    const int64_t num_copied_and_shared_edges = edge_type_selections[0].size() +
                                                split_edge_offsets.last();
    attribute.span.slice(0, num_copied_and_shared_edges).fill(false);
    attribute.span
        .slice(num_copied_and_shared_edges, attribute.span.size() - num_copied_and_shared_edges)
        .fill(true);
    attribute.finish();
  }

  return result;
}

}  // namespace blender::geometry
