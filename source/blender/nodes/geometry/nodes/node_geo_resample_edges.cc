/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array.hh"
#include "BLI_array_utils.hh"
#include "BLI_index_mask.hh"
#include "BLI_index_range.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"
#include "BLI_task.hh"
#include "BLI_virtual_array.hh"

#include "BKE_attribute.hh"
#include "BKE_attribute_filter.hh"
#include "BKE_attribute_math.hh"
#include "BKE_mesh.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_resample_edges_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Mesh").supported_type(GeometryComponent::Type::Mesh);
  b.add_input<decl::Int>("Count").field_on_all();
  b.add_input<decl::Bool>("Selection").default_value(true).field_on_all();

  b.add_output<decl::Geometry>("Mesh").propagate_all();
}

/* Conversion of range of new edges to range of new points. */
static IndexRange new_edges_to_points(const IndexRange edges, const int src_edge_index)
{
  return edges.shift(-src_edge_index).drop_back(1);
}

template<typename T, typename Func>
static void parallel_transform(MutableSpan<T> values, const int64_t grain_size, const Func &func)
{
  threading::parallel_for(values.index_range(), grain_size, [&](const IndexRange range) {
    MutableSpan<T> values_range = values.slice(range);
    std::transform(values_range.begin(), values_range.end(), values_range.begin(), func);
  });
}

template<typename T>
static void interpolate_point_values(const VArray<T> &src,
                                     const Span<int2> edges,
                                     const OffsetIndices<int> edge_offset,
                                     MutableSpan<T> dst)
{
  threading::parallel_for(edges.index_range(), 1024, [&](const IndexRange range) {
    for (const int index : range) {
      const int2 verties = edges[index];
      const IndexRange dst_range = new_edges_to_points(edge_offset[index], index);

      const T a_value = src[verties[0]];
      const T b_value = src[verties[1]];

      const float total_factor = dst_range.size() + 1;
      for (const int i : dst_range.index_range()) {
        const float factor = float(i + 1) / total_factor;
        dst[dst_range[i]] = bke::attribute_math::mix2(factor, a_value, b_value);
      }
    }
  });
}

static void interpolate_point_attributes(const AttributeAccessor src_attributes,
                                         const bke::AttributeFilter &attribute_filter,
                                         const Span<int2> edges,
                                         const int start_vert,
                                         const OffsetIndices<int> edge_offset,
                                         MutableAttributeAccessor dst_attributes)
{
  src_attributes.foreach_attribute([&](const bke::AttributeIter attribute) {
    if (attribute.domain != AttrDomain::Point) {
      return;
    }
    if (attribute_filter.allow_skip(attribute.name)) {
      return;
    }
    const bke::GAttributeReader src = src_attributes.lookup(attribute.name, AttrDomain::Point);
    bke::GSpanAttributeWriter dst = dst_attributes.lookup_or_add_for_write_only_span(
        attribute.name, AttrDomain::Point, attribute.data_type);
    if (!dst) {
      return;
    }
    bke::attribute_math::convert_to_static_type(src.varray.type(), [&](auto dummy) {
      using T = decltype(dummy);
      const VArray<T> src_typed = src.varray.typed<T>();
      MutableSpan<T> dst_typed = dst.span.typed<T>();
      array_utils::copy(src_typed, dst_typed.take_front(start_vert));
      interpolate_point_values<T>(src_typed, edges, edge_offset, dst_typed.drop_front(start_vert));
    });
    dst.finish();
    return;
  });
}

template<typename T>
static void fill_groups(const VArray<T> src, const OffsetIndices<int> groups, MutableSpan<T> dst)
{
  threading::parallel_for(src.index_range(), 2048, [&](const IndexRange range) {
    for (const int index : range) {
      dst.slice(groups[index]).fill(src[index]);
    }
  });
}

static void fill_groups_attributes(const AttributeAccessor src_attributes,
                                   const bke::AttributeFilter &attribute_filter,
                                   const OffsetIndices<int> groups,
                                   const AttrDomain domain,
                                   MutableAttributeAccessor dst_attributes)
{
  src_attributes.foreach_attribute([&](const bke::AttributeIter attribute) {
    if (attribute.domain != domain) {
      return;
    }
    if (attribute_filter.allow_skip(attribute.name)) {
      return;
    }
    const bke::GAttributeReader src = src_attributes.lookup(attribute.name, domain);
    bke::GSpanAttributeWriter dst = dst_attributes.lookup_or_add_for_write_only_span(
        attribute.name, domain, attribute.data_type);
    if (!dst) {
      return;
    }
    bke::attribute_math::convert_to_static_type(src.varray.type(), [&](auto dummy) {
      using T = decltype(dummy);
      fill_groups<T>(src.varray.typed<T>(), groups, dst.span.typed<T>());
    });
    dst.finish();
    return;
  });
}

static void build_edges(const Span<int2> src_edges,
                        const OffsetIndices<int> edge_offset,
                        const int start_vert,
                        MutableSpan<int2> dst_edges)
{
  threading::parallel_for(src_edges.index_range(), 1024, [&](const IndexRange range) {
    for (const int edge_index : range) {
      const int2 src_edge = src_edges[edge_index];
      MutableSpan<int2> edges = dst_edges.slice(edge_offset[edge_index]);
      BLI_assert(!edges.is_empty());
      if (edges.size() == 1) {
        edges.first() = src_edge;
        continue;
      }

      const IndexRange verts_on_edge = new_edges_to_points(edge_offset[edge_index], edge_index);
      const IndexRange dst_left_verts = verts_on_edge.drop_back(1);
      const IndexRange dst_right_verts = verts_on_edge.drop_front(1);

      edges.first()[0] = src_edge[0];
      edges.first()[1] = start_vert + verts_on_edge.first();

      const IndexRange edges_to_fill = edges.index_range().drop_front(1).drop_back(1);
      for (const int i : edges_to_fill.index_range()) {
        const int index = edges_to_fill[i];
        edges[index] = int2(start_vert) + int2(dst_left_verts[i], dst_right_verts[i]);
      }

      edges.last()[0] = start_vert + verts_on_edge.last();
      edges.last()[1] = src_edge[1];
    }
  });
}

static void build_faces(const Span<int> src_corner_verts,
                        const Span<int> src_corner_edges,
                        const Span<int2> src_edges,
                        const Span<int2> dst_edges,
                        const OffsetIndices<int> dst_corner_offsets,
                        const OffsetIndices<int> dst_edge_offsets,
                        MutableSpan<int> r_corner_verts,
                        MutableSpan<int> r_corner_edges)
{
  Array<bool> loop_edge_is_inverted(src_corner_verts.size());
  threading::parallel_for(dst_corner_offsets.index_range(), 4096, [&](const IndexRange range) {
    for (const int corner_index : range) {
      const int src_corner_vert = src_corner_verts[corner_index];
      const int src_corner_edge = src_corner_edges[corner_index];
      const int2 src_edge = src_edges[src_corner_edge];
      BLI_assert(ELEM(src_corner_vert, src_edge[0], src_edge[1]));
      loop_edge_is_inverted[corner_index] = src_edge[0] != src_corner_vert;
    }
  });

  threading::parallel_for(dst_corner_offsets.index_range(), 1024, [&](const IndexRange range) {
    for (const int corner_index : range) {
      const int src_corner_edge = src_corner_edges[corner_index];
      const IndexRange edges_range = dst_edge_offsets[src_corner_edge];
      MutableSpan<int> corner_edges = r_corner_edges.slice(dst_corner_offsets[corner_index]);
      if (!loop_edge_is_inverted[corner_index]) {
        array_utils::fill_index_range<int>(corner_edges, edges_range.first());
      }
      else {
        std::iota(corner_edges.rbegin(), corner_edges.rend(), edges_range.first());
      }
    }
  });

  threading::parallel_for(dst_corner_offsets.index_range(), 1024, [&](const IndexRange range) {
    for (const int corner_index : range) {
      const int src_corner_edge = src_corner_edges[corner_index];
      const IndexRange edges_range = dst_edge_offsets[src_corner_edge];
      const Span<int> corner_edges = r_corner_edges.slice(dst_corner_offsets[corner_index]);
      MutableSpan<int> corner_verts = r_corner_verts.slice(dst_corner_offsets[corner_index]);

      array_utils::copy<int>(corner_edges, corner_verts);
      if (!loop_edge_is_inverted[corner_index]) {
        parallel_transform(
            corner_verts, 2048, [&](const int edge_index) { return dst_edges[edge_index][0]; });
      }
      else {
        parallel_transform(
            corner_verts, 2048, [&](const int edge_index) { return dst_edges[edge_index][1]; });
      }
    }
  });
}

static void accumulate_face_offsets(const OffsetIndices<int> src_face_offsets,
                                    const OffsetIndices<int> corner_offsets,
                                    MutableSpan<int> dst_face_offsets)
{
  if (dst_face_offsets.is_empty()) {
    return;
  }
  threading::parallel_for(src_face_offsets.index_range(), 2048, [&](const IndexRange range) {
    for (const int index : range) {
      dst_face_offsets[index] = corner_offsets[src_face_offsets[index]].size();
    }
  });
  offset_indices::accumulate_counts_to_offsets(dst_face_offsets);
}

static Mesh *resample_edges(const Mesh &src_mesh,
                            const VArray<int> &count_varray,
                            const bke::AttributeFilter &attribute_filter)
{
  /* To avoid allocation of array to accumulate new verts, use edge offset[index] - index. */
  Array<int> accumulate_edges(src_mesh.edges_num + 1, 0);
  const OffsetIndices<int> edge_offset = [&]() {
    MutableSpan<int> accumulate_span = accumulate_edges.as_mutable_span().drop_back(1);
    array_utils::copy(count_varray, accumulate_span);
    parallel_transform(accumulate_span, 2048, [&](int count) { return count; });
    return offset_indices::accumulate_counts_to_offsets(accumulate_edges);
  }();

  Array<int> accumulate_corners(src_mesh.corners_num + 1);
  const OffsetIndices<int> corner_offset = [&]() {
    MutableSpan<int> accumulate_span = accumulate_corners.as_mutable_span().drop_back(1);
    array_utils::copy(src_mesh.corner_edges(), accumulate_span);
    devirtualize_varray(count_varray, [&](auto count_varray) {
      parallel_transform(
          accumulate_span, 2048, [&](int edge_index) { return count_varray[edge_index]; });
    });
    return offset_indices::accumulate_counts_to_offsets(accumulate_corners);
  }();

  const int total_vert = edge_offset.total_size() - src_mesh.edges_num + src_mesh.verts_num;
  Mesh *dst_mesh = BKE_mesh_new_nomain(
      total_vert, edge_offset.total_size(), src_mesh.faces_num, corner_offset.total_size());
  BKE_mesh_copy_parameters_for_eval(dst_mesh, &src_mesh);

  const AttributeAccessor src_attributes = src_mesh.attributes();
  MutableAttributeAccessor dst_attributes = dst_mesh->attributes_for_write();

  interpolate_point_attributes(src_attributes,
                               attribute_filter,
                               src_mesh.edges(),
                               src_mesh.verts_num,
                               edge_offset,
                               dst_attributes);
  fill_groups_attributes(src_attributes,
                         bke::attribute_filter_with_skip_ref(attribute_filter, {".edge_verts"}),
                         edge_offset,
                         AttrDomain::Edge,
                         dst_attributes);
  build_edges(src_mesh.edges(), edge_offset, src_mesh.verts_num, dst_mesh->edges_for_write());
  accumulate_face_offsets(src_mesh.faces(), corner_offset, dst_mesh->face_offsets_for_write());
  build_faces(src_mesh.corner_verts(),
              src_mesh.corner_edges(),
              src_mesh.edges(),
              dst_mesh->edges(),
              corner_offset,
              edge_offset,
              dst_mesh->corner_verts_for_write(),
              dst_mesh->corner_edges_for_write());
  bke::copy_attributes(
      src_attributes, AttrDomain::Face, AttrDomain::Face, attribute_filter, dst_attributes);
  fill_groups_attributes(
      src_attributes,
      bke::attribute_filter_with_skip_ref(attribute_filter, {".corner_vert", ".corner_edge"}),
      corner_offset,
      AttrDomain::Corner,
      dst_attributes);
  return dst_mesh;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh");

  Field<int> count_field = params.extract_input<Field<int>>("Count");
  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");

  static const auto exec_preset = mf::build::exec_presets::AllSpanOrSingle();

  static const auto clamp_fn = mf::build::SI1_SO<int, int>(
      "Hard Min", [](int value) { return math::max(0, value); }, exec_preset);
  const Field<int> clamped_count_field(FieldOperation::Create(clamp_fn, {std::move(count_field)}));

  static const auto select_fn = mf::build::SI2_SO<int, bool, int>(
      "Selection", [](int value, bool selection) { return value * int(selection); }, exec_preset);
  const Field<int> selected_count_field(FieldOperation::Create(
      select_fn, {std::move(clamped_count_field), std::move(selection_field)}));

  static const auto increment_fn = mf::build::SI1_SO<int, int>(
      "Verts count to edges", [](int value) { return value + 1; }, exec_preset);
  const Field<int> edge_count_field(
      FieldOperation::Create(increment_fn, {std::move(selected_count_field)}));

  const bke::AttributeFilter &attribute_filter = params.get_attribute_filter("Mesh");
  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    const Mesh *mesh = geometry_set.get_mesh();
    if (mesh == nullptr) {
      return;
    }
    const bke::AttributeAccessor attributes = mesh->attributes();
    const bke::MeshFieldContext context(*mesh, AttrDomain::Edge);
    fn::FieldEvaluator evaluator(context, attributes.domain_size(AttrDomain::Edge));
    evaluator.add(edge_count_field);
    evaluator.evaluate();
    const VArray<int> count_varray = evaluator.get_evaluated<int>(0);
    const std::optional<int> count = count_varray.get_if_single();
    if (count.has_value() && *count == 1) {
      return;
    }
    geometry_set.replace_mesh(resample_edges(*mesh, count_varray, attribute_filter));
  });
  params.set_output("Mesh", std::move(geometry_set));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeResampleEdges");
  ntype.ui_name = "Resample Edges";
  ntype.nclass = NODE_CLASS_GEOMETRY;

  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_resample_edges_cc
