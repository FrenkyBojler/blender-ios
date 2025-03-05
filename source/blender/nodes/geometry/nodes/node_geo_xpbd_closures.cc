/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute.hh"
#include "BKE_geometry_set.hh"

#include "NOD_xpbd_constraints.hh"

#include "node_geometry_util.hh"

#include <fmt/format.h>

namespace blender::nodes::xpbd_constraints {

template<typename T>
static AttributeReader<T> lookup_or_warn(AttributeAccessor &attributes,
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

static void position_goal__eval_positions(const ConstraintEvalParams &params,
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

  const IndexRange points_range = variables.positions.index_range();
  const Span<float3> positions = variables.positions;
  const Span<float3> old_positions = params.old_positions;

  group_mask.foreach_index(GrainSize(1024), [&](const int index) {
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
  });

  lambda_writer.finish();
  delta_position_writer.finish();

  r_active = VArray<bool>::ForSingle(true, attributes->domain_size(AttrDomain::Point));
  r_delta_positions = {*attributes->lookup<float3>("delta_position", AttrDomain::Point)};
  r_delta_rotations = {{}};
}

static Vector<VArray<int>> position_goal__get_mapping(const bke::GeometrySet &constraints)
{
  const PointCloudComponent &component = *constraints.get_component<PointCloudComponent>();
  bke::AttributeAccessor attributes = *component.attributes();

  return {*attributes.lookup_or_default<int>(ATTR_POINT1, AttrDomain::Point, 0)};
}

static void position_goal__init_position_step(bke::GeometrySet &constraints)
{
  PointCloudComponent &component = constraints.get_component_for_write<PointCloudComponent>();
  std::optional<bke::MutableAttributeAccessor> attributes = component.attributes_for_write();

  SpanAttributeWriter<float> lambda_writer = attributes->lookup_or_add_for_write_span<float>(
      "lambda", AttrDomain::Point);

  lambda_writer.span.fill(0.0f);

  lambda_writer.finish();
}

static void rotation_goal__eval_positions(const ConstraintEvalParams &params,
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
  VArraySpan<float> alphas = *attributes->lookup_or_default<float>(
      ATTR_ALPHA, AttrDomain::Point, 0.0f);
  VArraySpan<float> betas = *attributes->lookup_or_default<float>(
      ATTR_BETA, AttrDomain::Point, 0.0f);
  VArraySpan<math::Quaternion> goal_rotations = *lookup_or_warn<math::Quaternion>(
      *attributes,
      "goal_rotation",
      AttrDomain::Point,
      math::Quaternion::identity(),
      params.error_message_add);
  SpanAttributeWriter<float> lambda_w_writer = attributes->lookup_or_add_for_write_span<float>(
      "lambda_w", AttrDomain::Point);
  SpanAttributeWriter<float3> lambda_xyz_writer = attributes->lookup_or_add_for_write_span<float3>(
      "lambda_xyz", AttrDomain::Point);
  SpanAttributeWriter<float> delta_rotation_w_writer =
      attributes->lookup_or_add_for_write_span<float>("delta_rotation_w", AttrDomain::Point);
  SpanAttributeWriter<float3> delta_rotation_xyz_writer =
      attributes->lookup_or_add_for_write_span<float3>("delta_rotation_xyz", AttrDomain::Point);

  const IndexRange points_range = variables.positions.index_range();
  const Span<math::Quaternion> rotations = variables.rotations;
  const Span<math::Quaternion> old_rotations = params.old_rotations;

  group_mask.foreach_index(GrainSize(1024), [&](const int index) {
    const int point = points[index];
    if (!points_range.contains(point)) {
      return;
    }
    const math::Quaternion &goal = goal_rotations[index];
    float &lambda_w = lambda_w_writer.span[index];
    float3 &lambda_xyz = lambda_xyz_writer.span[index];
    float &delta_rotation_w = delta_rotation_w_writer.span[index];
    float3 &delta_rotation_xyz = delta_rotation_xyz_writer.span[index];

    const float4 lambda = float4(lambda_w, lambda_xyz);
    float4 residual;
    float4 delta_lambda;
    float4 delta_rotation;
    if constexpr (use_damping) {
      const float alpha = alphas[index] * params.inv_delta_time_squared;
      const float gamma = alphas[index] * betas[index] * params.inv_delta_time;
      xpbd_constraints::eval_rotation_goal2<linearized_quaternion>(goal,
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
      const float alpha = alphas[index] * params.inv_delta_time_squared;
      xpbd_constraints::eval_rotation_goal2<linearized_quaternion>(goal,
                                                                   alpha,
                                                                   0.0f,
                                                                   lambda,
                                                                   rotations[point],
                                                                   math::Quaternion::identity(),
                                                                   residual,
                                                                   delta_lambda,
                                                                   delta_rotation);
    }

    lambda_w += delta_lambda.x;
    lambda_xyz += delta_lambda.yzw();
    delta_rotation_w = delta_rotation.x;
    delta_rotation_xyz = delta_rotation.yzw();
  });

  lambda_w_writer.finish();
  lambda_xyz_writer.finish();
  delta_rotation_w_writer.finish();
  delta_rotation_xyz_writer.finish();

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

static Vector<VArray<int>> rotation_goal__get_mapping(const bke::GeometrySet &constraints)
{
  const PointCloudComponent &component = *constraints.get_component<PointCloudComponent>();
  bke::AttributeAccessor attributes = *component.attributes();

  return {*attributes.lookup_or_default<int>(ATTR_POINT1, AttrDomain::Point, 0)};
}

static void rotation_goal__init_position_step(bke::GeometrySet &constraints)
{
  PointCloudComponent &component = constraints.get_component_for_write<PointCloudComponent>();
  std::optional<bke::MutableAttributeAccessor> attributes = component.attributes_for_write();

  SpanAttributeWriter<float> lambda_w_writer = attributes->lookup_or_add_for_write_span<float>(
      "lambda_w", AttrDomain::Point);
  SpanAttributeWriter<float3> lambda_xyz_writer = attributes->lookup_or_add_for_write_span<float3>(
      "lambda_xyz", AttrDomain::Point);

  lambda_w_writer.span.fill(0.0f);
  lambda_xyz_writer.span.fill(float3(0.0f));

  lambda_w_writer.finish();
  lambda_xyz_writer.finish();
}

static void stretch_shear__eval_positions(const ConstraintEvalParams &params,
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

  const IndexRange points_range = variables.positions.index_range();
  const Span<float3> positions = variables.positions;
  const Span<math::Quaternion> rotations = variables.rotations;
  const Span<float3> old_positions = params.old_positions;
  const Span<math::Quaternion> old_rotations = params.old_rotations;

  group_mask.foreach_index(GrainSize(1024), [&](const int index) {
    const int point1 = points1[index];
    const int point2 = points2[index];
    if (!points_range.contains(point1) || !points_range.contains(point2)) {
      return;
    }
    const float weight_pos1 = params.position_weights[point1];
    const float weight_pos2 = params.position_weights[point2];
    const float weight_rot = params.rotation_weights[point1];
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
  });

  lambda_writer.finish();
  delta_position1_writer.finish();
  delta_position2_writer.finish();
  delta_rotation1_w_writer.finish();
  delta_rotation1_xyz_writer.finish();

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

static Vector<VArray<int>> stretch_shear__get_mapping(const bke::GeometrySet &constraints)
{
  const PointCloudComponent &component = *constraints.get_component<PointCloudComponent>();
  bke::AttributeAccessor attributes = *component.attributes();

  return {*attributes.lookup_or_default<int>(ATTR_POINT1, AttrDomain::Point, 0),
          *attributes.lookup_or_default<int>(ATTR_POINT2, AttrDomain::Point, 0)};
}

static void stretch_shear__init_position_step(bke::GeometrySet &constraints)
{
  PointCloudComponent &component = constraints.get_component_for_write<PointCloudComponent>();
  std::optional<bke::MutableAttributeAccessor> attributes = component.attributes_for_write();

  SpanAttributeWriter<float3> lambda_writer = attributes->lookup_or_add_for_write_span<float3>(
      "lambda", AttrDomain::Point);

  lambda_writer.span.fill(float3(0.0f));

  lambda_writer.finish();
}

static void bend_twist__eval_positions(const ConstraintEvalParams &params,
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
  /* XXX plain float4 attribute is not supported, have to store it as float + float3. */
  VArraySpan<float> darboux_w = *lookup_or_warn<float>(
      *attributes, "darboux_w", AttrDomain::Point, float(0.0f), params.error_message_add);
  VArraySpan<float3> darboux_xyz = *lookup_or_warn<float3>(
      *attributes, "darboux_xyz", AttrDomain::Point, float3(0.0f), params.error_message_add);
  SpanAttributeWriter<float> lambda_w_writer = attributes->lookup_or_add_for_write_span<float>(
      "lambda_w", AttrDomain::Point);
  SpanAttributeWriter<float3> lambda_xyz_writer = attributes->lookup_or_add_for_write_span<float3>(
      "lambda_xyz", AttrDomain::Point);
  SpanAttributeWriter<float> delta_rotation1_w_writer =
      attributes->lookup_or_add_for_write_span<float>("delta_rotation1_w", AttrDomain::Point);
  SpanAttributeWriter<float3> delta_rotation1_xyz_writer =
      attributes->lookup_or_add_for_write_span<float3>("delta_rotation1_xyz", AttrDomain::Point);
  SpanAttributeWriter<float> delta_rotation2_w_writer =
      attributes->lookup_or_add_for_write_span<float>("delta_rotation2_w", AttrDomain::Point);
  SpanAttributeWriter<float3> delta_rotation2_xyz_writer =
      attributes->lookup_or_add_for_write_span<float3>("delta_rotation2_xyz", AttrDomain::Point);

  const IndexRange points_range = variables.positions.index_range();
  const Span<math::Quaternion> rotations = variables.rotations;
  const Span<math::Quaternion> old_rotations = params.old_rotations;

  group_mask.foreach_index(GrainSize(1024), [&](const int index) {
    const int point1 = points1[index];
    const int point2 = points2[index];
    if (!points_range.contains(point1) || !points_range.contains(point2)) {
      return;
    }
    const float weight_rot1 = params.rotation_weights[point1];
    const float weight_rot2 = params.rotation_weights[point2];
    const math::Quaternion darboux_vector = math::Quaternion(darboux_w[index], darboux_xyz[index]);
    float &lambda_w = lambda_w_writer.span[index];
    float3 &lambda_xyz = lambda_xyz_writer.span[index];
    float &delta_rotation1_w = delta_rotation1_w_writer.span[index];
    float3 &delta_rotation1_xyz = delta_rotation1_xyz_writer.span[index];
    float &delta_rotation2_w = delta_rotation2_w_writer.span[index];
    float3 &delta_rotation2_xyz = delta_rotation2_xyz_writer.span[index];

    const float4 lambda = float4(lambda_w, lambda_xyz);
    float4 residual;
    float4 delta_lambda;
    float4 delta_rotation1, delta_rotation2;
    if constexpr (use_damping) {
      const float alpha = alphas[index] * params.inv_delta_time_squared;
      const float gamma = alphas[index] * betas[index] * params.inv_delta_time;
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

    lambda_w += delta_lambda.x;
    lambda_xyz += delta_lambda.yzw();
    delta_rotation1_w = delta_rotation1.x;
    delta_rotation1_xyz = delta_rotation1.yzw();
    delta_rotation2_w = delta_rotation2.x;
    delta_rotation2_xyz = delta_rotation2.yzw();
  });

  lambda_w_writer.finish();
  lambda_xyz_writer.finish();
  delta_rotation1_w_writer.finish();
  delta_rotation1_xyz_writer.finish();
  delta_rotation2_w_writer.finish();
  delta_rotation2_xyz_writer.finish();

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

static Vector<VArray<int>> bend_twist__get_mapping(const bke::GeometrySet &constraints)
{
  const PointCloudComponent &component = *constraints.get_component<PointCloudComponent>();
  bke::AttributeAccessor attributes = *component.attributes();

  return {*attributes.lookup_or_default<int>(ATTR_POINT1, AttrDomain::Point, 0),
          *attributes.lookup_or_default<int>(ATTR_POINT2, AttrDomain::Point, 0)};
}

static void bend_twist__init_position_step(bke::GeometrySet &constraints)
{
  PointCloudComponent &component = constraints.get_component_for_write<PointCloudComponent>();
  std::optional<bke::MutableAttributeAccessor> attributes = component.attributes_for_write();

  SpanAttributeWriter<float> lambda_w_writer = attributes->lookup_or_add_for_write_span<float>(
      "lambda_w", AttrDomain::Point);
  SpanAttributeWriter<float3> lambda_xyz_writer = attributes->lookup_or_add_for_write_span<float3>(
      "lambda_xyz", AttrDomain::Point);

  lambda_w_writer.span.fill(0.0f);
  lambda_xyz_writer.span.fill(float3(0.0f));

  lambda_w_writer.finish();
  lambda_xyz_writer.finish();
}

static void contact__eval_positions(const ConstraintEvalParams &params,
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

  const IndexRange points_range = variables.positions.index_range();
  const Span<float3> positions = variables.positions;
  const Span<math::Quaternion> rotations = variables.rotations;

  group_mask.foreach_index(GrainSize(1024), [&](const int index) {
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
    }
  });

  position_lambda_writer.finish();
  delta_position1_writer.finish();
  delta_rotation1_w_writer.finish();
  delta_rotation1_xyz_writer.finish();
  active_writer.finish();
  last_active_writer.finish();

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

static void contact__eval_velocities(const ConstraintEvalParams &params,
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

  const IndexRange points_range = variables.positions.index_range();
  const Span<float3> velocities = variables.velocities;
  const Span<float3> angular_velocities = variables.angular_velocities;

  group_mask.foreach_index(GrainSize(1024), [&](const int index) {
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
  });

  restitution_lambda_writer.finish();
  friction_lambda_writer.finish();
  delta_velocity1_writer.finish();
  delta_angular_velocity1_writer.finish();

  /* Note: velocity constraints are active if the position constraint has been active at any time
   * during the time step. */
  r_active = *attributes->lookup_or_default<bool>(ATTR_ACTIVE, AttrDomain::Point, false);
  r_delta_velocities = {*attributes->lookup<float3>("delta_velocity1", AttrDomain::Point)};
  r_delta_angular_velocities = {
      *attributes->lookup<float3>("delta_angular_velocity1", AttrDomain::Point)};
}

static Vector<VArray<int>> contact__get_mapping(const bke::GeometrySet &constraints)
{
  const PointCloudComponent &component = *constraints.get_component<PointCloudComponent>();
  bke::AttributeAccessor attributes = *component.attributes();

  return {*attributes.lookup_or_default<int>(ATTR_POINT1, AttrDomain::Point, 0)};
}

static void contact__init_position_step(bke::GeometrySet &constraints)
{
  PointCloudComponent &component = constraints.get_component_for_write<PointCloudComponent>();
  std::optional<bke::MutableAttributeAccessor> attributes = component.attributes_for_write();

  SpanAttributeWriter<float> position_lambda_writer =
      attributes->lookup_or_add_for_write_span<float>("position_lambda", AttrDomain::Point);

  position_lambda_writer.span.fill(0.0f);

  position_lambda_writer.finish();
}

static void contact__init_velocity_step(bke::GeometrySet &constraints)
{
  PointCloudComponent &component = constraints.get_component_for_write<PointCloudComponent>();
  std::optional<bke::MutableAttributeAccessor> attributes = component.attributes_for_write();

  SpanAttributeWriter<float> restitution_lambda_writer =
      attributes->lookup_or_add_for_write_span<float>("restitution_lambda", AttrDomain::Point);
  SpanAttributeWriter<float> friction_lambda_writer =
      attributes->lookup_or_add_for_write_span<float>("friction_lambda", AttrDomain::Point);

  restitution_lambda_writer.span.fill(0.0f);
  friction_lambda_writer.span.fill(0.0f);

  restitution_lambda_writer.finish();
  friction_lambda_writer.finish();
}

static Array<ConstraintTypeInfo> create_constraint_info()
{
  ConstraintTypeInfo position_goal_info = {"Position Goal Constraints",
                                           "Set position of a point to a target vector",
                                           0,
                                           position_goal__init_position_step,
                                           {},
                                           position_goal__eval_positions,
                                           {},
                                           position_goal__get_mapping};
  ConstraintTypeInfo rotation_goal_info = {"Rotation Goal Constraints",
                                           "Set orientation of an edge to a target rotation",
                                           1,
                                           rotation_goal__init_position_step,
                                           {},
                                           rotation_goal__eval_positions,
                                           {},
                                           rotation_goal__get_mapping};
  ConstraintTypeInfo stretch_shear_info = {
      "Stretch/Shear Constraints",
      "Enforces edge length and aligns forward direction with the edge vector",
      2,
      stretch_shear__init_position_step,
      {},
      stretch_shear__eval_positions,
      {},
      stretch_shear__get_mapping};
  ConstraintTypeInfo bend_twist_info = {
      "Bend/Twist Constraints",
      "Enforces angles between neighboring edges to their relative rest orientation",
      3,
      bend_twist__init_position_step,
      {},
      bend_twist__eval_positions,
      {},
      bend_twist__get_mapping};
  ConstraintTypeInfo contact_info = {"Contact Constraints",
                                     "Keep contact points from penetrating",
                                     4,
                                     contact__init_position_step,
                                     contact__init_velocity_step,
                                     contact__eval_positions,
                                     contact__eval_velocities,
                                     contact__get_mapping};

  /* Order of constraint passes is chosen by increasing "importance":
   * Later constraints have less residual error, and the last constraint type is solved exactly.
   */
  Array<ConstraintTypeInfo> constraint_info = {
      std::move(bend_twist_info),
      std::move(stretch_shear_info),
      std::move(rotation_goal_info),
      std::move(position_goal_info),
      std::move(contact_info),
  };

  return constraint_info;
}

Span<ConstraintTypeInfo> get_constraint_info()
{
  static const Array<ConstraintTypeInfo> constraint_info = create_constraint_info();
  return constraint_info;
}

Span<ConstraintTypeInfo> get_constraint_info_ordered()
{
  /* TODO currently relies on fixed order in get_constraint_info(),
   * could also re-order based on some priority value. */
  return get_constraint_info();
}

}  // namespace blender::nodes::xpbd_constraints
