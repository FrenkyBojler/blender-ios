/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"

#include "BKE_attribute.hh"
#include "BKE_geometry_set.hh"
#include "BKE_instances.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "NOD_rna_define.hh"
#include "NOD_xpbd_constraints.hh"
#include "NOD_xpbd_solver.hh"

#include "node_geometry_util.hh"

#include <fmt/format.h>

#if 1 /* Debug stuff */
#  include "BKE_global.hh"
#  include "BKE_main.hh"
#  include "BLI_fileops.hh"
#  include "BLI_path_utils.hh"
#  include <iostream>
#endif

namespace blender::nodes::node_geo_xpbd_constraint_residuals_cc {

using xpbd_constraints::ConstraintEvalData;
using xpbd_constraints::ConstraintEvalParams;
using xpbd_constraints::ConstraintTypeInfo;
using xpbd_constraints::ConstraintVariables;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  const int geometry_in = b.add_input<decl::Geometry>("Geometry").index();

  b.add_input<decl::Vector>("Position")
      .implicit_field_on(implicit_field_inputs::position, {geometry_in});
  b.add_input<decl::Rotation>("Rotation").field_on({geometry_in}).hide_value();
  b.add_input<decl::Vector>("Velocity").field_on({geometry_in}).hide_value();
  b.add_input<decl::Vector>("Angular Velocity").field_on({geometry_in}).hide_value();

  b.add_input<decl::Bundle>("Constraints").description("Bundle of constraint geometries");
  b.add_output<decl::Bundle>("Constraints")
      .description("Bundle of constraint geometries")
      .align_with_previous();

  b.add_input<decl::Geometry>("Colliders")
      .only_instances()
      .description("Instances of colliders to evaluate contact transforms");
}

static ConstraintEvalParams extract_eval_params(GeoNodeExecParams params)
{
  ConstraintEvalParams eval_params;
  eval_params.delta_time = 0.0f;
  eval_params.delta_time_squared = 0.0f;
  eval_params.inv_delta_time = 0.0f;
  eval_params.inv_delta_time_squared = 0.0f;
  eval_params.error_message_add = [params](const StringRef message) {
    params.error_message_add(geo_eval_log::NodeWarningType::Warning, message);
  };
  eval_params.debug_check = false;

  return eval_params;
}

static void get_constraint_data(GeoNodeExecParams params,
                                const bool debug_output,
                                Vector<ConstraintEvalData> &constraint_data)
{
  const BundlePtr constraints_ptr = params.extract_input<BundlePtr>("Constraints");

  const Span<ConstraintTypeInfo> constraint_infos = xpbd_constraints::get_constraint_info_ordered(
      debug_output);
  constraint_data.reinitialize(constraint_infos.size());

  for (const int i : constraint_infos.index_range()) {
    const ConstraintTypeInfo &info = constraint_infos[i];
    constraint_data[i].type = &info;

    if (!constraints_ptr) {
      continue;
    }
    const std::optional<Bundle::Item> item = constraints_ptr->lookup(
        SocketInterfaceKey(info.ui_name));
    if (!item || item->type != bke::node_socket_type_find_static(SOCK_GEOMETRY)) {
      continue;
    }

    const GeometrySet &geometry_set = *static_cast<const GeometrySet *>(item->value);
    if (geometry_set.has_pointcloud()) {
      const AttributeAccessor attributes =
          *geometry_set.get_component<PointCloudComponent>()->attributes();

      IndexMask constraints_mask = IndexRange(attributes.domain_size(AttrDomain::Point));
      constraint_data[i].geometry = geometry_set;
      constraint_data[i].constraints = std::move(constraints_mask);
      constraint_data[i].group_masks = {};
    }
    else {
      constraint_data[i].geometry = {};
      constraint_data[i].constraints = {};
      constraint_data[i].group_masks = {};
    }
  }
}

static void set_constraint_data_output(GeoNodeExecParams params,
                                       const Span<ConstraintEvalData> constraint_data)
{
  BundlePtr constraints_ptr = Bundle::create();
  BLI_assert(constraints_ptr->is_mutable());
  Bundle &constraints = const_cast<Bundle &>(*constraints_ptr);

  for (const ConstraintEvalData &data : constraint_data) {
    const bke::bNodeSocketType *stype = bke::node_socket_type_find_static(SOCK_GEOMETRY);

    if (data.geometry) {
      constraints.add(SocketInterfaceKey(data.type->ui_name), *stype, &(*data.geometry));
    }
    else {
      const GeometrySet geometry = {};
      constraints.add(SocketInterfaceKey(data.type->ui_name), *stype, &geometry);
    }
  }

  params.set_output("Constraints", std::move(constraints_ptr));
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");
  Field<float3> position_field = params.extract_input<Field<float3>>("Position");
  Field<math::Quaternion> rotation_field = params.extract_input<Field<math::Quaternion>>(
      "Rotation");
  Field<float3> velocity_field = params.extract_input<Field<float3>>("Velocity");
  Field<float3> angular_velocity_field = params.extract_input<Field<float3>>("Angular Velocity");

  /* XXX Would be nice if we could output these as an anonymous attribute instead of a named
   * attribute. However, the type can be different for each constraint (typically Float or Vector),
   * so a single attribute type may not work well. Casting to the largest possible type (float3,
   * maybe float4 in future) could work but may need explicit conversion.
   * When "rainbow" sockets are available that may also be an option. */
  // const std::optional<StringRef> residual_output_id =
  //     params.get_output_anonymous_attribute_id_if_needed("Residual");
  const std::optional<StringRef> residual_output_id = "residual";

  GeometrySet colliders_geometry_set = params.extract_input<GeometrySet>("Colliders");
  Span<float4x4> collider_transforms = colliders_geometry_set.has_instances() ?
                                           colliders_geometry_set.get_instances()->transforms() :
                                           Span<float4x4>{};

  ConstraintEvalParams eval_params = extract_eval_params(params);

  if (!residual_output_id) {
    /* Pass-through. */
    params.set_output<BundlePtr>("Constraints", params.extract_input<BundlePtr>("Constraints"));
    params.set_default_remaining_outputs();
    return;
  }

  Vector<ConstraintEvalData> constraint_data;
  get_constraint_data(params, false, constraint_data);

  static const Array<GeometryComponent::Type> types = {bke::GeometryComponent::Type::Mesh,
                                                       bke::GeometryComponent::Type::PointCloud,
                                                       bke::GeometryComponent::Type::Curve,
                                                       bke::GeometryComponent::Type::GreasePencil};
  for (const bke::GeometryComponent::Type component_type : types) {
    if (geometry_set.has(component_type)) {
      const bke::GeometryComponent &component = *geometry_set.get_component(component_type);

      if (eval_params.debug_recorder) {
        eval_params.debug_recorder->set_geometry(geometry_set, component_type);
      }

      const int num_points = component.attribute_domain_size(AttrDomain::Point);
      ConstraintVariables vars;
      vars.positions.reinitialize(num_points);
      vars.rotations.reinitialize(num_points);
      vars.velocities.reinitialize(num_points);
      vars.angular_velocities.reinitialize(num_points);

      const bke::GeometryFieldContext field_context{component, AttrDomain::Point};
      fn::FieldEvaluator evaluator{field_context, num_points};
      evaluator.add_with_destination(position_field, vars.positions.as_mutable_span());
      evaluator.add_with_destination(rotation_field, vars.rotations.as_mutable_span());
      evaluator.add_with_destination(velocity_field, vars.velocities.as_mutable_span());
      evaluator.add_with_destination(angular_velocity_field,
                                     vars.angular_velocities.as_mutable_span());
      evaluator.evaluate();

      eval_params.collider_transforms = collider_transforms;
      /* XXX Transforms of the previous frame are not currently available, these are always the
       * same as the current frame. Eventually this will allow transfer of velocity from animated
       * colliders. */
      eval_params.old_collider_transforms = eval_params.collider_transforms;

      xpbd_constraints::compute_residuals(eval_params, vars, constraint_data, *residual_output_id);
    }
  }

  set_constraint_data_output(params, constraint_data);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeConstraintResiduals");
  ntype.ui_name = "XPBD Constraint Residuals";
  ntype.ui_description = "Compute residuals of constraints as a measure of convergence";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  node_type_size(ntype, 200, 120, 300);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_xpbd_constraint_residuals_cc
