/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute.hh"
#include "BKE_curves.hh"
#include "BKE_geometry_set.hh"
#include "BKE_pointcloud.hh"

#include "NOD_xpbd_constraints.hh"

#include "node_geometry_util.hh"

#include <fmt/format.h>

namespace blender::nodes::xpbd_constraints {

constexpr GrainSize constraint_grain_size = GrainSize(1024);

template<typename T>
static AttributeReader<T> lookup_or_warn(const AttributeAccessor &attributes,
                                         const StringRef attribute_id,
                                         const AttrDomain domain,
                                         const T &default_value,
                                         ConstraintEvalParams::ErrorFn error_fn)
{
  if (!attributes.contains(attribute_id)) {
    error_fn(fmt::format("Missing \"{}\" attribute", attribute_id));
  }
  return attributes.lookup_or_default<T>(attribute_id, domain, default_value);
}

/* Constraint attributes. */
constexpr StringRef ATTR_SOLVER_GROUP = "solver_group";
constexpr StringRef ATTR_ALPHA = "compliance";
constexpr StringRef ATTR_BETA = "damping";
constexpr StringRef ATTR_POINT1 = "point1";
constexpr StringRef ATTR_POINT2 = "point2";
constexpr StringRef ATTR_ACTIVE = "active";
constexpr StringRef ATTR_LAST_ACTIVE = "last_active";

inline float trace(const float3 &v)
{
  return v.x + v.y + v.z;
}

namespace position_goal {

static void get_size(int &r_num_components,
                     int &r_num_position_vars,
                     int &r_num_rotation_vars,
                     bool &r_use_active_mask)
{
  r_num_components = 1;
  r_num_position_vars = 1;
  r_num_rotation_vars = 0;
  r_use_active_mask = false;
}

static void get_variable_indices(const bke::AttributeAccessor &attributes,
                                 const IndexMask &selection,
                                 MutableSpan<int> r_position_indices[4],
                                 MutableSpan<int> /*r_rotation_indices*/[4])
{
  const VArraySpan<int> points = *attributes.lookup_or_default<int>(
      ATTR_POINT1, AttrDomain::Point, 0);

  MutableSpan<int> position_indices = r_position_indices[0];
  selection.foreach_index(constraint_grain_size, [&](const int index, const int pos) {
    position_indices[pos] = points[index];
  });
}

static void init_step(bke::GeometrySet &constraints)
{
  PointCloudComponent &component = constraints.get_component_for_write<PointCloudComponent>();
  std::optional<bke::MutableAttributeAccessor> attributes = component.attributes_for_write();

  SpanAttributeWriter<float> lambda_writer = attributes->lookup_or_add_for_write_span<float>(
      "lambda", AttrDomain::Point);

  lambda_writer.span.fill(0.0f);

  lambda_writer.finish();
}

template<bool debug_output>
static void eval_positions(const ConstraintEvalParams &params,
                           const ConstraintVariables &variables,
                           const IndexMask &group_mask,
                           bke::GeometrySet &constraints,
                           VArray<bool> &r_active,
                           Vector<VArray<float3>> &r_delta_positions,
                           Vector<VArray<float4>> &r_delta_rotations)
{
  constexpr bool use_damping = true;

  PointCloudComponent &component = constraints.get_component_for_write<PointCloudComponent>();
  std::optional<bke::MutableAttributeAccessor> attributes = component.attributes_for_write();

  VArraySpan<int> points = *lookup_or_warn<int>(
      *attributes, ATTR_POINT1, AttrDomain::Point, 0, params.error_message_add);
  VArraySpan<float> alphas = *attributes->lookup_or_default<float>(
      ATTR_ALPHA, AttrDomain::Point, 0.0f);
  VArraySpan<float> betas = *attributes->lookup_or_default<float>(
      ATTR_BETA, AttrDomain::Point, 0.0f);
  VArraySpan<float3> goal_positions = *lookup_or_warn<float3>(
      *attributes, "goal_position", AttrDomain::Point, float3(0.0f), params.error_message_add);
  SpanAttributeWriter<float> lambda_writer = attributes->lookup_or_add_for_write_span<float>(
      "lambda", AttrDomain::Point);
  SpanAttributeWriter<float3> delta_position_writer =
      attributes->lookup_or_add_for_write_span<float3>("delta_position", AttrDomain::Point);
  SpanAttributeWriter<float> residual_writer;
  if constexpr (debug_output) {
    residual_writer = attributes->lookup_or_add_for_write_span<float>("residual",
                                                                      AttrDomain::Point);
  }
  else {
    UNUSED_VARS(residual_writer);
  }

  const IndexRange points_range = variables.positions.index_range();
  const Span<float3> positions = variables.positions;
  const Span<float3> old_positions = params.old_positions;

  group_mask.foreach_index(constraint_grain_size, [&](const int index) {
    const int point = points[index];
    if (!points_range.contains(point)) {
      return;
    }
    const float3 &goal = goal_positions[index];
    float &lambda = lambda_writer.span[index];
    float3 &delta_position = delta_position_writer.span[index];

    float residual, delta_lambda;
    if constexpr (use_damping) {
      const float alpha = alphas[index] * params.inv_delta_time_squared;
      const float gamma = alphas[index] * betas[index] * params.inv_delta_time;
      xpbd_constraints::eval_position_goal(goal,
                                           alpha,
                                           gamma,
                                           lambda,
                                           positions[point],
                                           old_positions[point],
                                           residual,
                                           delta_lambda,
                                           delta_position);
    }
    else {
      const float alpha = alphas[index] * params.inv_delta_time_squared;
      xpbd_constraints::eval_position_goal(goal,
                                           alpha,
                                           0.0f,
                                           lambda,
                                           positions[point],
                                           float3(0.0f),
                                           residual,
                                           delta_lambda,
                                           delta_position);
    }

    lambda += delta_lambda;
    if constexpr (debug_output) {
      residual_writer.span[index] = residual;
    }
  });

  lambda_writer.finish();
  delta_position_writer.finish();
  if constexpr (debug_output) {
    residual_writer.finish();
  }

  r_active = VArray<bool>::ForSingle(true, attributes->domain_size(AttrDomain::Point));
  r_delta_positions = {*attributes->lookup<float3>("delta_position", AttrDomain::Point)};
  r_delta_rotations = {{}};
}

static void linear_solve_elements(const ConstraintEvalParams &params,
                                  const ConstraintVariables &variables,
                                  const bke::AttributeAccessor &attributes,
                                  const IndexMask &selection,
                                  GMutableSpan r_alphas,
                                  GMutableSpan r_betas,
                                  GMutableSpan r_residuals,
                                  GMutableSpan r_position_gradients[4],
                                  GMutableSpan /*r_rotation_gradients*/[4],
                                  MutableSpan<bool> /*r_active*/)
{
  // r_alphas = GField(AttributeFieldInput::Create(ATTR_ALPHA, CPPType::get<float>()));
  // r_betas = GField(AttributeFieldInput::Create(ATTR_BETA, CPPType::get<float>()));

  // GField goal_positions = AttributeFieldInput::Create(ATTR_BETA, CPPType::get<float>());

  // static auto elements_fn = mf::build::SI2_SO2<float3, float3, float, float3>(
  //     "XPBD Position Goal Elements",
  //     eval_position_goal_elements,
  //     mf::build::exec_presets::Materialized());
  // r_residuals = GField(
  //     FieldOperation::Create(elements_fn, {std::move(goal_positions), variables.positions}));

  const VArraySpan<int> points = *lookup_or_warn<int>(
      attributes, ATTR_POINT1, AttrDomain::Point, 0, params.error_message_add);
  const VArraySpan<float> alphas_attr = *attributes.lookup_or_default<float>(
      ATTR_ALPHA, AttrDomain::Point, 0.0f);
  const VArraySpan<float> betas_attr = *attributes.lookup_or_default<float>(
      ATTR_BETA, AttrDomain::Point, 0.0f);
  const VArraySpan<float3> goal_positions = *lookup_or_warn<float3>(
      attributes, "goal_position", AttrDomain::Point, float3(0.0f), params.error_message_add);

  const IndexRange points_range = variables.positions.index_range();
  const Span<float3> positions = variables.positions;
  MutableSpan<float> alphas = r_alphas.typed<float>();
  MutableSpan<float> betas = r_betas.typed<float>();
  MutableSpan<float> residuals = r_residuals.typed<float>();
  MutableSpan<float3> position_gradients = r_position_gradients[0].typed<float3>();

  selection.foreach_index(constraint_grain_size, [&](const int index, const int pos) {
    const int point = points[index];
    if (!points_range.contains(point)) {
      return;
    }
    const float3 &goal = goal_positions[index];

    alphas[pos] = alphas_attr[index];
    betas[pos] = betas_attr[index];
    xpbd_constraints::eval_position_goal_elements(
        goal, positions[point], residuals[pos], position_gradients[pos]);
  });
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Curves").supported_type(GeometryComponent::Type::Curve);
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();

  b.add_input<decl::Float>("Compliance").min(0.0f).field_on_all();
  b.add_input<decl::Float>("Damping").min(0.0f).field_on_all();
  b.add_input<decl::Vector>("Goal").field_on_all().description(
      "Target location of the constraint");

  b.add_output<decl::Geometry>("Constraints");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry = params.extract_input<GeometrySet>("Curves");
  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");

  const Field<float> compliance_field = params.extract_input<Field<float>>("Compliance");
  const Field<float> damping_field = params.extract_input<Field<float>>("Damping");
  const Field<float3> goal_field = params.extract_input<Field<float3>>("Goal");

  if (!geometry.has_curves()) {
    params.set_default_remaining_outputs();
    return;
  }

  const CurveComponent &component = *geometry.get_component<CurveComponent>();
  bke::GeometryFieldContext context{component, AttrDomain::Point};
  fn::FieldEvaluator evaluator{context, component.attribute_domain_size(AttrDomain::Point)};
  evaluator.set_selection(selection_field);
  evaluator.add(compliance_field);
  evaluator.add(damping_field);
  evaluator.add(goal_field);
  evaluator.evaluate();

  const IndexMask selection = evaluator.get_evaluated_selection_as_mask();
  VArray<float> compliance = evaluator.get_evaluated<float>(0);
  VArray<float> damping = evaluator.get_evaluated<float>(1);
  VArray<float3> goal_position = evaluator.get_evaluated<float3>(2);

  PointCloud *points = BKE_pointcloud_new_nomain(selection.size());
  MutableAttributeAccessor attributes = points->attributes_for_write();
  SpanAttributeWriter<int> output_point1 = attributes.lookup_or_add_for_write_only_span<int>(
      ATTR_POINT1, AttrDomain::Point);
  SpanAttributeWriter<float> output_compliance =
      attributes.lookup_or_add_for_write_only_span<float>(ATTR_ALPHA, AttrDomain::Point);
  SpanAttributeWriter<float> output_damping = attributes.lookup_or_add_for_write_only_span<float>(
      ATTR_BETA, AttrDomain::Point);
  SpanAttributeWriter<float3> output_goal_position =
      attributes.lookup_or_add_for_write_only_span<float3>("goal_position", AttrDomain::Point);
  SpanAttributeWriter<int> output_solver_group = attributes.lookup_or_add_for_write_only_span<int>(
      ATTR_SOLVER_GROUP, AttrDomain::Point);

  selection.to_indices(output_point1.span);
  compliance.materialize_compressed(selection, output_compliance.span);
  damping.materialize_compressed(selection, output_damping.span);
  goal_position.materialize_compressed(selection, output_goal_position.span);
  output_solver_group.span.fill(0);

  output_point1.finish();
  output_compliance.finish();
  output_damping.finish();
  output_goal_position.finish();
  output_solver_group.finish();

  points->positions_for_write().fill(float3(0.0f));
  points->tag_positions_changed();

  params.set_output("Constraints", GeometrySet::from_pointcloud(points));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodePositionGoalConstraints");
  ntype.ui_name = "Position Goal Constraints";
  ntype.ui_description = "Define position goal constraints that move points to a given location";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  node_type_size(ntype, 200, 120, 300);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}

}  // namespace position_goal

namespace rotation_goal {

static void get_size(int &r_num_components,
                     int &r_num_position_vars,
                     int &r_num_rotation_vars,
                     bool &r_use_active_mask)
{
  r_num_components = 3;
  r_num_position_vars = 0;
  r_num_rotation_vars = 1;
  r_use_active_mask = false;
}

static void get_variable_indices(const bke::AttributeAccessor &attributes,
                                 const IndexMask &selection,
                                 MutableSpan<int> /*r_position_indices*/[4],
                                 MutableSpan<int> r_rotation_indices[4])
{
  const VArraySpan<int> points = *attributes.lookup_or_default<int>(
      ATTR_POINT1, AttrDomain::Point, 0);

  MutableSpan<int> rotation_indices = r_rotation_indices[0];
  selection.foreach_index(constraint_grain_size, [&](const int index, const int pos) {
    rotation_indices[pos] = points[index];
  });
}

static void init_step(bke::GeometrySet &constraints)
{
  PointCloudComponent &component = constraints.get_component_for_write<PointCloudComponent>();
  std::optional<bke::MutableAttributeAccessor> attributes = component.attributes_for_write();

  SpanAttributeWriter<float3> lambda_writer = attributes->lookup_or_add_for_write_span<float3>(
      "lambda", AttrDomain::Point);

  lambda_writer.span.fill(float3(0.0f));

  lambda_writer.finish();
}

template<bool debug_output>
static void eval_positions(const ConstraintEvalParams &params,
                           const ConstraintVariables &variables,
                           const IndexMask &group_mask,
                           bke::GeometrySet &constraints,
                           VArray<bool> &r_active,
                           Vector<VArray<float3>> &r_delta_positions,
                           Vector<VArray<float4>> &r_delta_rotations)
{
  constexpr bool linearized_quaternion = true;
  constexpr bool use_damping = true;

  PointCloudComponent &component = constraints.get_component_for_write<PointCloudComponent>();
  std::optional<bke::MutableAttributeAccessor> attributes = component.attributes_for_write();

  VArraySpan<int> points = *lookup_or_warn<int>(
      *attributes, ATTR_POINT1, AttrDomain::Point, 0, params.error_message_add);
  VArraySpan<float3> alphas = *attributes->lookup_or_default<float3>(
      ATTR_ALPHA, AttrDomain::Point, float3(0.0f));
  VArraySpan<float> betas = *attributes->lookup_or_default<float>(
      ATTR_BETA, AttrDomain::Point, 0.0f);
  VArraySpan<math::Quaternion> goal_rotations = *lookup_or_warn<math::Quaternion>(
      *attributes,
      "goal_rotation",
      AttrDomain::Point,
      math::Quaternion::identity(),
      params.error_message_add);
  SpanAttributeWriter<float3> lambda_writer = attributes->lookup_or_add_for_write_span<float3>(
      "lambda", AttrDomain::Point);
  SpanAttributeWriter<float> delta_rotation_w_writer =
      attributes->lookup_or_add_for_write_span<float>("delta_rotation_w", AttrDomain::Point);
  SpanAttributeWriter<float3> delta_rotation_xyz_writer =
      attributes->lookup_or_add_for_write_span<float3>("delta_rotation_xyz", AttrDomain::Point);
  SpanAttributeWriter<float3> residual_writer;
  if constexpr (debug_output) {
    residual_writer = attributes->lookup_or_add_for_write_span<float3>("residual",
                                                                       AttrDomain::Point);
  }
  else {
    UNUSED_VARS(residual_writer);
  }

  const IndexRange points_range = variables.positions.index_range();
  const Span<math::Quaternion> rotations = variables.rotations;
  const Span<math::Quaternion> old_rotations = params.old_rotations;

  group_mask.foreach_index(constraint_grain_size, [&](const int index) {
    const int point = points[index];
    if (!points_range.contains(point)) {
      return;
    }
    const math::Quaternion &goal = goal_rotations[index];
    float3 &lambda = lambda_writer.span[index];
    float &delta_rotation_w = delta_rotation_w_writer.span[index];
    float3 &delta_rotation_xyz = delta_rotation_xyz_writer.span[index];

    float3 residual;
    float3 delta_lambda;
    float4 delta_rotation;
    if constexpr (use_damping) {
      const float3 weight_rot = math::safe_rcp(params.local_inertia[point]);
      const float3 alpha = alphas[index] * params.inv_delta_time_squared;
      const float3 gamma = alphas[index] * betas[index] * params.inv_delta_time;
      xpbd_constraints::eval_rotation_goal2<linearized_quaternion>(weight_rot,
                                                                   goal,
                                                                   alpha,
                                                                   gamma,
                                                                   lambda,
                                                                   rotations[point],
                                                                   old_rotations[point],
                                                                   residual,
                                                                   delta_lambda,
                                                                   delta_rotation);
    }
    else {
      const float3 weight_rot = math::safe_rcp(params.local_inertia[point]);
      const float alpha = alphas[index] * params.inv_delta_time_squared;
      xpbd_constraints::eval_rotation_goal2<linearized_quaternion>(weight_rot,
                                                                   goal,
                                                                   alpha,
                                                                   0.0f,
                                                                   lambda,
                                                                   rotations[point],
                                                                   math::Quaternion::identity(),
                                                                   residual,
                                                                   delta_lambda,
                                                                   delta_rotation);
    }

    lambda += delta_lambda;
    delta_rotation_w = delta_rotation.x;
    delta_rotation_xyz = delta_rotation.yzw();
    if constexpr (debug_output) {
      residual_writer.span[index] = residual;
    }
  });

  lambda_writer.finish();
  delta_rotation_w_writer.finish();
  delta_rotation_xyz_writer.finish();
  if constexpr (debug_output) {
    residual_writer.finish();
  }

  r_active = VArray<bool>::ForSingle(true, attributes->domain_size(AttrDomain::Point));
  r_delta_positions = {{}};
  /* Attributes have to be stored separately as w/xyz, combine into a single VArray. */
  auto delta_rotation_fn =
      [delta_rotation_w = *attributes->lookup<float>("delta_rotation_w", AttrDomain::Point),
       delta_rotation_xyz = *attributes->lookup<float3>("delta_rotation_xyz", AttrDomain::Point)](
          const int64_t index) -> float4 {
    return float4(delta_rotation_w[index], delta_rotation_xyz[index]);
  };
  r_delta_rotations = {
      VArray<float4>::ForFunc(attributes->domain_size(AttrDomain::Point), delta_rotation_fn)};
}

static void linear_solve_elements(const ConstraintEvalParams &params,
                                  const ConstraintVariables &variables,
                                  const bke::AttributeAccessor &attributes,
                                  const IndexMask &selection,
                                  GMutableSpan r_alphas,
                                  GMutableSpan r_betas,
                                  GMutableSpan r_residuals,
                                  GMutableSpan /*r_position_gradients*/[4],
                                  GMutableSpan r_rotation_gradients[4],
                                  MutableSpan<bool> /*r_active*/)
{
  const VArraySpan<int> points = *lookup_or_warn<int>(
      attributes, ATTR_POINT1, AttrDomain::Point, 0, params.error_message_add);
  const VArraySpan<float3> alphas_attr = *attributes.lookup_or_default<float3>(
      ATTR_ALPHA, AttrDomain::Point, float3(0.0f));
  const VArraySpan<float3> betas_attr = *attributes.lookup_or_default<float3>(
      ATTR_BETA, AttrDomain::Point, float3(0.0f));
  const VArraySpan<math::Quaternion> goal_rotations = *lookup_or_warn<math::Quaternion>(
      attributes,
      "goal_rotation",
      AttrDomain::Point,
      math::Quaternion::identity(),
      params.error_message_add);

  const IndexRange points_range = variables.positions.index_range();
  const Span<math::Quaternion> rotations = variables.rotations;
  MutableSpan<float3> alphas = r_alphas.typed<float3>();
  MutableSpan<float3> betas = r_betas.typed<float3>();
  MutableSpan<float3> residuals = r_residuals.typed<float3>();
  MutableSpan<float4x4> rotation_gradients = r_rotation_gradients[0].typed<float4x4>();

  selection.foreach_index(constraint_grain_size, [&](const int index, const int pos) {
    const int point = points[index];
    if (!points_range.contains(point)) {
      return;
    }
    const math::Quaternion &goal = goal_rotations[index];

    alphas[pos] = alphas_attr[index];
    betas[pos] = betas_attr[index];
    xpbd_constraints::eval_rotation_goal_elements(
        goal, rotations[point], residuals[pos], rotation_gradients[pos]);
  });
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Curves").supported_type(GeometryComponent::Type::Curve);
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();

  b.add_input<decl::Float>("Compliance").min(0.0f).field_on_all();
  b.add_input<decl::Float>("Damping").min(0.0f).field_on_all();
  b.add_input<decl::Rotation>("Goal").field_on_all().description(
      "Target rotation of the constraint");

  b.add_output<decl::Geometry>("Constraints");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry = params.extract_input<GeometrySet>("Curves");
  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");

  const Field<float> compliance_field = params.extract_input<Field<float>>("Compliance");
  const Field<float> damping_field = params.extract_input<Field<float>>("Damping");
  const Field<math::Quaternion> goal_field = params.extract_input<Field<math::Quaternion>>("Goal");

  if (!geometry.has_curves()) {
    params.set_default_remaining_outputs();
    return;
  }

  const CurveComponent &component = *geometry.get_component<CurveComponent>();
  bke::GeometryFieldContext context{component, AttrDomain::Point};
  fn::FieldEvaluator evaluator{context, component.attribute_domain_size(AttrDomain::Point)};
  evaluator.set_selection(selection_field);
  evaluator.add(compliance_field);
  evaluator.add(damping_field);
  evaluator.add(goal_field);
  evaluator.evaluate();

  const IndexMask selection = evaluator.get_evaluated_selection_as_mask();
  const VArray<float> compliance = evaluator.get_evaluated<float>(0);
  const VArray<float> damping = evaluator.get_evaluated<float>(1);
  const VArray<math::Quaternion> goal_rotation = evaluator.get_evaluated<math::Quaternion>(2);

  PointCloud *points = BKE_pointcloud_new_nomain(selection.size());
  MutableAttributeAccessor attributes = points->attributes_for_write();
  SpanAttributeWriter<int> output_point1 = attributes.lookup_or_add_for_write_only_span<int>(
      ATTR_POINT1, AttrDomain::Point);
  SpanAttributeWriter<float> output_compliance =
      attributes.lookup_or_add_for_write_only_span<float>(ATTR_ALPHA, AttrDomain::Point);
  SpanAttributeWriter<float> output_damping = attributes.lookup_or_add_for_write_only_span<float>(
      ATTR_BETA, AttrDomain::Point);
  SpanAttributeWriter<math::Quaternion> output_goal_rotation =
      attributes.lookup_or_add_for_write_only_span<math::Quaternion>("goal_rotation",
                                                                     AttrDomain::Point);
  SpanAttributeWriter<int> output_solver_group = attributes.lookup_or_add_for_write_only_span<int>(
      ATTR_SOLVER_GROUP, AttrDomain::Point);

  selection.to_indices(output_point1.span);
  compliance.materialize_compressed(selection, output_compliance.span);
  damping.materialize_compressed(selection, output_damping.span);
  goal_rotation.materialize_compressed(selection, output_goal_rotation.span);
  output_solver_group.span.fill(0);

  output_point1.finish();
  output_compliance.finish();
  output_damping.finish();
  output_goal_rotation.finish();
  output_solver_group.finish();

  points->positions_for_write().fill(float3(0.0f));
  points->tag_positions_changed();

  params.set_output("Constraints", GeometrySet::from_pointcloud(points));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeRotationGoalConstraints");
  ntype.ui_name = "Rotation Goal Constraints";
  ntype.ui_description =
      "Define rotation goal constraints that align the rotation with a given orientation";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  node_type_size(ntype, 200, 120, 300);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}

}  // namespace rotation_goal

namespace stretch_shear {

static void get_size(int &r_num_components,
                     int &r_num_position_vars,
                     int &r_num_rotation_vars,
                     bool &r_use_active_mask)
{
  r_num_components = 3;
  r_num_position_vars = 2;
  r_num_rotation_vars = 1;
  r_use_active_mask = false;
}

static void get_variable_indices(const bke::AttributeAccessor &attributes,
                                 const IndexMask &selection,
                                 MutableSpan<int> r_position_indices[4],
                                 MutableSpan<int> r_rotation_indices[4])
{
  const VArraySpan<int> points1 = *attributes.lookup_or_default<int>(
      ATTR_POINT1, AttrDomain::Point, 0);
  const VArraySpan<int> points2 = *attributes.lookup_or_default<int>(
      ATTR_POINT2, AttrDomain::Point, 0);

  MutableSpan<int> position_indices1 = r_position_indices[0];
  MutableSpan<int> position_indices2 = r_position_indices[1];
  MutableSpan<int> rotation_indices = r_rotation_indices[0];
  selection.foreach_index(constraint_grain_size, [&](const int index, const int pos) {
    position_indices1[pos] = points1[index];
    position_indices2[pos] = points2[index];
    rotation_indices[pos] = points1[index];
  });
}

static void init_step(bke::GeometrySet &constraints)
{
  PointCloudComponent &component = constraints.get_component_for_write<PointCloudComponent>();
  std::optional<bke::MutableAttributeAccessor> attributes = component.attributes_for_write();

  SpanAttributeWriter<float3> lambda_writer = attributes->lookup_or_add_for_write_span<float3>(
      "lambda", AttrDomain::Point);

  lambda_writer.span.fill(float3(0.0f));

  lambda_writer.finish();
}

template<bool debug_output>
static void eval_positions(const ConstraintEvalParams &params,
                           const ConstraintVariables &variables,
                           const IndexMask &group_mask,
                           bke::GeometrySet &constraints,
                           VArray<bool> &r_active,
                           Vector<VArray<float3>> &r_delta_positions,
                           Vector<VArray<float4>> &r_delta_rotations)
{
  constexpr bool linearized_quaternion = true;
  constexpr bool use_damping = true;

  PointCloudComponent &component = constraints.get_component_for_write<PointCloudComponent>();
  std::optional<bke::MutableAttributeAccessor> attributes = component.attributes_for_write();

  VArraySpan<int> points1 = *lookup_or_warn<int>(
      *attributes, ATTR_POINT1, AttrDomain::Point, 0, params.error_message_add);
  VArraySpan<int> points2 = *lookup_or_warn<int>(
      *attributes, ATTR_POINT2, AttrDomain::Point, 0, params.error_message_add);
  VArraySpan<float> alphas = *attributes->lookup_or_default<float>(
      ATTR_ALPHA, AttrDomain::Point, 0.0f);
  VArraySpan<float> betas = *attributes->lookup_or_default<float>(
      ATTR_BETA, AttrDomain::Point, 0.0f);
  VArraySpan<float> edge_lengths = *lookup_or_warn<float>(
      *attributes, "edge_length", AttrDomain::Point, 0.0f, params.error_message_add);
  SpanAttributeWriter<float3> lambda_writer = attributes->lookup_or_add_for_write_span<float3>(
      "lambda", AttrDomain::Point);
  SpanAttributeWriter<float3> delta_position1_writer =
      attributes->lookup_or_add_for_write_span<float3>("delta_position1", AttrDomain::Point);
  SpanAttributeWriter<float3> delta_position2_writer =
      attributes->lookup_or_add_for_write_span<float3>("delta_position2", AttrDomain::Point);
  SpanAttributeWriter<float> delta_rotation1_w_writer =
      attributes->lookup_or_add_for_write_span<float>("delta_rotation1_w", AttrDomain::Point);
  SpanAttributeWriter<float3> delta_rotation1_xyz_writer =
      attributes->lookup_or_add_for_write_span<float3>("delta_rotation1_xyz", AttrDomain::Point);
  SpanAttributeWriter<float3> residual_writer;
  if constexpr (debug_output) {
    residual_writer = attributes->lookup_or_add_for_write_span<float3>("residual",
                                                                       AttrDomain::Point);
  }
  else {
    UNUSED_VARS(residual_writer);
  }

  const IndexRange points_range = variables.positions.index_range();
  const Span<float3> positions = variables.positions;
  const Span<math::Quaternion> rotations = variables.rotations;
  const Span<float3> old_positions = params.old_positions;
  const Span<math::Quaternion> old_rotations = params.old_rotations;

  group_mask.foreach_index(constraint_grain_size, [&](const int index) {
    const int point1 = points1[index];
    const int point2 = points2[index];
    if (!points_range.contains(point1) || !points_range.contains(point2)) {
      return;
    }
    const float weight_pos1 = math::safe_rcp(params.masses[point1]);
    const float weight_pos2 = math::safe_rcp(params.masses[point2]);
    const float weight_rot = math::safe_divide(2.0f, trace(params.local_inertia[point1]));
    const float edge_length = edge_lengths[index];
    float3 &lambda = lambda_writer.span[index];
    float3 &delta_pos1 = delta_position1_writer.span[index];
    float3 &delta_pos2 = delta_position2_writer.span[index];
    float &delta_rot1_w = delta_rotation1_w_writer.span[index];
    float3 &delta_rot1_xyz = delta_rotation1_xyz_writer.span[index];

    float3 residual;
    float3 delta_lambda;
    float4 delta_rot1;
    if constexpr (use_damping) {
      const float alpha = alphas[index] * params.inv_delta_time_squared;
      const float gamma = alphas[index] * betas[index] * params.inv_delta_time;
      xpbd_constraints::eval_position_stretch_shear<linearized_quaternion>(weight_pos1,
                                                                           weight_pos2,
                                                                           weight_rot,
                                                                           edge_length,
                                                                           alpha,
                                                                           gamma,
                                                                           lambda,
                                                                           positions[point1],
                                                                           positions[point2],
                                                                           rotations[point1],
                                                                           old_positions[point1],
                                                                           old_positions[point2],
                                                                           old_rotations[point1],
                                                                           residual,
                                                                           delta_lambda,
                                                                           delta_pos1,
                                                                           delta_pos2,
                                                                           delta_rot1);
    }
    else {
      const float alpha = alphas[index] * params.inv_delta_time_squared;
      xpbd_constraints::eval_position_stretch_shear<linearized_quaternion>(
          weight_pos1,
          weight_pos2,
          weight_rot,
          edge_length,
          alpha,
          0.0f,
          lambda,
          positions[point1],
          positions[point2],
          rotations[point1],
          float3(0.0f),
          float3(0.0f),
          math::Quaternion::identity(),
          residual,
          delta_lambda,
          delta_pos1,
          delta_pos2,
          delta_rot1);
    }

    lambda += delta_lambda;
    delta_rot1_w = delta_rot1.x;
    delta_rot1_xyz = delta_rot1.yzw();
    if constexpr (debug_output) {
      residual_writer.span[index] = residual;
    }
  });

  lambda_writer.finish();
  delta_position1_writer.finish();
  delta_position2_writer.finish();
  delta_rotation1_w_writer.finish();
  delta_rotation1_xyz_writer.finish();
  if constexpr (debug_output) {
    residual_writer.finish();
  }

  r_active = VArray<bool>::ForSingle(true, attributes->domain_size(AttrDomain::Point));
  r_delta_positions = {*attributes->lookup<float3>("delta_position1", AttrDomain::Point),
                       *attributes->lookup<float3>("delta_position2", AttrDomain::Point)};
  /* Attributes have to be stored separately as w/xyz, combine into a single VArray. */
  auto delta_rotation1_fn =
      [delta_rotation1_w = *attributes->lookup<float>("delta_rotation1_w", AttrDomain::Point),
       delta_rotation1_xyz = *attributes->lookup<float3>(
           "delta_rotation1_xyz", AttrDomain::Point)](const int64_t index) -> float4 {
    return float4(delta_rotation1_w[index], delta_rotation1_xyz[index]);
  };
  r_delta_rotations = {
      VArray<float4>::ForFunc(attributes->domain_size(AttrDomain::Point), delta_rotation1_fn), {}};
}

static void linear_solve_elements(const ConstraintEvalParams &params,
                                  const ConstraintVariables &variables,
                                  const bke::AttributeAccessor &attributes,
                                  const IndexMask &selection,
                                  GMutableSpan r_alphas,
                                  GMutableSpan r_betas,
                                  GMutableSpan r_residuals,
                                  GMutableSpan r_position_gradients[4],
                                  GMutableSpan r_rotation_gradients[4],
                                  MutableSpan<bool> /*r_active*/)
{
  const VArraySpan<int> points1 = *lookup_or_warn<int>(
      attributes, ATTR_POINT1, AttrDomain::Point, 0, params.error_message_add);
  const VArraySpan<int> points2 = *lookup_or_warn<int>(
      attributes, ATTR_POINT2, AttrDomain::Point, 0, params.error_message_add);
  const VArraySpan<float3> alphas_attr = *attributes.lookup_or_default<float3>(
      ATTR_ALPHA, AttrDomain::Point, float3(0.0f));
  const VArraySpan<float3> betas_attr = *attributes.lookup_or_default<float3>(
      ATTR_BETA, AttrDomain::Point, float3(0.0f));
  VArraySpan<float> edge_lengths = *lookup_or_warn<float>(
      attributes, "edge_length", AttrDomain::Point, 0.0f, params.error_message_add);

  const IndexRange points_range = variables.positions.index_range();
  const Span<float3> positions = variables.positions;
  const Span<math::Quaternion> rotations = variables.rotations;
  MutableSpan<float3> alphas = r_alphas.typed<float3>();
  MutableSpan<float3> betas = r_betas.typed<float3>();
  MutableSpan<float3> residuals = r_residuals.typed<float3>();
  MutableSpan<float4x4> position_gradients1 = r_position_gradients[0].typed<float4x4>();
  MutableSpan<float4x4> position_gradients2 = r_position_gradients[1].typed<float4x4>();
  MutableSpan<float4x4> rotation_gradients = r_rotation_gradients[0].typed<float4x4>();

  selection.foreach_index(constraint_grain_size, [&](const int index, const int pos) {
    const int point1 = points1[index];
    const int point2 = points2[index];
    if (!points_range.contains(point1) || !points_range.contains(point2)) {
      return;
    }
    const float edge_length = edge_lengths[index];

    alphas[pos] = alphas_attr[index];
    betas[pos] = betas_attr[index];
    xpbd_constraints::eval_stretch_shear_elements(edge_length,
                                                  positions[point1],
                                                  positions[point2],
                                                  rotations[point1],
                                                  residuals[pos],
                                                  position_gradients1[pos],
                                                  position_gradients2[pos],
                                                  rotation_gradients[pos]);
  });
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Curves").supported_type(GeometryComponent::Type::Curve);
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();

  b.add_input<decl::Float>("Compliance").min(0.0f).field_on_all();
  b.add_input<decl::Float>("Damping").min(0.0f).field_on_all();
  b.add_input<decl::Vector>("Rest Position")
      .implicit_field_on_all(NODE_DEFAULT_INPUT_POSITION_FIELD)
      .description("Rest position defining the edge length of constraints");

  b.add_output<decl::Geometry>("Constraints");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry = params.extract_input<GeometrySet>("Curves");
  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");

  const Field<float> compliance_field = params.extract_input<Field<float>>("Compliance");
  const Field<float> damping_field = params.extract_input<Field<float>>("Damping");
  const Field<float3> rest_position_field = params.extract_input<Field<float3>>("Rest Position");

  if (!geometry.has_curves()) {
    params.set_default_remaining_outputs();
    return;
  }

  const CurveComponent &component = *geometry.get_component<CurveComponent>();
  const bke::CurvesGeometry &curves = component.get()->geometry.wrap();
  const OffsetIndices points_by_curve = curves.points_by_curve();

  bke::GeometryFieldContext context{component, AttrDomain::Point};
  fn::FieldEvaluator evaluator{context, curves.points_num()};
  /* Note: selection is not used to limit the evaluation, because attributes from unselected points
   * may be needed to compute constraint properties (edge length). */
  evaluator.add(selection_field);
  evaluator.add(compliance_field);
  evaluator.add(damping_field);
  evaluator.add(rest_position_field);
  evaluator.evaluate();

  /* Skip end points of curves, these cannot have stretch/shear constraints. */
  Array<bool> point_valid(curves.points_num(), true);
  IndexMask(curves.curves_range()).foreach_index(GrainSize(256), [&](const int curve_i) {
    const IndexRange points = points_by_curve[curve_i];
    if (!points.is_empty()) {
      point_valid[points.last()] = false;
    }
  });

  IndexMaskMemory memory;
  const IndexMask selection = IndexMask::from_bools(
      evaluator.get_evaluated_as_mask(0), point_valid, memory);
  const VArray<float> compliance = evaluator.get_evaluated<float>(1);
  const VArray<float> damping = evaluator.get_evaluated<float>(2);
  const VArraySpan<float3> rest_position = evaluator.get_evaluated<float3>(3);

  PointCloud *points = BKE_pointcloud_new_nomain(selection.size());
  MutableAttributeAccessor attributes = points->attributes_for_write();
  SpanAttributeWriter<int> output_point1 = attributes.lookup_or_add_for_write_only_span<int>(
      ATTR_POINT1, AttrDomain::Point);
  SpanAttributeWriter<int> output_point2 = attributes.lookup_or_add_for_write_only_span<int>(
      ATTR_POINT2, AttrDomain::Point);
  SpanAttributeWriter<float> output_compliance =
      attributes.lookup_or_add_for_write_only_span<float>(ATTR_ALPHA, AttrDomain::Point);
  SpanAttributeWriter<float> output_damping = attributes.lookup_or_add_for_write_only_span<float>(
      ATTR_BETA, AttrDomain::Point);
  SpanAttributeWriter<float> output_edge_length =
      attributes.lookup_or_add_for_write_only_span<float>("edge_length", AttrDomain::Point);
  SpanAttributeWriter<int> output_solver_group = attributes.lookup_or_add_for_write_only_span<int>(
      ATTR_SOLVER_GROUP, AttrDomain::Point);

  selection.foreach_index(GrainSize(256), [&](const int index, const int pos) {
    /* Curve end points have been excluded, so index + 1 is safe. */
    output_point1.span[pos] = index;
    output_point2.span[pos] = index + 1;
    /* Use rest position distance as the edge length. */
    output_edge_length.span[pos] = math::distance(rest_position[index], rest_position[index + 1]);
    /* Alternating by odd/even index separates curve constraints into independent groups. */
    output_solver_group.span[pos] = index % 2;
  });
  compliance.materialize_compressed(selection, output_compliance.span);
  damping.materialize_compressed(selection, output_damping.span);

  output_point1.finish();
  output_point2.finish();
  output_compliance.finish();
  output_damping.finish();
  output_edge_length.finish();
  output_solver_group.finish();

  points->positions_for_write().fill(float3(0.0f));
  points->tag_positions_changed();

  params.set_output("Constraints", GeometrySet::from_pointcloud(points));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeStretchShearConstraints");
  ntype.ui_name = "Stretch/Shear Constraints";
  /* TODO Difficult to describe in a single sentence: The rotation of a hair segment is a generic
   * independent attribute. This constraint ensures that the distance between points matches the
   * expected edge length (zero stretch) and the orientation Z axis aligns with the actual
   * direction of the segment between neighboring points (zero shear). It can either move the
   * points or change the orientation, balanced by the relative position/rotation weights (inverse
   * point masses and moments of inertia). */
  ntype.ui_description =
      "Define stretch/shear constraints that limit edge length and align positions to segment "
      "rotation";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  node_type_size(ntype, 200, 120, 300);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}

}  // namespace stretch_shear

namespace bend_twist {

static void get_size(int &r_num_components,
                     int &r_num_position_vars,
                     int &r_num_rotation_vars,
                     bool &r_use_active_mask)
{
  r_num_components = 3;
  r_num_position_vars = 0;
  r_num_rotation_vars = 2;
  r_use_active_mask = false;
}

static void get_variable_indices(const bke::AttributeAccessor &attributes,
                                 const IndexMask &selection,
                                 MutableSpan<int> /*r_position_indices*/[4],
                                 MutableSpan<int> r_rotation_indices[4])
{
  const VArraySpan<int> points1 = *attributes.lookup_or_default<int>(
      ATTR_POINT1, AttrDomain::Point, 0);
  const VArraySpan<int> points2 = *attributes.lookup_or_default<int>(
      ATTR_POINT2, AttrDomain::Point, 0);

  MutableSpan<int> rotation_indices1 = r_rotation_indices[0];
  MutableSpan<int> rotation_indices2 = r_rotation_indices[1];
  selection.foreach_index(constraint_grain_size, [&](const int index, const int pos) {
    rotation_indices1[pos] = points1[index];
    rotation_indices2[pos] = points2[index];
  });
}

static void init_step(bke::GeometrySet &constraints)
{
  PointCloudComponent &component = constraints.get_component_for_write<PointCloudComponent>();
  std::optional<bke::MutableAttributeAccessor> attributes = component.attributes_for_write();

  SpanAttributeWriter<float3> lambda_writer = attributes->lookup_or_add_for_write_span<float3>(
      "lambda", AttrDomain::Point);

  lambda_writer.span.fill(float3(0.0f));

  lambda_writer.finish();
}

template<bool debug_output>
static void eval_positions(const ConstraintEvalParams &params,
                           const ConstraintVariables &variables,
                           const IndexMask &group_mask,
                           bke::GeometrySet &constraints,
                           VArray<bool> &r_active,
                           Vector<VArray<float3>> &r_delta_positions,
                           Vector<VArray<float4>> &r_delta_rotations)
{
  constexpr bool linearized_quaternion = true;
  constexpr bool use_damping = true;

  PointCloudComponent &component = constraints.get_component_for_write<PointCloudComponent>();
  std::optional<bke::MutableAttributeAccessor> attributes = component.attributes_for_write();

  VArraySpan<int> points1 = *lookup_or_warn<int>(
      *attributes, ATTR_POINT1, AttrDomain::Point, 0, params.error_message_add);
  VArraySpan<int> points2 = *lookup_or_warn<int>(
      *attributes, ATTR_POINT2, AttrDomain::Point, 0, params.error_message_add);
  VArraySpan<float3> alphas = *attributes->lookup_or_default<float3>(
      ATTR_ALPHA, AttrDomain::Point, float3(0.0f));
  VArraySpan<float> betas = *attributes->lookup_or_default<float>(
      ATTR_BETA, AttrDomain::Point, 0.0f);
  /* XXX plain float4 attribute is not supported, have to store it as float + float3. */
  VArraySpan<float3> darboux_vectors = *lookup_or_warn<float3>(
      *attributes, "darboux_vector", AttrDomain::Point, float3(0.0f), params.error_message_add);
  SpanAttributeWriter<float3> lambda_writer = attributes->lookup_or_add_for_write_span<float3>(
      "lambda", AttrDomain::Point);
  SpanAttributeWriter<float> delta_rotation1_w_writer =
      attributes->lookup_or_add_for_write_span<float>("delta_rotation1_w", AttrDomain::Point);
  SpanAttributeWriter<float3> delta_rotation1_xyz_writer =
      attributes->lookup_or_add_for_write_span<float3>("delta_rotation1_xyz", AttrDomain::Point);
  SpanAttributeWriter<float> delta_rotation2_w_writer =
      attributes->lookup_or_add_for_write_span<float>("delta_rotation2_w", AttrDomain::Point);
  SpanAttributeWriter<float3> delta_rotation2_xyz_writer =
      attributes->lookup_or_add_for_write_span<float3>("delta_rotation2_xyz", AttrDomain::Point);

  SpanAttributeWriter<float3> residual_writer;
  if constexpr (debug_output) {
    residual_writer = attributes->lookup_or_add_for_write_span<float3>("residual",
                                                                       AttrDomain::Point);
  }
  else {
    UNUSED_VARS(residual_writer);
  }

  const IndexRange points_range = variables.positions.index_range();
  const Span<math::Quaternion> rotations = variables.rotations;
  const Span<math::Quaternion> old_rotations = params.old_rotations;

  group_mask.foreach_index(constraint_grain_size, [&](const int index) {
    const int point1 = points1[index];
    const int point2 = points2[index];
    if (!points_range.contains(point1) || !points_range.contains(point2)) {
      return;
    }
    const float3 weight_rot1 = math::safe_rcp(params.local_inertia[point1]);
    const float3 weight_rot2 = math::safe_rcp(params.local_inertia[point2]);
    const float3 &darboux_vector = darboux_vectors[index];
    float3 &lambda = lambda_writer.span[index];
    float &delta_rotation1_w = delta_rotation1_w_writer.span[index];
    float3 &delta_rotation1_xyz = delta_rotation1_xyz_writer.span[index];
    float &delta_rotation2_w = delta_rotation2_w_writer.span[index];
    float3 &delta_rotation2_xyz = delta_rotation2_xyz_writer.span[index];

    float3 residual;
    float3 delta_lambda;
    float4 delta_rotation1, delta_rotation2;
    if constexpr (use_damping) {
      const float3 alpha = alphas[index] * params.inv_delta_time_squared;
      const float3 gamma = alphas[index] * betas[index] * params.inv_delta_time;
      xpbd_constraints::eval_position_bend_twist<linearized_quaternion>(weight_rot1,
                                                                        weight_rot2,
                                                                        darboux_vector,
                                                                        alpha,
                                                                        gamma,
                                                                        lambda,
                                                                        rotations[point1],
                                                                        rotations[point2],
                                                                        old_rotations[point1],
                                                                        old_rotations[point2],
                                                                        residual,
                                                                        delta_lambda,
                                                                        delta_rotation1,
                                                                        delta_rotation2);
    }
    else {
      const float alpha = alphas[index] * params.inv_delta_time_squared;
      xpbd_constraints::eval_position_bend_twist<linearized_quaternion>(
          weight_rot1,
          weight_rot2,
          darboux_vector,
          alpha,
          0.0f,
          lambda,
          rotations[point1],
          rotations[point2],
          math::Quaternion::identity(),
          math::Quaternion::identity(),
          residual,
          delta_lambda,
          delta_rotation1,
          delta_rotation2);
    }

    lambda += delta_lambda;
    delta_rotation1_w = delta_rotation1.x;
    delta_rotation1_xyz = delta_rotation1.yzw();
    delta_rotation2_w = delta_rotation2.x;
    delta_rotation2_xyz = delta_rotation2.yzw();
    if constexpr (debug_output) {
      residual_writer.span[index] = residual;
    }
  });

  lambda_writer.finish();
  delta_rotation1_w_writer.finish();
  delta_rotation1_xyz_writer.finish();
  delta_rotation2_w_writer.finish();
  delta_rotation2_xyz_writer.finish();
  if constexpr (debug_output) {
    residual_writer.finish();
  }

  r_active = VArray<bool>::ForSingle(true, attributes->domain_size(AttrDomain::Point));
  r_delta_positions = {{}, {}};
  /* Attributes have to be stored separately as w/xyz, combine into a single VArray. */
  auto delta_rotation1_fn =
      [delta_rotation1_w = *attributes->lookup<float>("delta_rotation1_w", AttrDomain::Point),
       delta_rotation1_xyz = *attributes->lookup<float3>(
           "delta_rotation1_xyz", AttrDomain::Point)](const int64_t index) -> float4 {
    return float4(delta_rotation1_w[index], delta_rotation1_xyz[index]);
  };
  auto delta_rotation2_fn =
      [delta_rotation2_w = *attributes->lookup<float>("delta_rotation2_w", AttrDomain::Point),
       delta_rotation2_xyz = *attributes->lookup<float3>(
           "delta_rotation2_xyz", AttrDomain::Point)](const int64_t index) -> float4 {
    return float4(delta_rotation2_w[index], delta_rotation2_xyz[index]);
  };
  r_delta_rotations = {
      VArray<float4>::ForFunc(attributes->domain_size(AttrDomain::Point), delta_rotation1_fn),
      VArray<float4>::ForFunc(attributes->domain_size(AttrDomain::Point), delta_rotation2_fn)};
}

static void linear_solve_elements(const ConstraintEvalParams &params,
                                  const ConstraintVariables &variables,
                                  const bke::AttributeAccessor &attributes,
                                  const IndexMask &selection,
                                  GMutableSpan r_alphas,
                                  GMutableSpan r_betas,
                                  GMutableSpan r_residuals,
                                  GMutableSpan /*r_position_gradients*/[4],
                                  GMutableSpan r_rotation_gradients[4],
                                  MutableSpan<bool> /*r_active*/)
{
  const VArraySpan<int> points1 = *lookup_or_warn<int>(
      attributes, ATTR_POINT1, AttrDomain::Point, 0, params.error_message_add);
  const VArraySpan<int> points2 = *lookup_or_warn<int>(
      attributes, ATTR_POINT2, AttrDomain::Point, 0, params.error_message_add);
  const VArraySpan<float3> alphas_attr = *attributes.lookup_or_default<float3>(
      ATTR_ALPHA, AttrDomain::Point, float3(0.0f));
  const VArraySpan<float3> betas_attr = *attributes.lookup_or_default<float3>(
      ATTR_BETA, AttrDomain::Point, float3(0.0f));
  const VArraySpan<float3> darboux_vectors = *lookup_or_warn<float3>(
      attributes, "darboux_vector", AttrDomain::Point, float3(0.0f), params.error_message_add);

  const IndexRange points_range = variables.rotations.index_range();
  const Span<math::Quaternion> rotations = variables.rotations;
  MutableSpan<float3> alphas = r_alphas.typed<float3>();
  MutableSpan<float3> betas = r_betas.typed<float3>();
  MutableSpan<float3> residuals = r_residuals.typed<float3>();
  MutableSpan<float4x4> rotation_gradients1 = r_rotation_gradients[0].typed<float4x4>();
  MutableSpan<float4x4> rotation_gradients2 = r_rotation_gradients[1].typed<float4x4>();

  selection.foreach_index(constraint_grain_size, [&](const int index, const int pos) {
    const int point1 = points1[index];
    const int point2 = points2[index];
    if (!points_range.contains(point1) || !points_range.contains(point2)) {
      return;
    }
    const float3 &darboux_vector = darboux_vectors[index];

    alphas[pos] = alphas_attr[index];
    betas[pos] = betas_attr[index];
    xpbd_constraints::eval_bend_twist_elements(darboux_vector,
                                               rotations[point1],
                                               rotations[point2],
                                               residuals[pos],
                                               rotation_gradients1[pos],
                                               rotation_gradients2[pos]);
  });
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Curves").supported_type(GeometryComponent::Type::Curve);
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();

  b.add_input<decl::Vector>("Compliance").min(0.0f).field_on_all();
  b.add_input<decl::Float>("Damping").min(0.0f).field_on_all();
  b.add_input<decl::Rotation>("Rest Rotation")
      .field_on_all()
      .description("Rest rotation defining the relative orientation of segments");

  b.add_output<decl::Geometry>("Constraints");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry = params.extract_input<GeometrySet>("Curves");
  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");

  const Field<float3> compliance_field = params.extract_input<Field<float3>>("Compliance");
  const Field<float> damping_field = params.extract_input<Field<float>>("Damping");
  const Field<math::Quaternion> rest_rotation_field =
      params.extract_input<Field<math::Quaternion>>("Rest Rotation");

  if (!geometry.has_curves()) {
    params.set_default_remaining_outputs();
    return;
  }

  const CurveComponent &component = *geometry.get_component<CurveComponent>();
  const bke::CurvesGeometry &curves = component.get()->geometry.wrap();
  const OffsetIndices points_by_curve = curves.points_by_curve();

  bke::GeometryFieldContext context{component, AttrDomain::Point};
  fn::FieldEvaluator evaluator{context, curves.points_num()};
  /* Note: selection is not used to limit the evaluation, because attributes from unselected points
   * may be needed to compute constraint properties (edge length). */
  evaluator.add(selection_field);
  evaluator.add(compliance_field);
  evaluator.add(damping_field);
  evaluator.add(rest_rotation_field);
  evaluator.evaluate();

  /* Skip end points of curves, these cannot have bend/twist constraints. */
  Array<bool> point_valid(curves.points_num(), true);
  IndexMask(curves.curves_range()).foreach_index(GrainSize(256), [&](const int curve_i) {
    const IndexRange points = points_by_curve[curve_i];
    if (!points.is_empty()) {
      point_valid[points.last()] = false;
    }
  });

  IndexMaskMemory memory;
  const IndexMask selection = IndexMask::from_bools(
      evaluator.get_evaluated_as_mask(0), point_valid, memory);
  const VArray<float3> compliance = evaluator.get_evaluated<float3>(1);
  const VArray<float> damping = evaluator.get_evaluated<float>(2);
  const VArraySpan<math::Quaternion> rest_rotation = evaluator.get_evaluated<math::Quaternion>(3);

  PointCloud *points = BKE_pointcloud_new_nomain(selection.size());
  MutableAttributeAccessor attributes = points->attributes_for_write();
  SpanAttributeWriter<int> output_point1 = attributes.lookup_or_add_for_write_only_span<int>(
      ATTR_POINT1, AttrDomain::Point);
  SpanAttributeWriter<int> output_point2 = attributes.lookup_or_add_for_write_only_span<int>(
      ATTR_POINT2, AttrDomain::Point);
  SpanAttributeWriter<float3> output_compliance =
      attributes.lookup_or_add_for_write_only_span<float3>(ATTR_ALPHA, AttrDomain::Point);
  SpanAttributeWriter<float> output_damping = attributes.lookup_or_add_for_write_only_span<float>(
      ATTR_BETA, AttrDomain::Point);
  SpanAttributeWriter<float3> output_darboux_vector =
      attributes.lookup_or_add_for_write_only_span<float3>("darboux_vector", AttrDomain::Point);
  SpanAttributeWriter<int> output_solver_group = attributes.lookup_or_add_for_write_only_span<int>(
      ATTR_SOLVER_GROUP, AttrDomain::Point);

  selection.foreach_index(GrainSize(256), [&](const int index, const int pos) {
    /* Curve end points have been excluded, so index + 1 is safe. */
    output_point1.span[pos] = index;
    output_point2.span[pos] = index + 1;
    /* Use rest rotation difference to compute a Darboux vector. */
    output_darboux_vector.span[pos] = (math::invert_normalized(rest_rotation[index]) *
                                       rest_rotation[index + 1])
                                          .imaginary_part();
    /* Alternating by odd/even index separates curve constraints into independent groups. */
    output_solver_group.span[pos] = index % 2;
  });
  compliance.materialize_compressed(selection, output_compliance.span);
  damping.materialize_compressed(selection, output_damping.span);

  output_point1.finish();
  output_point2.finish();
  output_compliance.finish();
  output_damping.finish();
  output_darboux_vector.finish();
  output_solver_group.finish();

  points->positions_for_write().fill(float3(0.0f));
  points->tag_positions_changed();

  params.set_output("Constraints", GeometrySet::from_pointcloud(points));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeBendTwistConstraints");
  ntype.ui_name = "Bend/Twist Constraints";
  ntype.ui_description =
      "Define bend/twist constraints that limit relative rotation between two orientations";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  node_type_size(ntype, 200, 120, 300);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}

}  // namespace bend_twist

namespace contact {

static void get_size(int &r_num_components,
                     int &r_num_position_vars,
                     int &r_num_rotation_vars,
                     bool &r_use_active_mask)
{
  r_num_components = 1;
  r_num_position_vars = 1;
  r_num_rotation_vars = 1;
  r_use_active_mask = true;
}

static void get_variable_indices(const bke::AttributeAccessor &attributes,
                                 const IndexMask &selection,
                                 MutableSpan<int> r_position_indices[4],
                                 MutableSpan<int> r_rotation_indices[4])
{
  const VArraySpan<int> points1 = *attributes.lookup_or_default<int>(
      ATTR_POINT1, AttrDomain::Point, 0);

  MutableSpan<int> position_indices1 = r_position_indices[0];
  MutableSpan<int> rotation_indices1 = r_rotation_indices[0];
  selection.foreach_index(constraint_grain_size, [&](const int index, const int pos) {
    position_indices1[pos] = points1[index];
    rotation_indices1[pos] = points1[index];
  });
}

static void init_step(bke::GeometrySet &constraints)
{
  PointCloudComponent &component = constraints.get_component_for_write<PointCloudComponent>();
  std::optional<bke::MutableAttributeAccessor> attributes = component.attributes_for_write();

  SpanAttributeWriter<float> position_lambda_writer =
      attributes->lookup_or_add_for_write_span<float>("position_lambda", AttrDomain::Point);
  SpanAttributeWriter<float> restitution_lambda_writer =
      attributes->lookup_or_add_for_write_span<float>("restitution_lambda", AttrDomain::Point);
  SpanAttributeWriter<float> friction_lambda_writer =
      attributes->lookup_or_add_for_write_span<float>("friction_lambda", AttrDomain::Point);

  position_lambda_writer.span.fill(0.0f);
  restitution_lambda_writer.span.fill(0.0f);
  friction_lambda_writer.span.fill(0.0f);

  position_lambda_writer.finish();
  restitution_lambda_writer.finish();
  friction_lambda_writer.finish();
}

template<bool debug_output>
static void eval_positions(const ConstraintEvalParams &params,
                           const ConstraintVariables &variables,
                           const IndexMask &group_mask,
                           bke::GeometrySet &constraints,
                           VArray<bool> &r_active,
                           Vector<VArray<float3>> &r_delta_positions,
                           Vector<VArray<float4>> &r_delta_rotations)
{
  PointCloudComponent &component = constraints.get_component_for_write<PointCloudComponent>();
  std::optional<bke::MutableAttributeAccessor> attributes = component.attributes_for_write();

  VArraySpan<int> points1 = *lookup_or_warn<int>(
      *attributes, ATTR_POINT1, AttrDomain::Point, 0, params.error_message_add);
  VArraySpan<int> collider_indices = *lookup_or_warn<int>(
      *attributes, "collider_index", AttrDomain::Point, 0, params.error_message_add);
  VArraySpan<float3> local_positions1 = *lookup_or_warn<float3>(
      *attributes, "local_position1", AttrDomain::Point, float3(0.0f), params.error_message_add);
  VArraySpan<float3> local_positions2 = *lookup_or_warn<float3>(
      *attributes, "local_position2", AttrDomain::Point, float3(0.0f), params.error_message_add);
  VArraySpan<float3> normals = *lookup_or_warn<float3>(
      *attributes, "normal", AttrDomain::Point, float3(0.0f), params.error_message_add);
  VArraySpan<float> alphas = *attributes->lookup_or_default<float>(
      ATTR_ALPHA, AttrDomain::Point, 0.0f);
  VArraySpan<float> betas = *attributes->lookup_or_default<float>(
      ATTR_BETA, AttrDomain::Point, 0.0f);
  SpanAttributeWriter<float> position_lambda_writer =
      attributes->lookup_or_add_for_write_span<float>("position_lambda", AttrDomain::Point);
  SpanAttributeWriter<float3> delta_position1_writer =
      attributes->lookup_or_add_for_write_span<float3>("delta_position1", AttrDomain::Point);
  SpanAttributeWriter<float> delta_rotation1_w_writer =
      attributes->lookup_or_add_for_write_span<float>("delta_rotation1_w", AttrDomain::Point);
  SpanAttributeWriter<float3> delta_rotation1_xyz_writer =
      attributes->lookup_or_add_for_write_span<float3>("delta_rotation1_xyz", AttrDomain::Point);
  SpanAttributeWriter<bool> active_writer = attributes->lookup_or_add_for_write_span<bool>(
      ATTR_ACTIVE, AttrDomain::Point);
  SpanAttributeWriter<bool> last_active_writer = attributes->lookup_or_add_for_write_span<bool>(
      ATTR_LAST_ACTIVE, AttrDomain::Point);
  SpanAttributeWriter<float> residual_position_writer;
  if constexpr (debug_output) {
    residual_position_writer = attributes->lookup_or_add_for_write_span<float>("residual_position",
                                                                               AttrDomain::Point);
  }
  else {
    UNUSED_VARS(residual_position_writer);
  }

  const IndexRange points_range = variables.positions.index_range();
  const Span<float3> positions = variables.positions;
  const Span<math::Quaternion> rotations = variables.rotations;

  group_mask.foreach_index(constraint_grain_size, [&](const int index) {
    const int point1 = points1[index];
    if (!points_range.contains(point1)) {
      return;
    }
    const int collider_index = collider_indices[index];
    if (!params.collider_transforms.index_range().contains(collider_index)) {
      return;
    };
    /* Only affect point1 for now (external colliders). */
    const float weight_pos1 = 1.0f;
    const float weight_pos2 = 0.0f;
    const float weight_rot1 = 1.0f;
    const float weight_rot2 = 0.0f;
    const float3 &local_position1 = local_positions1[index];
    const float3 &local_position2 = local_positions2[index];
    const float3 &normal = normals[index];
    float &lambda = position_lambda_writer.span[index];
    float3 &delta_pos1 = delta_position1_writer.span[index];
    float &delta_rot1_w = delta_rotation1_w_writer.span[index];
    float3 &delta_rot1_xyz = delta_rotation1_xyz_writer.span[index];
    bool &active = active_writer.span[index];
    bool &last_active = last_active_writer.span[index];

    const float4x4 collider_transform = params.collider_transforms[collider_index];
    float3 collider_position;
    math::Quaternion collider_rotation;
    float3 collider_scale;
    math::to_loc_rot_scale(
        collider_transform, collider_position, collider_rotation, collider_scale);

    float residual;
    float delta_lambda;
    float3 delta_pos_collider;
    float4 delta_rot1, delta_rot_collider;
    const float alpha = alphas[index] * params.inv_delta_time_squared;
    last_active = xpbd_constraints::eval_position_contact(weight_pos1,
                                                          weight_pos2,
                                                          weight_rot1,
                                                          weight_rot2,
                                                          local_position1,
                                                          local_position2,
                                                          normal,
                                                          alpha,
                                                          lambda,
                                                          positions[point1],
                                                          collider_position,
                                                          rotations[point1],
                                                          collider_rotation,
                                                          residual,
                                                          delta_lambda,
                                                          delta_pos1,
                                                          delta_pos_collider,
                                                          delta_rot1,
                                                          delta_rot_collider);

    /* Accumulate "active" flags over the entire time step. */
    if (last_active) {
      active |= last_active;
      lambda += delta_lambda;
      delta_rot1_w = delta_rot1.x;
      delta_rot1_xyz = delta_rot1.yzw();
      if constexpr (debug_output) {
        residual_position_writer.span[index] = residual;
      }
    }
  });

  position_lambda_writer.finish();
  delta_position1_writer.finish();
  delta_rotation1_w_writer.finish();
  delta_rotation1_xyz_writer.finish();
  active_writer.finish();
  last_active_writer.finish();
  if constexpr (debug_output) {
    residual_position_writer.finish();
  }

  r_active = *attributes->lookup<bool>(ATTR_LAST_ACTIVE, AttrDomain::Point);
  r_delta_positions = {*attributes->lookup<float3>("delta_position1", AttrDomain::Point)};
  /* Attributes have to be stored separately as w/xyz, combine into a single VArray. */
  auto delta_rotation1_fn =
      [delta_rotation1_w = *attributes->lookup<float>("delta_rotation1_w", AttrDomain::Point),
       delta_rotation1_xyz = *attributes->lookup<float3>(
           "delta_rotation1_xyz", AttrDomain::Point)](const int64_t index) -> float4 {
    return float4(delta_rotation1_w[index], delta_rotation1_xyz[index]);
  };
  r_delta_rotations = {
      VArray<float4>::ForFunc(attributes->domain_size(AttrDomain::Point), delta_rotation1_fn)};
}

template<bool debug_output>
static void eval_velocities(const ConstraintEvalParams &params,
                            const ConstraintVariables &variables,
                            const IndexMask &group_mask,
                            bke::GeometrySet &constraints,
                            VArray<bool> &r_active,
                            Vector<VArray<float3>> &r_delta_velocities,
                            Vector<VArray<float3>> &r_delta_angular_velocities)
{
  PointCloudComponent &component = constraints.get_component_for_write<PointCloudComponent>();
  std::optional<bke::MutableAttributeAccessor> attributes = component.attributes_for_write();

  VArraySpan<int> points1 = *lookup_or_warn<int>(
      *attributes, ATTR_POINT1, AttrDomain::Point, 0, params.error_message_add);
  VArraySpan<int> collider_indices = *lookup_or_warn<int>(
      *attributes, "collider_index", AttrDomain::Point, 0, params.error_message_add);
  VArraySpan<float3> local_positions1 = *lookup_or_warn<float3>(
      *attributes, "local_position1", AttrDomain::Point, float3(0.0f), params.error_message_add);
  VArraySpan<float3> local_positions2 = *lookup_or_warn<float3>(
      *attributes, "local_position2", AttrDomain::Point, float3(0.0f), params.error_message_add);
  VArraySpan<float3> normals = *lookup_or_warn<float3>(
      *attributes, "normal", AttrDomain::Point, float3(0.0f), params.error_message_add);
  VArraySpan<float> restitutions = *attributes->lookup_or_default<float>(
      "restitution", AttrDomain::Point, 0.0f);
  VArraySpan<float> frictions = *attributes->lookup_or_default<float>(
      "friction", AttrDomain::Point, 0.0f);
  VArraySpan<float> threshold_normal_velocities = *attributes->lookup_or_default<float>(
      "threshold_normal_velocity", AttrDomain::Point, 0.0f);
  VArraySpan<bool> active = *attributes->lookup_or_default<bool>(
      ATTR_ACTIVE, AttrDomain::Point, false);
  SpanAttributeWriter<float> restitution_lambda_writer =
      attributes->lookup_or_add_for_write_span<float>("restitution_lambda", AttrDomain::Point);
  SpanAttributeWriter<float> friction_lambda_writer =
      attributes->lookup_or_add_for_write_span<float>("friction_lambda", AttrDomain::Point);
  SpanAttributeWriter<float3> delta_velocity1_writer =
      attributes->lookup_or_add_for_write_span<float3>("delta_velocity1", AttrDomain::Point);
  SpanAttributeWriter<float3> delta_angular_velocity1_writer =
      attributes->lookup_or_add_for_write_span<float3>("delta_angular_velocity1",
                                                       AttrDomain::Point);
  SpanAttributeWriter<float> residual_restitution_writer;
  SpanAttributeWriter<float> residual_friction_writer;
  if constexpr (debug_output) {
    residual_restitution_writer = attributes->lookup_or_add_for_write_span<float>(
        "residual_restitution", AttrDomain::Point);
    residual_friction_writer = attributes->lookup_or_add_for_write_span<float>("residual_friction",
                                                                               AttrDomain::Point);
  }
  else {
    UNUSED_VARS(residual_restitution_writer, residual_friction_writer);
  }

  const IndexRange points_range = variables.positions.index_range();
  const Span<float3> velocities = variables.velocities;
  const Span<float3> angular_velocities = variables.angular_velocities;

  group_mask.foreach_index(constraint_grain_size, [&](const int index) {
    /* Active status is determined by the position evaluation. */
    if (!active[index]) {
      return;
    }

    const int point1 = points1[index];
    if (!points_range.contains(point1)) {
      return;
    }
    const int collider_index = collider_indices[index];
    if (!params.collider_transforms.index_range().contains(collider_index)) {
      return;
    };
    /* Only affect point1 for now (external colliders). */
    const float weight_pos1 = 1.0f;
    const float weight_pos2 = 0.0f;
    const float weight_rot1 = 1.0f;
    const float weight_rot2 = 0.0f;
    const float3 &local_position1 = local_positions1[index];
    const float3 &local_position2 = local_positions2[index];
    const float3 &normal = normals[index];
    float &lambda_restitution = restitution_lambda_writer.span[index];
    float &lambda_friction = friction_lambda_writer.span[index];
    float3 &delta_vel1 = delta_velocity1_writer.span[index];
    float3 &delta_angvel1 = delta_angular_velocity1_writer.span[index];

    /* Compute velocity from old/new collider transforms. */
    const float4x4 collider_transform = params.collider_transforms[collider_index];
    const float4x4 &old_collider_transform = params.old_collider_transforms[collider_index];
    float3 collider_loc, old_collider_loc;
    math::Quaternion collider_rot, old_collider_rot;
    float3 collider_scale, old_collider_scale;
    math::to_loc_rot_scale(collider_transform, collider_loc, collider_rot, collider_scale);
    math::to_loc_rot_scale(
        old_collider_transform, old_collider_loc, old_collider_rot, old_collider_scale);
    float3 collider_velocity = (collider_loc - old_collider_loc) * params.inv_delta_time;
    float3 collider_angular_velocity =
        2.0f * (math::invert_normalized(old_collider_rot) * collider_rot).imaginary_part() *
        params.inv_delta_time;

    const float3 &orig_velocity1 = params.orig_velocities[point1];
    const float3 &orig_angular_velocity1 = params.orig_angular_velocities[point1];
    /* No change in animated collider velocity. */
    const float3 orig_collider_velocity = collider_velocity;
    const float3 orig_collider_angular_velocity = collider_angular_velocity;

    float residual_restitution, residual_friction;
    float delta_lambda_restitution, delta_lambda_friction;
    float3 delta_vel_collider;
    float3 delta_angvel_collider;
    const float restitution = restitutions[index];
    const float friction = frictions[index];
    const float threshold_normal_velocity = threshold_normal_velocities[index];

    /* TODO Weights should at least be formal parameters for consistency, even if unused
     * internally. */
    UNUSED_VARS(weight_pos1, weight_pos2, weight_rot1, weight_rot2);
    xpbd_constraints::eval_velocity_contact(orig_velocity1,
                                            orig_collider_velocity,
                                            orig_angular_velocity1,
                                            orig_collider_angular_velocity,
                                            local_position1,
                                            local_position2,
                                            normal,
                                            restitution,
                                            friction,
                                            lambda_restitution,
                                            lambda_friction,
                                            velocities[point1],
                                            collider_velocity,
                                            angular_velocities[point1],
                                            collider_angular_velocity,
                                            threshold_normal_velocity,
                                            residual_restitution,
                                            residual_friction,
                                            delta_lambda_restitution,
                                            delta_lambda_friction,
                                            delta_vel1,
                                            delta_vel_collider,
                                            delta_angvel1,
                                            delta_angvel_collider);

    /* Accumulate "active" flags over the entire time step. */
    lambda_restitution += delta_lambda_restitution;
    lambda_friction += delta_lambda_friction;
    if constexpr (debug_output) {
      residual_restitution_writer.span[index] = residual_restitution;
      residual_friction_writer.span[index] = residual_friction;
    }
  });

  restitution_lambda_writer.finish();
  friction_lambda_writer.finish();
  delta_velocity1_writer.finish();
  delta_angular_velocity1_writer.finish();
  if constexpr (debug_output) {
    residual_restitution_writer.finish();
    residual_friction_writer.finish();
  }

  /* Note: velocity constraints are active if the position constraint has been active at any time
   * during the time step. */
  r_active = *attributes->lookup_or_default<bool>(ATTR_ACTIVE, AttrDomain::Point, false);
  r_delta_velocities = {*attributes->lookup<float3>("delta_velocity1", AttrDomain::Point)};
  r_delta_angular_velocities = {
      *attributes->lookup<float3>("delta_angular_velocity1", AttrDomain::Point)};
}

static void linear_solve_elements(const ConstraintEvalParams &params,
                                  const ConstraintVariables &variables,
                                  const bke::AttributeAccessor &attributes,
                                  const IndexMask &selection,
                                  GMutableSpan r_alphas,
                                  GMutableSpan r_betas,
                                  GMutableSpan r_residuals,
                                  GMutableSpan r_position_gradients[4],
                                  GMutableSpan r_rotation_gradients[4],
                                  MutableSpan<bool> r_active)
{
  VArraySpan<int> points1 = *lookup_or_warn<int>(
      attributes, ATTR_POINT1, AttrDomain::Point, 0, params.error_message_add);
  VArraySpan<int> collider_indices = *lookup_or_warn<int>(
      attributes, "collider_index", AttrDomain::Point, 0, params.error_message_add);
  VArraySpan<float3> local_positions1 = *lookup_or_warn<float3>(
      attributes, "local_position1", AttrDomain::Point, float3(0.0f), params.error_message_add);
  VArraySpan<float3> local_positions2 = *lookup_or_warn<float3>(
      attributes, "local_position2", AttrDomain::Point, float3(0.0f), params.error_message_add);
  VArraySpan<float3> normals = *lookup_or_warn<float3>(
      attributes, "normal", AttrDomain::Point, float3(0.0f), params.error_message_add);

  const IndexRange points_range = variables.rotations.index_range();
  const Span<float3> positions = variables.positions;
  const Span<math::Quaternion> rotations = variables.rotations;
  MutableSpan<float> alphas = r_alphas.typed<float>();
  MutableSpan<float> betas = r_betas.typed<float>();
  MutableSpan<float> residuals = r_residuals.typed<float>();
  MutableSpan<float3> position_gradients1 = r_position_gradients[0].typed<float3>();
  MutableSpan<float4> rotation_gradients1 = r_rotation_gradients[0].typed<float4>();

  selection.foreach_index(constraint_grain_size, [&](const int index, const int pos) {
    const int point1 = points1[index];
    const int collider_index = collider_indices[index];
    if (!points_range.contains(point1) ||
        !params.collider_transforms.index_range().contains(collider_index))
    {
      return;
    }

    /* Compliance and damping ignored for collisions. */
    alphas[pos] = 0.0f;
    betas[pos] = 0.0f;

    const float4x4 collider_transform = params.collider_transforms[collider_index];
    float3 collider_position;
    math::Quaternion collider_rotation;
    float3 collider_scale;
    math::to_loc_rot_scale(
        collider_transform, collider_position, collider_rotation, collider_scale);

    float3 collider_position_gradient;
    float4 collider_rotation_gradient;
    const bool active = xpbd_constraints::eval_contact_position_elements(
        local_positions1[index],
        local_positions2[index],
        normals[index],
        positions[point1],
        collider_position,
        rotations[point1],
        collider_rotation,
        residuals[pos],
        position_gradients1[pos],
        collider_position_gradient,
        rotation_gradients1[pos],
        collider_rotation_gradient);
    r_active[pos] = active;
  });
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Curves").supported_type(GeometryComponent::Type::Curve);
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();

  b.add_input<decl::Int>("Collider").min(0).description("Index of the collider");
  b.add_input<decl::Float>("Friction")
      .min(0.0f)
      .field_on_all()
      .description("Reduction of velocity along the surface during contact");
  b.add_input<decl::Float>("Restitution")
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .field_on_all()
      .description("Amount the point bounces back after colliding");
  b.add_input<decl::Float>("Threshold Normal Velocity")
      .min(0.0f)
      .default_value(0.5f)
      .field_on_all()
      .description("No bouncing occurs when normal velocity is below this threshold");
  b.add_input<decl::Vector>("Local Position")
      .field_on_all()
      .description("Contact position relative to the point");
  b.add_input<decl::Vector>("Collider Position")
      .field_on_all()
      .description("Contact position relative to the collider");
  b.add_input<decl::Vector>("Normal").field_on_all().description("Contact normal");

  b.add_output<decl::Geometry>("Constraints");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry = params.extract_input<GeometrySet>("Curves");
  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");

  const int collider_index = params.extract_input<int>("Collider");
  const Field<float> friction_field = params.extract_input<Field<float>>("Friction");
  const Field<float> restitution_field = params.extract_input<Field<float>>("Restitution");
  const Field<float> threshold_normal_velocity_field = params.extract_input<Field<float>>(
      "Threshold Normal Velocity");
  const Field<float3> local_position_field = params.extract_input<Field<float3>>("Local Position");
  const Field<float3> collider_position_field = params.extract_input<Field<float3>>(
      "Collider Position");
  const Field<float3> normal_field = params.extract_input<Field<float3>>("Normal");

  if (!geometry.has_curves()) {
    params.set_default_remaining_outputs();
    return;
  }

  const CurveComponent &component = *geometry.get_component<CurveComponent>();
  const bke::CurvesGeometry &curves = component.get()->geometry.wrap();

  bke::GeometryFieldContext context{component, AttrDomain::Point};
  fn::FieldEvaluator evaluator{context, curves.points_num()};
  evaluator.set_selection(selection_field);
  evaluator.add(friction_field);
  evaluator.add(restitution_field);
  evaluator.add(threshold_normal_velocity_field);
  evaluator.add(local_position_field);
  evaluator.add(collider_position_field);
  evaluator.add(normal_field);
  evaluator.evaluate();

  const IndexMask selection = evaluator.get_evaluated_selection_as_mask();
  const VArray<float> friction = evaluator.get_evaluated<float>(0);
  const VArray<float> restitution = evaluator.get_evaluated<float>(1);
  const VArray<float> threshold_normal_velocity = evaluator.get_evaluated<float>(2);
  const VArray<float3> local_position = evaluator.get_evaluated<float3>(3);
  const VArray<float3> collider_position = evaluator.get_evaluated<float3>(4);
  const VArraySpan<float3> normal = evaluator.get_evaluated<float3>(5);

  PointCloud *points = BKE_pointcloud_new_nomain(selection.size());
  MutableAttributeAccessor attributes = points->attributes_for_write();
  SpanAttributeWriter<int> output_point1 = attributes.lookup_or_add_for_write_only_span<int>(
      ATTR_POINT1, AttrDomain::Point);
  SpanAttributeWriter<int> output_collider_index =
      attributes.lookup_or_add_for_write_only_span<int>("collider_index", AttrDomain::Point);
  SpanAttributeWriter<float> output_friction = attributes.lookup_or_add_for_write_only_span<float>(
      "friction", AttrDomain::Point);
  SpanAttributeWriter<float> output_restitution =
      attributes.lookup_or_add_for_write_only_span<float>("restitution", AttrDomain::Point);
  SpanAttributeWriter<float> output_threshold_normal_velocity =
      attributes.lookup_or_add_for_write_only_span<float>("threshold_normal_velocity",
                                                          AttrDomain::Point);
  SpanAttributeWriter<float3> output_local_position1 =
      attributes.lookup_or_add_for_write_only_span<float3>("local_position1", AttrDomain::Point);
  SpanAttributeWriter<float3> output_local_position2 =
      attributes.lookup_or_add_for_write_only_span<float3>("local_position2", AttrDomain::Point);
  SpanAttributeWriter<float3> output_normal = attributes.lookup_or_add_for_write_only_span<float3>(
      "normal", AttrDomain::Point);
  SpanAttributeWriter<int> output_solver_group = attributes.lookup_or_add_for_write_only_span<int>(
      ATTR_SOLVER_GROUP, AttrDomain::Point);

  selection.to_indices(output_point1.span);
  output_collider_index.span.fill(collider_index);
  friction.materialize_compressed(selection, output_friction.span);
  restitution.materialize_compressed(selection, output_restitution.span);
  threshold_normal_velocity.materialize_compressed(selection,
                                                   output_threshold_normal_velocity.span);
  local_position.materialize_compressed(selection, output_local_position1.span);
  collider_position.materialize_compressed(selection, output_local_position2.span);
  selection.foreach_index(GrainSize(256), [&](const int index, const int pos) {
    output_normal.span[pos] = math::normalize(normal[index]);
  });
  /* There should only be one contact per point/collider pair, so the collider index can be used
   * to separate constraint groups. */
  output_solver_group.span.fill(collider_index);

  output_point1.finish();
  output_collider_index.finish();
  output_friction.finish();
  output_restitution.finish();
  output_threshold_normal_velocity.finish();
  output_local_position1.finish();
  output_local_position2.finish();
  output_normal.finish();
  output_solver_group.finish();

  points->positions_for_write().fill(float3(0.0f));
  points->tag_positions_changed();

  params.set_output("Constraints", GeometrySet::from_pointcloud(points));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeContactConstraints");
  ntype.ui_name = "Contact Constraints";
  ntype.ui_description = "Define contact constraints to react to collisions";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  node_type_size(ntype, 200, 120, 300);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}

}  // namespace contact

const SocketInterfaceKey ConstraintBundleItems::stretch_constraints_key = SocketInterfaceKey(
    "Stretch");
const SocketInterfaceKey ConstraintBundleItems::bending_constraints_key = SocketInterfaceKey(
    "Bending");
const SocketInterfaceKey ConstraintBundleItems::position_constraints_key = SocketInterfaceKey(
    "Position");
const SocketInterfaceKey ConstraintBundleItems::rotation_constraints_key = SocketInterfaceKey(
    "Rotation");
const SocketInterfaceKey ConstraintBundleItems::contact_constraints_key = SocketInterfaceKey(
    "Contact");

void set_constraints(BundlePtr &bundle_ptr,
                     const SocketInterfaceKey &key,
                     const bke::GeometrySet &geometry)
{
  BLI_assert(bundle_ptr->is_mutable());
  Bundle &bundle = const_cast<Bundle &>(*bundle_ptr);

  static const bke::bNodeSocketType *geometry_type = bke::node_socket_type_find_static(
      SOCK_GEOMETRY);
  BLI_assert(geometry_type != nullptr);

  bundle.remove(key);
  bundle.add(key, *geometry_type, &geometry);
}

bke::GeometrySet lookup_constraints(const Bundle &bundle, const SocketInterfaceKey &key)
{
  static const bke::bNodeSocketType *geometry_type = bke::node_socket_type_find_static(
      SOCK_GEOMETRY);
  BLI_assert(geometry_type != nullptr);

  const std::optional<Bundle::Item> value = bundle.lookup(key);
  if (!value) {
    return {};
  }
  GeometrySet output_geometry;
  if (!implicitly_convert_socket_value(
          *value->type, value->value, *geometry_type, &output_geometry))
  {
    return {};
  }
  return output_geometry;
};

BundlePtr combine_constraint_bundle(const ConstraintBundleItems &items)
{
  BundlePtr bundle_ptr = Bundle::create();

  set_constraints(
      bundle_ptr, ConstraintBundleItems::stretch_constraints_key, items.stretch_constraints);
  set_constraints(
      bundle_ptr, ConstraintBundleItems::bending_constraints_key, items.bending_constraints);
  set_constraints(
      bundle_ptr, ConstraintBundleItems::position_constraints_key, items.position_constraints);
  set_constraints(
      bundle_ptr, ConstraintBundleItems::rotation_constraints_key, items.rotation_constraints);
  set_constraints(
      bundle_ptr, ConstraintBundleItems::contact_constraints_key, items.contact_constraints);

  return bundle_ptr;
}

void separate_constraint_bundle(const Bundle &bundle, ConstraintBundleItems &items)
{
  items.stretch_constraints = lookup_constraints(bundle,
                                                 ConstraintBundleItems::stretch_constraints_key);
  items.bending_constraints = lookup_constraints(bundle,
                                                 ConstraintBundleItems::bending_constraints_key);
  items.position_constraints = lookup_constraints(bundle,
                                                  ConstraintBundleItems::position_constraints_key);
  items.rotation_constraints = lookup_constraints(bundle,
                                                  ConstraintBundleItems::rotation_constraints_key);
  items.contact_constraints = lookup_constraints(bundle,
                                                 ConstraintBundleItems::contact_constraints_key);
}

template<bool debug_output> static ConstraintTypeInfo create_info__position_goal()
{
  return ConstraintTypeInfo{"Position Goal Constraints",
                            "Set position of a point to a target vector",
                            0,
                            position_goal::get_size,
                            position_goal::get_variable_indices,
                            position_goal::init_step,
                            position_goal::eval_positions<debug_output>,
                            {},
                            position_goal::linear_solve_elements};
}

template<bool debug_output> static ConstraintTypeInfo create_info__rotation_goal()
{
  return ConstraintTypeInfo{"Rotation Goal Constraints",
                            "Set orientation of an edge to a target rotation",
                            1,
                            rotation_goal::get_size,
                            rotation_goal::get_variable_indices,
                            rotation_goal::init_step,
                            rotation_goal::eval_positions<debug_output>,
                            {},
                            rotation_goal::linear_solve_elements};
}

template<bool debug_output> static ConstraintTypeInfo create_info__stretch_shear()
{
  return ConstraintTypeInfo{
      "Stretch/Shear Constraints",
      "Enforces edge length and aligns forward direction with the edge vector",
      2,
      stretch_shear::get_size,
      stretch_shear::get_variable_indices,
      stretch_shear::init_step,
      stretch_shear::eval_positions<debug_output>,
      {},
      stretch_shear::linear_solve_elements};
}

template<bool debug_output> static ConstraintTypeInfo create_info__bend_twist()
{
  return ConstraintTypeInfo{
      "Bend/Twist Constraints",
      "Enforces angles between neighboring edges to their relative rest orientation",
      3,
      bend_twist::get_size,
      bend_twist::get_variable_indices,
      bend_twist::init_step,
      bend_twist::eval_positions<debug_output>,
      {},
      bend_twist::linear_solve_elements};
}

template<bool debug_output> static ConstraintTypeInfo create_info__contact()
{
  return ConstraintTypeInfo{"Contact Constraints",
                            "Keep contact points from penetrating",
                            4,
                            contact::get_size,
                            contact::get_variable_indices,
                            contact::init_step,
                            contact::eval_positions<debug_output>,
                            contact::eval_velocities<debug_output>,
                            contact::linear_solve_elements};
}

using ConstraintTypeInfoMap = Map<std::string, ConstraintTypeInfo>;

static ConstraintTypeInfoMap create_type_info_map(const bool debug_check)
{
  ConstraintTypeInfoMap info_map;
  if (debug_check) {
    info_map.add_new(ConstraintBundleItems::stretch_constraints_key.identifiers().first(),
                     create_info__stretch_shear<true>());
    info_map.add_new(ConstraintBundleItems::bending_constraints_key.identifiers().first(),
                     create_info__bend_twist<true>());
    info_map.add_new(ConstraintBundleItems::position_constraints_key.identifiers().first(),
                     create_info__position_goal<true>());
    info_map.add_new(ConstraintBundleItems::rotation_constraints_key.identifiers().first(),
                     create_info__rotation_goal<true>());
    info_map.add_new(ConstraintBundleItems::contact_constraints_key.identifiers().first(),
                     create_info__contact<true>());
  }
  else {
    info_map.add_new(ConstraintBundleItems::stretch_constraints_key.identifiers().first(),
                     create_info__stretch_shear<false>());
    info_map.add_new(ConstraintBundleItems::bending_constraints_key.identifiers().first(),
                     create_info__bend_twist<false>());
    info_map.add_new(ConstraintBundleItems::position_constraints_key.identifiers().first(),
                     create_info__position_goal<false>());
    info_map.add_new(ConstraintBundleItems::rotation_constraints_key.identifiers().first(),
                     create_info__rotation_goal<false>());
    info_map.add_new(ConstraintBundleItems::contact_constraints_key.identifiers().first(),
                     create_info__contact<false>());
  }
  return info_map;
}

const ConstraintTypeInfo &get_info(const SocketInterfaceKey &key, const bool debug_check)
{
  static const ConstraintTypeInfoMap info_map = create_type_info_map(false);
  static const ConstraintTypeInfoMap info_map_debug = create_type_info_map(true);
  return debug_check ? info_map_debug.lookup(key.identifiers().first()) :
                       info_map.lookup(key.identifiers().first());
}

Span<ConstraintTypeInfo> get_constraint_info(const bool debug_output)
{
  /* Order of constraint passes is chosen by increasing "importance":
   * Later constraints have less residual error, and the last constraint type is solved exactly.
   */
  static Array<ConstraintTypeInfo> constraint_info = {
      get_info(ConstraintBundleItems::bending_constraints_key, true),
      get_info(ConstraintBundleItems::stretch_constraints_key, true),
      get_info(ConstraintBundleItems::rotation_constraints_key, true),
      get_info(ConstraintBundleItems::position_constraints_key, true),
      get_info(ConstraintBundleItems::contact_constraints_key, true),
  };
  static Array<ConstraintTypeInfo> constraint_info_debug = {
      get_info(ConstraintBundleItems::bending_constraints_key, false),
      get_info(ConstraintBundleItems::stretch_constraints_key, false),
      get_info(ConstraintBundleItems::rotation_constraints_key, false),
      get_info(ConstraintBundleItems::position_constraints_key, false),
      get_info(ConstraintBundleItems::contact_constraints_key, false),
  };
  return debug_output ? constraint_info_debug : constraint_info;
}

Span<ConstraintTypeInfo> get_constraint_info_ordered(const bool debug_output)
{
  /* TODO currently relies on fixed order in get_constraint_info(),
   * could also re-order based on some priority value. */
  return get_constraint_info(debug_output);
}

static void node_register()
{
  position_goal::node_register();
  rotation_goal::node_register();
  stretch_shear::node_register();
  bend_twist::node_register();
  contact::node_register();
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::xpbd_constraints
