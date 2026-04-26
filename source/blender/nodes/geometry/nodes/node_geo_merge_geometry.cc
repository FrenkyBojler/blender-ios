/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_mesh_types.h"
#include "DNA_pointcloud_types.h"

#include "BLI_map.hh"

#include "GEO_foreach_geometry.hh"
#include "GEO_mesh_merge_by_distance.hh"
#include "GEO_point_merge_by_distance.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_merge_geometry_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Geometry>("Geometry"_ustr)
      .supported_type({GeometryComponent::Type::PointCloud, GeometryComponent::Type::Mesh})
      .description("Point cloud or mesh to merge points of");
  b.add_output<decl::Geometry>("Geometry"_ustr).propagate_all().align_with_previous();
  b.add_input<decl::Bool>("Selection"_ustr).default_value(true).hide_value().field_on_all();
  b.add_input<decl::Int>("Merge ID"_ustr)
      .hide_value()
      .field_on_all()
      .implicit_field(NODE_DEFAULT_INPUT_INDEX_FIELD)
      .description("ID of group of the points to merge");
}

static std::optional<int> masked_ids_to_merging_roots(const fn::FieldContext &context,
                                                      const Field<int> &group_id_field,
                                                      const Field<bool> &selection_field,
                                                      const int domain_size,
                                                      Array<int> &r_roots)
{
  FieldEvaluator evaluator(context, domain_size);
  evaluator.add(group_id_field);
  evaluator.set_selection(selection_field);
  evaluator.evaluate();

  const IndexMask selection = evaluator.get_evaluated_selection_as_mask();
  if (selection.is_empty()) {
    return std::nullopt;
  }

  const VArray<int> group_id = evaluator.get_evaluated<int>(0);
  std::optional<VArraySpan<int>> group_id_span;
  Map<int, int> group_id_to_root;
  if (group_id.is_single()) {
    group_id_to_root.add(group_id.get_internal_single(), selection.first());
  }
  else {
    group_id_span.emplace(group_id);
    selection.foreach_index_optimized<int>(
        [&](const int index) { group_id_to_root.add((*group_id_span)[index], index); });
  }

  if ((selection.size() == domain_size) && (group_id_to_root.size() == 1)) {
    BLI_assert(selection.bounds() == IndexRange(domain_size));
    r_roots.reinitialize(domain_size);
    r_roots.fill(0);
    return domain_size - 1;
  }

  if (group_id_to_root.size() == domain_size) {
    BLI_assert(selection.size() == domain_size);
    return std::nullopt;
  }

  r_roots.reinitialize(domain_size);
  /* TODO: Explicitly create groups of indices in merge code and skip unit groups from future
   * processing... */
  r_roots.as_mutable_span().fill(-1);

  if (group_id_to_root.size() == 1) {
    BLI_assert(group_id_to_root.lookup(group_id[selection.first()]) == selection.first());
    index_mask::masked_fill<int>(r_roots.as_mutable_span(), selection.first(), selection);
  }
  else {
    Array<bool> is_unit_group(domain_size, true);
    selection.foreach_index_optimized<int>(
        [&](const int index) {
          const int group_root = group_id_to_root.lookup((*group_id_span)[index]);
          if (group_root != index) {
            is_unit_group[group_root] = false;
          }
        },
        exec_mode::parallel);

    selection.foreach_index_optimized<int>(
        [&](const int index) {
          const int group_root = group_id_to_root.lookup((*group_id_span)[index]);
          if (is_unit_group[group_root]) {
            BLI_assert(group_root == index);
            BLI_assert(r_roots[index] == -1);
            return;
          }
          r_roots[index] = group_root;
        },
        exec_mode::parallel);
  }

  return selection.size() - group_id_to_root.size();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry"_ustr);
  const Field<int> group_id_field = params.extract_input<Field<int>>("Merge ID"_ustr);
  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection"_ustr);

  const AttributeFilter &attribute_filter = params.get_attribute_filter("Geometry"_ustr);

  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry_set) {
    if (const PointCloud *pointcloud = geometry_set.get_pointcloud()) {
      const bke::PointCloudFieldContext context(*pointcloud);
      Array<int> masked_group_ids;
      const std::optional<int> total_merge_ops = masked_ids_to_merging_roots(
          context, group_id_field, selection_field, pointcloud->totpoint, masked_group_ids);
      if (total_merge_ops.has_value()) {
        PointCloud *new_pointcloud = geometry::point_merge_indices(*pointcloud,
                                                                   masked_group_ids.as_span(),
                                                                   pointcloud->totpoint -
                                                                       *total_merge_ops,
                                                                   attribute_filter);
        geometry_set.replace_pointcloud(new_pointcloud);
      }
    }
    if (const Mesh *mesh = geometry_set.get_mesh()) {
      const bke::MeshFieldContext context(*mesh, AttrDomain::Point);
      Array<int> masked_group_ids;
      const std::optional<int> total_merge_ops = masked_ids_to_merging_roots(
          context, group_id_field, selection_field, mesh->verts_num, masked_group_ids);
      if (total_merge_ops.has_value()) {
        Mesh *new_mesh = geometry::mesh_merge_verts(
            *mesh, masked_group_ids.as_mutable_span(), *total_merge_ops, true);
        geometry_set.replace_mesh(new_mesh);
      }
    }
  });

  params.set_output("Geometry"_ustr, std::move(geometry_set));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeMergeGeometry"_ustr);
  ntype.ui_name = "Merge Geometry";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_merge_geometry_cc
