/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute.hh"
#include "BKE_geometry_set.hh"
#include "BKE_instances.hh"

#include "NOD_xpbd_solver.hh"

#include "node_geometry_util.hh"

#include <fmt/format.h>

#include <Eigen/SparseCholesky>

namespace blender::nodes::xpbd_constraints {

/* -------------------------------------------------------------------- */
/** \name Debug Recorder
 * \{ */

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

/** \} */

/* -------------------------------------------------------------------- */
/** \name Gauss-Seidel Solver
 * \{ */

void apply_gauss_seidel_positions_group(const ConstraintEvalParams &eval_params,
                                        const ConstraintTypeInfo &constraint_info,
                                        GeometrySet &constraints,
                                        const IndexMask &group_mask,
                                        ConstraintVariables &variables,
                                        const VariableIndexArrays &index_arrays,
                                        IndexMaskMemory &memory)
{
  constexpr bool linearized_quaternion = true;

  if (!constraint_info.evaluate_position) {
    return;
  }

  int num_components, num_position_vars, num_rotation_vars;
  bool use_active_mask;
  constraint_info.get_size(num_components, num_position_vars, num_rotation_vars, use_active_mask);

  VArray<bool> active;
  Vector<VArray<float3>> delta_positions;
  Vector<VArray<float4>> delta_rotations;
  constraint_info.evaluate_position(
      eval_params, variables, group_mask, constraints, active, delta_positions, delta_rotations);
  IndexMask group_and_active_mask = IndexMask::from_bools(group_mask, active, memory);

  /* TODO optimize: constraints should have at most 4 point maps and associated deltas.
   * It should be possible to unroll the mapping loop and use only a single group mask iteration.
   */
  const IndexRange points_range = variables.positions.index_range();
  for (const int var_i : IndexRange(num_position_vars)) {
    const Span<int> indices = index_arrays.position_indices[var_i];
    const VArraySpan<float3> delta_pos = delta_positions[var_i];
    group_and_active_mask.foreach_index(GrainSize(4096), [&](const int index) {
      /* Gauss-Seidel solver has a unique source for each point and can just write to it. */
      const int point = indices[index];
      if (points_range.contains(point)) {
        xpbd_constraints::apply_position_impulse(delta_pos[index], variables.positions[point]);
      }
    });
  }
  for (const int var_i : IndexRange(num_rotation_vars)) {
    const Span<int> indices = index_arrays.rotation_indices[var_i];
    const VArraySpan<float4> delta_rot = delta_rotations[var_i];
    group_and_active_mask.foreach_index(GrainSize(4096), [&](const int index) {
      const int point = indices[index];
      if (points_range.contains(point)) {
        xpbd_constraints::apply_rotation_impulse<linearized_quaternion>(
            delta_rot[index], variables.rotations[point]);
      }
    });
  }

  if (eval_params.debug_recorder) {
    const std::string label = fmt::format("Evaluate: {}", constraint_info.ui_name);
    eval_params.debug_recorder->record_step(
        label, &constraints, constraint_info.type_code, group_and_active_mask, variables);
  }
}

void apply_gauss_seidel_velocities_group(const ConstraintEvalParams &eval_params,
                                         const ConstraintTypeInfo &constraint_info,
                                         GeometrySet &constraints,
                                         const IndexMask &group_mask,
                                         ConstraintVariables &variables,
                                         const VariableIndexArrays &index_arrays,
                                         IndexMaskMemory &memory)
{
  if (!constraint_info.evaluate_velocity) {
    return;
  }

  int num_components, num_position_vars, num_rotation_vars;
  bool use_active_mask;
  constraint_info.get_size(num_components, num_position_vars, num_rotation_vars, use_active_mask);

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

  /* TODO optimize: constraints should have at most 4 point maps and associated deltas.
   * It should be possible to unroll the mapping loop and use only a single group mask iteration.
   */
  const IndexRange points_range = variables.positions.index_range();
  for (const int var_i : IndexRange(num_position_vars)) {
    const Span<int> indices = index_arrays.position_indices[var_i];
    const VArraySpan<float3> delta_vel = delta_velocities[var_i];
    group_and_active_mask.foreach_index(GrainSize(4096), [&](const int index) {
      /* Gauss-Seidel solver has a unique source for each point and can just write to it. */
      const int point = indices[index];
      if (points_range.contains(point)) {
        xpbd_constraints::apply_velocity_impulse(delta_vel[index], variables.velocities[point]);
      }
    });
  }
  for (const int var_i : IndexRange(num_rotation_vars)) {
    const Span<int> indices = index_arrays.rotation_indices[var_i];
    const VArraySpan<float3> delta_angvel = delta_angular_velocities[var_i];
    group_and_active_mask.foreach_index(GrainSize(4096), [&](const int index) {
      const int point = indices[index];
      if (points_range.contains(point)) {
        xpbd_constraints::apply_angular_velocity_impulse(delta_angvel[index],
                                                         variables.angular_velocities[point]);
      }
    });
  }

  if (eval_params.debug_recorder) {
    const std::string label = fmt::format("Evaluate: {}", constraint_info.ui_name);
    eval_params.debug_recorder->record_step(
        label, &constraints, constraint_info.type_code, group_and_active_mask, variables);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Jacobi Solver
 * \{ */

void add_jacobi_position_deltas(const ConstraintEvalParams &eval_params,
                                const ConstraintTypeInfo &constraint_info,
                                GeometrySet &constraints,
                                const IndexMask &constraints_mask,
                                ConstraintVariables &variables,
                                const VariableIndexArrays &index_arrays,
                                MutableSpan<float3> point_delta_positions,
                                MutableSpan<float4> point_delta_rotations,
                                MutableSpan<int> position_weights,
                                MutableSpan<int> rotation_weights,
                                IndexMaskMemory &memory)
{
  if (!constraint_info.evaluate_position) {
    return;
  }

  int num_components, num_position_vars, num_rotation_vars;
  bool use_active_mask;
  constraint_info.get_size(num_components, num_position_vars, num_rotation_vars, use_active_mask);

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

  /* TODO optimize: constraints should have at most 4 point maps and associated deltas.
   * It should be possible to unroll the mapping loop and use only a single group mask iteration.
   */
  const IndexRange positions_range = variables.positions.index_range();
  const IndexRange rotations_range = variables.rotations.index_range();

  struct ThreadLocalDelta {
    Array<float3> pos_delta;
    Array<float4> rot_delta;
    Array<int> pos_weight;
    Array<int> rot_weight;

    ThreadLocalDelta(const int64_t positions_size, const int64_t rotations_size)
        : pos_delta(positions_size, float3(0.0f)),
          rot_delta(rotations_size, float4(0.0f)),
          pos_weight(rotations_size, 0),
          rot_weight(rotations_size, 0)
    {
    }
  };

  threading::EnumerableThreadSpecific<ThreadLocalDelta> thread_delta(
      ThreadLocalDelta{positions_range.size(), rotations_range.size()});
  threading::parallel_for(active_mask.index_range(), 1024, [&](const IndexRange range) {
    const IndexMask sub_mask = active_mask.slice(range);
    ThreadLocalDelta &local_delta = thread_delta.local();

    for (const int var_i : IndexRange(num_position_vars)) {
      const Span<int> indices = index_arrays.position_indices[var_i];
      const VArraySpan<float3> delta_pos = delta_positions[var_i];
      sub_mask.foreach_index([&](const int index) {
        const int point = indices[index];
        if (positions_range.contains(point)) {
          ++local_delta.pos_weight[point];
          local_delta.pos_delta[point] += delta_pos[index];
        }
      });
    }
    for (const int var_i : IndexRange(num_rotation_vars)) {
      const Span<int> indices = index_arrays.rotation_indices[var_i];
      const VArraySpan<float4> delta_rot = delta_rotations[var_i];
      sub_mask.foreach_index([&](const int index) {
        const int point = indices[index];
        if (rotations_range.contains(point)) {
          ++local_delta.rot_weight[point];
          local_delta.rot_delta[point] += delta_rot[index];
        }
      });
    }
  });

  threading::parallel_for(positions_range, 1024, [&](const IndexRange range) {
    for (const ThreadLocalDelta &local_delta : thread_delta) {
      for (const int index : range) {
        position_weights[index] += local_delta.pos_weight[index];
        rotation_weights[index] += local_delta.rot_weight[index];
        point_delta_positions[index] += local_delta.pos_delta[index];
        point_delta_rotations[index] += local_delta.rot_delta[index];
      }
    }
  });

  if (eval_params.debug_recorder) {
    const std::string label = fmt::format("Evaluate: {}", constraint_info.ui_name);
    eval_params.debug_recorder->record_step(
        label, &constraints, constraint_info.type_code, active_mask, variables);
  }
}

void add_jacobi_velocity_deltas(const ConstraintEvalParams &eval_params,
                                const ConstraintTypeInfo &constraint_info,
                                GeometrySet &constraints,
                                const IndexMask &constraints_mask,
                                ConstraintVariables &variables,
                                const VariableIndexArrays &index_arrays,
                                MutableSpan<float3> point_delta_velocities,
                                MutableSpan<float3> point_delta_angular_velocities,
                                MutableSpan<int> velocity_weights,
                                MutableSpan<int> angular_velocity_weights,
                                IndexMaskMemory &memory)
{
  if (!constraint_info.evaluate_velocity) {
    return;
  }

  int num_components, num_position_vars, num_rotation_vars;
  bool use_active_mask;
  constraint_info.get_size(num_components, num_position_vars, num_rotation_vars, use_active_mask);

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

  /* TODO optimize: constraints should have at most 4 point maps and associated deltas.
   * It should be possible to unroll the mapping loop and use only a single group mask iteration.
   */
  const IndexRange positions_range = variables.positions.index_range();
  const IndexRange rotations_range = variables.rotations.index_range();

  struct ThreadLocalDelta {
    Array<float3> vel_delta;
    Array<float3> angvel_delta;
    Array<int> vel_weight;
    Array<int> angvel_weight;

    ThreadLocalDelta(const int64_t positions_size, const int64_t rotations_size)
        : vel_delta(positions_size, float3(0.0f)),
          angvel_delta(rotations_size, float3(0.0f)),
          vel_weight(rotations_size, 0),
          angvel_weight(rotations_size, 0)
    {
    }
  };

  threading::EnumerableThreadSpecific<ThreadLocalDelta> thread_delta(
      ThreadLocalDelta{positions_range.size(), rotations_range.size()});
  threading::parallel_for(active_mask.index_range(), 1024, [&](const IndexRange range) {
    const IndexMask sub_mask = active_mask.slice(range);
    ThreadLocalDelta &local_delta = thread_delta.local();

    for (const int var_i : IndexRange(num_position_vars)) {
      const Span<int> indices = index_arrays.position_indices[var_i];
      const VArraySpan<float3> delta_vel = delta_velocities[var_i];
      sub_mask.foreach_index([&](const int index) {
        const int point = indices[index];
        if (positions_range.contains(point)) {
          ++local_delta.vel_weight[point];
          local_delta.vel_delta[point] += delta_vel[index];
        }
      });
    }
    for (const int var_i : IndexRange(num_rotation_vars)) {
      const Span<int> indices = index_arrays.rotation_indices[var_i];
      const VArraySpan<float3> delta_angvel = delta_angular_velocities[var_i];
      sub_mask.foreach_index([&](const int index) {
        const int point = indices[index];
        if (rotations_range.contains(point)) {
          ++local_delta.angvel_weight[point];
          local_delta.angvel_delta[point] += delta_angvel[index];
        }
      });
    }
  });

  threading::parallel_for(positions_range, 1024, [&](const IndexRange range) {
    for (const ThreadLocalDelta &local_delta : thread_delta) {
      for (const int index : range) {
        velocity_weights[index] += local_delta.vel_weight[index];
        angular_velocity_weights[index] += local_delta.angvel_weight[index];
        point_delta_velocities[index] += local_delta.vel_delta[index];
        point_delta_angular_velocities[index] += local_delta.angvel_delta[index];
      }
    }
  });

  if (eval_params.debug_recorder) {
    const std::string label = fmt::format("Evaluate: {}", constraint_info.ui_name);
    eval_params.debug_recorder->record_step(
        label, &constraints, constraint_info.type_code, active_mask, variables);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Global Linear Solver
 * \{ */

inline float4x4 quaternion_matrix(const math::Quaternion &q)
{
  float4x4 result;
  result[0] = float4{q.w, q.x, q.y, q.z};
  result[1] = float4{-q.x, q.w, -q.z, q.y};
  result[2] = float4{-q.y, q.z, q.w, -q.x};
  result[3] = float4{-q.z, -q.y, q.x, q.w};
  return result;
}

static void read_constraint_attributes(const Span<ConstraintEvalData> constraint_data,
                                       MutableSpan<GVArray> lambdas_by_type)
{
  for (const int constraint_i : constraint_data.index_range()) {
    const ConstraintEvalData &data = constraint_data[constraint_i];
    if (!data.geometry || !data.geometry->has_pointcloud()) {
      continue;
    }

    int num_components, num_position_vars, num_rotation_vars;
    bool use_active_mask;
    data.type->get_size(num_components, num_position_vars, num_rotation_vars, use_active_mask);

    const GeometryComponent &component = *data.geometry->get_component<PointCloudComponent>();
    const AttributeAccessor attributes = *component.attributes();
    switch (num_components) {
      case 1:
        lambdas_by_type[constraint_i] = *attributes.lookup_or_default<float>(
            "lambda", AttrDomain::Point, 0.0f);
        break;
      case 2:
        lambdas_by_type[constraint_i] = *attributes.lookup_or_default<float2>(
            "lambda", AttrDomain::Point, float2(0.0f));
        break;
      case 3:
        lambdas_by_type[constraint_i] = *attributes.lookup_or_default<float3>(
            "lambda", AttrDomain::Point, float3(0.0f));
        break;
      default:
        BLI_assert_unreachable();
        break;
    }
  }
}

static void write_constraint_attributes(MutableSpan<ConstraintEvalData> constraint_data,
                                        MutableSpan<GSpanAttributeWriter> lambda_writers_by_type)
{
  for (const int constraint_i : constraint_data.index_range()) {
    ConstraintEvalData &data = constraint_data[constraint_i];
    if (!data.geometry || !data.geometry->has_pointcloud()) {
      continue;
    }

    int num_components, num_position_vars, num_rotation_vars;
    bool use_active_mask;
    data.type->get_size(num_components, num_position_vars, num_rotation_vars, use_active_mask);

    GeometryComponent &component = data.geometry->get_component_for_write<PointCloudComponent>();
    MutableAttributeAccessor attributes = *component.attributes_for_write();
    switch (num_components) {
      case 1:
        lambda_writers_by_type[constraint_i] = attributes.lookup_or_add_for_write_span(
            "lambda", AttrDomain::Point, CD_PROP_FLOAT);
        break;
      case 2:
        lambda_writers_by_type[constraint_i] = attributes.lookup_or_add_for_write_span(
            "lambda", AttrDomain::Point, CD_PROP_FLOAT2);
        break;
      case 3:
        lambda_writers_by_type[constraint_i] = attributes.lookup_or_add_for_write_span(
            "lambda", AttrDomain::Point, CD_PROP_FLOAT3);
        break;
      default:
        BLI_assert_unreachable();
        break;
    }
  }
}

void read_constraint_topology(const Span<ConstraintEvalData> constraint_data,
                              MutableSpan<VariableIndexArrays> indices_by_type)
{
  for (const int constraint_i : constraint_data.index_range()) {
    const ConstraintEvalData &data = constraint_data[constraint_i];
    VariableIndexArrays &index_arrays = indices_by_type[constraint_i];
    if (!data.type->get_size || !data.type->get_variable_indices) {
      continue;
    }
    if (!data.geometry || !data.geometry->has_pointcloud()) {
      continue;
    }

    int num_components, num_position_vars, num_rotation_vars;
    bool use_active_mask;
    data.type->get_size(num_components, num_position_vars, num_rotation_vars, use_active_mask);

    const int num_constraints = data.constraints.size();
    const GeometryComponent &component = *data.geometry->get_component<PointCloudComponent>();
    const AttributeAccessor attributes = *component.attributes();
    /* Only allocate data for variables that are actually needed by the constraint type. */
    for (const int var_i : IndexRange(num_position_vars)) {
      index_arrays.position_indices[var_i].reinitialize(num_constraints);
    }
    for (const int var_i : IndexRange(num_rotation_vars)) {
      index_arrays.rotation_indices[var_i].reinitialize(num_constraints);
    }
    /* Arrays of spans to use as function arguments. */
    MutableSpan<int> position_indices[4] = {index_arrays.position_indices[0],
                                            index_arrays.position_indices[1],
                                            index_arrays.position_indices[2],
                                            index_arrays.position_indices[3]};
    MutableSpan<int> rotation_indices[4] = {index_arrays.rotation_indices[0],
                                            index_arrays.rotation_indices[1],
                                            index_arrays.rotation_indices[2],
                                            index_arrays.rotation_indices[3]};
    /* Fill the index arrays */
    data.type->get_variable_indices(
        attributes, data.constraints, position_indices, rotation_indices);
  }
}

static void debug_check_constraint_topology(const ConstraintEvalParams &params,
                                            const Span<ConstraintEvalData> constraint_data,
                                            const int num_positions,
                                            const int num_rotations,
                                            Span<VariableIndexArrays> indices_by_type)
{
  for (const int constraint_i : constraint_data.index_range()) {
    const ConstraintEvalData &data = constraint_data[constraint_i];
    if (!data.geometry || !data.geometry->has_pointcloud()) {
      continue;
    }

    int num_components, num_position_vars, num_rotation_vars;
    bool use_active_mask;
    data.type->get_size(num_components, num_position_vars, num_rotation_vars, use_active_mask);

    std::atomic_bool has_invalid_position_index = false;
    std::atomic_bool has_invalid_rotation_index = false;
    for (const int var_i : IndexRange(num_position_vars)) {
      for (const int index : indices_by_type[constraint_i].position_indices[var_i]) {
        if (!IndexRange(num_positions).contains(index)) {
          has_invalid_position_index.store(true, std::memory_order_relaxed);
        }
      }
    }
    for (const int var_i : IndexRange(num_rotation_vars)) {
      for (const int index : indices_by_type[constraint_i].rotation_indices[var_i]) {
        if (!IndexRange(num_rotations).contains(index)) {
          has_invalid_rotation_index.store(true, std::memory_order_relaxed);
        }
      }
    }
    if (has_invalid_position_index) {
      params.error_message_add(
          fmt::format("Constraint type \"{}\" uses invalid position index", data.type->ui_name));
    }
    if (has_invalid_rotation_index) {
      params.error_message_add(
          fmt::format("Constraint type \"{}\" uses invalid rotation index", data.type->ui_name));
    }
  }
}

/* Count number of constraint values and dependent variables,
 * which determines the size of the global solver matrix. */
static void count_global_solve_matrix_entries(const Span<ConstraintEvalData> constraint_data,
                                              const ConstraintVariables &variables,
                                              int &r_non_zeroes_capacity,
                                              int &r_max_columns)
{
  const int num_positions = variables.positions.size();
  const int num_rotations = variables.rotations.size();

  /* Maximal number of rows/columns of the sparse matrix (if all constraints are active). */
  r_max_columns = 0;
  /* Maximal number of non-zero entries in the sparse matrix (if all constraints are active). */
  r_non_zeroes_capacity = 0;
  /* Upper-left sub-matrix has 3 rows/columns for each position
   * and 4 rows/columns for each rotation. */
  r_max_columns += num_positions * 3 + num_rotations * 4;
  /* Each position adds 3 mass entries on the diagonal.
   * Each rotation adds 4 moment-of-inertia entries on the diagonal. */
  r_non_zeroes_capacity += num_positions * 3 + num_rotations * 4;
  for (const ConstraintEvalData &data : constraint_data) {
    if (!data.type->get_size) {
      continue;
    }

    int num_components, num_position_vars, num_rotation_vars;
    bool use_active_mask;
    data.type->get_size(num_components, num_position_vars, num_rotation_vars, use_active_mask);

    const int num_constraints = data.constraints.size();
    /* Lower-right sub-matrix has a row/column for each constraint component. */
    r_max_columns += num_constraints * num_components;
    /* Each component adds a compliance entry and derivatives for each variable. */
    const int entries_per_component = 1 + 2 * (num_position_vars * 3 + num_rotation_vars * 4);
    r_non_zeroes_capacity += num_constraints * num_components * entries_per_component;
  }
}

inline float get_component(const float v, const int i)
{
#ifdef NDEBUG
  UNUSED_VARS(i);
#else
  BLI_assert(i == 0);
#endif
  return v;
}

inline float get_component(const float2 &v, const int i)
{
  return v[i];
}

inline float get_component(const float3 &v, const int i)
{
  return v[i];
}

inline float get_component(const float3 &g, const int i, const int j)
{
#ifdef NDEBUG
  UNUSED_VARS(i);
#else
  BLI_assert(i == 0);
#endif
  return g[j];
}

inline float get_component(const float4 &g, const int i, const int j)
{
#ifdef NDEBUG
  UNUSED_VARS(i);
#else
  BLI_assert(i == 0);
#endif
  return g[j];
}

inline float get_component(const float4x4 &g, const int i, const int j)
{
  return g[i][j];
}

inline float mul_position_gradient(const float3 &gradient, const float3 &vec)
{
  return math::dot(gradient, vec);
}

inline float4 mul_position_gradient(const float4x4 &gradient, const float3 &vec)
{
  /* Gradient matrix is transpose of the Jacobian, multiply from the left. */
  return vec * gradient.view<4, 3>();
}

inline float mul_rotation_gradient(const float4 &gradient, const float4 &vec)
{
  return math::dot(gradient, vec);
}

inline float4 mul_rotation_gradient(const float4x4 &gradient, const float4 &vec)
{
  /* Gradient matrix is transpose of the Jacobian, multiply from the left. */
  return vec * gradient;
}

template<typename GradientT>
void append_gradient(const IndexRange component_columns,
                     const IndexRange variable_columns,
                     const GradientT &gradient,
                     Vector<Eigen::Triplet<float>> &triplets)
{
  for (const int v : variable_columns.index_range()) {
    for (const int u : component_columns.index_range()) {
      /* Lower-left corner contains gradient row vectors (Jacobian transpose matrix blocks). */
      triplets.append_unchecked_as(
          int(component_columns[u]), int(variable_columns[v]), get_component(gradient, u, v));
    }
  }
  /* Upper-right corner contains gradient column vectors (Jacobian matrix blocks). */
  for (const int u : component_columns.index_range()) {
    for (const int v : variable_columns.index_range()) {
      triplets.append_unchecked_as(
          int(variable_columns[v]), int(component_columns[u]), get_component(gradient, u, v));
    }
  }
}

/* Set matrix elements for a type of constraint.
 * Template of the component number so that vector types can be defined statically. */
template<int num_components, typename ValueT, typename PosGradT, typename RotGradT>
static void set_global_solve_elements(const ConstraintEvalParams &params,
                                      const ConstraintVariables &variables,
                                      const IndexRange positions_range,
                                      const IndexRange rotations_range,
                                      const ConstraintEvalData &data,
                                      const VariableIndexArrays &index_arrays,
                                      const GVArray &lambdas,
                                      Vector<Eigen::Triplet<float>> &triplets,
                                      Vector<float> &rhs_values,
                                      IndexMaskMemory &memory,
                                      int &r_num_columns,
                                      IndexMask &r_active_constraints)
{
  const int num_positions = variables.positions.size();
  const int num_rotations = variables.rotations.size();

  /* XXX does not work unfortunately. */
  // using ValueT = VecBase<float, num_components>;
  // /* Note: These are transposed Jacobian matrices to better match common types
  //  * (float3 instead of "float1x3"). */
  // using PosGradT = MatBase<float, num_components, 3>;
  // using RotGradT = MatBase<float, num_components, 4>;

  if (!data.geometry || !data.geometry->has_pointcloud()) {
    return;
  }
  const int num_constraints = data.constraints.size();
  const IndexMask constraint_mask = data.constraints;
  const IndexRange components_range = {rhs_values.size(), num_constraints * num_components};

  int num_components_rt, num_position_vars, num_rotation_vars;
  bool use_active_mask;
  data.type->get_size(num_components_rt, num_position_vars, num_rotation_vars, use_active_mask);
  BLI_assert(num_components_rt == num_components);
  BLI_assert(num_position_vars <= 4);
  BLI_assert(num_rotation_vars <= 4);

  const float inv_dt = params.inv_delta_time;
  Array<ValueT> alphas(num_constraints);
  Array<ValueT> betas(num_constraints);
  Array<ValueT> residuals(num_constraints);
  Array<PosGradT> position_gradients[4];
  for (const int i : IndexRange(num_position_vars)) {
    position_gradients[i].reinitialize(num_constraints);
  }
  Array<RotGradT> rotation_gradients[4];
  for (const int i : IndexRange(num_rotation_vars)) {
    rotation_gradients[i].reinitialize(num_constraints);
  }
  Array<bool> active_mask;
  if (use_active_mask) {
    active_mask.reinitialize(num_constraints);
  }
  GMutableSpan position_gradient_spans[4] = {position_gradients[0].as_mutable_span(),
                                             position_gradients[1].as_mutable_span(),
                                             position_gradients[2].as_mutable_span(),
                                             position_gradients[3].as_mutable_span()};
  GMutableSpan rotation_gradient_spans[4] = {rotation_gradients[0].as_mutable_span(),
                                             rotation_gradients[1].as_mutable_span(),
                                             rotation_gradients[2].as_mutable_span(),
                                             rotation_gradients[3].as_mutable_span()};

  const GeometryComponent &component = *data.geometry->get_component<PointCloudComponent>();
  const AttributeAccessor attributes = *component.attributes();
  data.type->linear_solve_elements(params,
                                   variables,
                                   attributes,
                                   constraint_mask,
                                   alphas.as_mutable_span(),
                                   betas.as_mutable_span(),
                                   residuals.as_mutable_span(),
                                   position_gradient_spans,
                                   rotation_gradient_spans,
                                   active_mask);

  const IndexMask active_constraints = use_active_mask ? IndexMask::from_bools(data.constraints,
                                                                               active_mask,
                                                                               memory) :
                                                         data.constraints;
  r_num_columns = active_constraints.size() * num_components;

  active_constraints.foreach_index([&](const int index, const int pos) {
    const ValueT alpha = math::max(alphas[index], ValueT(0.0f));
    const ValueT beta = math::max(betas[index], ValueT(0.0f));
    const ValueT compliance = alpha * params.inv_delta_time_squared /
                              (ValueT(1.0f) + alpha * beta);

    const IndexRange component_columns = components_range.slice(pos * num_components,
                                                                num_components);
    /* Compliance entries. */
    for (const int u : component_columns.index_range()) {
      triplets.append_unchecked_as(
          int(component_columns[u]), int(component_columns[u]), -get_component(compliance, u));
    }
  });

  /* Gradient entries. */
  for (const int var_i : IndexRange(num_position_vars)) {
    const Span<int> indices = index_arrays.position_indices[var_i];
    const Span<PosGradT> pos_gradients = position_gradient_spans[var_i].typed<PosGradT>();

    active_constraints.foreach_index([&](const int index, const int pos) {
      const int point_index = indices[index];
      if (!IndexRange(num_positions).contains(point_index)) {
        return;
      }
      const PosGradT gradient = pos_gradients[index];

      const IndexRange component_columns = components_range.slice(pos * num_components,
                                                                  num_components);
      const IndexRange position_columns = positions_range.slice(point_index * 3, 3);
      append_gradient<PosGradT>(component_columns, position_columns, -gradient, triplets);
    });
  }
  for (const int var_i : IndexRange(num_rotation_vars)) {
    const Span<int> indices = index_arrays.rotation_indices[var_i];
    const Span<RotGradT> rot_gradients = rotation_gradient_spans[var_i].typed<RotGradT>();

    active_constraints.foreach_index([&](const int index, const int pos) {
      const int point_index = indices[index];
      if (!IndexRange(num_rotations).contains(point_index)) {
        return;
      }
      const RotGradT gradient = rot_gradients[index];

      const IndexRange component_columns = components_range.slice(pos * num_components,
                                                                  num_components);
      const IndexRange rotation_columns = rotations_range.slice(point_index * 4, 4);
      append_gradient<RotGradT>(component_columns, rotation_columns, -gradient, triplets);
    });
  }

  /* RHS target vector includes the residual, compliance offset, and damping offset.
   * This is based on the minimization problem described in the XPBD paper
   * "XPBD: Position-Based Simulation of Compliant Constrained Dynamics" (Macklin et al.).
   *
   * The original XPBD paper only implements the Gauss-Seidel method, and uses the Schur
   * complement to solve for the Lagrance multipliers separately. By contrast the global solver
   * retains the full system of equations, as described in: "Direct Position-Based Solver for
   * Stiff Rods" (Kugelstadt et al.). Adding the damping potential "beta" modifies the constraint
   * equations:
   *
   *         M * dx  - grad(C)^T * dLambda = 0
   *   grad(C) * dx + alpha/dt^2 * dLambda =
   *       (C + alpha/dt^2 * lambda + alpha * beta * grad(C) * (x-x0) / dt) / (1 + alpha * beta)
   *
   * The damping factor (1 + alpha * beta) is moved to the RHS to keep the matrix symmetric and
   * allow solving it using Cholesky decomposition or Conjugate Gradient methods.
   */
  VArraySpan<ValueT> lambdas_span = lambdas.typed<ValueT>();
  active_constraints.foreach_index([&](const int index) {
    const ValueT lambda = lambdas_span[index];
    const ValueT alpha = math::max(alphas[index], ValueT(0.0f));
    const ValueT beta = math::max(betas[index], ValueT(0.0f));

    const ValueT residual = residuals[index];
    ValueT target = residual + alpha * lambda * params.inv_delta_time_squared;
    if (!math::is_zero(beta)) {
      /* Add velocity damping terms for all dependent variables. */
      ValueT velocity = ValueT(0.0f);
      for (const int var_i : IndexRange(num_position_vars)) {
        const Span<PosGradT> pos_gradients = position_gradient_spans[var_i].typed<PosGradT>();
        const PosGradT &gradient = pos_gradients[index];
        const int point_index = index_arrays.position_indices[var_i][index];
        const float3 delta_pos = variables.positions[point_index] -
                                 params.old_positions[point_index];
        velocity += ValueT(mul_position_gradient(gradient, delta_pos)) * inv_dt;
      }
      for (const int var_i : IndexRange(num_rotation_vars)) {
        const Span<RotGradT> rot_gradients = rotation_gradient_spans[var_i].typed<RotGradT>();
        const RotGradT &gradient = rot_gradients[index];
        const int point_index = index_arrays.rotation_indices[var_i][index];
        // XXX should this be angular velocity? i.e. (0, 2*Im(old_rot^T * rot)/dt)
        const float4 delta_rot = float4(variables.rotations[point_index]) -
                                 float4(params.old_rotations[point_index]);
        velocity += ValueT(mul_rotation_gradient(gradient, delta_rot)) * inv_dt;
      }
      target += alpha * beta * velocity;
    }
    /* Damping factor. */
    target *= math::rcp(ValueT(1.0f) + alpha * beta);

    for (const int u : IndexRange(num_components)) {
      rhs_values.append_unchecked(get_component(target, u));
    }
  });

  r_active_constraints = std::move(active_constraints);
}

static void set_global_solve_elements(const ConstraintEvalParams &params,
                                      const ConstraintVariables &variables,
                                      const IndexRange positions_range,
                                      const IndexRange rotations_range,
                                      const ConstraintEvalData &data,
                                      const VariableIndexArrays &index_arrays,
                                      const GVArray &lambdas,
                                      const int num_components,
                                      Vector<Eigen::Triplet<float>> &triplets,
                                      Vector<float> &rhs_values,
                                      IndexMaskMemory &memory,
                                      int &r_num_columns,
                                      IndexMask &r_active_constraints)
{
  /* XXX Matrix types float2x3, float3x3, float2x4, float 3x4 have no registered CPPType, so a
   * larger type is used for output arrays. */
  switch (num_components) {
    case 1:
      set_global_solve_elements<1, float, float3, float4>(params,
                                                          variables,
                                                          positions_range,
                                                          rotations_range,
                                                          data,
                                                          index_arrays,
                                                          lambdas,
                                                          triplets,
                                                          rhs_values,
                                                          memory,
                                                          r_num_columns,
                                                          r_active_constraints);
      break;
    case 2:
      set_global_solve_elements<2, float2, float4x4, float4x4>(params,
                                                               variables,
                                                               positions_range,
                                                               rotations_range,
                                                               data,
                                                               index_arrays,
                                                               lambdas,
                                                               triplets,
                                                               rhs_values,
                                                               memory,
                                                               r_num_columns,
                                                               r_active_constraints);
      break;
    case 3:
      set_global_solve_elements<3, float3, float4x4, float4x4>(params,
                                                               variables,
                                                               positions_range,
                                                               rotations_range,
                                                               data,
                                                               index_arrays,
                                                               lambdas,
                                                               triplets,
                                                               rhs_values,
                                                               memory,
                                                               r_num_columns,
                                                               r_active_constraints);
      break;
    default:
      BLI_assert_unreachable();
      break;
  }
}

static GlobalSolverSystem build_global_solve_matrix_from_triplets(
    const ConstraintEvalParams &params,
    const Span<ConstraintEvalData> constraint_data,
    const ConstraintVariables &variables,
    const Span<VariableIndexArrays> indices_by_type,
    const Span<GVArray> lambdas_by_type,
    const int non_zeroes_capacity,
    const int max_columns,
    IndexMaskMemory &memory)
{
  const int num_positions = variables.positions.size();
  const int num_rotations = variables.rotations.size();

  const IndexRange positions_range = {0, num_positions * 3};
  const IndexRange rotations_range = positions_range.after(num_rotations * 4);

  Vector<Eigen::Triplet<float>> triplets;
  triplets.reserve(non_zeroes_capacity);
  Vector<float> rhs_values;
  rhs_values.reserve(max_columns);

  /* Mass entries. */
  for (const int index : IndexRange(num_positions)) {
    const float mass = params.masses[index];

    const IndexRange columns = positions_range.slice(index * 3, 3);
    for (const int u : columns.index_range()) {
      triplets.append_unchecked_as(int(columns[u]), int(columns[u]), mass);
      rhs_values.append_unchecked(0.0f);
    }
  }
  for (const int index : IndexRange(num_rotations)) {
    const float3 local_inertia = params.local_inertia[index];
    /* Inertia tensor according to
     * "Rigid body dynamics with a scalable body, quaternions and perfect constraints",
     * Moeller et al., section 6, equation 64.
     * The W component is half the trace of the local moment-of-inertia vector, which is
     * not clearly explained in the Kugelstadt paper.
     */
    const float inertia_trace = local_inertia.x + local_inertia.y + local_inertia.z;
    const float4 inertia_diag = {0.5f * inertia_trace, local_inertia};

    const IndexRange columns = rotations_range.slice(index * 4, 4);
    for (const int u : columns.index_range()) {
      triplets.append_unchecked_as(int(columns[u]), int(columns[u]), inertia_diag[u]);
      rhs_values.append_unchecked(0.0f);
    }
  }

  int tot_columns = rhs_values.size();
  Array<IndexMask> constraint_mapping(constraint_data.size());
  for (const int constraint_i : constraint_data.index_range()) {
    const ConstraintEvalData &data = constraint_data[constraint_i];
    if (!data.type->get_size || !data.type->linear_solve_elements) {
      continue;
    }

    int num_components, num_position_vars, num_rotation_vars;
    bool use_active_mask;
    data.type->get_size(num_components, num_position_vars, num_rotation_vars, use_active_mask);

    // TODO Eventually these callbacks should be based around Fields instead of arrays, so that
    // node closures can be used directly. For now mapping and mask evaluation takes place
    // inside the callback.

    // struct {
    //   GField alpha, beta, residual;
    //   GField position_gradient[4];
    //   GField rotation_gradient[4];
    // } result_fields;
    // data.type->linear_solve_elements(params,
    //                                  variables,
    //                                  attributes,
    //                                  data.constraints,
    //                                  result_fields.alpha,
    //                                  result_fields.beta,
    //                                  result_fields.residual,
    //                                  result_fields.position_gradient,
    //                                  result_fields.rotation_gradient);

    // bke::GeometryFieldContext field_context(component, AttrDomain::Point);
    // FieldEvaluator field_evaluator(field_context, &data.constraints);
    // field_evaluator.add(result_fields.alpha);
    // field_evaluator.add(result_fields.beta);
    // field_evaluator.add(result_fields.residual);
    // for (const int var_i : IndexRange(num_position_vars)) {
    //   field_evaluator.add(result_fields.position_gradient[var_i]);
    // }
    // for (const int var_i : IndexRange(num_rotation_vars)) {
    //   field_evaluator.add(result_fields.rotation_gradient[var_i]);
    // }
    // field_evaluator.evaluate();
    // VArray<float> alphas = field_evaluator.get_evaluated<float>(0);
    // VArray<float> betas = field_evaluator.get_evaluated<float>(1);
    // VArray<float> residuals = field_evaluator.get_evaluated<float>(2);
    // GVArray position_gradients[4];
    // GVArray rotation_gradients[4];
    // for (const int var_i : IndexRange(num_position_vars)) {
    //   position_gradients[var_i] = field_evaluator.get_evaluated<float>(3 + var_i);
    // }
    // for (const int var_i : IndexRange(num_rotation_vars)) {
    //   rotation_gradients[var_i] = field_evaluator.get_evaluated<float>(3 + num_position_vars
    //   +
    //                                                                    var_i);
    // }

    const VariableIndexArrays &index_arrays = indices_by_type[constraint_i];
    const GVArray &lambdas = lambdas_by_type[constraint_i];
    int num_columns = 0;
    IndexMask &active_constraints = constraint_mapping[constraint_i];
    set_global_solve_elements(params,
                              variables,
                              positions_range,
                              rotations_range,
                              data,
                              index_arrays,
                              lambdas,
                              num_components,
                              triplets,
                              rhs_values,
                              memory,
                              num_columns,
                              active_constraints);
    tot_columns += num_columns;
  }

  Eigen::SparseMatrix<float> H(tot_columns, tot_columns);
  Eigen::VectorXf b;
  b.resize(tot_columns);
  /* TODO any way to avoid this copy? Resizing a Eigen::VectorX destroys all values. */
  for (const int i : IndexRange(tot_columns)) {
    b[i] = rhs_values[i];
  }

  H.setFromTriplets(triplets.begin(), triplets.end());

  return {std::move(H), std::move(b), std::move(constraint_mapping)};
}

template<bool debug_check>
static GlobalSolverSystem do_build_global_solve_system(
    const ConstraintEvalParams &params,
    const Span<ConstraintEvalData> constraint_data,
    const ConstraintVariables &variables,
    IndexMaskMemory &memory)
{
  const int num_positions = variables.positions.size();
  const int num_rotations = variables.rotations.size();

  int non_zeroes_capacity, max_columns;
  count_global_solve_matrix_entries(constraint_data, variables, non_zeroes_capacity, max_columns);

  Array<GVArray> lambdas_by_type(constraint_data.size());
  read_constraint_attributes(constraint_data, lambdas_by_type);
  /* Read constraint topology.
   * Constraints can use up to 4 variables, any extra arrays remain empty. */
  Array<VariableIndexArrays> indices_by_type(constraint_data.size());
  read_constraint_topology(constraint_data, indices_by_type);
  if constexpr (debug_check) {
    debug_check_constraint_topology(
        params, constraint_data, num_positions, num_rotations, indices_by_type);
  }

  /* LHS matrix describing equations of motion and constraint impulses. */
  return build_global_solve_matrix_from_triplets(params,
                                                 constraint_data,
                                                 variables,
                                                 indices_by_type,
                                                 lambdas_by_type,
                                                 non_zeroes_capacity,
                                                 max_columns,
                                                 memory);
}

GlobalSolverSystem build_global_solve_system(const ConstraintEvalParams &params,
                                             const Span<ConstraintEvalData> constraint_data,
                                             const ConstraintVariables &variables,
                                             const bool debug_check,
                                             IndexMaskMemory &memory)
{
  if (debug_check) {
    return do_build_global_solve_system<true>(params, constraint_data, variables, memory);
  }
  else {
    return do_build_global_solve_system<false>(params, constraint_data, variables, memory);
  }
}

static SolverResult solver_result_from_eigen(const Eigen::ComputationInfo computation_info)
{
  switch (computation_info) {
    case Eigen::ComputationInfo::Success:
      return SolverResult::Success;
    case Eigen::ComputationInfo::NumericalIssue:
      return SolverResult::NumericalIssue;
    case Eigen::ComputationInfo::NoConvergence:
      return SolverResult::NoConvergence;
    case Eigen::ComputationInfo::InvalidInput:
      return SolverResult::InvalidInput;
  }
  BLI_assert_unreachable();
  return SolverResult::Success;
}

SolverResult solve_global_system(GlobalSolverSystem &&system,
                                 ConstraintVariables &variables,
                                 MutableSpan<ConstraintEvalData> constraint_data,
                                 Eigen::VectorXf *r_solution)
{
  constexpr bool linearized_quaternion = true;

  Eigen::SimplicialLDLT<Eigen::Ref<Eigen::SparseMatrix<float>>> eigen_solver(system.matrix);
  Eigen::VectorXf x = eigen_solver.solve(system.target);
  const SolverResult result = solver_result_from_eigen(eigen_solver.info());
  if (result != SolverResult::Success) {
    return result;
  }

  const IndexRange position_rows = {0, variables.positions.size() * 3};
  const IndexRange rotation_rows = position_rows.after(variables.rotations.size() * 4);

  for (const int point_index : variables.positions.index_range()) {
    const IndexRange rows = position_rows.slice(point_index * 3, 3);
    const float3 delta_pos = {x[rows[0]], x[rows[1]], x[rows[2]]};
    xpbd_constraints::apply_position_impulse(delta_pos, variables.positions[point_index]);
  }
  for (const int point_index : variables.rotations.index_range()) {
    const IndexRange rows = rotation_rows.slice(point_index * 4, 4);
    const float4 delta_rot = {x[rows[0]], x[rows[1]], x[rows[2]], x[rows[3]]};
    xpbd_constraints::apply_rotation_impulse<linearized_quaternion>(
        delta_rot, variables.rotations[point_index]);
  }

  Array<GSpanAttributeWriter> lambda_writers_by_type(constraint_data.size());
  write_constraint_attributes(constraint_data, lambda_writers_by_type);

  IndexRange prev_rows = rotation_rows;
  for (const int constraint_i : constraint_data.index_range()) {
    const ConstraintEvalData &data = constraint_data[constraint_i];
    int num_components, num_position_vars, num_rotation_vars;
    bool use_active_mask;
    data.type->get_size(num_components, num_position_vars, num_rotation_vars, use_active_mask);

    const IndexMask &constraints = system.constraint_mapping[constraint_i];
    const IndexRange lambda_rows = prev_rows.after(constraints.size() * num_components);
    switch (num_components) {
      case 1: {
        MutableSpan<float> lambdas = lambda_writers_by_type[constraint_i].span.typed<float>();
        constraints.foreach_index(GrainSize(1024), [&](const int index, const int pos) {
          lambdas[index] = x[lambda_rows[pos]];
        });
        break;
      }
      case 2: {
        MutableSpan<float2> lambdas = lambda_writers_by_type[constraint_i].span.typed<float2>();
        constraints.foreach_index(GrainSize(1024), [&](const int index, const int pos) {
          const IndexRange rows = lambda_rows.slice(pos * 2, 2);
          lambdas[index] = {x[rows[0]], x[rows[1]]};
        });
        break;
      }
      case 3: {
        MutableSpan<float3> lambdas = lambda_writers_by_type[constraint_i].span.typed<float3>();
        constraints.foreach_index(GrainSize(1024), [&](const int index, const int pos) {
          const IndexRange rows = lambda_rows.slice(pos * 3, 3);
          lambdas[index] = {x[rows[0]], x[rows[1]], x[rows[2]]};
        });
        break;
      }
      default:
        BLI_assert_unreachable();
        break;
    }

    lambda_writers_by_type[constraint_i].finish();

    prev_rows = lambda_rows;
  }

  if (r_solution) {
    *r_solution = std::move(x);
  }
  return result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Residuals
 * \{ */

template<int num_components, typename ValueT>
static void compute_residuals_t(const ConstraintEvalParams &params,
                                const ConstraintTypeInfo &constraint_info,
                                bke::MutableAttributeAccessor &attributes,
                                StringRef attribute_id,
                                const IndexMask &constraints_mask,
                                const VariableIndexArrays &index_arrays)
{
}

static void compute_residuals_n(const ConstraintEvalParams &params,
                                const ConstraintTypeInfo &constraint_info,
                                bke::MutableAttributeAccessor &attributes,
                                StringRef attribute_id,
                                const IndexMask &constraints_mask,
                                const VariableIndexArrays &index_arrays,
                                const int num_components)
{
  /* XXX Matrix types float2x3, float3x3, float2x4, float 3x4 have no registered CPPType, so a
   * larger type is used for output arrays. */
  switch (num_components) {
    case 1:
      compute_residuals_t<1, float>(
          params, constraint_info, attributes, attribute_id, constraints_mask, index_arrays);
      break;
    case 2:
      compute_residuals_t<2, float2>(
          params, constraint_info, attributes, attribute_id, constraints_mask, index_arrays);
      break;
    case 3:
      compute_residuals_t<3, float3>(
          params, constraint_info, attributes, attribute_id, constraints_mask, index_arrays);
      break;
    default:
      BLI_assert_unreachable();
      break;
  }
}

void compute_residuals(const ConstraintEvalParams &eval_params,
                       const ConstraintTypeInfo &constraint_info,
                       bke::MutableAttributeAccessor &attributes,
                       StringRef attribute_id,
                       const IndexMask &constraints_mask,
                       const VariableIndexArrays &index_arrays)
{
}

/** \} */

}  // namespace blender::nodes::xpbd_constraints
