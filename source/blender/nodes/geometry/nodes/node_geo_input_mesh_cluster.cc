/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"
#include "BLI_disjoint_set.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_sort.hh"
#include "BLI_task.hh"

#include "BKE_geometry_fields.hh"
#include "BKE_mesh.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_mesh_cluster_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Vector>("Position"_ustr)
      .implicit_field_on_all(NODE_DEFAULT_INPUT_POSITION_FIELD)
      .supports_field();
  b.add_input<decl::Float>("Weight"_ustr).default_value(1.0f).hide_value().supports_field();
  b.add_input<decl::Float>("Distance"_ustr).default_value(0.001f).min(0.0f).subtype(PROP_DISTANCE);
  b.add_input<decl::Bool>("Selection"_ustr).default_value(false).hide_value().supports_field();

  b.add_output<decl::Int>("Cluster ID"_ustr).field_source_reference_all();
}

class MeshClusterFieldInput final : public bke::MeshFieldInput {
 private:
  Field<bool> selection_field_;
  Field<float3> position_field_;
  Field<float> weight_field_;
  float min_distance_;

 public:
  MeshClusterFieldInput(Field<bool> selection_field,
                        Field<float3> position_field,
                        Field<float> weight_field,
                        float min_distance)
      : bke::MeshFieldInput(CPPType::get<int>(), "Mesh Cluster Field"),
        selection_field_(std::move(selection_field)),
        position_field_(std::move(position_field)),
        weight_field_(std::move(weight_field)),
        min_distance_(min_distance)
  {
  }

  GVArray get_varray_for_context(const Mesh &mesh,
                                 const AttrDomain domain,
                                 const IndexMask &mask) const final
  {
    const Span<int2> edges = mesh.edges();

    const bke::MeshFieldContext edge_context(mesh, AttrDomain::Edge);
    fn::FieldEvaluator edge_evaluator(edge_context, mesh.edges_num);
    edge_evaluator.add(weight_field_);
    edge_evaluator.set_selection(selection_field_);
    edge_evaluator.evaluate();
    const VArray<float> edge_weight = edge_evaluator.get_evaluated<float>(0);
    const IndexMask selection = edge_evaluator.get_evaluated_selection_as_mask();

    if (selection.is_empty()) {
      return fn::IndexFieldInput::get_index_varray(mask);
    }

    const bke::MeshFieldContext vert_context(mesh, AttrDomain::Point);
    fn::FieldEvaluator point_evaluator(vert_context, mesh.verts_num);
    point_evaluator.add(position_field_);
    point_evaluator.evaluate();
    const VArraySpan<float3> position = point_evaluator.get_evaluated<float3>(0);

    DisjointSet<int> vertex_cluster(mesh.verts_num);

    Array<float3> vert_cluster_centre(mesh.verts_num);
    array_utils::copy(position, vert_cluster_centre.as_mutable_span());
    Array<int> vert_cluster_size(mesh.verts_num, 1);

    Array<int> edge_indices;
    if (!edge_weight.is_single()) {
      edge_indices.reinitialize(selection.size());
      selection.to_indices(edge_indices.as_mutable_span());
      const VArraySpan<float> edge_weight_span = edge_weight;
      parallel_sort(
          edge_indices.begin(), edge_indices.end(), [&](const int edge_a, const int edge_b) {
            if (edge_weight_span[edge_a] == edge_weight_span[edge_b]) {
              return edge_a < edge_b;
            }
            return edge_weight_span[edge_a] < edge_weight_span[edge_b];
          });
    }

    const auto try_join_edge = [&](const int edge_i) {
      const int2 edge = edges[edge_i];

      const int2 edge_clusters(vertex_cluster.find_root(edge[0]),
                               vertex_cluster.find_root(edge[1]));
      if (edge_clusters[0] == edge_clusters[1]) {
        return;
      }

      const float3 vert_a_cluster = vert_cluster_centre[edge_clusters[0]];
      const float3 vert_b_cluster = vert_cluster_centre[edge_clusters[1]];

      const float distance = math::distance(vert_a_cluster, vert_b_cluster);

      if (distance >= min_distance_) {
        return;
      }

      /* Use original vertices instead of already found roots to keep path folding optimization. */
      const int new_cluster_root = vertex_cluster.join(edge[0], edge[1]);

      const int new_cluster_size = vert_cluster_size[edge_clusters[0]] +
                                   vert_cluster_size[edge_clusters[1]];
      const float3 new_cluster_center = math::interpolate(vert_a_cluster,
                                                          vert_b_cluster,
                                                          vert_cluster_size[edge_clusters[1]] /
                                                              float(new_cluster_size));

      vert_cluster_centre[new_cluster_root] = new_cluster_center;
      vert_cluster_size[new_cluster_root] = new_cluster_size;
    };

    if (edge_indices.is_empty()) {
      selection.foreach_index(try_join_edge);
    }
    else {
      for (const int edge_i : edge_indices) {
        try_join_edge(edge_i);
      }
    }

    Array<int> cluster_indices(mesh.verts_num);
    threading::parallel_for(
        IndexRange(mesh.verts_num),
        1024,
        [&, vertex_cluster = std::as_const(vertex_cluster)](const IndexRange range) {
          for (const int i : range) {
            cluster_indices[i] = vertex_cluster.find_root(i);
          }
        });

    return mesh.attributes().adapt_domain<int>(
        VArray<int>::from_container(std::move(cluster_indices)), AttrDomain::Point, domain);
  }

  void foreach_recursive_field(FunctionRef<void(const GField &)> fn) const override
  {
    fn(selection_field_);
    fn(position_field_);
    fn(weight_field_);
  }

  uint64_t hash() const override
  {
    return get_default_hash(selection_field_, position_field_, weight_field_, min_distance_);
  }

  bool is_equal_to(const fn::FieldInput &other) const override
  {
    if (const MeshClusterFieldInput *other_field = dynamic_cast<const MeshClusterFieldInput *>(
            &other))
    {
      if (this->min_distance_ != other_field->min_distance_) {
        return false;
      }
      if (this->selection_field_ != other_field->selection_field_) {
        return false;
      }
      if (this->position_field_ != other_field->position_field_) {
        return false;
      }
      if (this->weight_field_ != other_field->weight_field_) {
        return false;
      }
      return true;
    }
    return false;
  }

  std::optional<AttrDomain> preferred_domain(const Mesh & /*mesh*/) const override
  {
    return AttrDomain::Point;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  params.set_output("Cluster ID"_ustr,
                    Field<int>::from_input<MeshClusterFieldInput>(
                        params.extract_input<Field<bool>>("Selection"_ustr),
                        params.extract_input<Field<float3>>("Position"_ustr),
                        params.extract_input<Field<float>>("Weight"_ustr),
                        params.extract_input<float>("Distance"_ustr)));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeInputMeshCluster"_ustr);
  ntype.ui_name = "Mesh Cluster";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_mesh_cluster_cc
