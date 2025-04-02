/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_geometry_set_instances.hh"
#include "BKE_instances.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_instance_bounds_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Bool>("Use Radius")
      .default_value(true)
      .description(
          "For curves, point clouds, and Grease Pencil, take the radius attribute into account "
          "when computing the bounds.");
  b.add_output<decl::Vector>("Bounds Min").field_source();
  b.add_output<decl::Vector>("Bounds Max").field_source();
}

class InstanceBoundsField final : public bke::InstancesFieldInput {
 private:
  bool use_radius_;
  bool return_max_;

 public:
  InstanceBoundsField(bool use_radius, bool return_max)
      : bke::InstancesFieldInput(CPPType::get<float3>(), return_max ? "Bounds Max" : "Bounds Min"),
        use_radius_(use_radius),
        return_max_(return_max)
  {
  }

  GVArray get_varray_for_context(const bke::Instances &instances,
                                 const IndexMask &mask) const final
  {
    Span<int> handles = instances.reference_handles();
    Array<float3> bounds(instances.references().size());
    Array<float3> output_bounds(mask.size());

    bke::Instances &const_instances = const_cast<bke::Instances &>(instances);
    const_instances.ensure_geometry_instances();

    for (const int handle : instances.references().index_range()) {
      const blender::bke::InstanceReference &reference = instances.references()[handle];
      if (reference.type() == blender::bke::InstanceReference::Type::GeometrySet) {
        GeometrySet &instance_geometry = const_instances.geometry_set_from_reference(handle);

        std::optional<Bounds<float3>> sub_bounds =
            instance_geometry.compute_boundbox_without_instances(use_radius_);

        if (sub_bounds) {
          bounds[handle] = return_max_ ? sub_bounds->min : sub_bounds->max;
        }
      }
    }

    mask.foreach_index(
        [&](const int i, const int start_pos) { output_bounds[start_pos] = bounds[handles[i]]; });

    return VArray<float3>::ForContainer(std::move(output_bounds));
  }

  uint64_t hash() const override
  {
    return get_default_hash(use_radius_, return_max_);
  }

  bool is_equal_to(const fn::FieldNode &other) const override
  {
    if (const auto *other_field = dynamic_cast<const InstanceBoundsField *>(&other)) {
      return use_radius_ == other_field->use_radius_ && return_max_ == other_field->return_max_;
    }
    return false;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  bool use_radius = params.extract_input<bool>("Use Radius");
  Field<float3> min_field{std::make_shared<InstanceBoundsField>(use_radius, true)};
  Field<float3> max_field{std::make_shared<InstanceBoundsField>(use_radius, false)};

  params.set_output("Bounds Min", min_field);
  params.set_output("Bounds Max", max_field);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeInputInstanceBounds");
  ntype.ui_name = "Instance Bounds";
  ntype.ui_description = "Calculate position bounds of each instance's geometry set";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_instance_bounds_cc
