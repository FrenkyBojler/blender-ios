/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_mesh_types.h"
#include "DNA_pointcloud_types.h"

#include "BLI_array.hh"
#include "BLI_array_utils.hh"
#include "BLI_index_mask.hh"
#include "BLI_index_mask_expression.hh"
#include "BLI_kdtree.hh"

#include "GEO_foreach_geometry.hh"
#include "GEO_mesh_merge_by_distance.hh"
#include "GEO_point_merge_by_distance.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_cluster_field_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Vector>("Position")
      .implicit_field(NODE_DEFAULT_INPUT_POSITION_FIELD)
      .structure_type(StructureType::Field);

  b.add_input<decl::Int>("Group ID").supports_field().hide_value();
  b.add_input<decl::Float>("Distance").default_value(0.001f).min(0.0f).subtype(PROP_DISTANCE);
  b.add_input<decl::Bool>("Selection").default_value(true).supports_field().hide_value();

  b.add_output<decl::Int>("Cluster ID").field_source_reference_all();
}

class ClusterFieldInput final : public bke::GeometryFieldInput {
 private:
  Field<float3> positions_field_;
  Field<int> group_field_;
  Field<bool> selection_field_;
  float distance_;

 public:
  ClusterFieldInput(Field<float3> positions_field,
                    Field<int> group_field,
                    Field<bool> selection_field,
                    const float distance)
      : bke::GeometryFieldInput(CPPType::get<int>(), "Index of Nearest"),
        positions_field_(std::move(positions_field)),
        group_field_(std::move(group_field)),
        selection_field_(std::move(selection_field)),
        distance_(distance)
  {
  }

  GVArray get_varray_for_context(const bke::GeometryFieldContext &context,
                                 const IndexMask &mask) const final
  {
    if (!context.attributes()) {
      return {};
    }
    const int domain_size = context.attributes()->domain_size(context.domain());
    fn::FieldEvaluator evaluator{context, domain_size};
    evaluator.add(positions_field_);
    evaluator.add(group_field_);
    evaluator.set_selection(selection_field_);
    evaluator.evaluate();
    const VArraySpan<float3> positions = evaluator.get_evaluated<float3>(0);
    const VArray<int> group_ids = evaluator.get_evaluated<int>(1);
    const IndexMask selection = evaluator.get_evaluated_selection_as_mask();

    const auto default_no_clusters_to_out = [&]() {
      Array<int> cluster_ids(mask.min_array_size());
      array_utils::fill_index_range(cluster_ids.as_mutable_span());
      return VArray<int>::from_container(std::move(cluster_ids));
    };

    if (selection.is_empty()) {
      return default_no_clusters_to_out();
    }

    if (mask.last() < selection.first()) {
      return default_no_clusters_to_out();
    }

    KDTree_3d *tree = kdtree_3d_new(selection.size());
    selection.foreach_index([&](const int64_t i) { kdtree_3d_insert(tree, i, positions[i]); });
    kdtree_3d_balance(tree);

    constexpr int no_cluster_value = -1;
    Array<int> gathered_cluster_ids(selection.min_array_size(), no_cluster_value);
    /* If #selection was not full then #gathered_cluster_ids will point to position in #selection,
     * but not to value. */
    const int total_merge_ops = kdtree_3d_calc_duplicates_fast(
        tree, distance_, true, gathered_cluster_ids.data());
    BLI_assert(!gathered_cluster_ids.as_span().contains(no_cluster_value));
    kdtree_3d_free(tree);

    if (total_merge_ops == 0) {
      return default_no_clusters_to_out();
    }

    const int last_reqered_gathered_index = selection.iterator_to_index(
        *selection.find_smaller_equal(mask.last()));
    const IndexRange requered_selection = IndexRange::from_begin_end_inclusive(
        0, last_reqered_gathered_index);
    const IndexMask requered_selection_mask = selection.slice(requered_selection);
    threading::parallel_for(requered_selection, 1024, [&](const IndexRange range) {
      for (const int i : range) {
        if (gathered_cluster_ids[i] == no_cluster_value) {
          gathered_cluster_ids[i] = i;
        }
      }
    });

    Array<int> selection_reverse(selection.size());
    selection.to_indices(selection_reverse.as_mutable_span());
    array_utils::gather(
        selection_reverse.as_span(),
        gathered_cluster_ids.as_span().take_front(last_reqered_gathered_index + 1),
        gathered_cluster_ids.as_mutable_span().take_front(last_reqered_gathered_index + 1));

    Array<int> cluster_ids(mask.min_array_size());
    array_utils::fill_index_range(cluster_ids.as_mutable_span());
    array_utils::scatter(
        gathered_cluster_ids.as_span().take_front(last_reqered_gathered_index + 1),
        selection.slice(requered_selection),
        cluster_ids.as_mutable_span());

    BLI_assert(!cluster_ids.as_span().contains(no_cluster_value));
    return VArray<int>::from_container(std::move(cluster_ids));
  }

  void for_each_field_input_recursive(FunctionRef<void(const FieldInput &)> fn) const override
  {
    positions_field_.node().for_each_field_input_recursive(fn);
    group_field_.node().for_each_field_input_recursive(fn);
    selection_field_.node().for_each_field_input_recursive(fn);
  }

  uint64_t hash() const final
  {
    return get_default_hash(positions_field_, group_field_, selection_field_);
  }

  bool is_equal_to(const fn::FieldNode &other) const final
  {
    if (const auto *other_field = dynamic_cast<const ClusterFieldInput *>(&other)) {
      return distance_ == other_field->distance_ &&
             positions_field_ == other_field->positions_field_ &&
             group_field_ == other_field->group_field_ &&
             selection_field_ == other_field->selection_field_;
    }
    return false;
  }

  std::optional<AttrDomain> preferred_domain(const GeometryComponent &component) const final
  {
    return bke::try_detect_field_domain(component, positions_field_);
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  Field<float3> position_field = params.extract_input<Field<float3>>("Position");
  Field<int> group_field = params.extract_input<Field<int>>("Group ID");
  Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");
  const float distance = params.extract_input<float>("Distance");

  params.set_output("Cluster ID",
                    Field<int>(std::make_shared<ClusterFieldInput>(std::move(position_field),
                                                                   std::move(group_field),
                                                                   std::move(selection_field),
                                                                   distance)));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeClusterField");
  ntype.ui_name = "Cluster Field";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_cluster_field_cc
