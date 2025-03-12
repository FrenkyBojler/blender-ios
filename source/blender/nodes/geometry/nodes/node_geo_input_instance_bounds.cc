/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_instances.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_instance_bounds_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Instances");
  b.add_input<decl::Bool>("Use Radius")
      .default_value(true)
      .description(
          "For curves, point clouds, and Grease Pencil, take the radius attribute into account "
          "when computing the bounds.");
  b.add_output<decl::Vector>("Bounds Min").field_source();
  b.add_output<decl::Vector>("Bounds Max").field_source();
}

class InstanceVectorFieldInput final : public bke::InstancesFieldInput {
 private:
  GVArray gvarray;

 public:
  InstanceVectorFieldInput(VArray<float3> vectors)
      : bke::InstancesFieldInput(CPPType::get<float3>(), "Vectors"), gvarray(std::move(vectors))
  {
  }

  GVArray get_varray_for_context(const bke::Instances &instances,
                                 const IndexMask & /*mask*/) const final
  {
    return gvarray;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.get_input<GeometrySet>("Instances");
  const bool use_radius = params.extract_input<bool>("Use Radius");

  if (!geometry_set.has_instances()) {
    params.set_default_remaining_outputs();
    return;
  }

  blender::bke::Instances &instances = *geometry_set.get_instances_for_write();
  Span<int> handles = instances.reference_handles();
  const int instance_count = instances.instances_num();
  const int handle_count = instances.references().size();

  Array<float3> bounds_min(handle_count, float3(0.0f));
  Array<float3> bounds_max(handle_count, float3(0.0f));
  Array<float3> inst_bounds_min(instance_count, float3(0.0f));
  Array<float3> inst_bounds_max(instance_count, float3(0.0));

  instances.ensure_geometry_instances();
  for (const int handle : instances.references().index_range()) {

    const blender::bke::InstanceReference &reference = instances.references()[handle];
    if (reference.type() == blender::bke::InstanceReference::Type::GeometrySet) {
      GeometrySet &instance_geometry = instances.geometry_set_from_reference(handle);

      std::optional<Bounds<float3>> sub_bounds =
          instance_geometry.compute_boundbox_without_instances(use_radius);

      if (sub_bounds) {
        bounds_min[handle] = sub_bounds->min;
        bounds_max[handle] = sub_bounds->max;
      }
    }
  }

  for (int i = 0; i < instance_count; ++i) {
    inst_bounds_min[i] = bounds_min[handles[i]];
    inst_bounds_max[i] = bounds_max[handles[i]];
  }

  Field<float3> min_field{std::make_shared<InstanceVectorFieldInput>(
      VArray<float3>::ForContainer(std::move(inst_bounds_min)))};
  Field<float3> max_field{std::make_shared<InstanceVectorFieldInput>(
      VArray<float3>::ForContainer(std::move(inst_bounds_max)))};

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
