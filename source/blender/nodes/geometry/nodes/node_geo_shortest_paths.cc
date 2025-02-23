/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <queue>

#include "BLI_array_utils.hh"
#include "BLI_kdtree.h"
#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_shortest_paths_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Vector>("Position").implicit_field(implicit_field_inputs::position);
  b.add_input<decl::Bool>("End Selection").default_value(false).hide_value().supports_field();
  b.add_input<decl::Int>("Branching Limit").default_value(1).min(1).supports_field();

  b.add_output<decl::Int>("Next Vertex Index").field_source().reference_pass_all();
}

static KDTree_3d *build_kdtree(const Span<float3> positions, const IndexMask &mask)
{
  KDTree_3d *tree = BLI_kdtree_3d_new(mask.size());
  mask.foreach_index(
      [&](const int index) { BLI_kdtree_3d_insert(tree, index, positions[index]); });
  BLI_kdtree_3d_balance(tree);
  return tree;
}

static int find_first_nearest(const KDTree_3d &tree,
                              const Span<bool> visited,
                              const Span<int> skip,
                              const Span<float> distances,
                              const float length,
                              const float3 &position)
{
  return BLI_kdtree_3d_find_nearest_cb_cpp(
      &tree, position, nullptr, [&](const int index, const float * /*co*/, const float dist_sq) {
        if (visited[index]) {
          return 0;
        }
        if (math::sqrt(dist_sq) + length > distances[index]) {
          return 0;
        }
        if (skip.contains(index)) {
          return 0;
        }
        return 1;
      });
}

using VertPriority = std::pair<float, int>;

static void shortest_paths(const KDTree_3d &tree,
                           const IndexMask end_selection,
                           const Span<float3> positions,
                           const VArray<int> &branching_limit,
                           MutableSpan<int> r_next_index,
                           MutableSpan<float> r_distance)
{
  Array<bool> visited(positions.size(), false);

  std::priority_queue<VertPriority, std::vector<VertPriority>, std::greater<VertPriority>> queue;

  end_selection.foreach_index([&](const int start_vert_i) {
    r_distance[start_vert_i] = 0.0f;
    queue.emplace(0.0f, start_vert_i);
  });

  Vector<int> branches;
  while (!queue.empty()) {
    branches.clear();
    const auto [length, index] = queue.top();
    queue.pop();
    if (visited[index]) {
      continue;
    }
    visited[index] = true;
    const float3 &position = positions[index];

    for ([[maybe_unused]] const int i : IndexRange(math::max(1, branching_limit[index]))) {
      const int other_i = find_first_nearest(
          tree, visited, branches, r_distance, length, position);
      if (other_i == -1) {
        continue;
      }
      const float distance = math::distance(position, positions[other_i]);
      const float new_length = length + distance;
      r_distance[other_i] = new_length;
      r_next_index[other_i] = index;
      queue.emplace(new_length, other_i);
      branches.append(other_i);
    }
  }
}

class ShortestPathsNextVertFieldInput final : public bke::GeometryFieldInput {
 private:
  Field<float3> positions_;
  Field<bool> end_selection_;
  Field<int> branching_limit_field_;

 public:
  ShortestPathsNextVertFieldInput(Field<float3> positions,
                                  Field<bool> end_selection,
                                  Field<int> branching_limit_field)
      : bke::GeometryFieldInput(CPPType::get<int>(), "Shortest Paths Next Vertex Field"),
        positions_(std::move(positions)),
        end_selection_(std::move(end_selection)),
        branching_limit_field_(std::move(branching_limit_field))
  {
    category_ = Category::Generated;
  }

  GVArray get_varray_for_context(const bke::GeometryFieldContext &context,
                                 const IndexMask &mask) const final
  {
    const int domain_size = context.attributes()->domain_size(context.domain());
    fn::FieldEvaluator evaluator(context, domain_size);
    evaluator.add(end_selection_);
    evaluator.add(positions_);
    evaluator.add(branching_limit_field_);
    evaluator.evaluate();
    const IndexMask end_selection = evaluator.get_evaluated_as_mask(0);
    const VArraySpan<float3> positions = evaluator.get_evaluated<float3>(1);
    const VArray<int> branching_limit = evaluator.get_evaluated<int>(2);

    Array<int> next_index(domain_size, -1);
    Array<float> distance(domain_size, FLT_MAX);

    if (end_selection.is_empty()) {
      array_utils::fill_index_range<int>(next_index);
      return VArray<int>::ForContainer(std::move(next_index));
    }

    KDTree_3d *tree = build_kdtree(positions, IndexRange(domain_size));
    shortest_paths(*tree, end_selection, positions, branching_limit, next_index, distance);
    BLI_kdtree_3d_free(tree);

    mask.foreach_index_optimized<int>(GrainSize(2048), [&](const int index) {
      if (next_index[index] == -1) {
        next_index[index] = index;
      }
    });

    return VArray<int>::ForContainer(std::move(next_index));
  }

  void for_each_field_input_recursive(FunctionRef<void(const FieldInput &)> fn) const override
  {
    end_selection_.node().for_each_field_input_recursive(fn);
    positions_.node().for_each_field_input_recursive(fn);
  }

  uint64_t hash() const override
  {
    return get_default_hash(positions_, end_selection_);
  }

  bool is_equal_to(const fn::FieldNode &other) const override
  {
    if (const ShortestPathsNextVertFieldInput *other_field =
            dynamic_cast<const ShortestPathsNextVertFieldInput *>(&other))
    {
      return other_field->positions_ == positions_ &&
             other_field->end_selection_ == end_selection_;
    }
    return false;
  }

  std::optional<AttrDomain> preferred_domain(
      const GeometryComponent & /*component*/) const override
  {
    return AttrDomain::Point;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  Field<float3> position_field = params.extract_input<Field<float3>>("Position");
  Field<bool> end_selection_field = params.extract_input<Field<bool>>("End Selection");
  Field<int> branching_limit_field = params.extract_input<Field<int>>("Branching Limit");

  Field<int> next_vert_field{
      std::make_shared<ShortestPathsNextVertFieldInput>(std::move(position_field),
                                                        std::move(end_selection_field),
                                                        std::move(branching_limit_field))};

  params.set_output("Next Vertex Index", std::move(next_vert_field));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeShortestPath");
  ntype.ui_name = "Shortest Paths";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_shortest_paths_cc
