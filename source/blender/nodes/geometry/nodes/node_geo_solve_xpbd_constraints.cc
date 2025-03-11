/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"

#include "BKE_attribute.hh"
#include "BKE_geometry_set.hh"
#include "BKE_instances.hh"

#include "NOD_xpbd_constraints.hh"

#include "node_geometry_util.hh"

#include <fmt/format.h>

#include <Eigen/Sparse>

namespace blender::nodes::xpbd_constraints {

static void append_instance_item(GeometrySet &container,
                                 GeometrySet item,
                                 const StringRef name,
                                 const float4x4 &transform = float4x4::identity())
{
  if (!container.has_instances()) {
    container.replace_instances(new bke::Instances);
  }
  bke::Instances &instances =
      *container.get_component_for_write<InstancesComponent>().get_for_write();

  item.name = name;
  const int handle = instances.add_new_reference(std::move(item));
  instances.add_instance(handle, transform);
}

DebugRecorder::DebugRecorder(const bke::GeometrySet &debug_steps)
    : component_type_(GeometryComponent::Type::PointCloud), debug_steps_(debug_steps)
{
}

void DebugRecorder::set_geometry(const GeometrySet &geometry_set,
                                 GeometryComponent::Type component_type)
{
  geometry_set_ = geometry_set;
  component_type_ = component_type;
}

void DebugRecorder::record_step(const StringRef label,
                                GeometrySet *constraints,
                                const int constraint_type_code,
                                const IndexMask &group_mask,
                                const ConstraintVariables &variables)
{
  GeometrySet step_geometry;

  {
    GeometrySet updated_geometry = geometry_set_;
    GeometryComponent &component = updated_geometry.get_component_for_write(component_type_);
    MutableAttributeAccessor attributes = *component.attributes_for_write();
    if (!variables.positions.is_empty()) {
      AttributeWriter<float3> positions_writer = attributes.lookup_or_add_for_write<float3>(
          "position", AttrDomain::Point);
      positions_writer.varray.set_all(variables.positions);
      positions_writer.finish();
    }
    if (!variables.rotations.is_empty()) {
      AttributeWriter<math::Quaternion> rotations_writer =
          attributes.lookup_or_add_for_write<math::Quaternion>("rotation", AttrDomain::Point);
      rotations_writer.varray.set_all(variables.rotations);
      rotations_writer.finish();
    }
    if (!variables.velocities.is_empty()) {
      AttributeWriter<float3> velocities_writer = attributes.lookup_or_add_for_write<float3>(
          "velocity", AttrDomain::Point);
      velocities_writer.varray.set_all(variables.velocities);
      velocities_writer.finish();
    }
    if (!variables.angular_velocities.is_empty()) {
      AttributeWriter<float3> angular_velocities_writer =
          attributes.lookup_or_add_for_write<float3>("angular_velocity", AttrDomain::Point);
      angular_velocities_writer.varray.set_all(variables.angular_velocities);
      angular_velocities_writer.finish();
    }

    append_instance_item(step_geometry, updated_geometry, "Geometry");
  }

  if (constraints) {
    PointCloudComponent &constraint_component =
        constraints->get_component_for_write<PointCloudComponent>();
    MutableAttributeAccessor attributes = *constraint_component.attributes_for_write();
    attributes.remove("group_active");
    SpanAttributeWriter<bool> group_active_writer = attributes.lookup_or_add_for_write_span<bool>(
        "group_active", AttrDomain::Point);
    group_mask.foreach_index(GrainSize(4096),
                             [&](const int index) { group_active_writer.span[index] = true; });
    group_active_writer.finish();

    append_instance_item(step_geometry, *constraints, "Constraints");
  }

  MutableAttributeAccessor instance_attributes = step_geometry
                                                     .get_component_for_write<InstancesComponent>()
                                                     .get_for_write()
                                                     ->attributes_for_write();
  AttributeWriter<int> type_code_writer = instance_attributes.lookup_or_add_for_write<int>(
      "type_code", AttrDomain::Instance);
  type_code_writer.varray.set(0, -1);
  if (constraints) {
    type_code_writer.varray.set(1, constraint_type_code);
  }
  type_code_writer.finish();

  append_instance_item(debug_steps_, step_geometry, label);
}

const bke::GeometrySet &DebugRecorder::debug_steps() const
{
  return debug_steps_;
}

}  // namespace blender::nodes::xpbd_constraints

namespace blender::nodes::node_geo_solve_xpbd_constraints_cc {

using xpbd_constraints::ConstraintEvalParams;
using xpbd_constraints::ConstraintTypeInfo;
using xpbd_constraints::ConstraintVariables;

constexpr float default_fps = 1.0f / 25.0f;

enum class EvaluationTarget {
  Positions,
  Velocities,
};

enum class SolverMethod {
  Global,
  GaussSeidel,
  Jacobi,
};

enum class ConstraintInit {
  /* Zero initialize lambda and compute change from current positions. */
  ZeroInit,
  /* Warm start: Use previous lambda as initial step. */
  WarmStart,
};

static void node_declare_positions(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_input<decl::Float>("Delta Time").default_value(default_fps).min(0.0f).hide_value();
  b.add_input<decl::Bool>("Use Global Solve").default_value(true);
  b.add_input<decl::Int>("Gauss-Seidel Iterations").default_value(1).min(0);
  b.add_input<decl::Int>("Jacobi Iterations").default_value(0).min(0);
  b.add_input<decl::Bool>("Warm Start")
      .default_value(false)
      .description("Use previous lambda value when initializing instead of starting from zero");

  const int geometry_in = b.add_input<decl::Geometry>("Geometry").index();
  const int geometry_out = b.add_output<decl::Geometry>("Geometry").align_with_previous().index();

  b.add_input<decl::Vector>("Position")
      .implicit_field_on(implicit_field_inputs::position, {geometry_in});
  b.add_output<decl::Vector>("Position").field_on({geometry_out}).align_with_previous();
  b.add_input<decl::Rotation>("Rotation").field_on({geometry_in}).hide_value();
  b.add_output<decl::Rotation>("Rotation").field_on({geometry_out}).align_with_previous();
  b.add_input<decl::Vector>("Old Position").field_on({geometry_in}).hide_value();
  b.add_input<decl::Rotation>("Old Rotation").field_on({geometry_in}).hide_value();

  b.add_input<decl::Float>("Position Weight")
      .default_value(1.0f)
      .field_on({geometry_in})
      .description("Influence weight of constraints for each point (inverse mass)");
  b.add_input<decl::Float>("Rotation Weight")
      .default_value(1.0f)
      .field_on({geometry_in})
      .description("Influence weight of constraints for each point (inverse moment of inertia)");

  for (const ConstraintTypeInfo &info : xpbd_constraints::get_constraint_info(false)) {
    b.add_input<decl::Geometry>(info.ui_name)
        .supported_type(GeometryComponent::Type::PointCloud)
        .description(info.ui_description);
    b.add_output<decl::Geometry>(info.ui_name)
        .description(info.ui_description)
        .align_with_previous();
  }

  b.add_input<decl::Geometry>("Colliders")
      .only_instances()
      .description("Instances of colliders to evaluate contact transforms");

  b.add_input<decl::Bool>("Debug Checks")
      .default_value(false)
      .description("Perform checks on input data, which can impact performance");
  b.add_input<decl::Geometry>("Debug Steps")
      .description("Complete constraint and geometry information for each solver iteration");
  b.add_output<decl::Geometry>("Debug Steps")
      .description("Complete constraint and geometry information for each solver iteration")
      .align_with_previous();
}

static void node_declare_velocities(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_input<decl::Float>("Delta Time").default_value(default_fps).min(0.0f).hide_value();
  b.add_input<decl::Bool>("Use Global Solve").default_value(true);
  b.add_input<decl::Int>("Gauss-Seidel Iterations").default_value(1).min(0);
  b.add_input<decl::Int>("Jacobi Iterations").default_value(0).min(0);
  b.add_input<decl::Bool>("Warm Start")
      .default_value(true)
      .description("Use previous lambda value when initializing instead of starting from zero");

  const int geometry_in = b.add_input<decl::Geometry>("Geometry").index();
  const int geometry_out = b.add_output<decl::Geometry>("Geometry").align_with_previous().index();

  b.add_input<decl::Vector>("Position")
      .implicit_field_on(implicit_field_inputs::position, {geometry_in});
  b.add_input<decl::Rotation>("Rotation").field_on({geometry_in}).hide_value();
  b.add_input<decl::Vector>("Velocity").field_on({geometry_in}).hide_value();
  b.add_output<decl::Vector>("Velocity").field_on({geometry_out}).align_with_previous();
  b.add_input<decl::Vector>("Angular Velocity").field_on({geometry_in}).hide_value();
  b.add_output<decl::Vector>("Angular Velocity").field_on({geometry_out}).align_with_previous();
  b.add_input<decl::Vector>("Original Velocity").field_on({geometry_in}).hide_value();
  b.add_input<decl::Vector>("Original Angular Velocity").field_on({geometry_in}).hide_value();

  b.add_input<decl::Float>("Position Weight")
      .default_value(1.0f)
      .field_on({geometry_in})
      .description("Influence weight of constraints for each point (inverse mass)");
  b.add_input<decl::Float>("Rotation Weight")
      .default_value(1.0f)
      .field_on({geometry_in})
      .description("Influence weight of constraints for each point (inverse moment of inertia)");

  for (const ConstraintTypeInfo &info : xpbd_constraints::get_constraint_info(false)) {
    b.add_input<decl::Geometry>(info.ui_name)
        .supported_type(GeometryComponent::Type::PointCloud)
        .description(info.ui_description);
    b.add_output<decl::Geometry>(info.ui_name)
        .description(info.ui_description)
        .align_with_previous();
  }

  b.add_input<decl::Geometry>("Colliders")
      .only_instances()
      .description("Instances of colliders to evaluate contact transforms");

  b.add_input<decl::Bool>("Debug Checks")
      .default_value(false)
      .description("Perform checks on input data, which can impact performance");
  b.add_input<decl::Geometry>("Debug Steps")
      .description("Complete constraint and geometry information for each solver iteration");
  b.add_output<decl::Geometry>("Debug Steps")
      .description("Complete constraint and geometry information for each solver iteration")
      .align_with_previous();
}

static void apply_gauss_seidel_positions_group(const ConstraintEvalParams &eval_params,
                                               const ConstraintTypeInfo &constraint_info,
                                               GeometrySet &constraints,
                                               const IndexMask &group_mask,
                                               ConstraintVariables &variables,
                                               IndexMaskMemory &memory)
{
  constexpr bool linearized_quaternion = true;

  if (!constraint_info.evaluate_position) {
    return;
  }

  VArray<bool> active;
  Vector<VArray<float3>> delta_positions;
  Vector<VArray<float4>> delta_rotations;
  constraint_info.evaluate_position(
      eval_params, variables, group_mask, constraints, active, delta_positions, delta_rotations);
  IndexMask group_and_active_mask = IndexMask::from_bools(group_mask, active, memory);

  Vector<VArray<int>> mapping = constraint_info.get_mapping(constraints);
  BLI_assert(delta_positions.size() == mapping.size());
  BLI_assert(delta_rotations.size() == mapping.size());
  /* TODO optimize: constraints should have at most 4 point maps and associated deltas.
   * It should be possible to unroll the mapping loop and use only a single group mask iteration.
   */
  const IndexRange points_range = variables.positions.index_range();
  for (const int map_i : mapping.index_range()) {
    const VArraySpan<int> map = mapping[map_i];
    /* Gauss-Seidel solver has a unique source for each point and can just write to it. */
    if (delta_positions[map_i]) {
      const VArraySpan<float3> delta_pos = delta_positions[map_i];
      group_and_active_mask.foreach_index(GrainSize(4096), [&](const int index) {
        const int point = map[index];
        if (points_range.contains(point)) {
          xpbd_constraints::apply_position_impulse(delta_pos[index], variables.positions[point]);
        }
      });
    }
    if (delta_rotations[map_i]) {
      const VArraySpan<float4> delta_rot = delta_rotations[map_i];
      group_and_active_mask.foreach_index(GrainSize(4096), [&](const int index) {
        const int point = map[index];
        if (points_range.contains(point)) {
          xpbd_constraints::apply_rotation_impulse<linearized_quaternion>(
              delta_rot[index], variables.rotations[point]);
        }
      });
    }
  }

  if (eval_params.debug_recorder) {
    const std::string label = fmt::format("Evaluate: {}", constraint_info.ui_name);
    eval_params.debug_recorder->record_step(
        label, &constraints, constraint_info.type_code, group_and_active_mask, variables);
  }
}

static void apply_gauss_seidel_velocities_group(const ConstraintEvalParams &eval_params,
                                                const ConstraintTypeInfo &constraint_info,
                                                GeometrySet &constraints,
                                                const IndexMask &group_mask,
                                                ConstraintVariables &variables,
                                                IndexMaskMemory &memory)
{
  if (!constraint_info.evaluate_velocity) {
    return;
  }

  VArray<bool> active;
  Vector<VArray<float3>> delta_velocities;
  Vector<VArray<float3>> delta_angular_velocities;
  constraint_info.evaluate_velocity(eval_params,
                                    variables,
                                    group_mask,
                                    constraints,
                                    active,
                                    delta_velocities,
                                    delta_angular_velocities);
  IndexMask group_and_active_mask = IndexMask::from_bools(group_mask, active, memory);

  Vector<VArray<int>> mapping = constraint_info.get_mapping(constraints);
  BLI_assert(mapping.size() == delta_velocities.size());
  BLI_assert(mapping.size() == delta_angular_velocities.size());
  /* TODO optimize: constraints should have at most 4 point maps and associated deltas.
   * It should be possible to unroll the mapping loop and use only a single group mask iteration.
   */
  const IndexRange points_range = variables.positions.index_range();
  for (const int map_i : mapping.index_range()) {
    const VArraySpan<int> map = mapping[map_i];
    /* Gauss-Seidel solver has a unique source for each point and can just write to it. */
    if (delta_velocities[map_i]) {
      const VArraySpan<float3> delta_vel = delta_velocities[map_i];
      group_and_active_mask.foreach_index(GrainSize(4096), [&](const int index) {
        const int point = map[index];
        if (points_range.contains(point)) {
          xpbd_constraints::apply_velocity_impulse(delta_vel[index], variables.velocities[point]);
        }
      });
    }
    if (delta_angular_velocities[map_i]) {
      const VArraySpan<float3> delta_angvel = delta_angular_velocities[map_i];
      group_and_active_mask.foreach_index(GrainSize(4096), [&](const int index) {
        const int point = map[index];
        if (points_range.contains(point)) {
          xpbd_constraints::apply_angular_velocity_impulse(delta_angvel[index],
                                                           variables.angular_velocities[point]);
        }
      });
    }
  }

  if (eval_params.debug_recorder) {
    const std::string label = fmt::format("Evaluate: {}", constraint_info.ui_name);
    eval_params.debug_recorder->record_step(
        label, &constraints, constraint_info.type_code, group_and_active_mask, variables);
  }
}

static void add_jacobi_position_deltas(const ConstraintEvalParams &eval_params,
                                       const ConstraintTypeInfo &constraint_info,
                                       GeometrySet &constraints,
                                       const IndexMask &constraints_mask,
                                       ConstraintVariables &variables,
                                       MutableSpan<float3> point_delta_positions,
                                       MutableSpan<float4> point_delta_rotations,
                                       MutableSpan<int> point_weights,
                                       IndexMaskMemory &memory)
{
  if (!constraint_info.evaluate_position) {
    return;
  }

  VArray<bool> active;
  Vector<VArray<float3>> delta_positions;
  Vector<VArray<float4>> delta_rotations;
  constraint_info.evaluate_position(eval_params,
                                    variables,
                                    constraints_mask,
                                    constraints,
                                    active,
                                    delta_positions,
                                    delta_rotations);
  IndexMask active_mask = IndexMask::from_bools(constraints_mask, active, memory);

  Vector<VArray<int>> mapping = constraint_info.get_mapping(constraints);
  BLI_assert(delta_positions.size() == mapping.size());
  BLI_assert(delta_rotations.size() == mapping.size());
  /* TODO optimize: constraints should have at most 4 point maps and associated deltas.
   * It should be possible to unroll the mapping loop and use only a single group mask iteration.
   */
  const IndexRange points_range = variables.positions.index_range();
  for (const int map_i : mapping.index_range()) {
    const VArraySpan<int> map = mapping[map_i];
    active_mask.foreach_index(GrainSize(4096), [&](const int index) {
      const int point = map[index];
      if (points_range.contains(point)) {
        ++point_weights[point];
      }
    });
    if (delta_positions[map_i]) {
      const VArraySpan<float3> delta_pos = delta_positions[map_i];
      active_mask.foreach_index(GrainSize(4096), [&](const int index) {
        const int point = map[index];
        if (points_range.contains(point)) {
          point_delta_positions[point] += delta_pos[index];
        }
      });
    }
    if (delta_rotations[map_i]) {
      const VArraySpan<float4> delta_rot = delta_rotations[map_i];
      active_mask.foreach_index(GrainSize(4096), [&](const int index) {
        const int point = map[index];
        if (points_range.contains(point)) {
          point_delta_rotations[point] += delta_rot[index];
        }
      });
    }
  }

  if (eval_params.debug_recorder) {
    const std::string label = fmt::format("Evaluate: {}", constraint_info.ui_name);
    eval_params.debug_recorder->record_step(
        label, &constraints, constraint_info.type_code, active_mask, variables);
  }
}

static void add_jacobi_velocity_deltas(const ConstraintEvalParams &eval_params,
                                       const ConstraintTypeInfo &constraint_info,
                                       GeometrySet &constraints,
                                       const IndexMask &constraints_mask,
                                       ConstraintVariables &variables,
                                       MutableSpan<float3> point_delta_velocities,
                                       MutableSpan<float3> point_delta_angular_velocities,
                                       MutableSpan<int> point_weights,
                                       IndexMaskMemory &memory)
{
  if (!constraint_info.evaluate_velocity) {
    return;
  }

  VArray<bool> active;
  Vector<VArray<float3>> delta_velocities;
  Vector<VArray<float3>> delta_angular_velocities;
  constraint_info.evaluate_velocity(eval_params,
                                    variables,
                                    constraints_mask,
                                    constraints,
                                    active,
                                    delta_velocities,
                                    delta_angular_velocities);
  IndexMask active_mask = IndexMask::from_bools(constraints_mask, active, memory);

  Vector<VArray<int>> mapping = constraint_info.get_mapping(constraints);
  BLI_assert(delta_velocities.size() == mapping.size());
  BLI_assert(delta_angular_velocities.size() == mapping.size());
  /* TODO optimize: constraints should have at most 4 point maps and associated deltas.
   * It should be possible to unroll the mapping loop and use only a single group mask iteration.
   */
  const IndexRange points_range = variables.positions.index_range();
  for (const int map_i : mapping.index_range()) {
    const VArraySpan<int> map = mapping[map_i];
    active_mask.foreach_index(GrainSize(4096), [&](const int index) {
      const int point = map[index];
      if (points_range.contains(point)) {
        ++point_weights[point];
      }
    });
    if (delta_velocities[map_i]) {
      const VArraySpan<float3> delta_vel = delta_velocities[map_i];
      active_mask.foreach_index(GrainSize(4096), [&](const int index) {
        const int point = map[index];
        if (points_range.contains(point)) {
          point_delta_velocities[point] += delta_vel[index];
        }
      });
    }
    if (delta_angular_velocities[map_i]) {
      const VArraySpan<float3> delta_angvel = delta_angular_velocities[map_i];
      active_mask.foreach_index(GrainSize(4096), [&](const int index) {
        const int point = map[index];
        if (points_range.contains(point)) {
          point_delta_angular_velocities[point] += delta_angvel[index];
        }
      });
    }
  }

  if (eval_params.debug_recorder) {
    const std::string label = fmt::format("Evaluate: {}", constraint_info.ui_name);
    eval_params.debug_recorder->record_step(
        label, &constraints, constraint_info.type_code, active_mask, variables);
  }
}

/* A closure and associated solver group masks. */
struct ConstraintEvalData {
  const ConstraintTypeInfo *type;
  std::optional<GeometrySet> geometry;
  IndexMask constraints;
  Vector<IndexMask> group_masks;
};

static void do_global_solve(const EvaluationTarget target,
                            const ConstraintEvalParams &eval_params,
                            MutableSpan<ConstraintEvalData> constraint_data,
                            ConstraintVariables &variables)
{
  const int num_positions = variables.positions.size();
  const int num_rotations = variables.rotations.size();
  int tot_components = 0;
  // IndexMaskMemory memory;

  /* Count number of constraint values and dependent variables,
   * which determines the size of the global solver matrix. */

  /* Number of rows/columns of the sparse solver matrix. */
  int num_columns = 0;
  /* Total number of non-zero entries in the sparse solver matrix. */
  int num_non_zeroes = 0;
  /* Upper-left sub-matrix has 3 rows/columns for each position
   * and 4 rows/columns for each rotation. */
  num_columns += num_positions * 3 + num_rotations * 4;
  /* Each position adds 3 mass entries on the diagonal.
   * Each rotation adds 3x3 moment-of-inertia block-diagonal entries. */
  num_non_zeroes += num_positions * 3 + num_rotations * 9;
  for (ConstraintEvalData &data : constraint_data) {
    if (!data.type->linear_solve_size) {
      continue;
    }
    const int num_constraints = data.constraints.size();
    int num_components, num_position_vars, num_rotation_vars;
    data.type->linear_solve_size(num_components, num_position_vars, num_rotation_vars);
    tot_components += num_components;

    /* Lower-right sub-matrix has a row/column for each constraint component. */
    num_columns += num_constraints * num_components;
    /* Each component adds a compliance entry and derivatives for each variable. */
    const int entries_per_component = 1 + num_position_vars * 3 + num_rotation_vars * 4;
    num_non_zeroes += num_constraints * num_components * entries_per_component;
  }
  /* Matrix is square. */
  const int num_rows = num_columns;

  /* LHS matrix describing equations of motion and constraint impulses. */
  Eigen::SparseMatrix<float> H(num_rows, num_columns);
  H.makeCompressed();
  H.reserve(num_non_zeroes);
  /* Should be defined as column-major storage. */
  static_assert(!H.IsRowMajor);

  /* Determine size of per-column arrays. */
  {
    const IndexRange positions_range = {0, num_positions};
    const IndexRange rotations_range = positions_range.after(num_rotations);
    const IndexRange components_range = rotations_range.after(tot_components);
    MutableSpan<int> column_sizes = {H.outerIndexPtr(), num_columns + 1};

    /* Diagonal entries for point masses. */
    for (const int i :) {
      column_sizes[i] += 1;
    }

    for (ConstraintEvalData &data : constraint_data) {
      if (!data.type->linear_solve_size) {
        continue;
      }

      const int num_constraints = data.constraints.size();
      int num_components, num_position_vars, num_rotation_vars;
      data.type->linear_solve_size(num_components, num_position_vars, num_rotation_vars);
    }
    column_sizes.last() = 0;

    offset_indices::accumulate_counts_to_offsets(column_sizes);
  }

  const OffsetIndices<int> column_offsets = {Span<int>(H.outerIndexPtr(), num_columns + 1)};
  /* Redundant for compressed matrix, non-zeros per column is the same as index range derived
   * from offsets. */
  // const Span<int> column_nonzeros = {H.innerNonZeroPtr(), num_columns};

  {
    MutableSpan<int> row_indices = {H.innerIndexPtr(), num_non_zeroes};
    MutableSpan<float> values = {H.valuePtr(), num_non_zeroes};
    for (ConstraintEvalData &data : constraint_data) {
      if (!data.geometry) {
        continue;
      }

      // /* Solve in consistent order by using the sorted index set. */
      // for (const IndexMask &group_mask : data.group_masks) {
      //   switch (target) {
      //     case EvaluationTarget::Positions: {
      //       apply_gauss_seidel_positions_group(
      //           eval_params, *data.type, *data.geometry, group_mask, variables, memory);
      //       break;
      //     }
      //     case EvaluationTarget::Velocities: {
      //       apply_gauss_seidel_velocities_group(
      //           eval_params, *data.type, *data.geometry, group_mask, variables, memory);
      //       break;
      //     }
      //   }
      // }
    }
  }
}

static void do_gauss_seidel_step(const EvaluationTarget target,
                                 const ConstraintEvalParams &eval_params,
                                 MutableSpan<ConstraintEvalData> constraint_data,
                                 ConstraintVariables &variables)
{
  IndexMaskMemory memory;

  for (ConstraintEvalData &data : constraint_data) {
    if (!data.geometry) {
      continue;
    }

    /* Solve in consistent order by using the sorted index set. */
    for (const IndexMask &group_mask : data.group_masks) {
      switch (target) {
        case EvaluationTarget::Positions: {
          apply_gauss_seidel_positions_group(
              eval_params, *data.type, *data.geometry, group_mask, variables, memory);
          break;
        }
        case EvaluationTarget::Velocities: {
          apply_gauss_seidel_velocities_group(
              eval_params, *data.type, *data.geometry, group_mask, variables, memory);
          break;
        }
      }
    }
  }
}

static void do_jacobi_step(const EvaluationTarget target,
                           const ConstraintEvalParams &eval_params,
                           MutableSpan<ConstraintEvalData> constraint_data,
                           ConstraintVariables &variables)
{
  constexpr bool linearized_quaternion = true;

  IndexMaskMemory memory;

  switch (target) {
    case EvaluationTarget::Positions: {
      Array<int> point_weights(variables.positions.size(), 0);
      Array<float3> point_delta_positions(variables.positions.size(), float3(0.0f));
      Array<float4> point_delta_rotations(variables.positions.size(), float4(0.0f));
      for (ConstraintEvalData &data : constraint_data) {
        if (data.geometry) {
          add_jacobi_position_deltas(eval_params,
                                     *data.type,
                                     *data.geometry,
                                     data.constraints,
                                     variables,
                                     point_delta_positions,
                                     point_delta_rotations,
                                     point_weights,
                                     memory);
        }
      }

      threading::parallel_for(point_weights.index_range(), 4096, [&](const IndexRange range) {
        for (const int point : range) {
          const int weight = point_weights[point];
          if (weight > 0) {
            const float norm = 1.0f / float(weight);
            const float3 delta_pos = point_delta_positions[point] * norm;
            const float4 delta_rot = point_delta_rotations[point] * norm;
            xpbd_constraints::apply_position_impulse(delta_pos, variables.positions[point]);
            xpbd_constraints::apply_rotation_impulse<linearized_quaternion>(
                delta_rot, variables.rotations[point]);
          }
        }
      });
      break;
    }
    case EvaluationTarget::Velocities: {
      Array<int> point_weights(variables.positions.size(), 0);
      Array<float3> point_delta_velocities(variables.positions.size(), float3(0.0f));
      Array<float3> point_delta_angular_velocities(variables.positions.size(), float3(0.0f));
      for (ConstraintEvalData &data : constraint_data) {
        if (data.geometry) {
          add_jacobi_velocity_deltas(eval_params,
                                     *data.type,
                                     *data.geometry,
                                     data.constraints,
                                     variables,
                                     point_delta_velocities,
                                     point_delta_angular_velocities,
                                     point_weights,
                                     memory);
        }
      }

      threading::parallel_for(point_weights.index_range(), 4096, [&](const IndexRange range) {
        for (const int point : range) {
          const int weight = point_weights[point];
          if (weight > 0) {
            const float norm = 1.0f / float(weight);
            const float3 delta_vel = point_delta_velocities[point] * norm;
            const float3 delta_angvel = point_delta_angular_velocities[point] * norm;
            xpbd_constraints::apply_velocity_impulse(delta_vel, variables.velocities[point]);
            xpbd_constraints::apply_angular_velocity_impulse(delta_angvel,
                                                             variables.angular_velocities[point]);
          }
        }
      });
      break;
    }
  }
}

static void zero_init_solver(const EvaluationTarget target,
                             MutableSpan<ConstraintEvalData> constraint_data)
{
  for (ConstraintEvalData &data : constraint_data) {
    if (!data.geometry) {
      continue;
    }
    switch (target) {
      case EvaluationTarget::Positions:
        if (data.type->init_position_step) {
          data.type->init_position_step(*data.geometry);
        }
        break;
      case EvaluationTarget::Velocities:
        if (data.type->init_velocity_step) {
          data.type->init_velocity_step(*data.geometry);
        }
        break;
    }
  }
}

static void warm_start_solver(EvaluationTarget target,
                              const ConstraintEvalParams &eval_params,
                              MutableSpan<ConstraintEvalData> constraint_data)
{
  for (ConstraintEvalData &data : constraint_data) {
    if (!data.geometry) {
      continue;
    }
    /* TODO */
    eval_params.error_message_add("Warm starting not yet implemented");
    switch (target) {
      case EvaluationTarget::Positions:
        if (data.type->init_position_step) {
          data.type->init_position_step(*data.geometry);
        }
        break;
      case EvaluationTarget::Velocities:
        if (data.type->init_velocity_step) {
          data.type->init_velocity_step(*data.geometry);
        }
        break;
    }
  }
}

static void do_solver_iterations(const SolverMethod method,
                                 const int iterations,
                                 const EvaluationTarget target,
                                 const ConstraintInit init_mode,
                                 const ConstraintEvalParams &eval_params,
                                 MutableSpan<ConstraintEvalData> constraint_data,
                                 ConstraintVariables &variables)
{
  switch (init_mode) {
    case ConstraintInit::ZeroInit:
      zero_init_solver(target, constraint_data);
      break;
    case ConstraintInit::WarmStart:
      warm_start_solver(target, eval_params, constraint_data);
      break;
  }

  std::string label;
  switch (target) {
    case EvaluationTarget::Positions:
      label = fmt::format("Init position target, ");
      break;
    case EvaluationTarget::Velocities:
      label = fmt::format("Init velocity target, ");
      break;
  }
  switch (method) {
    case SolverMethod::Global:
      label = fmt::format("{}, Global steps", label);
      break;
    case SolverMethod::GaussSeidel:
      label = fmt::format("{}, Gauss-Seidel steps", label);
      break;
    case SolverMethod::Jacobi:
      label = fmt::format("{}, Jacobi steps", label);
      break;
  }
  if (eval_params.debug_recorder) {
    eval_params.debug_recorder->record_step(label, nullptr, -1, {}, variables);
  }

  for ([[maybe_unused]] const int i : IndexRange(iterations)) {
    switch (method) {
      case SolverMethod::Global:
        do_global_solve(target, eval_params, constraint_data, variables);
        break;
      case SolverMethod::GaussSeidel:
        do_gauss_seidel_step(target, eval_params, constraint_data, variables);
        break;
      case SolverMethod::Jacobi:
        do_jacobi_step(target, eval_params, constraint_data, variables);
        break;
    }
  }
}

static ConstraintEvalParams extract_eval_params(GeoNodeExecParams params)
{
  const float delta_time = std::max(params.extract_input<float>("Delta Time"), 0.0f);
  const float delta_time_squared = delta_time * delta_time;
  const float inv_delta_time = math::safe_rcp(delta_time);
  const float inv_delta_time_squared = math::safe_rcp(delta_time_squared);
  const bool debug_check = params.extract_input<bool>("Debug Checks");
  const bool use_debug_steps = params.output_is_required("Debug Steps");

  ConstraintEvalParams eval_params;
  eval_params.delta_time = delta_time;
  eval_params.delta_time_squared = delta_time_squared;
  eval_params.inv_delta_time = inv_delta_time;
  eval_params.inv_delta_time_squared = inv_delta_time_squared;
  eval_params.error_message_add = [params](const StringRef message) {
    params.error_message_add(geo_eval_log::NodeWarningType::Warning, message);
  };
  eval_params.debug_check = debug_check;
  if (use_debug_steps) {
    eval_params.debug_recorder = std::make_unique<xpbd_constraints::DebugRecorder>(
        params.extract_input<GeometrySet>("Debug Steps"));
  }

  return eval_params;
}

static Vector<IndexMask> build_group_masks(const IndexMask &constraints,
                                           const VArray<int> &solver_groups,
                                           IndexMaskMemory &memory)
{
  if (solver_groups.is_empty()) {
    return {};
  }

  VectorSet<int> unique_group_ids;
  Vector<IndexMask> group_index_masks = IndexMask::from_group_ids(
      constraints, solver_groups, memory, unique_group_ids);

  /* Sort group indices to ensure solver groups are executed in consistent order.
   * IndexMask::from_group_ids creates masks in the order they appear in the data:
   * whichever element comes first creates a mask and index, regardless of the actual group ID
   * values and their relative ordering.
   */
  Array<int> sorted_group_indices(unique_group_ids.size());
  array_utils::fill_index_range(sorted_group_indices.as_mutable_span());
  std::sort(sorted_group_indices.begin(),
            sorted_group_indices.end(),
            [&](const int index_a, const int index_b) {
              const int group_id_a = unique_group_ids[index_a];
              const int group_id_b = unique_group_ids[index_b];
              return group_id_a < group_id_b;
            });

  Vector<IndexMask> group_masks;
  group_masks.resize(sorted_group_indices.size());
  for (const int i : sorted_group_indices.index_range()) {
    /* Group ID is not really relevant at this point. */
    /* const int group_id = unique_group_ids[i_group]; */

    group_masks[i] = std::move(group_index_masks[sorted_group_indices[i]]);
  }
  return group_masks;
}

static void get_constraint_data(GeoNodeExecParams params,
                                const bool debug_output,
                                Vector<ConstraintEvalData> &constraint_data,
                                IndexMaskMemory &memory)
{
  const Span<ConstraintTypeInfo> constraint_infos = xpbd_constraints::get_constraint_info_ordered(
      debug_output);
  constraint_data.reinitialize(constraint_infos.size());
  for (const int i : constraint_infos.index_range()) {
    const ConstraintTypeInfo &info = constraint_infos[i];
    constraint_data[i].type = &info;

    GeometrySet geometry_set = params.extract_input<GeometrySet>(info.ui_name);
    if (geometry_set.has_component<PointCloudComponent>()) {
      const AttributeAccessor attributes =
          *geometry_set.get_component<PointCloudComponent>()->attributes();
      const VArray<int> solver_groups = *attributes.lookup_or_default<int>(
          "solver_group", AttrDomain::Point, 0);

      IndexMask constraints_mask = IndexRange(attributes.domain_size(AttrDomain::Point));
      Vector<IndexMask> group_masks = build_group_masks(
          constraints_mask, std::move(solver_groups), memory);
      constraint_data[i].geometry = std::move(geometry_set);
      constraint_data[i].constraints = std::move(constraints_mask);
      constraint_data[i].group_masks = std::move(group_masks);
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
  for (const ConstraintEvalData &data : constraint_data) {
    if (data.geometry) {
      params.set_output(data.type->ui_name, *data.geometry);
    }
    else {
      params.set_output(data.type->ui_name, GeometrySet{});
    }
  }
}

static void node_geo_exec_positions(GeoNodeExecParams params)
{
  ConstraintInit init_mode = params.extract_input<bool>("Warm Start") ? ConstraintInit::WarmStart :
                                                                        ConstraintInit::ZeroInit;
  const int global_iterations = (params.extract_input<bool>("Use Global Solve") ? 1 : 0);
  const int gauss_seidel_iterations = std::max(
      params.extract_input<int>("Gauss-Seidel Iterations"), 0);
  const int jacobi_iterations = std::max(params.extract_input<int>("Jacobi Iterations"), 0);
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");
  Field<float3> position_field = params.extract_input<Field<float3>>("Position");
  Field<math::Quaternion> rotation_field = params.extract_input<Field<math::Quaternion>>(
      "Rotation");
  Field<float3> old_position_field = params.extract_input<Field<float3>>("Old Position");
  Field<math::Quaternion> old_rotation_field = params.extract_input<Field<math::Quaternion>>(
      "Old Rotation");
  std::optional<std::string> position_output_id =
      params.get_output_anonymous_attribute_id_if_needed("Position");
  std::optional<std::string> rotation_output_id =
      params.get_output_anonymous_attribute_id_if_needed("Rotation");
  Field<float> position_weight_field = params.extract_input<Field<float>>("Position Weight");
  Field<float> rotation_weight_field = params.extract_input<Field<float>>("Rotation Weight");

  GeometrySet colliders_geometry_set = params.extract_input<GeometrySet>("Colliders");
  Span<float4x4> collider_transforms = colliders_geometry_set.has_instances() ?
                                           colliders_geometry_set.get_instances()->transforms() :
                                           Span<float4x4>{};

  ConstraintEvalParams eval_params = extract_eval_params(params);
  const bool debug_output = (eval_params.debug_recorder != nullptr);

  Vector<ConstraintEvalData> constraint_data;
  IndexMaskMemory memory;
  get_constraint_data(params, debug_output, constraint_data, memory);

  static const Array<GeometryComponent::Type> types = {bke::GeometryComponent::Type::Mesh,
                                                       bke::GeometryComponent::Type::PointCloud,
                                                       bke::GeometryComponent::Type::Curve,
                                                       bke::GeometryComponent::Type::GreasePencil};
  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    for (const bke::GeometryComponent::Type component_type : types) {
      if (geometry_set.has(component_type)) {
        bke::GeometryComponent &component = geometry_set.get_component_for_write(component_type);
        std::optional<bke::MutableAttributeAccessor> attributes = component.attributes_for_write();
        if (!attributes) {
          continue;
        }

        if (eval_params.debug_recorder) {
          eval_params.debug_recorder->set_geometry(geometry_set, component_type);
        }

        const int num_points = attributes->domain_size(AttrDomain::Point);
        ConstraintVariables vars;
        vars.positions.reinitialize(num_points);
        vars.rotations.reinitialize(num_points);

        const bke::GeometryFieldContext field_context{component, AttrDomain::Point};
        fn::FieldEvaluator evaluator{field_context, num_points};
        evaluator.add_with_destination(position_field, vars.positions.as_mutable_span());
        evaluator.add_with_destination(rotation_field, vars.rotations.as_mutable_span());
        evaluator.add(old_position_field);
        evaluator.add(old_rotation_field);
        evaluator.add(position_weight_field);
        evaluator.add(rotation_weight_field);
        evaluator.evaluate();
        eval_params.old_positions = evaluator.get_evaluated<float3>(2);
        eval_params.old_rotations = evaluator.get_evaluated<math::Quaternion>(3);
        eval_params.position_weights = evaluator.get_evaluated<float>(4);
        eval_params.rotation_weights = evaluator.get_evaluated<float>(5);

        eval_params.collider_transforms = collider_transforms;
        /* XXX Transforms of the previous frame are not currently available, these are always the
         * same as the current frame. Eventually this will allow transfer of velocity from animated
         * colliders. */
        eval_params.old_collider_transforms = eval_params.collider_transforms;

        do_solver_iterations(SolverMethod::Global,
                             global_iterations,
                             EvaluationTarget::Positions,
                             init_mode,
                             eval_params,
                             constraint_data,
                             vars);
        do_solver_iterations(SolverMethod::GaussSeidel,
                             gauss_seidel_iterations,
                             EvaluationTarget::Positions,
                             init_mode,
                             eval_params,
                             constraint_data,
                             vars);
        do_solver_iterations(SolverMethod::Jacobi,
                             jacobi_iterations,
                             EvaluationTarget::Positions,
                             init_mode,
                             eval_params,
                             constraint_data,
                             vars);

        if (position_output_id) {
          AttributeWriter<float3> positions_writer = attributes->lookup_or_add_for_write<float3>(
              *position_output_id, AttrDomain::Point);
          BLI_assert(vars.positions.size() == num_points);
          positions_writer.varray.set_all(vars.positions);
          positions_writer.finish();
        }
        if (rotation_output_id) {
          AttributeWriter<math::Quaternion> rotations_writer =
              attributes->lookup_or_add_for_write<math::Quaternion>(*rotation_output_id,
                                                                    AttrDomain::Point);
          BLI_assert(vars.rotations.size() == num_points);
          rotations_writer.varray.set_all(vars.rotations);
          rotations_writer.finish();
        }
      }
    }
  });

  params.set_output("Geometry", geometry_set);
  set_constraint_data_output(params, constraint_data);
  if (eval_params.debug_recorder) {
    params.set_output("Debug Steps", eval_params.debug_recorder->debug_steps());
  }
}

static void node_geo_exec_velocities(GeoNodeExecParams params)
{
  const ConstraintInit init_mode = params.extract_input<bool>("Warm Start") ?
                                       ConstraintInit::WarmStart :
                                       ConstraintInit::ZeroInit;
  const int global_iterations = (params.extract_input<bool>("Use Global Solve") ? 1 : 0);
  const int gauss_seidel_iterations = std::max(
      params.extract_input<int>("Gauss-Seidel Iterations"), 0);
  const int jacobi_iterations = std::max(params.extract_input<int>("Jacobi Iterations"), 0);
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");
  Field<float3> position_field = params.extract_input<Field<float3>>("Position");
  Field<math::Quaternion> rotation_field = params.extract_input<Field<math::Quaternion>>(
      "Rotation");
  Field<float3> velocity_field = params.extract_input<Field<float3>>("Velocity");
  Field<float3> angular_velocity_field = params.extract_input<Field<float3>>("Angular Velocity");
  std::optional<std::string> velocity_output_id =
      params.get_output_anonymous_attribute_id_if_needed("Velocity");
  std::optional<std::string> angular_velocity_output_id =
      params.get_output_anonymous_attribute_id_if_needed("Angular Velocity");
  Field<float3> orig_velocity_field = params.extract_input<Field<float3>>("Original Velocity");
  Field<float3> orig_angular_velocity_field = params.extract_input<Field<float3>>(
      "Original Angular Velocity");
  Field<float> position_weight_field = params.extract_input<Field<float>>("Position Weight");
  Field<float> rotation_weight_field = params.extract_input<Field<float>>("Rotation Weight");

  GeometrySet colliders_geometry_set = params.extract_input<GeometrySet>("Colliders");
  Span<float4x4> collider_transforms = colliders_geometry_set.has_instances() ?
                                           colliders_geometry_set.get_instances()->transforms() :
                                           Span<float4x4>{};

  ConstraintEvalParams eval_params = extract_eval_params(params);
  const bool debug_output = (eval_params.debug_recorder != nullptr);

  Vector<ConstraintEvalData> constraint_data;
  IndexMaskMemory memory;
  get_constraint_data(params, debug_output, constraint_data, memory);

  static const Array<GeometryComponent::Type> types = {bke::GeometryComponent::Type::Mesh,
                                                       bke::GeometryComponent::Type::PointCloud,
                                                       bke::GeometryComponent::Type::Curve,
                                                       bke::GeometryComponent::Type::GreasePencil};
  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    for (const bke::GeometryComponent::Type component_type : types) {
      if (geometry_set.has(component_type)) {
        bke::GeometryComponent &component = geometry_set.get_component_for_write(component_type);
        std::optional<bke::MutableAttributeAccessor> attributes = component.attributes_for_write();
        if (!attributes) {
          continue;
        }

        if (eval_params.debug_recorder) {
          eval_params.debug_recorder->set_geometry(geometry_set, component_type);
        }

        const int num_points = attributes->domain_size(AttrDomain::Point);
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
        evaluator.add(orig_velocity_field);
        evaluator.add(orig_angular_velocity_field);
        evaluator.add(position_weight_field);
        evaluator.add(rotation_weight_field);
        evaluator.evaluate();
        eval_params.orig_velocities = evaluator.get_evaluated<float3>(4);
        eval_params.orig_angular_velocities = evaluator.get_evaluated<float3>(5);
        eval_params.position_weights = evaluator.get_evaluated<float>(6);
        eval_params.rotation_weights = evaluator.get_evaluated<float>(7);

        eval_params.collider_transforms = collider_transforms;
        /* XXX Transforms of the previous frame are not currently available, these are always the
         * same as the current frame. Eventually this will allow transfer of velocity from animated
         * colliders. */
        eval_params.old_collider_transforms = eval_params.collider_transforms;

        do_solver_iterations(SolverMethod::Global,
                             global_iterations,
                             EvaluationTarget::Velocities,
                             init_mode,
                             eval_params,
                             constraint_data,
                             vars);
        do_solver_iterations(SolverMethod::GaussSeidel,
                             gauss_seidel_iterations,
                             EvaluationTarget::Velocities,
                             init_mode,
                             eval_params,
                             constraint_data,
                             vars);
        do_solver_iterations(SolverMethod::Jacobi,
                             jacobi_iterations,
                             EvaluationTarget::Velocities,
                             init_mode,
                             eval_params,
                             constraint_data,
                             vars);

        if (velocity_output_id) {
          AttributeWriter<float3> velocities_writer = attributes->lookup_or_add_for_write<float3>(
              *velocity_output_id, AttrDomain::Point);
          BLI_assert(vars.velocities.size() == num_points);
          velocities_writer.varray.set_all(vars.velocities);
          velocities_writer.finish();
        }
        if (angular_velocity_output_id) {
          AttributeWriter<float3> angular_velocities_writer =
              attributes->lookup_or_add_for_write<float3>(*angular_velocity_output_id,
                                                          AttrDomain::Point);
          BLI_assert(vars.angular_velocities.size() == num_points);
          angular_velocities_writer.varray.set_all(vars.angular_velocities);
          angular_velocities_writer.finish();
        }
      }
    }
  });

  params.set_output("Geometry", geometry_set);
  set_constraint_data_output(params, constraint_data);
  if (eval_params.debug_recorder) {
    params.set_output("Debug Steps", eval_params.debug_recorder->debug_steps());
  }
}

static void node_register_position_solve()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSolvePositionConstraints");
  ntype.ui_name = "Solve XPBD Position Constraints";
  ntype.ui_description =
      "Solve position and rotation constraints on geometry using the XPBD framework";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  node_type_size(ntype, 200, 120, 300);
  ntype.geometry_node_execute = node_geo_exec_positions;
  ntype.declare = node_declare_positions;
  blender::bke::node_register_type(ntype);
}

static void node_register_velocity_solve()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSolveVelocityConstraints");
  ntype.ui_name = "Solve XPBD Velocity Constraints";
  ntype.ui_description =
      "Solve linear and angular velocity constraints on geometry using the XPBD framework";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  node_type_size(ntype, 200, 120, 300);
  ntype.geometry_node_execute = node_geo_exec_velocities;
  ntype.declare = node_declare_velocities;
  blender::bke::node_register_type(ntype);
}

static void node_register()
{
  node_register_position_solve();
  node_register_velocity_solve();
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_solve_xpbd_constraints_cc
