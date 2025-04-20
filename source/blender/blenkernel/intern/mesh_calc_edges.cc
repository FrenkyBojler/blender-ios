/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BLI_array_utils.hh"
#include "BLI_math_base.h"
#include "BLI_ordered_edge.hh"
#include "BLI_set.hh"
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
      if (task_index == (parallel_mask & edge_hash_2(ordered_edge))) {
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
          if (task_index == (parallel_mask & edge_hash_2(ordered_edge))) {
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
        const int task_index = parallel_mask & edge_hash_2(ordered_edge);
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
      const int task_index = parallel_mask & edge_hash_2(ordered_edge);
      const EdgeMap &edge_map = edge_maps[task_index];
      const int edge_i = edge_map.index_of(ordered_edge);
      const int dst_edge_i = edge_offsets[task_index][edge_i];
      src_to_dst_edges[src_edge_i] = dst_edge_i;
    }
  });
}

}  // namespace calc_edges

static Array<std::pair<std::string, AttributeDomainAndType>> filer_attributes(const AttributeAccessor attributes,
                                                                              const AttrDomain domain,
                                                                              const AttributeFilter &attribute_filter)
{
  Array<std::pair<std::string, AttributeDomainAndType>> attributes_metadata;
  attributes.foreach_attribute([&](const bke::AttributeIter &attribute) {
    if (attribute.domain != domain) {
      return;
    }
    if (filer.allow_skip(attribute.name)) {
      return;
    }
    if (attribute.data_type == CD_PROP_STRING) {
      return;
    }
    attributes_metadata.append_as(attribute.name, {attribute.domain, attribute.data_type});
  });
  return attributes_metadata;
}

static void add_new_mesh_edges(Mesh &mesh,
                               const Span<EdgeMap> edge_maps,
                               const uint32_t parallel_mask,
                               const OffsetIndices<int> edge_offsets,
                               const OffsetIndices<int> existing_edge_offsets)
{
  BLI_assert(edge_offsets.total_size() > mesh.edges_num);
  const int old_edges_num = mesh.edges_num;
  mesh.edges_num = edge_offsets.total_size();
  CustomData_realloc(&mesh.edge_data, old_edges_num, mesh.edges_num, CD_SET_DEFAULT);

  MutableSpan<int2> edges = mesh.edges_for_write();

  MutableAttributeAccessor attributes = mesh.attributes_for_write();
  attributes.add<int>(".corner_edge", AttrDomain::Corner, AttributeInitConstruct());

  Array<int> new_edge_sizes(edge_maps.size() + 1);
  for (const int index : IndexRange(edge_maps.size() + 1)) {
    const int new_edges_num = edge_offsets.data[index] - existing_edge_offsets.data[index];
    new_edge_sizes[index] = old_edges_num + new_edges_num;
  }
  const OffsetIndices<int> new_edges_offsets(new_edge_sizes);

  const OffsetIndices<int> face = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  MutableSpan<int> corner_edges = mesh.corner_edges_for_write();

  threading::parallel_for(edge_maps.index_range(), 2048, [&](const IndexRange range) {
    for (const int taks_i : range) {
      const IndexRange new_edges_range = new_edges_offsets[task_i];
      const Span<OrderedEdge> new_task_edges = edge_maps[taks_i].as_span().take_back(new_edges_range.size());
      edges.slice(new_edges_range).copy_from(new_task_edges.cast<int2>());
    }
  }, threading::accumulated_task_sizes([&](const IndexRange range) { return new_edges_offsets[range].size(); }));

  threading::parallel_for(faces.index_range(), 100, [&](IndexRange range) {
    for (const int face_index : range) {
      const IndexRange face = faces[face_index];
      for (const int corner : face) {
        const int vert = corner_verts[corner];
        const int vert_prev = corner_verts[bke::mesh::face_corner_next(face, corner)];
        const OrderedEdge ordered_edge(vert_prev, vert);
        const int task_index = parallel_mask & edge_hash_2(ordered_edge);
        const EdgeMap &edge_map = edge_maps[task_index];
        const int edge_i = edge_map.index_of(ordered_edge);

        const IndexRange original_edges_in_task = existing_edge_offsets[task_index];
        const bool is_new_edge = original_edges_in_task.size() <= edge_i;
        if (is_new_edge) {
          const int new_edge_index = new_edges_offsets[task_i][edge_i - original_edges_in_task.one_after_last()];
          BLI_assert(OrderedEdge(edges[new_edge_index]) == ordered_edge);
          corner_edges[corner] = new_edge_index;
          continue;
        }

        const int original_edge_index = original_edges_in_task[edge_i];
        BLI_assert(OrderedEdge(edges[original_edge_index]) == ordered_edge);
        corner_edges[corner] = original_edge_index;
      }
    }
  });
}

static void deduplicate_mesh_edges(Mesh &mesh,
                                   const Span<EdgeMap> edge_maps,
                                   const uint32_t parallel_mask,
                                   const OffsetIndices<int> edge_offsets)
{
  BLI_assert(edge_offsets.total_size() > mesh.edges_num);
  const int old_edges_num = mesh.edges_num;
  mesh.edges_num += edge_offsets.total_size();
  CustomData_realloc(&mesh.edge_data, old_edges_num, mesh.edges_num);

  /* TODO: Only one edge value is picked to result, no mixing done between duplicates. */

  MutableAttributeAccessor attributes = mesh.attributes_for_write();
  attributes.add<int>(".corner_edge", AttrDomain::Corner, AttributeInitConstruct());
  calc_edges::update_edge_indices_in_face_loops(mesh.faces(),
                                                mesh.corner_verts(),
                                                edge_maps,
                                                parallel_mask,
                                                edge_offsets,
                                                mesh.corner_edges_for_write());
  
  
}

static void new_mesh_edges(Mesh &mesh,
                           const Span<EdgeMap> edge_maps,
                           const uint32_t parallel_mask,
                           const OffsetIndices<int> edge_offsets,
                           const OffsetIndices<int> existing_edge_offsets)
{
  BLI_assert(edge_offsets.total_size() > mesh.edges_num);
  const int old_edges_num = mesh.edges_num;
  mesh.edges_num += edge_offsets.total_size();
  CustomData_realloc(&mesh.edge_data, old_edges_num, mesh.edges_num);
  
  
}

void mesh_calc_edges(Mesh &mesh,
                     bool keep_existing_edges,
                     const bool select_new_edges,
                     const AttributeFilter &attribute_filter)
{
  /* Parallelization is achieved by having multiple hash tables for different subsets of edges.
   * Each edge is assigned to one of the hash maps based on the lower bits of a hash value. */
  const int parallel_maps = calc_edges::get_parallel_maps_count(mesh);
  BLI_assert(is_power_of_2_i(parallel_maps));
  const uint32_t parallel_mask = uint32_t(parallel_maps) - 1;
  Array<calc_edges::EdgeMap> edge_maps(parallel_maps);
  calc_edges::reserve_hash_maps(mesh, keep_existing_edges, edge_maps);

  Array<std::pair<std::string, AttributeDomainAndType>> edge_attributes;
  if (keep_existing_edges) {
    edge_attributes = filer_attributes(mesh.attributes(), AttrDomain::Edge, attribute_filter);
  }

  Array<int> existing_edge_sizes;
  if (keep_existing_edges) {
    calc_edges::add_existing_edges_to_hash_maps(mesh, parallel_mask, edge_maps);

    existing_edge_sizes.reinitialize(edge_maps.size() + 1);
    for (const int i : edge_maps.index_range()) {
      existing_edge_sizes[i] = edge_maps[i].size();
    }
  }
  const OffsetIndices<int> existing_edge_offsets = offset_indices::accumulate_counts_to_offsets(existing_edge_sizes);

  calc_edges::add_face_edges_to_hash_maps(mesh, parallel_mask, edge_maps);
  Array<int> edge_sizes(edge_maps.size() + 1);
  for (const int i : edge_maps.index_range()) {
    edge_sizes[i] = edge_maps[i].size();
  }
  const OffsetIndices<int> edge_offsets = offset_indices::accumulate_counts_to_offsets(edge_sizes);

  const bool original_edges_are_distinct = existing_edge_offsets.total_size() == mesh.edges_num;
  const bool no_new_edges = edge_offsets.total_size() == mesh.edges_num;
  if (keep_existing_edges && original_edges_are_distinct && no_new_edges) {
    return;
  }

  if (!keep_existing_edges && no_new_edges) {
    CustomData_free(&mesh.edge_data);
    CustomData_reset(&mesh.edge_data);
    mesh.edges_num = 0;
    return;
  }

  if (keep_existing_edges && original_edges_are_distinct && !no_new_edges) {
    add_new_mesh_edges(mesh, edge_maps, parallel_mask, edge_offsets, existing_edge_offsets);
  }

  if (keep_existing_edges && !original_edges_are_distinct && no_new_edges) {
    deduplicate_mesh_edges(mesh, edge_maps, parallel_mask, edge_offsets);
  }

  if (!keep_existing_edges && !no_new_edges) {
    new_mesh_edges(mesh, edge_maps, parallel_mask, edge_offsets, existing_edge_offsets);
  }
  
  /* We have to totally rebuild original edges due to duplicates and add new edges due to lack of them in faces. */

  {
    MutableAttributeAccessor attributes = mesh.attributes_for_write();
    attributes.add<int>(".corner_edge", AttrDomain::Corner, AttributeInitConstruct());
    calc_edges::update_edge_indices_in_face_loops(mesh.faces(),
                                                  mesh.corner_verts(),
                                                  edge_maps,
                                                  parallel_mask,
                                                  edge_offsets,
                                                  mesh.corner_edges_for_write());
  }

  Mesh *mesh_with_old_edges = nullptr;
  BLI_SCOPED_DEFER([&]() {
    if (mesh_with_old_edges != nullptr) {
      BKE_id_free(nullptr, mesh_with_old_edges);
    }
  });

  if (keep_existing_edges || select_new_edges) {
    mesh_with_old_edges = mesh_new_no_attributes(0, 0, 0, 0);
    BLI_assert(mesh_with_old_edges != nullptr);
    CustomData_free(&mesh_with_old_edges->edge_data);
    CustomData_init_from(
        &mesh.edge_data, &mesh_with_old_edges->edge_data, CD_MASK_MESH.emask, mesh.edges_num);
    mesh_with_old_edges->edges_num = mesh.edges_num;
  }

  CustomData_free(&mesh.edge_data);
  CustomData_reset(&mesh.edge_data);
  mesh.edges_num = edge_offsets.total_size();

  {
    MutableAttributeAccessor attributes = mesh.attributes_for_write();
    MutableSpan<int2> new_edges(MEM_malloc_arrayN<int2>(edge_offsets.total_size(), __func__),
                                edge_offsets.total_size());
    calc_edges::serialize_and_initialize_deduplicated_edges(edge_maps, edge_offsets, new_edges);
    attributes.add<int2>(
        ".edge_verts", AttrDomain::Edge, AttributeInitMoveArray(new_edges.data()));
  }

  MutableAttributeAccessor dst_attributes = mesh.attributes_for_write();

  if (select_new_edges) {
    SpanAttributeWriter<bool> select_edge = dst_attributes.lookup_or_add_for_write_span<bool>(
        ".select_edge", AttrDomain::Edge);
    select_edge.span.fill(true);
    select_edge.finish();
  }

  if (mesh_with_old_edges != nullptr) {
    const AttributeAccessor old_edge_attributes = mesh_with_old_edges->attributes();

    const VArraySpan<int2> original_edges = *old_edge_attributes.lookup<int2>(".edge_verts",
                                                                              AttrDomain::Edge);
    /* TODO: Predict common case when there is no attributes to propagate. */
    Array<int, 0> src_to_dst_edges(original_edges.size());

    calc_edges::known_edges_to_new(
        edge_offsets, edge_maps, parallel_mask, original_edges, src_to_dst_edges);

    if (select_new_edges) {
      SpanAttributeWriter<bool> select_edge = dst_attributes.lookup_for_write_span<bool>(
          ".select_edge");
      select_edge.span.fill_indices(src_to_dst_edges.as_span(), false);
      select_edge.finish();
    }

    /* Static storage to extend life-time of strings for reference filter. */
    static const Set<std::string> skip = {".edge_verts", ".select_edge"};
    const auto filer = bke::attribute_filter_with_skip_ref(attribute_filter, skip);
    old_edge_attributes.foreach_attribute([&](const bke::AttributeIter &src_attribute) {
      BLI_assert(src_attribute.domain == bke::AttrDomain::Edge);
      if (filer.allow_skip(src_attribute.name)) {
        return;
      }
      GSpanAttributeWriter dst_attribute = dst_attributes.lookup_or_add_for_write_span(
          src_attribute.name, src_attribute.domain, src_attribute.data_type);

      attribute_math::convert_to_static_type(dst_attribute.span.type(), [&](auto dummy) {
        using T = decltype(dummy);
        const VArraySpan<T> src = src_attribute.get<T>().varray;
        MutableSpan<T> dst = dst_attribute.span.typed<T>();
        array_utils::scatter(Span<T>(src), src_to_dst_edges.as_span(), dst);
      });
      dst_attribute.finish();
    });
  }

  if (!keep_existing_edges) {
    /* All edges are rebuilt from the faces, so there are no loose edges. */
    mesh.tag_loose_edges_none();
  }

  /* Explicitly clear edge maps, because that way it can be parallelized. */
  calc_edges::clear_hash_tables(edge_maps);
}

}  // namespace blender::bke
