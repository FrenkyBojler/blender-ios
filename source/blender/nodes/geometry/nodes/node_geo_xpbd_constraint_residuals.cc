/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"

#include "BKE_attribute.hh"
#include "BKE_geometry_set.hh"
#include "BKE_instances.hh"

#include "GEO_hair_solver.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "NOD_geo_hair_constraints.hh"
#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_rna_define.hh"

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

using geometry::hair_constraints::ConstraintEvalParams;
using geometry::hair_constraints::ConstraintTypeInfo;
using geometry::hair_constraints::ConstraintVariables;
using geometry::hair_solver::ConstraintEvalData;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  const int geometry_in = b.add_input<decl::Geometry>("Geometry").index();

  b.add_input<decl::Vector>("Position")
      .implicit_field_on(NODE_DEFAULT_INPUT_POSITION_FIELD, {geometry_in});
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
  return ConstraintEvalParams(
      0.0f,
      [params](const StringRef message) {
        params.error_message_add(NodeWarningType::Warning, message);
      },
      false,
      std::nullopt);
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

  IndexMaskMemory memory;
  Vector<ConstraintEvalData> constraint_data = hair_constraints::constraint_bundle_to_eval_data(
      params.extract_input<BundlePtr>("Constraints"), false, memory);

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

      geometry::hair_solver::compute_residuals(
          eval_params, vars, constraint_data, *residual_output_id);
    }
  }

  params.set_output("Constraints",
                    hair_constraints::constraint_eval_data_to_bundle(constraint_data));
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
