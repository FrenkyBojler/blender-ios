/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"

#include "BKE_attribute.hh"
#include "BKE_geometry_set.hh"
#include "BKE_instances.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "NOD_geometry_nodes_bundle.hh"
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

namespace blender::nodes::node_geo_solve_xpbd_constraints_cc {

using xpbd_constraints::ConstraintEvalData;
using xpbd_constraints::ConstraintEvalParams;
using xpbd_constraints::ConstraintTypeInfo;
using xpbd_constraints::ConstraintVariables;

constexpr float default_fps = 1.0f / 25.0f;

enum class EvaluationTarget {
  Positions,
  Velocities,
};

enum class SolverMethod {
  /* Kugelstadt 2016: pure GS  */
  /* Mueller 2016(?): GS + Jacobi */
  /* Deul/Kugelstadt 2018: Global + GS */
  /* Soler 2018: PD */
  /* Ly 2020: PD + Local */
  /* Daviet 2023: ADMM */
  GaussSeidel,
  Jacobi,
  GaussSeidelJacobi,
  ProjectiveDynamics,
  ADMM,
};

static const EnumPropertyItem rna_enum_solver_method_items[] = {
    {int(SolverMethod::GaussSeidel),
     "GAUSS_SEIDEL",
     0,
     "Gauss-Seidel",
     "Solves constraints sequentially in independent groups over multiple iterations"},
    {int(SolverMethod::Jacobi),
     "JACOBI",
     0,
     "Jacobi",
     "Solves constraints in parallel and averages the solutions"},
    {int(SolverMethod::GaussSeidelJacobi),
     "GAUSS_SEIDEL_JACOBI",
     0,
     "Gauss-Seidel/Jacobi",
     "Combination of Gauss-Seidel and Jacobi methods"},
    {int(SolverMethod::ProjectiveDynamics),
     "PROJECTIVE_DYNAMICS",
     0,
     "Projective Dynamics",
     "Combined local projection with a single-iteration linear solver, does not support hard "
     "constraints"},
    {int(SolverMethod::ADMM),
     "ADMM",
     0,
     "ADMM",
     "Alternating Direction Method of Multipliers, supports hard constraints"},
    {0, nullptr, 0, nullptr, nullptr},
};

enum class ConstraintInit {
  /* Zero initialize lambda and compute change from current positions. */
  ZeroInit,
  /* Warm start: Use previous lambda as initial step. */
  WarmStart,
};

static bool needs_gauss_seidel_iterations(const SolverMethod solver_method)
{
  return ELEM(solver_method, SolverMethod::GaussSeidel, SolverMethod::GaussSeidelJacobi);
}

static bool needs_jacobi_iterations(const SolverMethod solver_method)
{
  return ELEM(solver_method, SolverMethod::Jacobi, SolverMethod::GaussSeidelJacobi);
}

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();

  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_input<decl::Float>("Delta Time").default_value(default_fps).min(0.0f).hide_value();

  if (node != nullptr) {
    const SolverMethod solver_method = SolverMethod(node->custom1);
    b.add_default_layout();

    if (needs_gauss_seidel_iterations(solver_method)) {
      b.add_input<decl::Int>("Gauss-Seidel Iterations").default_value(5).min(0);
    }
    if (needs_jacobi_iterations(solver_method)) {
      b.add_input<decl::Int>("Jacobi Iterations").default_value(5).min(0);
    }
  }

  b.add_input<decl::Bool>("Warm Start")
      .default_value(false)
      .description("Use previous lambda value when initializing instead of starting from zero");

  const int geometry_in = b.add_input<decl::Geometry>("Geometry").index();
  const int geometry_out = b.add_output<decl::Geometry>("Geometry").align_with_previous().index();

  b.add_input<decl::Vector>("Old Position").field_on({geometry_in}).hide_value();
  b.add_input<decl::Rotation>("Old Rotation").field_on({geometry_in}).hide_value();
  b.add_input<decl::Vector>("Position")
      .implicit_field_on(NODE_DEFAULT_INPUT_POSITION_FIELD, {geometry_in});
  b.add_output<decl::Vector>("Position").field_on({geometry_out}).align_with_previous();
  b.add_input<decl::Rotation>("Rotation").field_on({geometry_in}).hide_value();
  b.add_output<decl::Rotation>("Rotation").field_on({geometry_out}).align_with_previous();
  b.add_input<decl::Vector>("Velocity").field_on({geometry_in}).hide_value();
  b.add_output<decl::Vector>("Velocity").field_on({geometry_out}).align_with_previous();
  b.add_input<decl::Vector>("Angular Velocity").field_on({geometry_in}).hide_value();
  b.add_output<decl::Vector>("Angular Velocity").field_on({geometry_out}).align_with_previous();

  b.add_input<decl::Float>("Mass")
      .default_value(1.0f)
      .field_on({geometry_in})
      .description("Linear inertial mass");
  b.add_input<decl::Vector>("Inertia")
      .default_value(float3(1.0f))
      .field_on({geometry_in})
      .description("Principal moments of inertia");

  b.add_input<decl::Bundle>("Constraints").description("Bundle of constraint geometries");
  b.add_output<decl::Bundle>("Constraints")
      .description("Bundle of constraint geometries")
      .align_with_previous();

  b.add_input<decl::Geometry>("Colliders")
      .only_instances()
      .description("Instances of colliders to evaluate contact transforms");

  PanelDeclarationBuilder &debug_panel = b.add_panel("Debug").default_closed(true);
  debug_panel.add_input<decl::Bool>("Debug Checks")
      .default_value(false)
      .description("Perform checks on input data, which can impact performance");
  debug_panel.add_input<decl::Geometry>("Debug Steps")
      .description("Complete constraint and geometry information for each solver iteration");
  debug_panel.add_output<decl::Geometry>("Debug Steps")
      .description("Complete constraint and geometry information for each solver iteration")
      .align_with_previous();
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiLayoutSetPropSep(layout, true);
  uiLayoutSetPropDecorate(layout, false);
  layout->prop(ptr, "solver_method", UI_ITEM_NONE, "", ICON_NONE);
}

static void output_solver_matrix(const Eigen::SparseMatrix<float> &matrix,
                                 const Eigen::VectorXf &target,
                                 const Eigen::VectorXf &solution)
{
  char filepath[FILENAME_MAX] = "//system_matrix.txt";
  BLI_path_abs(filepath, BKE_main_blendfile_path(G.main));

  /* Check if file write permission is ok. */
  if (BLI_exists(filepath) && !BLI_file_is_writable(filepath)) {
    return;
  }

  fstream fs(filepath, std::ios_base::out);
  if (!fs) {
    return;
  }

  Eigen::IOFormat format;
  fs << "---------------- MATRIX ----------------" << std::endl;
  fs << matrix.toDense().format(format) << std::endl;
  fs << "---------------- TARGET ----------------" << std::endl;
  fs << target.format(format) << std::endl;
  fs << "---------------- SOLUTION ----------------" << std::endl;
  fs << solution.format(format) << std::endl;

  fs.close();
}

static void do_global_solve(const EvaluationTarget /*target*/,
                            const ConstraintEvalParams &eval_params,
                            MutableSpan<ConstraintEvalData> constraint_data,
                            ConstraintVariables &variables)
{
  constexpr bool debug_output = false;

  IndexMaskMemory memory;
  xpbd_constraints::GlobalSolverSystem system = build_global_solve_system(
      eval_params, constraint_data, variables, eval_params.debug_check, memory);

  Eigen::VectorXf solution;
  xpbd_constraints::SolverResult result = xpbd_constraints::solve_global_system(
      std::move(system), variables, constraint_data, debug_output ? &solution : nullptr);
  // BLI_assert(result == xpbd_constraints::SolverResult::Success);
  switch (result) {
    case xpbd_constraints::SolverResult::Success:
      /* Continue. */
      break;
    case xpbd_constraints::SolverResult::NumericalIssue:
      eval_params.error_message_add("Global Solver: Numerical Issue");
      return;
    case xpbd_constraints::SolverResult::NoConvergence:
      eval_params.error_message_add("Global Solver: No Convergence");
      return;
    case xpbd_constraints::SolverResult::InvalidInput:
      eval_params.error_message_add("Global Solver: Invalid Input");
      return;
  }

  if (debug_output) {
    const int max_rows = 200;
    const int max_cols = 200;

    const auto matrix_view = system.matrix.block(0,
                                                 0,
                                                 std::min(int(system.matrix.rows()), max_rows),
                                                 std::min(int(system.matrix.cols()), max_cols));
    const auto target_view = system.target.block(
        0, 0, std::min(int(system.target.rows()), max_cols), 1);
    const auto solution_view = solution.block(
        0, 0, std::min(int(system.target.rows()), max_cols), 1);
    // std::cout << matrix_view.toDense().format(format) << std::endl;
    output_solver_matrix(matrix_view, target_view, solution_view);
  }
}

static void do_gauss_seidel_iteration(const EvaluationTarget target,
                                      const ConstraintEvalParams &eval_params,
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
      switch (target) {
        case EvaluationTarget::Positions: {
          apply_gauss_seidel_positions_group(
              eval_params, *data.type, *data.geometry, group_mask, variables, indices, memory);
          break;
        }
        case EvaluationTarget::Velocities: {
          apply_gauss_seidel_velocities_group(
              eval_params, *data.type, *data.geometry, group_mask, variables, indices, memory);
          break;
        }
      }
    }
  }
}

static void do_jacobi_iteration(const EvaluationTarget target,
                                const ConstraintEvalParams &eval_params,
                                MutableSpan<ConstraintEvalData> constraint_data,
                                ConstraintVariables &variables)
{
  constexpr bool linearized_quaternion = true;

  IndexMaskMemory memory;

  Array<xpbd_constraints::VariableIndexArrays> indices_by_type(constraint_data.size());
  xpbd_constraints::read_constraint_topology(constraint_data, indices_by_type);

  switch (target) {
    case EvaluationTarget::Positions: {
      Array<int> position_weights(variables.positions.size(), 0);
      Array<int> rotation_weights(variables.rotations.size(), 0);
      Array<float3> point_delta_positions(variables.positions.size(), float3(0.0f));
      Array<float4> point_delta_rotations(variables.positions.size(), float4(0.0f));
      for (const int constraint_i : constraint_data.index_range()) {
        ConstraintEvalData &data = constraint_data[constraint_i];
        if (data.geometry) {
          add_jacobi_position_deltas(eval_params,
                                     *data.type,
                                     *data.geometry,
                                     data.constraints,
                                     variables,
                                     indices_by_type[constraint_i],
                                     point_delta_positions,
                                     point_delta_rotations,
                                     position_weights,
                                     rotation_weights,
                                     memory);
        }
      }

      threading::parallel_for(
          variables.positions.index_range(), 4096, [&](const IndexRange range) {
            for (const int point : range) {
              const int weight = position_weights[point];
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
      Array<int> position_weights(variables.positions.size(), 0);
      Array<int> rotation_weights(variables.rotations.size(), 0);
      Array<float3> point_delta_velocities(variables.positions.size(), float3(0.0f));
      Array<float3> point_delta_angular_velocities(variables.positions.size(), float3(0.0f));
      for (const int constraint_i : constraint_data.index_range()) {
        ConstraintEvalData &data = constraint_data[constraint_i];
        if (data.geometry) {
          add_jacobi_velocity_deltas(eval_params,
                                     *data.type,
                                     *data.geometry,
                                     data.constraints,
                                     variables,
                                     indices_by_type[constraint_i],
                                     point_delta_velocities,
                                     point_delta_angular_velocities,
                                     position_weights,
                                     rotation_weights,
                                     memory);
        }
      }

      threading::parallel_for(
          variables.rotations.index_range(), 4096, [&](const IndexRange range) {
            for (const int point : range) {
              const int weight = rotation_weights[point];
              if (weight > 0) {
                const float norm = 1.0f / float(weight);
                const float3 delta_vel = point_delta_velocities[point] * norm;
                const float3 delta_angvel = point_delta_angular_velocities[point] * norm;
                xpbd_constraints::apply_velocity_impulse(delta_vel, variables.velocities[point]);
                xpbd_constraints::apply_angular_velocity_impulse(
                    delta_angvel, variables.angular_velocities[point]);
              }
            }
          });
      break;
    }
  }
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

static void init_constraints(const ConstraintInit init_mode,
                             const ConstraintEvalParams &eval_params,
                             MutableSpan<ConstraintEvalData> constraint_data)
{
  switch (init_mode) {
    case ConstraintInit::ZeroInit:
      zero_init_solver(constraint_data);
      break;
    case ConstraintInit::WarmStart:
      warm_start_solver(eval_params, constraint_data);
      break;
  }
}

static void execute_solver_method_on_geometry(const SolverMethod method,
                                              ConstraintEvalParams &eval_params,
                                              MutableSpan<ConstraintEvalData> constraint_data,
                                              ConstraintVariables &variables,
                                              const int gauss_seidel_iterations,
                                              const int jacobi_iterations)
{
  Array<float3> orig_velocities;
  Array<float3> orig_angular_velocities;

  switch (method) {
    case SolverMethod::GaussSeidel: {
      if (eval_params.debug_recorder) {
        const std::string label = fmt::format("Initialize Gauss-Seidel, ");
        eval_params.debug_recorder->record_step(label, nullptr, -1, {}, variables);
      }

      for ([[maybe_unused]] const int i : IndexRange(gauss_seidel_iterations)) {
        do_gauss_seidel_iteration(
            EvaluationTarget::Positions, eval_params, constraint_data, variables);
      }

      estimate_velocity(eval_params, orig_velocities, orig_angular_velocities, variables);

      do_gauss_seidel_iteration(
          EvaluationTarget::Velocities, eval_params, constraint_data, variables);
      break;
    }

    case SolverMethod::Jacobi: {
      if (eval_params.debug_recorder) {
        const std::string label = fmt::format("Initialize Jacobi, ");
        eval_params.debug_recorder->record_step(label, nullptr, -1, {}, variables);
      }

      for ([[maybe_unused]] const int i : IndexRange(jacobi_iterations)) {
        do_jacobi_iteration(EvaluationTarget::Positions, eval_params, constraint_data, variables);
      }

      estimate_velocity(eval_params, orig_velocities, orig_angular_velocities, variables);

      do_gauss_seidel_iteration(
          EvaluationTarget::Velocities, eval_params, constraint_data, variables);
      break;
    }

    case SolverMethod::GaussSeidelJacobi: {
      if (eval_params.debug_recorder) {
        const std::string label = fmt::format("Initialize Gauss-Seidel/Jacobi, ");
        eval_params.debug_recorder->record_step(label, nullptr, -1, {}, variables);
      }

      for ([[maybe_unused]] const int i : IndexRange(gauss_seidel_iterations)) {
        do_gauss_seidel_iteration(
            EvaluationTarget::Positions, eval_params, constraint_data, variables);
      }
      for ([[maybe_unused]] const int i : IndexRange(jacobi_iterations)) {
        do_jacobi_iteration(EvaluationTarget::Positions, eval_params, constraint_data, variables);
      }

      estimate_velocity(eval_params, orig_velocities, orig_angular_velocities, variables);

      do_gauss_seidel_iteration(
          EvaluationTarget::Velocities, eval_params, constraint_data, variables);
      break;
    }

    case SolverMethod::ProjectiveDynamics: {
      if (eval_params.debug_recorder) {
        const std::string label = fmt::format("Initialize Projective Dynamics, ");
        eval_params.debug_recorder->record_step(label, nullptr, -1, {}, variables);
      }

      do_global_solve(EvaluationTarget::Positions, eval_params, constraint_data, variables);

      estimate_velocity(eval_params, orig_velocities, orig_angular_velocities, variables);

      do_gauss_seidel_iteration(
          EvaluationTarget::Velocities, eval_params, constraint_data, variables);
      break;
    }

    case SolverMethod::ADMM: {
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
    params.error_message_add(NodeWarningType::Warning, message);
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
      const VArray<int> solver_groups = *attributes.lookup_or_default<int>(
          "solver_group", AttrDomain::Point, 0);

      IndexMask constraints_mask = IndexRange(attributes.domain_size(AttrDomain::Point));
      Vector<IndexMask> group_masks = build_group_masks(
          constraints_mask, std::move(solver_groups), memory);
      constraint_data[i].geometry = geometry_set;
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
  const SolverMethod solver_method = SolverMethod(params.node().custom1);
  const int gauss_seidel_iterations = needs_gauss_seidel_iterations(solver_method) ?
                                          std::max(
                                              params.extract_input<int>("Gauss-Seidel Iterations"),
                                              0) :
                                          0;
  const int jacobi_iterations = needs_jacobi_iterations(solver_method) ?
                                    std::max(params.extract_input<int>("Jacobi Iterations"), 0) :
                                    0;
  ConstraintInit init_mode = params.extract_input<bool>("Warm Start") ? ConstraintInit::WarmStart :
                                                                        ConstraintInit::ZeroInit;
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");
  Field<float> mass_field = params.extract_input<Field<float>>("Mass");
  Field<float3> inertia_field = params.extract_input<Field<float3>>("Inertia");
  Field<float3> old_position_field = params.extract_input<Field<float3>>("Old Position");
  Field<math::Quaternion> old_rotation_field = params.extract_input<Field<math::Quaternion>>(
      "Old Rotation");
  Field<float3> position_field = params.extract_input<Field<float3>>("Position");
  Field<math::Quaternion> rotation_field = params.extract_input<Field<math::Quaternion>>(
      "Rotation");
  Field<float3> velocity_field = params.extract_input<Field<float3>>("Velocity");
  Field<float3> angular_velocity_field = params.extract_input<Field<float3>>("Angular Velocity");
  std::optional<std::string> position_output_id =
      params.get_output_anonymous_attribute_id_if_needed("Position");
  std::optional<std::string> rotation_output_id =
      params.get_output_anonymous_attribute_id_if_needed("Rotation");
  std::optional<std::string> velocity_output_id =
      params.get_output_anonymous_attribute_id_if_needed("Velocity");
  std::optional<std::string> angular_velocity_output_id =
      params.get_output_anonymous_attribute_id_if_needed("Angular Velocity");

  GeometrySet colliders_geometry_set = params.extract_input<GeometrySet>("Colliders");
  Span<float4x4> collider_transforms = colliders_geometry_set.has_instances() ?
                                           colliders_geometry_set.get_instances()->transforms() :
                                           Span<float4x4>{};

  ConstraintEvalParams eval_params = extract_eval_params(params);
  const bool debug_output = (eval_params.debug_recorder != nullptr);

  Vector<ConstraintEvalData> constraint_data;
  IndexMaskMemory memory;
  get_constraint_data(params, debug_output, constraint_data, memory);

  init_constraints(init_mode, eval_params, constraint_data);

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
        evaluator.add(mass_field);
        evaluator.add(inertia_field);
        evaluator.add(old_position_field);
        evaluator.add(old_rotation_field);
        evaluator.add_with_destination(position_field, vars.positions.as_mutable_span());
        evaluator.add_with_destination(rotation_field, vars.rotations.as_mutable_span());
        evaluator.add_with_destination(velocity_field, vars.velocities.as_mutable_span());
        evaluator.add_with_destination(angular_velocity_field,
                                       vars.angular_velocities.as_mutable_span());
        evaluator.evaluate();
        eval_params.masses = evaluator.get_evaluated<float>(0);
        eval_params.local_inertia = evaluator.get_evaluated<float3>(1);
        eval_params.old_positions = evaluator.get_evaluated<float3>(2);
        eval_params.old_rotations = evaluator.get_evaluated<math::Quaternion>(3);

        eval_params.collider_transforms = collider_transforms;
        /* XXX Transforms of the previous frame are not currently available, these are always the
         * same as the current frame. Eventually this will allow transfer of velocity from animated
         * colliders. */
        eval_params.old_collider_transforms = eval_params.collider_transforms;

        execute_solver_method_on_geometry(solver_method,
                                          eval_params,
                                          constraint_data,
                                          vars,
                                          gauss_seidel_iterations,
                                          jacobi_iterations);

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

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(srna,
                    "solver_method",
                    "Solver Method",
                    "Method to use for solving constraints",
                    rna_enum_solver_method_items,
                    NOD_inline_enum_accessors(custom1),
                    int(SolverMethod::GaussSeidel));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSolveConstraints");
  ntype.ui_name = "Solve XPBD Constraints";
  ntype.ui_description =
      "Solve position and rotation constraints on geometry using the XPBD framework";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  node_type_size(ntype, 200, 120, 300);
  ntype.draw_buttons = node_layout;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_solve_xpbd_constraints_cc
