/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_geometry_set_instances.hh"
#include "BKE_instances.hh"

#include "BLI_math_matrix.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_instance_bounds_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Bool>("Local Space").default_value(true);
  b.add_input<decl::Bool>("Use Radius")
      .default_value(true)
      .description(
          "For curves, point clouds, and Grease Pencil, take the radius attribute into account "
          "when computing the bounds.");
  b.add_output<decl::Vector>("Min").field_source();
  b.add_output<decl::Vector>("Max").field_source();
}

class InstanceBoundsField final : public bke::InstancesFieldInput {
 private:
  bool local_space_;
  bool use_radius_;
  bool return_max_;

 public:
  InstanceBoundsField(bool local_space, bool use_radius, bool return_max)
      : bke::InstancesFieldInput(CPPType::get<float3>(), return_max ? "Max" : "Min"),
        local_space_(local_space),
        use_radius_(use_radius),
        return_max_(return_max)
  {
  }

  GVArray get_varray_for_context(const bke::Instances &instances,
                                 const IndexMask &mask) const final
  {
    const Span<int> handles = instances.reference_handles();
    const Span<bke::InstanceReference> references = instances.references();
    const Span<float4x4> transforms = instances.transforms();

    IndexMaskMemory memory;
    IndexMask reference_mask(references.size());
    Array<bool> reference_in_mask(references.size(), false);

    mask.foreach_index(
        [&](const int i) {
          const int handle = handles[i];
          if (handle < reference_in_mask.size()) {
            reference_in_mask[handle] = true;
          }
        },
        exec_mode::grain_size(2048));

    reference_mask = IndexMask::from_bools(reference_in_mask.as_span(), memory);
    Array<float3> reference_bounds(references.size());

    reference_mask.foreach_index(
        [&](const int reference_index) {
          const bke::InstanceReference &reference = references[reference_index];

          GeometrySet instance_geometry;
          switch (reference.type()) {
            case bke::InstanceReference::Type::GeometrySet:
              instance_geometry = reference.geometry_set();
              break;
            case bke::InstanceReference::Type::Object:
              instance_geometry = bke::object_get_evaluated_geometry_set(reference.object());
              break;
            case bke::InstanceReference::Type::Collection:
              break;
            case bke::InstanceReference::Type::None:
              break;
          }

          std::optional<Bounds<float3>> sub_bounds =
              instance_geometry.compute_boundbox_without_instances(use_radius_);

          if (sub_bounds) {
            reference_bounds[reference_index] = return_max_ ? sub_bounds->max : sub_bounds->min;
          }
          else {
            reference_bounds[reference_index] = float3(0.0f);
          }
        },
        exec_mode::grain_size(128));

    Array<float3> output_bounds(mask.min_array_size());
    mask.foreach_index(GrainSize(4096), [&](const int instance_index) {
      const float3 bound_point = reference_bounds[handles[instance_index]];
      if (local_space_) {
        output_bounds[instance_index] = bound_point;
      }
      else {
        const float4x4 &transform = transforms[instance_index];
        output_bounds[instance_index] = math::transform_point(transform, bound_point);
      }
    });

    return VArray<float3>::from_container(std::move(output_bounds));
  }

  uint64_t hash() const override
  {
    return get_default_hash(local_space_, use_radius_, return_max_);
  }

  bool is_equal_to(const fn::FieldNode &other) const override
  {
    if (const auto *other_field = dynamic_cast<const InstanceBoundsField *>(&other)) {
      return local_space_ == other_field->local_space_ &&
             use_radius_ == other_field->use_radius_ && return_max_ == other_field->return_max_;
    }
    return false;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  const bool local_space = params.extract_input<bool>("Local Space");
  const bool use_radius = params.extract_input<bool>("Use Radius");
  params.set_output(
      "Min", Field<float3>(std::make_shared<InstanceBoundsField>(local_space, use_radius, false)));
  params.set_output(
      "Max", Field<float3>(std::make_shared<InstanceBoundsField>(local_space, use_radius, true)));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeInputInstanceBounds");
  ntype.ui_name = "Instance Bounds";
  ntype.ui_description = "Calculate position bounds of each instance's geometry set";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_instance_bounds_cc
