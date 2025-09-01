/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BLI_array_utils.hh"
#include "BLI_math_base.h"
#include "BLI_ordered_edge.hh"
#include "BLI_task.hh"
#include "BLI_threads.h"
#include "BLI_vector_set.hh"

#include "BKE_attribute.hh"
#include "BKE_attribute_filter.hh"
#include "BKE_attribute_filters.hh"
#include "BKE_attribute_math.hh"
#include "BKE_customdata.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"

namespace blender::bke {

namespace calc_edges {

/**
 * Return a hash value that is likely to be different in the low bits from the normal `hash()`
 * function. This is necessary to avoid collisions in #mesh_calc_edges.
 */
static uint64_t edge_hash_2(const OrderedEdge &edge)
{
  return edge.v_low;
}

using EdgeMap = VectorSet<OrderedEdge,
                          16,
                          DefaultProbingStrategy,
                          DefaultHash<OrderedEdge>,
                          DefaultEquality<OrderedEdge>,
                          SimpleVectorSetSlot<OrderedEdge, int>,
                          GuardedAllocator>;

static void reserve_hash_maps(const Mesh &mesh,
                              const bool keep_existing_edges,
                              MutableSpan<EdgeMap> edge_maps)
{
  const int totedge_guess = std::max(keep_existing_edges ? mesh.edges_num : 0, mesh.faces_num * 2);
  threading::parallel_for_each(
      edge_maps, [&](EdgeMap &edge_map) { edge_map.reserve(totedge_guess / edge_maps.size()); });
}

static int edge_to_hash_map_i(const OrderedEdge edge, const uint32_t parallel_mask)
{
  return parallel_mask & edge_hash_2(edge);
}

static void add_existing_edges_to_hash_maps(const Mesh &mesh,
                                            const uint32_t parallel_mask,
                                            MutableSpan<EdgeMap> edge_maps)
{
  /* Assume existing edges are valid. */
  const Span<int2> edges = mesh.edges();
  threading::parallel_for_each(edge_maps, [&](EdgeMap &edge_map) {
    const int task_index = &edge_map - edge_maps.data();
    for (const int2 edge : edges) {
      const OrderedEdge ordered_edge(edge);
      /* Only add the edge when it belongs into this map. */
      if (task_index == edge_to_hash_map_i(ordered_edge, parallel_mask)) {
        edge_map.add(ordered_edge);
      }
    }
  });
}

static void add_face_edges_to_hash_maps(const Mesh &mesh,
                                        const uint32_t parallel_mask,
                                        MutableSpan<EdgeMap> edge_maps)
{
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  threading::parallel_for_each(edge_maps, [&](EdgeMap &edge_map) {
    const int task_index = &edge_map - edge_maps.data();
    for (const int face_i : faces.index_range()) {
      const IndexRange face = faces[face_i];
      for (const int corner : face) {
        const int vert = corner_verts[corner];
        const int vert_prev = corner_verts[bke::mesh::face_corner_prev(face, corner)];
        /* Can only be the same when the mesh data is invalid. */
        if (LIKELY(vert_prev != vert)) {
          const OrderedEdge ordered_edge(vert_prev, vert);
          /* Only add the edge when it belongs into this map. */
          if (task_index == edge_to_hash_map_i(ordered_edge, parallel_mask)) {
            edge_map.add(ordered_edge);
          }
        }
      }
    }
  });
}

static void serialize_and_initialize_deduplicated_edges(MutableSpan<EdgeMap> edge_maps,
                                                        const OffsetIndices<int> edge_offsets,
                                                        MutableSpan<int2> new_edges)
{
  threading::parallel_for_each(edge_maps, [&](EdgeMap &edge_map) {
    const int task_index = &edge_map - edge_maps.data();
    if (edge_offsets[task_index].is_empty()) {
      return;
    }

    MutableSpan<int2> result_edges = new_edges.slice(edge_offsets[task_index]);
    result_edges.copy_from(edge_map.as_span().cast<int2>());
  });
}

static void update_edge_indices_in_face_loops(const OffsetIndices<int> faces,
                                              const Span<int> corner_verts,
                                              const Span<EdgeMap> edge_maps,
                                              const uint32_t parallel_mask,
                                              const OffsetIndices<int> edge_offsets,
                                              MutableSpan<int> corner_edges)
{
  threading::parallel_for(faces.index_range(), 100, [&](IndexRange range) {
    for (const int face_index : range) {
      const IndexRange face = faces[face_index];
      for (const int corner : face) {
        const int vert = corner_verts[corner];
        const int vert_prev = corner_verts[bke::mesh::face_corner_next(face, corner)];
        if (UNLIKELY(vert == vert_prev)) {
          /* This is an invalid edge; normally this does not happen in Blender,
           * but it can be part of an imported mesh with invalid geometry. See
           * #76514. */
          corner_edges[corner] = 0;
          continue;
        }

        const OrderedEdge ordered_edge(vert_prev, vert);
        const int task_index = edge_to_hash_map_i(ordered_edge, parallel_mask);
        const EdgeMap &edge_map = edge_maps[task_index];
        const int edge_i = edge_map.index_of(ordered_edge);
        const int edge_index = edge_offsets[task_index][edge_i];
        corner_edges[corner] = edge_index;
      }
    }
  });
}

static int get_parallel_maps_count(const Mesh &mesh)
{
  /* Don't use parallelization when the mesh is small. */
  if (mesh.faces_num < 1000) {
    return 1;
  }
  /* Use at most 8 separate hash tables. Using more threads has diminishing returns. These threads
   * are better off doing something more useful instead. */
  const int system_thread_count = BLI_system_thread_count();
  return power_of_2_min_i(std::min(8, system_thread_count));
}

static void clear_hash_tables(MutableSpan<EdgeMap> edge_maps)
{
  threading::parallel_for_each(edge_maps, [](EdgeMap &edge_map) { edge_map.clear(); });
}

static void known_edges_to_new(const OffsetIndices<int> edge_offsets,
                               const Span<EdgeMap> edge_maps,
                               const uint32_t parallel_mask,
                               const Span<int2> known_edges,
                               MutableSpan<int> src_to_dst_edges)
{
  threading::parallel_for(known_edges.index_range(), 2048, [&](const IndexRange range) {
    for (const int src_edge_i : range) {
      const OrderedEdge ordered_edge(known_edges[src_edge_i]);
      const int task_index = edge_to_hash_map_i(ordered_edge, parallel_mask);
      const EdgeMap &edge_map = edge_maps[task_index];
      const int edge_i = edge_map.index_of(ordered_edge);
      const int dst_edge_i = edge_offsets[task_index][edge_i];
      src_to_dst_edges[src_edge_i] = dst_edge_i;
    }
  });
}

}  // namespace calc_edges

static void delete_attributes(const Span<std::string> attributes_list,
                              MutableAttributeAccessor attributes)
{
  for (const std::string &attribute : attributes_list) {
    attributes.remove(attribute);
  }
}

static Vector<std::string> attributes_by_domain(const AttributeAccessor attributes,
                                                const AttrDomain domain)
{
  Vector<std::string> attributes_list;
  ;
  attributes.foreach_attribute([&](const bke::AttributeIter &attribute) {
    if (attribute.domain != domain) {
      return;
    }
    if (attribute.data_type == AttrType::String) {
      return;
    }
    attributes_list.append_as(attribute.name);
  });
  return attributes_list;
}

void mesh_calc_edges(Mesh &mesh,
                     bool keep_existing_edges,
                     const bool select_new_edges,
                     const AttributeFilter &attribute_filter)
{
  if (mesh.edges_num == 0 && mesh.corners_num == 0) {
    BLI_assert(BKE_mesh_is_valid(&mesh));
    return;
  }

  const Vector<std::string> edge_attributes = attributes_by_domain(mesh.attributes(),
                                                                   AttrDomain::Edge);
  if (mesh.corners_num == 0 && !keep_existing_edges) {
    delete_attributes(edge_attributes, mesh.attributes_for_write());
    mesh.edges_num = 0;
    BLI_assert(BKE_mesh_is_valid(&mesh));
    return;
  }

  /* Parallelization is achieved by having multiple hash tables for different subsets of edges.
   * Each edge is assigned to one of the hash maps based on the lower bits of a hash value. */
  const int parallel_maps = calc_edges::get_parallel_maps_count(mesh);
  BLI_assert(is_power_of_2_i(parallel_maps));
  const uint32_t parallel_mask = uint32_t(parallel_maps) - 1;
  Array<calc_edges::EdgeMap> edge_maps(parallel_maps);
  calc_edges::reserve_hash_maps(mesh, keep_existing_edges, edge_maps);

  Array<int> original_edge_maps_prefix_size;
  if (keep_existing_edges) {
    calc_edges::add_existing_edges_to_hash_maps(mesh, parallel_mask, edge_maps);
    original_edge_maps_prefix_size.reinitialize(edge_maps.size() + 1);
    for (const int i : edge_maps.index_range()) {
      original_edge_maps_prefix_size[i] = edge_maps[i].size();
    }
    offset_indices::accumulate_counts_to_offsets(original_edge_maps_prefix_size);
  }
  const OffsetIndices<int> original_edge_maps_prefix(original_edge_maps_prefix_size.as_span());
  const int original_unique_edge_num = original_edge_maps_prefix.total_size();
  const bool original_edges_are_distinct = original_unique_edge_num == mesh.edges_num;
  if (mesh.corners_num == 0 && keep_existing_edges && original_edges_are_distinct) {
    BLI_assert(BKE_mesh_is_valid(&mesh));
    return;
  }

  calc_edges::add_face_edges_to_hash_maps(mesh, parallel_mask, edge_maps);
  Array<int> edge_sizes(edge_maps.size() + 1);
  for (const int i : edge_maps.index_range()) {
    edge_sizes[i] = edge_maps[i].size();
  }
  const OffsetIndices<int> edge_offsets = offset_indices::accumulate_counts_to_offsets(edge_sizes);
  const bool no_new_edges = edge_offsets.total_size() == mesh.edges_num;

  MutableAttributeAccessor dst_attributes = mesh.attributes_for_write();
  dst_attributes.add<int>(".corner_edge", AttrDomain::Corner, AttributeInitConstruct());
  MutableSpan<int> corner_edges = mesh.corner_edges_for_write();
#ifndef NDEBUG
  corner_edges.fill(-1);
#endif

  const OffsetIndices<int> faces = mesh.faces();
  const Span<int2> original_edges = mesh.edges();
  const Span<int> corner_verts = mesh.corner_verts();
  if (keep_existing_edges && original_edges_are_distinct && no_new_edges) {
    /* We need a way to say from caller side if we should generate corner edge attribute even in
     * that case. */

    threading::parallel_for(IndexRange(mesh.faces_num), 1024, [&](const IndexRange range) {
      for (const int face : range) {
        for (const int corner : faces[face]) {
          const int next_corner = bke::mesh::face_corner_next(faces[face], corner);
          const OrderedEdge corner_edge(corner_verts[corner], corner_verts[next_corner]);
          const int edge_map = calc_edges::edge_to_hash_map_i(corner_edge, parallel_mask);
          corner_edges[corner] = edge_maps[edge_map].index_of(corner_edge);
        }
      }
    });

    BLI_assert(!corner_edges.contains(-1));
    BLI_assert(BKE_mesh_is_valid(&mesh));
    return;
  }

  BLI_assert_msg(keep_existing_edges || !no_new_edges,
                 "Mesh must not contain corners at this point");

  const int new_edges_num = edge_offsets.total_size();

  IndexMaskMemory memory;
  IndexRange mask_new_edges;
  IndexMask src_to_dst_mask;

  MutableSpan<int2> edge_verts(MEM_malloc_arrayN<int2>(new_edges_num, AT), new_edges_num);
#ifndef NDEBUG
  edge_verts.fill(int2(-1));
#endif

  if (keep_existing_edges) {
    mask_new_edges = IndexRange(new_edges_num).drop_front(original_unique_edge_num);

    if (original_edges_are_distinct) {
      src_to_dst_mask = IndexRange(original_unique_edge_num);
    }
    else {
      constexpr int no_original_edge = std::numeric_limits<int>::max();
      Array<int> map_edge_to_first_original(edge_offsets.total_size());
      map_edge_to_first_original.as_mutable_span().fill(no_original_edge);

      for (const int edge_i : original_edges.index_range()) {
        const OrderedEdge edge = original_edges[edge_i];
        const int map_i = calc_edges::edge_to_hash_map_i(edge, parallel_mask);
        const int edge_index = edge_maps[map_i].index_of(edge);
        int &original_edge = map_edge_to_first_original[edge_offsets[map_i][edge_index]];
        original_edge = math::min(original_edge, edge_i);
      }

      BLI_assert(!map_edge_to_first_original.as_span().contains(no_original_edge));

      src_to_dst_mask = IndexMask::from_predicate(
          IndexRange(mesh.edges_num), GrainSize(2048), memory, [&](const int srd_edge_i) {
            const OrderedEdge edge = original_edges[srd_edge_i];
            const int map_i = calc_edges::edge_to_hash_map_i(edge, parallel_mask);
            const int edge_index = edge_maps[map_i].index_of(edge);
            return map_edge_to_first_original[edge_offsets[map_i][edge_index]] == srd_edge_i;
          });
      BLI_assert(src_to_dst_mask.size() == original_unique_edge_num);
    }

    array_utils::gather(
        original_edges, src_to_dst_mask, edge_verts.take_front(original_unique_edge_num));

    Array<int> map_edge_to_dst(edge_offsets.total_size());
#ifndef NDEBUG
    map_edge_to_dst.as_mutable_span().fill(-1);
#endif

    if (original_edges_are_distinct) {
      Array<int> map_iter(edge_maps.size(), 0);
      /* TODO: Is this really faster than VectorSet::index_of if will be parallel? */
      for (const int edge_i : IndexRange(mesh.edges_num)) {
        const int edge_map = calc_edges::edge_to_hash_map_i(original_edges[edge_i], parallel_mask);
        map_edge_to_dst[edge_offsets[edge_map][map_iter[edge_map]]] = edge_i;
        map_iter[edge_map]++;
      }
    }
    else {
      src_to_dst_mask.foreach_index(
          GrainSize(1024), [&](const int src_index, const int dst_index) {
            const OrderedEdge edge = original_edges[src_index];
            const int map_i = calc_edges::edge_to_hash_map_i(edge, parallel_mask);
            const int edge_index = edge_maps[map_i].index_of(edge);
            map_edge_to_dst[edge_offsets[map_i][edge_index]] = dst_index;
          });
    }

    if (!no_new_edges) {
      BLI_assert(edge_offsets.data().size() == original_edge_maps_prefix.data().size());
      Array<int> new_edge_sizes(edge_offsets.data().size());
      for (const int i : edge_offsets.data().index_range()) {
        new_edge_sizes[i] = edge_offsets.data()[i] - original_edge_maps_prefix.data()[i];
      }
      const OffsetIndices<int> new_edge_offsets(new_edge_sizes.as_span());
      BLI_assert(new_edges_num == original_unique_edge_num + new_edge_offsets.total_size());

      const int new_edges_start = original_unique_edge_num;
      for (const int map_i : edge_maps.index_range()) {
        const int map_new_edges_start = new_edge_offsets[map_i].start();

        array_utils::fill_index_range(map_edge_to_dst.as_mutable_span()
                                          .slice(edge_offsets[map_i])
                                          .drop_front(original_edge_maps_prefix[map_i].size()),
                                      new_edges_start + map_new_edges_start);
      }
    }

    BLI_assert(!map_edge_to_dst.as_span().contains(-1));

    threading::parallel_for(IndexRange(mesh.faces_num), 2048, [&](const IndexRange range) {
      for (const int face_i : range) {
        const IndexRange face = faces[face_i];
        for (const int corner : face) {
          const int next_corner = bke::mesh::face_corner_next(face, corner);
          const OrderedEdge corner_edge(corner_verts[corner], corner_verts[next_corner]);
          const int edge_map = calc_edges::edge_to_hash_map_i(corner_edge, parallel_mask);
          const int edge_index = edge_maps[edge_map].index_of(corner_edge);
          corner_edges[corner] = map_edge_to_dst[edge_offsets[edge_map][edge_index]];
        }
      }
    });

    MutableSpan<int2> new_edge_verts = edge_verts.drop_front(original_unique_edge_num);
    for (const int map_i : edge_maps.index_range()) {
      const IndexRange all_map_edges = edge_offsets[map_i];
      const IndexRange original_map_edges = original_edge_maps_prefix[map_i];
      const IndexRange new_map_edges = IndexRange::from_begin_size(
          all_map_edges.start() - original_map_edges.start(),
          all_map_edges.size() - original_map_edges.size());
      new_edge_verts.slice(new_map_edges).copy_from(edge_maps[map_i].as_span().cast<int2>().take_back(new_map_edges.size()));
    }
  }
  else {
    if (mesh.edges_num != 0) {
      src_to_dst_mask = IndexMask::from_predicate(
          IndexRange(mesh.edges_num), GrainSize(1024), memory, [&](const int edge_i) {
            const OrderedEdge edge = original_edges[edge_i];
            const int edge_map = calc_edges::edge_to_hash_map_i(edge, parallel_mask);
            return edge_maps[edge_map].contains(edge);
          });
      const int total_face_old_edges = src_to_dst_mask.size();

      mask_new_edges = IndexRange(new_edges_num).drop_front(total_face_old_edges);
      array_utils::gather(
          original_edges, src_to_dst_mask, edge_verts.take_front(total_face_old_edges));

      Array<int> new_map_edge_sizes(new_edges_num, 0);
      for (const int map_i : edge_maps.index_range()) {
        for (const int i : edge_maps[map_i].index_range()) {
          new_map_edge_sizes[edge_offsets[map_i][i]] = 1;
        }
      }
      offset_indices::accumulate_counts_to_offsets(new_map_edge_sizes.as_mutable_span());

      src_to_dst_mask.foreach_index([&](const int original_edge_i, const int dst_edge_i) {
        const OrderedEdge edge = original_edges[original_edge_i];
        const int edge_map = calc_edges::edge_to_hash_map_i(edge, parallel_mask);
        const int edge_index = edge_maps[edge_map].index_of(edge);
        new_map_edge_sizes[edge_offsets[edge_map][edge_index]] = dst_edge_i;
      });

      threading::parallel_for(IndexRange(mesh.faces_num), 2048, [&](const IndexRange range) {
        for (const int face_i : range) {
          const IndexRange face = faces[face_i];
          for (const int corner : face) {
            const int next_corner = bke::mesh::face_corner_next(face, corner);
            const OrderedEdge corner_edge(corner_verts[corner], corner_verts[next_corner]);
            const int edge_map = calc_edges::edge_to_hash_map_i(corner_edge, parallel_mask);
            const int edge_index = edge_maps[edge_map].index_of(corner_edge);
            corner_edges[corner] = new_map_edge_sizes[edge_offsets[edge_map][edge_index]];
          }
        }
      });
    }
    else {
      mask_new_edges = IndexRange(new_edges_num);
      calc_edges::serialize_and_initialize_deduplicated_edges(edge_maps, edge_offsets, edge_verts);
      calc_edges::update_edge_indices_in_face_loops(
          faces, corner_verts, edge_maps, parallel_mask, edge_offsets, corner_edges);
    }
  }

  BLI_assert(!corner_edges.contains(-1));
  BLI_assert(!edge_verts.contains(int2(-1)));

  Mesh *edge_buffer_mesh = mesh_new_no_attributes(0, 0, 0, 0);
  CustomData_free(&edge_buffer_mesh->edge_data);
  CustomData_reset(&edge_buffer_mesh->edge_data);
  edge_buffer_mesh->edges_num = mesh.edges_num;
  CustomData_init_from(
      &mesh.edge_data, &edge_buffer_mesh->edge_data, CD_MASK_MESH.emask, mesh.edges_num);

  CustomData_free(&mesh.edge_data);
  CustomData_reset(&mesh.edge_data);
  mesh.edges_num = new_edges_num;

  const AttributeAccessor src_attributes = edge_buffer_mesh->attributes();

  BLI_assert(src_to_dst_mask.size() + mask_new_edges.size() == new_edges_num);
  BLI_assert(mask_new_edges.one_after_last() == new_edges_num);

  /* Static storage to extend life-time of strings for reference filter. */
  constexpr std::array<StringRef, 2> skip = {".edge_verts", ".select_edge"};
  const auto edge_attribute_filer = bke::attribute_filter_with_skip_ref(attribute_filter, skip);

  scatter_attributes(src_attributes,
                     AttrDomain::Edge,
                     AttrDomain::Edge,
                     edge_attribute_filer,
                     src_to_dst_mask,
                     dst_attributes);

  fill_attribute_range_default(
      dst_attributes, AttrDomain::Edge, edge_attribute_filer, mask_new_edges);

  BKE_id_free(nullptr, edge_buffer_mesh);

  dst_attributes.add<int2>(
      ".edge_verts", AttrDomain::Edge, AttributeInitMoveArray(edge_verts.data()));

  if (select_new_edges) {
    SpanAttributeWriter<bool> select_edge = dst_attributes.lookup_or_add_for_write_span<bool>(
        ".select_edge", AttrDomain::Edge);
    select_edge.span.drop_back(mask_new_edges.size()).fill(false);
    select_edge.span.take_back(mask_new_edges.size()).fill(true);
    select_edge.finish();
  }

  if (!keep_existing_edges) {
    /* All edges are rebuilt from the faces, so there are no loose edges. */
    mesh.tag_loose_edges_none();
  }

  /* Explicitly clear edge maps, because that way it can be parallelized. */
  calc_edges::clear_hash_tables(edge_maps);

  BLI_assert(BKE_mesh_is_valid(&mesh));
}

}  // namespace blender::bke
