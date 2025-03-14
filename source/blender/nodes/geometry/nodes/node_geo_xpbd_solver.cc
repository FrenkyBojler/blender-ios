/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute.hh"
#include "BKE_geometry_set.hh"
#include "BKE_instances.hh"

#include "NOD_xpbd_solver.hh"

#include "node_geometry_util.hh"

#include <fmt/format.h>

#include <Eigen/Sparse>

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

void apply_gauss_seidel_velocities_group(const ConstraintEvalParams &eval_params,
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

/** \} */

/* -------------------------------------------------------------------- */
/** \name Jacobi Solver
 * \{ */

void add_jacobi_position_deltas(const ConstraintEvalParams &eval_params,
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

void add_jacobi_velocity_deltas(const ConstraintEvalParams &eval_params,
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

/** \} */

/* -------------------------------------------------------------------- */
/** \name Global Linear Solver
 * \{ */

// static void read_constraint_topology(const ConstraintEvalParams &params,
//                                      const ConstraintEvalData &data,
//                                      const IndexMask &selection,
//                                      MutableSpan<int> position_indices,
//                                      MutableSpan<int> rotation_indices)
// {
//   if (!data.type->linear_solve_variables) {
//     return;
//   }
//   if (!data.geometry || !data.geometry->has_component<PointCloudComponent>()) {
//     return;
//   }
//   const GeometryComponent &component = *data.geometry->get_component<PointCloudComponent>();
//   AttributeAccessor attributes = *component.attributes();

//   data.type->linear_solve_variables(params, attributes, selection, position_indices,
//   rotation_indices);
// }

inline float4x4 quaternion_matrix(const math::Quaternion &q)
{
  float4x4 result;
  result[0] = float4{q.w, q.x, q.y, q.z};
  result[1] = float4{-q.x, q.x, -q.z, q.y};
  result[1] = float4{-q.y, q.z, q.y, -q.x};
  result[1] = float4{-q.z, -q.y, q.x, q.z};
  return result;
}

using VariableIndexArrays = std::array<Array<int>, 4>;

static void read_constraint_topology(const Span<ConstraintEvalData> constraint_data,
                                     MutableSpan<VariableIndexArrays> position_indices_by_type,
                                     MutableSpan<VariableIndexArrays> rotation_indices_by_type)
{
  for (const int constraint_i : constraint_data.index_range()) {
    const ConstraintEvalData &data = constraint_data[constraint_i];
    if (!data.type->linear_solve_size || !data.type->linear_solve_variables) {
      continue;
    }
    if (!data.geometry || !data.geometry->has_component<PointCloudComponent>()) {
      continue;
    }

    int num_components, num_position_vars, num_rotation_vars;
    data.type->linear_solve_size(num_components, num_position_vars, num_rotation_vars);

    const int num_constraints = data.constraints.size();
    const GeometryComponent &component = *data.geometry->get_component<PointCloudComponent>();
    const AttributeAccessor attributes = *component.attributes();
    /* Only allocate data for variables that are actually needed by the constraint type. */
    for (const int var_i : IndexRange(num_position_vars)) {
      position_indices_by_type[constraint_i][var_i].reinitialize(num_constraints *
                                                                 num_position_vars);
    }
    for (const int var_i : IndexRange(num_rotation_vars)) {
      rotation_indices_by_type[constraint_i][var_i].reinitialize(num_constraints *
                                                                 num_rotation_vars);
    }
    /* Arrays of spans to use as function arguments. */
    MutableSpan<int> position_indices[4] = {position_indices_by_type[constraint_i][0],
                                            position_indices_by_type[constraint_i][1],
                                            position_indices_by_type[constraint_i][2],
                                            position_indices_by_type[constraint_i][3]};
    MutableSpan<int> rotation_indices[4] = {rotation_indices_by_type[constraint_i][0],
                                            rotation_indices_by_type[constraint_i][1],
                                            rotation_indices_by_type[constraint_i][2],
                                            rotation_indices_by_type[constraint_i][3]};
    /* Fill the index arrays */
    data.type->linear_solve_variables(
        attributes, data.constraints, position_indices, rotation_indices);
  }
}

static void debug_check_constraint_topology(const ConstraintEvalParams &params,
                                            const Span<ConstraintEvalData> constraint_data,
                                            const int num_positions,
                                            const int num_rotations,
                                            Span<VariableIndexArrays> position_indices_by_type,
                                            Span<VariableIndexArrays> rotation_indices_by_type)
{
  for (const int constraint_i : constraint_data.index_range()) {
    const ConstraintEvalData &data = constraint_data[constraint_i];
    if (!data.geometry || !data.geometry->has_component<PointCloudComponent>()) {
      continue;
    }

    int num_components, num_position_vars, num_rotation_vars;
    data.type->linear_solve_size(num_components, num_position_vars, num_rotation_vars);

    std::atomic_bool has_invalid_position_index = false;
    std::atomic_bool has_invalid_rotation_index = false;
    for (const int var_i : IndexRange(num_position_vars)) {
      for (const int index : position_indices_by_type[constraint_i][var_i]) {
        if (!IndexRange(num_positions).contains(index)) {
          has_invalid_position_index.store(true, std::memory_order_relaxed);
        }
      }
    }
    for (const int var_i : IndexRange(num_rotation_vars)) {
      for (const int index : rotation_indices_by_type[constraint_i][var_i]) {
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
                                              int &r_num_non_zeroes,
                                              int &r_num_rows,
                                              int &r_num_columns)
{
  const int num_positions = variables.positions.size();
  const int num_rotations = variables.rotations.size();

  /* Number of rows/columns of the sparse solver matrix. */
  r_num_columns = 0;
  /* Total number of non-zero entries in the sparse solver matrix. */
  r_num_non_zeroes = 0;
  /* Upper-left sub-matrix has 3 rows/columns for each position
   * and 4 rows/columns for each rotation. */
  r_num_columns += num_positions * 3 + num_rotations * 4;
  /* Each position adds 3 mass entries on the diagonal.
   * Each rotation adds 4x4 moment-of-inertia block-diagonal entries. */
  r_num_non_zeroes += num_positions * 3 + num_rotations * 16;
  for (const ConstraintEvalData &data : constraint_data) {
    if (!data.type->linear_solve_size) {
      continue;
    }

    int num_components, num_position_vars, num_rotation_vars;
    data.type->linear_solve_size(num_components, num_position_vars, num_rotation_vars);

    const int num_constraints = data.constraints.size();
    /* Lower-right sub-matrix has a row/column for each constraint component. */
    r_num_columns += num_constraints * num_components;
    /* Each component adds a compliance entry and derivatives for each variable. */
    const int entries_per_component = 1 + num_position_vars * 3 + num_rotation_vars * 4;
    r_num_non_zeroes += num_constraints * num_components * entries_per_component;
  }
  /* Matrix is square. */
  r_num_rows = r_num_columns;
}

static Eigen::SparseMatrix<float> build_global_solve_matrix_from_spans(
    const ConstraintEvalParams &params,
    const Span<ConstraintEvalData> constraint_data,
    const ConstraintVariables &variables,
    const Span<VariableIndexArrays> position_indices_by_type,
    const Span<VariableIndexArrays> rotation_indices_by_type,
    const int num_non_zeroes,
    const int num_rows,
    const int num_columns)
{
  const int num_positions = variables.positions.size();
  const int num_rotations = variables.rotations.size();

  Eigen::SparseMatrix<float> H(num_rows, num_columns);
  H.makeCompressed();
  H.resizeNonZeros(num_non_zeroes);
  /* Should be defined as column-major storage. */
  static_assert(!H.IsRowMajor);

  const IndexRange positions_range = {0, num_positions * 3};
  const IndexRange rotations_range = positions_range.after(num_rotations * 4);

  /* Determine size of per-column arrays. */
  {
    MutableSpan<int> column_sizes = {H.outerIndexPtr(), num_columns + 1};

    auto add_position_uses = [&](const int index, const int count) {
      const int start = index * 3;
      column_sizes[positions_range[start + 0]] += count;
      column_sizes[positions_range[start + 1]] += count;
      column_sizes[positions_range[start + 2]] += count;
    };
    auto add_rotation_uses = [&](const int index, const int count) {
      const int start = index * 4;
      column_sizes[rotations_range[start + 0]] += count;
      column_sizes[rotations_range[start + 1]] += count;
      column_sizes[rotations_range[start + 2]] += count;
      column_sizes[rotations_range[start + 3]] += count;
    };

    /* Diagonal entries for point masses. */
    for (const int index : IndexRange(num_positions)) {
      add_position_uses(index, 1);
    }
    /* Diagonal block entries for moment of inertia tensors. */
    for (const int index : IndexRange(num_rotations)) {
      add_rotation_uses(index, 4);
    }

    IndexRange prev_range = rotations_range;
    for (const int constraint_i : constraint_data.index_range()) {
      const ConstraintEvalData &data = constraint_data[constraint_i];
      if (!data.type->linear_solve_size) {
        continue;
      }

      int num_components, num_position_vars, num_rotation_vars;
      data.type->linear_solve_size(num_components, num_position_vars, num_rotation_vars);

      const int num_constraints = data.constraints.size();
      const IndexRange components_range = prev_range.after(num_constraints * num_components);
      auto add_component_uses = [&](const int index, const int count) {
        const int start = index * num_components;
        for (const int i : IndexRange(num_components)) {
          column_sizes[components_range[start + i]] += count;
        }
      };

      /* Diagonal entries for compliance. */
      for (const int index : IndexRange(num_constraints)) {
        add_component_uses(index, 1);
      }

      /* Lower-left corner:
       * A column represents a variable, each row is a derivative of one component.
       * Constraints add component entries in all columns of dependent variables. */
      for (const int var_i : IndexRange(num_position_vars)) {
        const Span<int> position_indices = position_indices_by_type[constraint_i][var_i];
        for (const int index : position_indices) {
          add_position_uses(index, num_components);
        }
      }
      for (const int var_i : IndexRange(num_rotation_vars)) {
        const Span<int> rotation_indices = rotation_indices_by_type[constraint_i][var_i];
        for (const int index : rotation_indices) {
          add_rotation_uses(index, num_components);
        }
      }

      /* Upper-right corner:
       * A column represents a component, each row is a derivative wrt. one variable.
       * Constraints add as many entries in their columns as they have dependent variables. */
      for (const int index : IndexRange(num_constraints)) {
        add_component_uses(index, num_position_vars * 3 + num_rotation_vars * 4);
      }

      prev_range = components_range;
    }

    offset_indices::accumulate_counts_to_offsets(column_sizes);
  }

  const OffsetIndices<int> column_offsets = {Span<int>(H.outerIndexPtr(), num_columns + 1)};
  /* Redundant for compressed matrix, non-zeros per column is the same as index range derived
   * from offsets. */
  // const Span<int> column_nonzeros = {H.innerNonZeroPtr(), num_columns};

  {
    MutableSpan<int> row_indices = {H.innerIndexPtr(), num_non_zeroes};
    MutableSpan<float> values = {H.valuePtr(), num_non_zeroes};

    /* Point masses in the upper left positions section. */
    for (const int index : IndexRange(num_positions)) {
      const float mass = params.masses[index];

      const IndexRange columns = positions_range.slice(index * 3, 3);
      for (const int k : columns.index_range()) {
        const int column = columns[k];
        MutableSpan<int> column_row_indices = row_indices.slice(column_offsets[column]);
        MutableSpan<float> column_values = values.slice(column_offsets[column]);

        column_row_indices[0] = column;
        column_values[0] = mass;
      }
    }
    /* Inertia tensors in the upper left rotations section. */
    for (const int index : IndexRange(num_rotations)) {
      const float4x4 inertia_tensor = quaternion_matrix(
          variables.rotations[index] * math::Quaternion(0.0f, params.local_inertia[index]));

      const IndexRange columns = rotations_range.slice(index * 4, 4);
      for (const int k : columns.index_range()) {
        const int column = columns[k];
        MutableSpan<int> column_row_indices = row_indices.slice(column_offsets[column]);
        MutableSpan<float> column_values = values.slice(column_offsets[column]);

        column_row_indices[0] = columns[0];
        column_row_indices[1] = columns[1];
        column_row_indices[2] = columns[2];
        column_row_indices[3] = columns[3];
        column_values[0] = inertia_tensor[k][0];
        column_values[1] = inertia_tensor[k][1];
        column_values[2] = inertia_tensor[k][2];
        column_values[3] = inertia_tensor[k][3];
      }
    }

    IndexRange prev_range = rotations_range;
    for (const ConstraintEvalData &data : constraint_data) {
      if (!data.geometry) {
        continue;
      }

      int num_components, num_position_vars, num_rotation_vars;
      data.type->linear_solve_size(num_components, num_position_vars, num_rotation_vars);

      const int num_constraints = data.constraints.size();
      const IndexRange components_range = prev_range.after(num_constraints * num_components);
      // auto add_component_uses = [&](const int index, const int count) {
      //   const int start = index * num_components;
      //   for (const int i : IndexRange(num_components)) {
      //     column_sizes[components_range[start + i]] += count;
      //   }
      // };

      /* Compliance values in the lower-right corner. */
      // for (const int index : IndexRange(num_constraints)) {
      //   // add_component_uses(index, 1);
      // }

      prev_range = components_range;
    }
  }

  return H;
}

inline float get_component(const float v, const int i)
{
  BLI_assert(i == 0);
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

/* Set matrix elements for a type of constraint.
 * Template of the component number so that vector types can be defined statically. */
template<typename ValueT, typename PosGradT, typename RotGradT>
static void set_global_solve_elements(const ConstraintEvalParams &params,
                                      const ConstraintVariables &variables,
                                      const IndexRange positions_range,
                                      const IndexRange rotations_range,
                                      const IndexRange components_range,
                                      const ConstraintEvalData &data,
                                      const VariableIndexArrays &position_index_arrays,
                                      const VariableIndexArrays &rotation_index_arrays,
                                      Vector<Eigen::Triplet<float>> &triplets)
{
  const int num_positions = variables.positions.size();
  const int num_rotations = variables.rotations.size();

  /* XXX does not work unfortunately. */
  // using ValueT = VecBase<float, num_components>;
  // /* Note: These are transposed Jacobian matrices to better match common types
  //  * (float3 instead of "float1x3"). */
  // using PosGradT = MatBase<float, num_components, 3>;
  // using RotGradT = MatBase<float, num_components, 4>;

  if (!data.geometry || !data.geometry->has_component<PointCloudComponent>()) {
    return;
  }
  const int num_constraints = data.constraints.size();
  const IndexMask constraint_mask = data.constraints;

  int num_components, num_position_vars, num_rotation_vars;
  data.type->linear_solve_size(num_components, num_position_vars, num_rotation_vars);
  BLI_assert(num_position_vars <= 4);
  BLI_assert(num_rotation_vars <= 4);

  Array<ValueT> alphas(num_constraints * num_components);
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
                                   rotation_gradient_spans);

  for (const int index : IndexRange(num_constraints)) {
    const ValueT compliance = ValueT(1.0f) + alphas[index];

    const IndexRange component_columns = components_range.slice(index * num_components,
                                                                num_components);
    /* Compliance entries. */
    for (const int u : component_columns.index_range()) {
      triplets.append_unchecked_as(
          int(component_columns[u]), int(component_columns[u]), get_component(compliance, u));
    }
  }

  /* Gradient entries. */
  for (const int var_i : IndexRange(num_position_vars)) {
    const Span<int> indices = position_index_arrays[var_i];
    const Span<PosGradT> gradients = position_gradient_spans[var_i].typed<PosGradT>();

    for (const int index : IndexRange(num_constraints)) {
      const int point_index = indices[index];
      if (!IndexRange(num_positions).contains(point_index)) {
        continue;
      }
      const PosGradT gradient = gradients[index];

      const IndexRange component_columns = components_range.slice(index * num_components,
                                                                  num_components);
      const IndexRange position_columns = positions_range.slice(point_index * 3, 3);
      /* Upper-right corner contains gradients. */
      for (const int u : component_columns.index_range()) {
        for (const int v : position_columns.index_range()) {
          triplets.append_unchecked_as(
              int(component_columns[u]), int(position_columns[v]), get_component(compliance, u));
        }
      }
      /* Lower-left corner contains gradients with the damping factor applied. */
      for (const int u : component_columns.index_range()) {
        for (const int v : position_columns.index_range()) {
          triplets.append_unchecked_as(
              int(component_columns[u]), int(position_columns[v]), get_component(compliance, u));
        }
      }
    }
  }

  static void set_global_solve_elements(const ConstraintEvalParams &params,
                                        const ConstraintVariables &variables,
                                        const IndexRange positions_range,
                                        const IndexRange rotations_range,
                                        const IndexRange components_range,
                                        const ConstraintEvalData &data,
                                        const int num_components,
                                        Vector<Eigen::Triplet<float>> &triplets)
  {
    /* XXX Matrix types float2x3, float3x3, float2x4, float 3x4 have no registered CPPType, so a
     * larger type is used for output arrays. */
    switch (num_components) {
      case 1:
        set_global_solve_elements<float, float3, float4>(
            params, variables, positions_range, rotations_range, components_range, data, triplets);
        break;
      case 2:
        set_global_solve_elements<float2, float4x4, float4x4>(
            params, variables, positions_range, rotations_range, components_range, data, triplets);
        break;
      case 3:
        set_global_solve_elements<float3, float4x4, float4x4>(
            params, variables, positions_range, rotations_range, components_range, data, triplets);
        break;
      default:
        BLI_assert_unreachable();
        break;
    }
  }

  static Eigen::SparseMatrix<float> build_global_solve_matrix_from_triplets(
      const ConstraintEvalParams &params,
      const Span<ConstraintEvalData> constraint_data,
      const ConstraintVariables &variables,
      const Span<VariableIndexArrays> position_indices_by_type,
      const Span<VariableIndexArrays> rotation_indices_by_type,
      const int num_non_zeroes,
      const int num_rows,
      const int num_columns)
  {
    const int num_positions = variables.positions.size();
    const int num_rotations = variables.rotations.size();

    Eigen::SparseMatrix<float> H(num_rows, num_columns);

    const IndexRange positions_range = {0, num_positions * 3};
    const IndexRange rotations_range = positions_range.after(num_rotations * 4);

    Vector<Eigen::Triplet<float>> triplets;
    triplets.reserve(num_non_zeroes);

    /* Mass entries. */
    for (const int index : IndexRange(num_positions)) {
      const float mass = params.masses[index];

      const IndexRange columns = positions_range.slice(index * 3, 3);
      for (const int u : columns.index_range()) {
        triplets.append_unchecked_as(int(columns[u]), int(columns[u]), mass);
      }
    }
    for (const int index : IndexRange(num_rotations)) {
      const float3 local_inertia = params.local_inertia[index];
      const math::Quaternion global_inertia = variables.rotations[index] *
                                              math::Quaternion(0.0f, local_inertia);
      const float4x4 inertia_tensor = quaternion_matrix(global_inertia);

      const IndexRange columns = rotations_range.slice(index * 4, 4);
      for (const int u : columns.index_range()) {
        for (const int v : columns.index_range()) {
          triplets.append_unchecked_as(int(columns[v]), int(columns[u]), inertia_tensor[u][v]);
        }
      }
    }

    IndexRange prev_range = rotations_range;
    for (const ConstraintEvalData &data : constraint_data) {
      if (!data.type->linear_solve_size || !data.type->linear_solve_elements) {
        continue;
      }
      const int num_constraints = data.constraints.size();

      int num_components, num_position_vars, num_rotation_vars;
      data.type->linear_solve_size(num_components, num_position_vars, num_rotation_vars);

      // const GeometryComponent &component =
      // *data.geometry->get_component<PointCloudComponent>(); const AttributeAccessor attributes
      // = *component.attributes();

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

      // /* Convert constraint elements to Eigen triplets for matrix construction. */
      const IndexRange components_range = prev_range.after(num_constraints * num_components);

      set_global_solve_elements(params,
                                variables,
                                positions_range,
                                rotations_range,
                                components_range,
                                data,
                                num_components,
                                triplets);

      // for (const int index : IndexRange(num_constraints)) {
      //   const float compliance_damping = 1.0f + alphas[index];

      //   const IndexRange component_columns = components_range.slice(index * num_components,
      //                                                               num_components);
      //   for (const int u : component_columns.index_range()) {
      //     triplets.append_unchecked_as(
      //         int(component_columns[u]), int(component_columns[u]), compliance_damping);
      //   }

      //   // for (const int u : columns.index_range()) {
      //   //   for (const int v : columns.index_range()) {
      //   //     triplets.append_unchecked_as(int(columns[v]), int(columns[u]),
      //   inertia_tensor[u][v]);
      //   //   }
      //   // }
      //   // triplets.append_unchecked_as();
      // }

      prev_range = components_range;
    }

    H.setFromTriplets(triplets.begin(), triplets.end());

    return H;
  }

  template<bool debug_check>
  static void do_build_global_solve_system(const ConstraintEvalParams &params,
                                           MutableSpan<ConstraintEvalData> constraint_data,
                                           ConstraintVariables &variables,
                                           Eigen::SparseMatrix<float> &r_H,
                                           Eigen::VectorXf &r_b)
  {
    const int num_positions = variables.positions.size();
    const int num_rotations = variables.rotations.size();

    int num_non_zeroes, num_rows, num_columns;
    count_global_solve_matrix_entries(
        constraint_data, variables, num_non_zeroes, num_rows, num_columns);

    /* Read constraint topology.
     * Constraints can use up to 4 variables, any extra arrays remain empty. */
    Array<VariableIndexArrays> position_indices_by_type(constraint_data.size());
    Array<VariableIndexArrays> rotation_indices_by_type(constraint_data.size());
    read_constraint_topology(constraint_data, position_indices_by_type, rotation_indices_by_type);
    if constexpr (debug_check) {
      debug_check_constraint_topology(params,
                                      constraint_data,
                                      num_positions,
                                      num_rotations,
                                      position_indices_by_type,
                                      rotation_indices_by_type);
    }

    /* LHS matrix describing equations of motion and constraint impulses. */
    Eigen::SparseMatrix<float> H;
    if (false) {
      H = build_global_solve_matrix_from_spans(params,
                                               constraint_data,
                                               variables,
                                               position_indices_by_type,
                                               rotation_indices_by_type,
                                               num_non_zeroes,
                                               num_rows,
                                               num_columns);
    }
    else {
      H = build_global_solve_matrix_from_triplets(params,
                                                  constraint_data,
                                                  variables,
                                                  position_indices_by_type,
                                                  rotation_indices_by_type,
                                                  num_non_zeroes,
                                                  num_rows,
                                                  num_columns);
    }

    Eigen::VectorXf b;
    b.resize(num_columns);
    // TODO set b entries

    r_H = std::move(H);
    r_b = std::move(b);
  }

  void build_global_solve_system(const ConstraintEvalParams &params,
                                 MutableSpan<ConstraintEvalData> constraint_data,
                                 ConstraintVariables &variables,
                                 const bool debug_check,
                                 Eigen::SparseMatrix<float> &r_H,
                                 Eigen::VectorXf &r_b)
  {
    if (debug_check) {
      do_build_global_solve_system<true>(params, constraint_data, variables, r_H, r_b);
    }
    else {
      do_build_global_solve_system<false>(params, constraint_data, variables, r_H, r_b);
    }
  }

  /** \} */

}  // namespace blender::nodes::xpbd_constraints
