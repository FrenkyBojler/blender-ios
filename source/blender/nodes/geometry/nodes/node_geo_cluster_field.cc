/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array.hh"
#include "BLI_array_utils.hh"
#include "BLI_index_mask.hh"
#include "BLI_index_mask_expression.hh"
#include "BLI_kdtree.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_cluster_field_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Vector>("Position"_ustr)
      .implicit_field(NODE_DEFAULT_INPUT_POSITION_FIELD)
      .structure_type(StructureType::Field);

  b.add_input<decl::Int>("Group ID"_ustr).supports_field().hide_value();
  b.add_input<decl::Float>("Distance"_ustr).default_value(0.001f).min(0.0f).subtype(PROP_DISTANCE);
  b.add_input<decl::Bool>("Selection"_ustr).default_value(true).supports_field().hide_value();

  b.add_output<decl::Int>("Cluster ID"_ustr).field_source_reference_all();
}

static constexpr int no_cluster_value = -1;

static void masked_cluster_ids(const Span<float3> all_positions,
                               const IndexMask &mask_to_cluster,
                               const float distance,
                               MutableSpan<int> r_cluster_ids)
{
  BLI_assert(mask_to_cluster.size() == r_cluster_ids.size());
  KDTree<float3> *tree = kdtree_new<float3>(mask_to_cluster.size());
  mask_to_cluster.foreach_index(
      [&](const int i, const int pos) { kdtree_insert<float3>(tree, pos, all_positions[i]); });
  kdtree_balance<float3>(tree);

  r_cluster_ids.fill(no_cluster_value);
  kdtree_calc_duplicates_fast<float3>(tree, distance, true, r_cluster_ids.data());
  kdtree_free<float3>(tree);

  threading::parallel_for(mask_to_cluster.index_range(), 1024 * 4, [&](const IndexRange range) {
    for (const int i : range) {
      if (r_cluster_ids[i] == no_cluster_value) {
        r_cluster_ids[i] = i;
      }
    }
  });
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
      : bke::GeometryFieldInput(CPPType::get<int>(), "Cluster Field"),
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
    const VArraySpan<int> group_ids = evaluator.get_evaluated<int>(1);
    const IndexMask selection = evaluator.get_evaluated_selection_as_mask();

    IndexMaskMemory memory;
    /* With context info about mask to compute we can skip processing of rest values. But in
     * current case this will affect result values since some cluster might have lowest ID of
     * element outside of visible mask. */
    const IndexMask mask_to_cluster = IndexMask::from_intersection(mask, selection, memory);
    const IndexMask mask_to_fallback = index_mask::evaluate_expression(
        (index_mask::ExprBuilder{}).subtract(&mask, {&selection}), memory);

    if (mask_to_cluster.is_empty()) {
      /* TODO: VArray from index range. */
      return VArray<int>::from_func(mask.min_array_size(), [](int i) { return i; });
    }

    Array<int> cluster_ids(mask.min_array_size());

#ifndef NDEBUG
    cluster_ids.as_mutable_span().fill(no_cluster_value);
#endif

    mask_to_fallback.foreach_index_optimized<int>(
        [&](const int index) { cluster_ids[index] = index; }, exec_mode::parallel);

    if (distance_ == 0.0f) {
      /* TODO: Is this is really faster then explicit creation of groups for parallel processing
       * (#IndexMask::from_groups)? */
      Map<std::pair<float3, int>, int> clusters;
      mask_to_cluster.foreach_index([&](const int index) {
        clusters.add(std::make_pair(positions[index], group_ids[index]), index);
      });

      if (clusters.size() == 1) {
        const int first_selected = mask_to_cluster.first();
        BLI_assert(clusters.lookup(std::make_pair(positions[first_selected],
                                                  group_ids[first_selected])) == first_selected);
        index_mask::masked_fill<int>(
            cluster_ids.as_mutable_span(), first_selected, mask_to_cluster);
        return VArray<int>::from_container(std::move(cluster_ids));
      }

      mask_to_cluster.foreach_index(
          [&](const int index) {
            cluster_ids[index] = clusters.lookup(
                std::make_pair(positions[index], group_ids[index]));
          },
          exec_mode::parallel);

      return VArray<int>::from_container(std::move(cluster_ids));
    }

    /* TODO: We must be able to check group_ids.is_single() and skip this at all. */
    const VectorSet<int> group_indexing = [&]() {
      VectorSet<int> group_indexing;
      mask_to_cluster.foreach_index(
          [&](const int index) { group_indexing.add(group_ids[index]); });
      return group_indexing;
    }();
    const int groups_num = group_indexing.size();

    const auto get_group_index = [&](const int i) {
      return group_indexing.index_of(group_ids[i]);
    };

    Array<IndexMask> all_indices_by_group_id(groups_num);
    IndexMask::from_groups<int>(mask_to_cluster, memory, get_group_index, all_indices_by_group_id);

    /* The grain size should be larger as each group gets smaller. */
    const int avg_group_size = domain_size / group_indexing.size();
    const int grain_size = std::max(8192 / avg_group_size, 1);
    threading::parallel_for(IndexRange(groups_num), grain_size, [&](const IndexRange range) {
      Vector<int, 64> buffer;
      for (const int group_i : range) {
        const IndexMask &group_indices = all_indices_by_group_id[group_i];
        buffer.reinitialize(group_indices.size() * 2);

        MutableSpan<int> group_cluser_ids = buffer.as_mutable_span().take_front(
            group_indices.size());
        masked_cluster_ids(positions, group_indices, distance_, group_cluser_ids);

        MutableSpan<int> mask_indices = buffer.as_mutable_span().take_back(group_indices.size());
        group_indices.to_indices(mask_indices);

        group_indices.foreach_index_optimized<int>(
            [&](const int index, const int pos) {
              cluster_ids[index] = mask_indices[group_cluser_ids[pos]];
            },
            exec_mode::parallel);
      }
    });

#ifndef NDEBUG
    mask.foreach_index([&](const int i) { BLI_assert(cluster_ids[i] != no_cluster_value); });
#endif

    return VArray<int>::from_container(std::move(cluster_ids));
  }

  void foreach_recursive_field(FunctionRef<void(const GField &)> fn) const override
  {
    fn(positions_field_);
    fn(group_field_);
    fn(selection_field_);
  }

  uint64_t hash() const final
  {
    return get_default_hash(positions_field_, group_field_, selection_field_);
  }

  bool is_equal_to(const fn::FieldInput &other) const final
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
  params.set_output("Cluster ID"_ustr,
                    Field<int>::from_input<ClusterFieldInput>(
                        params.extract_input<Field<float3>>("Position"_ustr),
                        params.extract_input<Field<int>>("Group ID"_ustr),
                        params.extract_input<Field<bool>>("Selection"_ustr),
                        params.extract_input<float>("Distance"_ustr)));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeClusterField");
  ntype.ui_name = "Cluster Field";
  ntype.ui_description = "Group elements into integer IDs based on proximity of vector values";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_cluster_field_cc
