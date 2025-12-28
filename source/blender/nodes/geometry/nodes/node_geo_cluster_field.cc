/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array.hh"
#include "BLI_array_utils.hh"
#include "BLI_index_mask.hh"
#include "BLI_kdtree.hh"

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

static constexpr int no_cluster_value = -1;

static void masked_cluster_ids(const Span<float3> all_positions,
                               const IndexMask &mask_to_cluster,
                               const float distance,
                               MutableSpan<int> r_cluster_ids)
{
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

    const auto default_no_clusters_to_out = [&]() {
      Array<int> cluster_ids(mask.min_array_size());
      array_utils::fill_index_range(cluster_ids.as_mutable_span());
      return VArray<int>::from_container(std::move(cluster_ids));
    };

    const int domain_size = context.attributes()->domain_size(context.domain());
    fn::FieldEvaluator evaluator{context, domain_size};
    evaluator.add(positions_field_);
    evaluator.add(group_field_);
    evaluator.set_selection(selection_field_);
    evaluator.evaluate();
    const VArraySpan<float3> positions = evaluator.get_evaluated<float3>(0);
    const VArraySpan<int> group_ids = evaluator.get_evaluated<int>(1);
    const IndexMask selection = evaluator.get_evaluated_selection_as_mask();

    if (selection.is_empty()) {
      return default_no_clusters_to_out();
    }

    if (mask.last() < selection.first()) {
      return default_no_clusters_to_out();
    }

    if (distance_ == 0.0f) {
      /* TODO: Do it really faster then explicit creation of groups for parallel processing? */
      Map<std::pair<float3, int>, int> clasters;
      selection.foreach_index([&](const int index) {
        clasters.add(std::make_pair(positions[index], group_ids[index]), index);
      });

      Array<int> cluster_ids(mask.min_array_size());
      array_utils::fill_index_range(cluster_ids.as_mutable_span());

      if (clasters.size() == 1) {
        const int first_selected = selection.first();
        BLI_assert(clasters.lookup(std::make_pair(positions[first_selected],
                                                  group_ids[first_selected])) == first_selected);
        index_mask::masked_fill<int>(cluster_ids.as_mutable_span(), first_selected, selection);
        return VArray<int>::from_container(std::move(cluster_ids));
      }

      selection.foreach_index(GrainSize(1024), [&](const int index) {
        cluster_ids[index] = clasters.lookup(std::make_pair(positions[index], group_ids[index]));
      });

      return VArray<int>::from_container(std::move(cluster_ids));
    }

    /* TODO: We must be able to check group_ids.is_single() and skip this at all. */
    const VectorSet<int> group_indexing(group_ids);
    const int groups_num = group_indexing.size();

    const auto get_group_index = [&](const int i) {
      return group_indexing.index_of(group_ids[i]);
    };

    IndexMaskMemory memory;
    Array<IndexMask> all_indices_by_group_id(groups_num);
    IndexMask::from_groups<int>(selection, memory, get_group_index, all_indices_by_group_id);

    Array<Array<int>> cluster_ids_by_group(all_indices_by_group_id.size());

    /* The grain size should be larger as each group gets smaller. */
    const int avg_group_size = domain_size / group_indexing.size();
    const int grain_size = std::max(8192 / avg_group_size, 1);
    threading::parallel_for(IndexRange(groups_num), grain_size, [&](const IndexRange range) {
      Vector<int> group_cluser_ids;
      for (const int group_i : range) {
        const IndexMask &group_indices = all_indices_by_group_id[group_i];
        if (mask.bounds().intersect(group_indices.bounds()).is_empty()) {
          continue;
        }

        group_cluser_ids.reinitialize(group_indices.size());
        masked_cluster_ids(
            positions, group_indices, distance_, group_cluser_ids.as_mutable_span());
        cluster_ids_by_group[group_i] = group_cluser_ids.as_span();
      }
    });

    Array<int> cluster_ids(mask.min_array_size());
    array_utils::fill_index_range(cluster_ids.as_mutable_span());

    threading::parallel_for(IndexRange(groups_num), grain_size, [&](const IndexRange range) {
      Vector<int> mask_indices;
      for (const int group_i : range) {
        const IndexMask &group_indices = all_indices_by_group_id[group_i];
        if (mask.bounds().intersect(group_indices.bounds()).is_empty()) {
          continue;
        }

        const int last_reqered_group_element = group_indices.iterator_to_index(
            *group_indices.find_smaller_equal(mask.last()));
        const IndexMask requered_group_mask = group_indices.slice(0,
                                                                  last_reqered_group_element + 1);

        mask_indices.reinitialize(requered_group_mask.size());
        requered_group_mask.to_indices(mask_indices.as_mutable_span());

        const Span<int> group_ids = cluster_ids_by_group[group_i];

        requered_group_mask.foreach_index_optimized<int>(
            GrainSize(2048), [&](const int index, const int pos) {
              cluster_ids[index] = mask_indices[group_ids[pos]];
            });
      }
    });

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
