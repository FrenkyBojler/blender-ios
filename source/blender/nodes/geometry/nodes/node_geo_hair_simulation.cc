/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_userdef_types.h"

#include "BKE_geometry_set.hh"
#include "BKE_type_conversions.hh"
#include "NOD_geometry_nodes_bundle.hh"

#include "NOD_rna_define.hh"
#include "NOD_socket_search_link.hh"
#include "NOD_xpbd_constraints.hh"
#include "NOD_xpbd_solver.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "RNA_enum_types.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_hair_simulation_cc {

using xpbd_constraints::ConstraintEvalData;
using xpbd_constraints::ConstraintEvalParams;
using xpbd_constraints::ConstraintTypeInfo;
using xpbd_constraints::ConstraintVariables;

struct ConstraintBundleItems {
  static const SocketInterfaceKey stretch_constraints;
  static const SocketInterfaceKey bending_constraints;
  static const SocketInterfaceKey position_constraints;
  static const SocketInterfaceKey rotation_constraints;
  static const SocketInterfaceKey contact_constraints;
};

const SocketInterfaceKey ConstraintBundleItems::stretch_constraints = SocketInterfaceKey(
    "Stretch");
const SocketInterfaceKey ConstraintBundleItems::bending_constraints = SocketInterfaceKey(
    "Bending");
const SocketInterfaceKey ConstraintBundleItems::position_constraints = SocketInterfaceKey(
    "Position");
const SocketInterfaceKey ConstraintBundleItems::rotation_constraints = SocketInterfaceKey(
    "Rotation");
const SocketInterfaceKey ConstraintBundleItems::contact_constraints = SocketInterfaceKey(
    "Contact");

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Hair").supported_type(bke::GeometryComponent::Type::Curve);
  b.add_input<decl::Int>("Constraint Iterations").default_value(5).min(0);

  b.add_output<decl::Geometry>("Hair").propagate_all();
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  UNUSED_VARS(ptr);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  UNUSED_VARS(node);
}

static void store_hair_rest_shape(GeometrySet &hair_geometry) {}

/* Capture hair attributes for mass, moments of inertia, rod stiffness and damping. */
static void init_hair_physics(GeometrySet &hair_geometry) {}

/* Create internal stretch/shear and bending constraints. */
static void generate_elastic_rod_constraints(const GeometrySet &hair_geometry,
                                             GeometrySet &stretch_constraints,
                                             GeometrySet &bending_constraints)
{
}

/* Create root attachment constraints. */
static void generate_root_attachment_constraints(const GeometrySet &hair_geometry,
                                                 GeometrySet &position_constraints,
                                                 GeometrySet &rotation_constraints)
{
}

static BundlePtr combine_constraint_bundle(GeometrySet &stretch_constraints,
                                           GeometrySet &bending_constraints,
                                           GeometrySet &position_constraints,
                                           GeometrySet &rotation_constraints,
                                           GeometrySet &contact_constraints)
{
  BundlePtr bundle_ptr = Bundle::create();
  Bundle &bundle = const_cast<Bundle &>(*bundle_ptr);

  // const NodeGeometryCombineBundleItem &item = storage.items[i];
  const bke::bNodeSocketType *geometry_type = bke::node_socket_type_find_static(SOCK_GEOMETRY);
  BLI_assert(geometry_type != nullptr);
  bundle.add_new(ConstraintBundleItems::stretch_constraints, *geometry_type, &stretch_constraints);
  bundle.add_new(ConstraintBundleItems::bending_constraints, *geometry_type, &bending_constraints);
  bundle.add_new(
      ConstraintBundleItems::position_constraints, *geometry_type, &position_constraints);
  bundle.add_new(
      ConstraintBundleItems::rotation_constraints, *geometry_type, &rotation_constraints);
  bundle.add_new(ConstraintBundleItems::contact_constraints, *geometry_type, &contact_constraints);

  return bundle_ptr;
}

static void separate_constraint_bundle(const Bundle &bundle,
                                       GeometrySet &stretch_constraints,
                                       GeometrySet &bending_constraints,
                                       GeometrySet &position_constraints,
                                       GeometrySet &rotation_constraints,
                                       GeometrySet &contact_constraints)
{
  auto lookup_constraints = [&](const SocketInterfaceKey &key) -> GeometrySet {
    const std::optional<Bundle::Item> value = bundle.lookup(key);
    if (!value) {
      return {};
    }
    const bke::bNodeSocketType *geometry_type = bke::node_socket_type_find_static(SOCK_GEOMETRY);
    BLI_assert(geometry_type != nullptr);
    GeometrySet output_geometry;
    if (!implicitly_convert_socket_value(
            *value->type, value->value, *geometry_type, &output_geometry))
    {
      return {};
    }
    return output_geometry;
  };

  stretch_constraints = lookup_constraints(ConstraintBundleItems::stretch_constraints);
  bending_constraints = lookup_constraints(ConstraintBundleItems::bending_constraints);
  position_constraints = lookup_constraints(ConstraintBundleItems::position_constraints);
  rotation_constraints = lookup_constraints(ConstraintBundleItems::rotation_constraints);
  contact_constraints = lookup_constraints(ConstraintBundleItems::contact_constraints);
}

static void zero_init_solver(MutableSpan<ConstraintEvalData> constraint_data)
{
  for (ConstraintEvalData &data : constraint_data) {
    if (!data.geometry) {
      continue;
    }
    if (data.type->init_step) {
      data.type->init_step(*data.geometry);
    }
  }
}

static void warm_start_solver(const ConstraintEvalParams &eval_params,
                              MutableSpan<ConstraintEvalData> constraint_data)
{
  for (ConstraintEvalData &data : constraint_data) {
    if (!data.geometry) {
      continue;
    }
    /* TODO */
    eval_params.error_message_add("Warm starting not yet implemented");
    if (data.type->init_step) {
      data.type->init_step(*data.geometry);
    }
  }
}

static void estimate_velocity(ConstraintEvalParams &params,
                              Array<float3> &orig_velocities,
                              Array<float3> &orig_angular_velocities,
                              ConstraintVariables &vars)
{
  const Span<float3> old_positions = params.old_positions;
  const Span<math::Quaternion> old_rotations = params.old_rotations;
  const Span<float3> positions = vars.positions;
  const Span<math::Quaternion> rotations = vars.rotations;
  MutableSpan<float3> velocities = vars.velocities;
  MutableSpan<float3> angular_velocities = vars.angular_velocities;
  const IndexMask positions_mask = vars.positions.index_range();
  const IndexMask rotations_mask = vars.rotations.index_range();
  const float inv_dt = params.inv_delta_time;

  orig_velocities = vars.velocities;
  orig_angular_velocities = vars.angular_velocities;
  params.orig_velocities = orig_velocities;
  params.orig_angular_velocities = orig_angular_velocities;

  positions_mask.foreach_index(GrainSize(1024), [&](const int index) {
    velocities[index] = inv_dt * (positions[index] - old_positions[index]);
  });
  rotations_mask.foreach_index(GrainSize(1024), [&](const int index) {
    angular_velocities[index] =
        2.0f * inv_dt *
        (math::invert_normalized(old_rotations[index]) * rotations[index]).imaginary_part();
  });
}

static void do_position_constraints_iteration(const ConstraintEvalParams &eval_params,
                                              MutableSpan<ConstraintEvalData> constraint_data,
                                              ConstraintVariables &variables)
{
  IndexMaskMemory memory;

  Array<xpbd_constraints::VariableIndexArrays> indices_by_type(constraint_data.size());
  xpbd_constraints::read_constraint_topology(constraint_data, indices_by_type);

  for (const int constraint_i : constraint_data.index_range()) {
    ConstraintEvalData &data = constraint_data[constraint_i];
    const xpbd_constraints::VariableIndexArrays &indices = indices_by_type[constraint_i];
    if (!data.geometry) {
      continue;
    }

    /* Solve in consistent order by using the sorted index set. */
    for (const IndexMask &group_mask : data.group_masks) {
      apply_gauss_seidel_positions_group(
          eval_params, *data.type, *data.geometry, group_mask, variables, indices, memory);
    }
  }
}

static void do_velocity_constraints_iteration(const ConstraintEvalParams &eval_params,
                                              MutableSpan<ConstraintEvalData> constraint_data,
                                              ConstraintVariables &variables)
{
  IndexMaskMemory memory;

  Array<xpbd_constraints::VariableIndexArrays> indices_by_type(constraint_data.size());
  xpbd_constraints::read_constraint_topology(constraint_data, indices_by_type);

  for (const int constraint_i : constraint_data.index_range()) {
    ConstraintEvalData &data = constraint_data[constraint_i];
    const xpbd_constraints::VariableIndexArrays &indices = indices_by_type[constraint_i];
    if (!data.geometry) {
      continue;
    }

    /* Solve in consistent order by using the sorted index set. */
    for (const IndexMask &group_mask : data.group_masks) {
      apply_gauss_seidel_velocities_group(
          eval_params, *data.type, *data.geometry, group_mask, variables, indices, memory);
    }
  }
}

static void solve_constraints(GeometrySet &hair_geometry, const int iterations)
{
  /* TODO warm start doesn't work properly yet. */
  const bool warm_start = false;

  // Field<float> mass_field = params.extract_input<Field<float>>("Mass");
  // Field<float3> inertia_field = params.extract_input<Field<float3>>("Inertia");
  // Field<float3> old_position_field = params.extract_input<Field<float3>>("Old Position");
  // Field<math::Quaternion> old_rotation_field = params.extract_input<Field<math::Quaternion>>(
  //     "Old Rotation");
  // Field<float3> position_field = params.extract_input<Field<float3>>("Position");
  // Field<math::Quaternion> rotation_field = params.extract_input<Field<math::Quaternion>>(
  //     "Rotation");
  // Field<float3> velocity_field = params.extract_input<Field<float3>>("Velocity");
  // Field<float3> angular_velocity_field = params.extract_input<Field<float3>>("Angular
  // Velocity"); std::optional<std::string> position_output_id =
  //     params.get_output_anonymous_attribute_id_if_needed("Position");
  // std::optional<std::string> rotation_output_id =
  //     params.get_output_anonymous_attribute_id_if_needed("Rotation");
  // std::optional<std::string> velocity_output_id =
  //     params.get_output_anonymous_attribute_id_if_needed("Velocity");
  // std::optional<std::string> angular_velocity_output_id =
  //     params.get_output_anonymous_attribute_id_if_needed("Angular Velocity");

  // GeometrySet colliders_geometry_set = params.extract_input<GeometrySet>("Colliders");
  // Span<float4x4> collider_transforms = colliders_geometry_set.has_instances() ?
  //                                          colliders_geometry_set.get_instances()->transforms()
  //                                          : Span<float4x4>{};

  // ConstraintEvalParams eval_params = extract_eval_params(params);
  // const bool debug_output = (eval_params.debug_recorder != nullptr);

  // Vector<ConstraintEvalData> constraint_data;
  // IndexMaskMemory memory;
  // get_constraint_data(params, debug_output, constraint_data, memory);

  // init_constraints(init_mode, eval_params, constraint_data);

  // static const Array<GeometryComponent::Type> types = {bke::GeometryComponent::Type::Mesh,
  //                                                      bke::GeometryComponent::Type::PointCloud,
  //                                                      bke::GeometryComponent::Type::Curve,
  //                                                      bke::GeometryComponent::Type::GreasePencil};
  // geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
  //   for (const bke::GeometryComponent::Type component_type : types) {
  //     if (geometry_set.has(component_type)) {
  //       bke::GeometryComponent &component =
  //       geometry_set.get_component_for_write(component_type);
  //       std::optional<bke::MutableAttributeAccessor> attributes =
  //       component.attributes_for_write(); if (!attributes) {
  //         continue;
  //       }

  //      if (eval_params.debug_recorder) {
  //        eval_params.debug_recorder->set_geometry(geometry_set, component_type);
  //      }

  //      const int num_points = attributes->domain_size(AttrDomain::Point);
  //      ConstraintVariables vars;
  //      vars.positions.reinitialize(num_points);
  //      vars.rotations.reinitialize(num_points);
  //      vars.velocities.reinitialize(num_points);
  //      vars.angular_velocities.reinitialize(num_points);

  //      const bke::GeometryFieldContext field_context{component, AttrDomain::Point};
  //      fn::FieldEvaluator evaluator{field_context, num_points};
  //      evaluator.add(mass_field);
  //      evaluator.add(inertia_field);
  //      evaluator.add(old_position_field);
  //      evaluator.add(old_rotation_field);
  //      evaluator.add_with_destination(position_field, vars.positions.as_mutable_span());
  //      evaluator.add_with_destination(rotation_field, vars.rotations.as_mutable_span());
  //      evaluator.add_with_destination(velocity_field, vars.velocities.as_mutable_span());
  //      evaluator.add_with_destination(angular_velocity_field,
  //                                     vars.angular_velocities.as_mutable_span());
  //      evaluator.evaluate();
  //      eval_params.masses = evaluator.get_evaluated<float>(0);
  //      eval_params.local_inertia = evaluator.get_evaluated<float3>(1);
  //      eval_params.old_positions = evaluator.get_evaluated<float3>(2);
  //      eval_params.old_rotations = evaluator.get_evaluated<math::Quaternion>(3);

  //      eval_params.collider_transforms = collider_transforms;
  //      /* XXX Transforms of the previous frame are not currently available, these are always the
  //       * same as the current frame. Eventually this will allow transfer of velocity from
  //       animated
  //       * colliders. */
  //      eval_params.old_collider_transforms = eval_params.collider_transforms;

  //      // execute_solver_method_on_geometry(solver_method,
  //      //                                   eval_params,
  //      //                                   constraint_data,
  //      //                                   vars,
  //      //                                   gauss_seidel_iterations,
  //      //                                   jacobi_iterations);
  //      if (eval_params.debug_recorder) {
  //        const std::string label = fmt::format("Initialize Gauss-Seidel, ");
  //        eval_params.debug_recorder->record_step(label, nullptr, -1, {}, variables);
  //      }

  //      for ([[maybe_unused]] const int i : IndexRange(gauss_seidel_iterations)) {
  //        do_gauss_seidel_iteration(
  //            EvaluationTarget::Positions, eval_params, constraint_data, variables);
  //      }

  //      estimate_velocity(eval_params, orig_velocities, orig_angular_velocities, variables);

  //      do_gauss_seidel_iteration(
  //          EvaluationTarget::Velocities, eval_params, constraint_data, variables);

  //      if (position_output_id) {
  //        AttributeWriter<float3> positions_writer = attributes->lookup_or_add_for_write<float3>(
  //            *position_output_id, AttrDomain::Point);
  //        BLI_assert(vars.positions.size() == num_points);
  //        positions_writer.varray.set_all(vars.positions);
  //        positions_writer.finish();
  //      }
  //      if (rotation_output_id) {
  //        AttributeWriter<math::Quaternion> rotations_writer =
  //            attributes->lookup_or_add_for_write<math::Quaternion>(*rotation_output_id,
  //                                                                  AttrDomain::Point);
  //        BLI_assert(vars.rotations.size() == num_points);
  //        rotations_writer.varray.set_all(vars.rotations);
  //        rotations_writer.finish();
  //      }
  //      if (velocity_output_id) {
  //        AttributeWriter<float3> velocities_writer =
  //        attributes->lookup_or_add_for_write<float3>(
  //            *velocity_output_id, AttrDomain::Point);
  //        BLI_assert(vars.velocities.size() == num_points);
  //        velocities_writer.varray.set_all(vars.velocities);
  //        velocities_writer.finish();
  //      }
  //      if (angular_velocity_output_id) {
  //        AttributeWriter<float3> angular_velocities_writer =
  //            attributes->lookup_or_add_for_write<float3>(*angular_velocity_output_id,
  //                                                        AttrDomain::Point);
  //        BLI_assert(vars.angular_velocities.size() == num_points);
  //        angular_velocities_writer.varray.set_all(vars.angular_velocities);
  //        angular_velocities_writer.finish();
  //      }
  //    }
  //  }
  //});

  // params.set_output("Geometry", geometry_set);
  // set_constraint_data_output(params, constraint_data);
  // if (eval_params.debug_recorder) {
  //   params.set_output("Debug Steps", eval_params.debug_recorder->debug_steps());
  // }
}

// static ConstraintEvalParams extract_eval_params(GeoNodeExecParams params)
//{
//   const float delta_time = std::max(params.extract_input<float>("Delta Time"), 0.0f);
//   const float delta_time_squared = delta_time * delta_time;
//   const float inv_delta_time = math::safe_rcp(delta_time);
//   const float inv_delta_time_squared = math::safe_rcp(delta_time_squared);
//   const bool debug_check = params.extract_input<bool>("Debug Checks");
//   const bool use_debug_steps = params.output_is_required("Debug Steps");
//
//   ConstraintEvalParams eval_params;
//   eval_params.delta_time = delta_time;
//   eval_params.delta_time_squared = delta_time_squared;
//   eval_params.inv_delta_time = inv_delta_time;
//   eval_params.inv_delta_time_squared = inv_delta_time_squared;
//   eval_params.error_message_add = [params](const StringRef message) {
//     params.error_message_add(NodeWarningType::Warning, message);
//   };
//   eval_params.debug_check = debug_check;
//   if (use_debug_steps) {
//     eval_params.debug_recorder = std::make_unique<xpbd_constraints::DebugRecorder>(
//         params.extract_input<GeometrySet>("Debug Steps"));
//   }
//
//   return eval_params;
// }

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet hair_geometry = params.extract_input<GeometrySet>("Hair");
  const int constraint_iterations = std::max(params.extract_input<int>("Constraint Iterations"),
                                             0);

  params.set_output("Hair", std::move(hair_geometry));
}

static void node_rna(StructRNA *srna)
{
  UNUSED_VARS(srna);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeHairSimulation");
  ntype.ui_name = "Hair Simulation";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_hair_simulation_cc
