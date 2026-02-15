/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_base.h"
#include "BLI_math_vector.hh"

#include "GEO_xpbd_constraint_utils.hh"

#include "BKE_pointcloud.hh"

namespace blender::xpbd {

struct DistanceConstraintResult {
  float delta_lambda = 0.0f;
  float3 offset0 = float3(0.0f);
  float3 offset1 = float3(0.0f);
};

inline DistanceConstraintResult evaluate_distance_constraint(const float3 &p0,
                                                             const float3 &p1,
                                                             const float inv_m0,
                                                             const float inv_m1,
                                                             const float rest_distance,
                                                             const float compliance_term,
                                                             const float lambda_prev)
{
  if (inv_m0 == 0.0f && inv_m1 == 0.0f) {
    return {};
  }

  const float3 p_diff = p1 - p0;
  float length;
  const float3 normalized_dir = math::normalize_and_get_length(p_diff, length);
  const float length_diff = length - rest_distance;
  const float delta_lambda = (-length_diff - compliance_term * lambda_prev) /
                             (inv_m0 + inv_m1 + compliance_term);

  const float3 offset0 = -delta_lambda * inv_m0 * normalized_dir;
  const float3 offset1 = delta_lambda * inv_m1 * normalized_dir;

  return {delta_lambda, offset0, offset1};
}

struct AlignRotationsConstraintResult {
  float4 delta_lambda;
  math::Quaternion offset0 = math::Quaternion(0.0f, 0.0f, 0.0f, 0.0f);
  math::Quaternion offset1 = math::Quaternion(0.0f, 0.0f, 0.0f, 0.0f);
};

inline AlignRotationsConstraintResult evaluate_align_rotations_constraint(
    const math::Quaternion &r0,
    const math::Quaternion &r1,
    const float3 &inertia0,
    const float3 &inertia1,
    const math::Quaternion &rest_rotation,
    const float compliance_term,
    const float4 &lambda_prev)
{
  const float inv_lumped_inertia0 = math::safe_rcp(0.5f * (inertia0.x + inertia0.y + inertia0.z));
  const float inv_lumped_inertia1 = math::safe_rcp(0.5f * (inertia1.x + inertia1.y + inertia1.z));
  if (inv_lumped_inertia0 == 0.0f && inv_lumped_inertia1 == 0.0f) {
    /* Everything is pinned, so the constraint can't do anything. */
    return {};
  }

  const float4 rest_rot_f = float4(rest_rotation);

  /* Note In "Position and Orientation Based Cosserat Rods" (Kugelstadt, Schoemer) the W
   * component of the Darboux vector is ignored. In "Sag-Free Initialization for Strand-Based
   * Hybrid Hair Simulation" (Hsu et al.) it is included to improve stability in cases where the
   * hair is bent at nearly 180 degrees. */
  const math::Quaternion &rot_diff = math::invert_normalized(r0) * r1;
  const float4 rot_diff_f = float4(rot_diff);

  const float4 residual_neg = rot_diff_f - rest_rot_f;
  const float4 residual_pos = rot_diff_f + rest_rot_f;
  const float4 residual = math::length_squared(residual_neg) < math::length_squared(residual_pos) ?
                              residual_neg :
                              residual_pos;

  const float4 delta_lambda = (-residual - compliance_term * lambda_prev) /
                              (inv_lumped_inertia0 + inv_lumped_inertia1 + compliance_term);

  const math::Quaternion offset0 = r1 * math::conjugate(
                                            math::Quaternion(delta_lambda * inv_lumped_inertia0));
  const math::Quaternion offset1 = r0 * math::Quaternion(delta_lambda * inv_lumped_inertia1);

  return {delta_lambda, offset0, offset1};
}

struct RodStretchAndShearConstraintResult {
  float3 delta_lambda_pos;
  float3 delta_lambda_rot;
  float3 offset0 = float3(0.0f);
  float3 offset1 = float3(0.0f);
  math::Quaternion offset_rot = math::Quaternion(0.0f, 0.0f, 0.0f, 0.0f);
};

inline RodStretchAndShearConstraintResult evaluate_rod_stretch_and_shear_constraint(
    const float3 &p0,
    const float3 &p1,
    const math::Quaternion &rot,
    const float inv_m0,
    const float inv_m1,
    const float3 &inertia,
    const float rest_length,
    const float compliance_term,
    const float3 &lambda_pos_prev,
    const float3 &lambda_rot_prev)
{
  /* Lumped weight for the rotation influence. The higher the inertia, the lower the change of
   * the rotation should be compared to the change in point positions. */
  const float inv_lumped_inertia = math::safe_rcp(0.5f * (inertia.x + inertia.y + inertia.z));

  if (inv_m0 == 0.0f && inv_m1 == 0.0f && inv_lumped_inertia == 0.0f) {
    /* Everything is pinned, so the constraint can't do anything. */
    return {};
  }

  /* TODO The positional and rotational parts use different residuals to avoid errors when the
   * current segment length deviates too much from the rest length. The rotational offset uses
   * the residual as the angle of rotation which becomes larger with stretching. To avoid
   * instabilities the rotation residual is computed relative to the current length.
   * This should be cleaned up and optimized if possible. */

  /* Current non-normalized tangent of the rod. */
  const float3 p_diff = p1 - p0;
  const float p_len = math::length(p_diff);
  /* Expected non-normalized tangent of the rod based on the rotation. */
  const float3 forward_rest = math::transform_point(rot, float3(0.0f, 0.0f, rest_length));
  const float3 forward = math::transform_point(rot, float3(0.0f, 0.0f, p_len));
  /* How much the rod is stretched and sheared. */
  const float3 residual_pos = p_diff - forward_rest;
  const float3 residual_rot = p_diff - forward;

  /* Based on "Position and Orientation Based Cosserat Rods" (Kugelstadt, Schömer, 2016). */
  const float weight_sum = inv_m0 + inv_m1 + 4.0f * inv_lumped_inertia * pow2f(rest_length);
  const float weight_sum_rot = inv_m0 + inv_m1 + 4.0f * inv_lumped_inertia * pow2f(p_len);
  const float3 delta_lambda_pos = (-residual_pos - compliance_term * lambda_pos_prev) /
                                  (weight_sum + compliance_term);
  const float3 delta_lambda_rot = (-residual_rot - compliance_term * lambda_rot_prev) /
                                  (weight_sum_rot + compliance_term);

  RodStretchAndShearConstraintResult result;
  result.delta_lambda_pos = delta_lambda_pos;
  result.delta_lambda_rot = delta_lambda_rot;
  result.offset0 = -delta_lambda_pos * inv_m0;
  result.offset1 = delta_lambda_pos * inv_m1;
  result.offset_rot = math::Quaternion(0.0f, -delta_lambda_rot * inv_lumped_inertia * p_len) *
                      rot * math::Quaternion(0, 0, 0, -1);
  return result;
}

class PinPositionConstraintSet : public TemplatedConstraintSet<PinPositionConstraintSet> {
 private:
  /** Indexed by constraint index. */
  Span<int> point_indices_;
  Span<float3> pin_positions_;
  Span<float> compliances_;
  MutableSpan<float> lambdas_;

 public:
  static constexpr StringRefNull debug_name = "Pinned Position";

  PinPositionConstraintSet(const int geo_i,
                           const Span<int> point_indices,
                           const Span<float3> pin_positions,
                           const Span<float> compliances,
                           const MutableSpan<float> lambdas)
      : TemplatedConstraintSet<PinPositionConstraintSet>(point_indices.size(), {geo_i}),
        point_indices_(point_indices),
        pin_positions_(pin_positions),
        compliances_(compliances),
        lambdas_(lambdas)
  {
  }

  void reset_force(const int constraint_i) const
  {
    lambdas_[constraint_i] = 0.0f;
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       const ConstraintSetParams &params,
                       const int constraint_i) const
  {
    const int geo_i = affected_geo_indices_[0];
    const int point_i = point_indices_[constraint_i];
    const DistanceConstraintResult result = evaluate_distance_constraint(
        params.position(geo_i, point_i),
        pin_positions_[constraint_i],
        params.inverse_mass(geo_i, point_i),
        0.0f,
        0.0f,
        compliances_[constraint_i] * params.compliance_term_factor,
        lambdas_[constraint_i]);
    lambdas_[constraint_i] += result.delta_lambda;
    updater.update_position(geo_i, point_i, result.offset0);
  }

  Vector<IndexMask> generate_independent_masks(IndexMaskMemory &memory) const override
  {
    return unary_constraints_to_independent_masks(point_indices_, memory);
  }

  bke::GeometrySet as_debug_geometry(Vector<bke::GSpanAttributeWriter> & /*r_attributes*/) const
  {
    return {};
  }
};

class PinRotationConstraintSet : public TemplatedConstraintSet<PinRotationConstraintSet> {
 private:
  /** Indexed by constraint index. */
  Span<float> compliances_;
  Span<int> point_indices_;
  Span<math::Quaternion> pin_rotations_;
  MutableSpan<float4> lambdas_;

 public:
  static constexpr StringRefNull debug_name = "Pin Rotation";

  PinRotationConstraintSet(const int geo_i,
                           const Span<int> point_indices,
                           const Span<math::Quaternion> pin_rotations,
                           const Span<float> compliances,
                           MutableSpan<float4> lambdas)
      : TemplatedConstraintSet<PinRotationConstraintSet>(point_indices.size(), {geo_i}),
        compliances_(compliances),
        point_indices_(point_indices),
        pin_rotations_(pin_rotations),
        lambdas_(lambdas)
  {
  }

  void reset_force(const int constraint_i) const
  {
    lambdas_[constraint_i] = float4(0.0f);
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       const ConstraintSetParams &params,
                       const int constraint_i) const
  {
    const int geo_i = affected_geo_indices_[0];
    const int point_i = point_indices_[constraint_i];
    const AlignRotationsConstraintResult result = evaluate_align_rotations_constraint(
        params.rotation(geo_i, point_i),
        pin_rotations_[constraint_i],
        params.moment_of_inertia(geo_i, point_i),
        float3(std::numeric_limits<float>::infinity()),
        math::Quaternion::identity(),
        compliances_[constraint_i] * params.compliance_term_factor,
        lambdas_[constraint_i]);
    lambdas_[constraint_i] += result.delta_lambda;
    updater.update_rotation(geo_i, point_i, result.offset0);
  }

  Vector<IndexMask> generate_independent_masks(IndexMaskMemory &memory) const override
  {
    return unary_constraints_to_independent_masks(point_indices_, memory);
  }

  bke::GeometrySet as_debug_geometry(Vector<bke::GSpanAttributeWriter> & /*r_attributes*/) const
  {
    return {};
  }
};

class DistanceConstraintSet : public TemplatedConstraintSet<DistanceConstraintSet> {
 private:
  int geo_i_;
  Span<int2> point_pairs_;
  Span<float> distances_;
  Span<float> compliance_terms_;
  MutableSpan<float> lambdas_;

 public:
  static constexpr StringRefNull debug_name = "Distance";

  DistanceConstraintSet(const int geo_i,
                        const Span<int2> point_pairs,
                        const Span<float> distances,
                        const Span<float> compliance_terms,
                        MutableSpan<float> lambdas)
      : TemplatedConstraintSet<DistanceConstraintSet>(point_pairs.size(), {geo_i}),
        geo_i_(geo_i),
        point_pairs_(point_pairs),
        distances_(distances),
        compliance_terms_(compliance_terms),
        lambdas_(lambdas)
  {
    BLI_assert(point_pairs.size() == distances.size());
  }

  void reset_force(const int constraint_i) const
  {
    lambdas_[constraint_i] = 0.0f;
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       const ConstraintSetParams &params,
                       const int constraint_i) const
  {
    const int2 &point_pair = point_pairs_[constraint_i];
    const int point_i0 = point_pair[0];
    const int point_i1 = point_pair[1];
    const DistanceConstraintResult result = evaluate_distance_constraint(
        params.position(geo_i_, point_i0),
        params.position(geo_i_, point_i1),
        params.inverse_mass(geo_i_, point_i0),
        params.inverse_mass(geo_i_, point_i1),
        distances_[constraint_i],
        compliance_terms_[constraint_i],
        lambdas_[constraint_i]);
    lambdas_[constraint_i] += result.delta_lambda;
    updater.update_position(geo_i_, point_i0, result.offset0);
    updater.update_position(geo_i_, point_i1, result.offset1);
  }

  Vector<IndexMask> generate_independent_masks(IndexMaskMemory &memory) const override
  {
    return binary_constraints_to_independent_masks(point_pairs_, memory);
  }

  bke::GeometrySet as_debug_geometry(Vector<bke::GSpanAttributeWriter> & /*r_attributes*/) const
  {
    return {};
  }
};

/* Constraint implementation for static and dynamic friction is based on
 * "Detailed Rigid Body Simulation with Extended Position Based Dynamics",
 * Mueller, Macklin, et al., 2020 */
class CollisionPlaneConstraintSet : public TemplatedConstraintSet<CollisionPlaneConstraintSet> {
 private:
  int geo_i_;
  Span<int> points_;
  Span<float3> contact_points_on_plane_;
  Span<float3> contact_points_motion_;
  Span<float3> separating_axes_;
  Span<float> compliance_terms_;
  Span<float> static_frictions_;
  Span<float> dynamic_frictions_;
  MutableSpan<bool> active_states_;
  MutableSpan<float> lambdas_normal_;

  enum DebugAttribute {
    ContactPointOnPlane,
    PointIndex,
    SeparatingAxis,
    Active,
    IsStatic,
    StaticFriction,
    DynamicFriction,
    ColliderMotion,
    LambdaNormal,
  };

 public:
  static constexpr StringRefNull debug_name = "Collision Plane";

  CollisionPlaneConstraintSet(const int geo_i,
                              const Span<int> points,
                              const Span<float3> contact_points_on_plane,
                              const Span<float3> contact_points_motion,
                              const Span<float3> separating_axes,
                              const Span<float> compliance_terms,
                              const Span<float> static_frictions,
                              const Span<float> dynamic_frictions,
                              MutableSpan<bool> active_states,
                              MutableSpan<float> lambdas_normal)
      : TemplatedConstraintSet<CollisionPlaneConstraintSet>(points.size(), {geo_i}),
        geo_i_(geo_i),
        points_(points),
        contact_points_on_plane_(contact_points_on_plane),
        contact_points_motion_(contact_points_motion),
        separating_axes_(separating_axes),
        compliance_terms_(compliance_terms),
        static_frictions_(static_frictions),
        dynamic_frictions_(dynamic_frictions),
        active_states_(active_states),
        lambdas_normal_(lambdas_normal)
  {
  }

  void reset_force(const int constraint_i) const
  {
    active_states_[constraint_i] = false;
    lambdas_normal_[constraint_i] = 0.0f;
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       const ConstraintSetParams &params,
                       const int constraint_i) const
  {
    const int point_i = points_[constraint_i];
    const float3 &pos = params.position(geo_i_, point_i);
    const float3 &plane_pos = contact_points_on_plane_[constraint_i];
    const float3 &axis = separating_axes_[constraint_i];
    const float compliance_term = compliance_terms_[constraint_i];
    const float inv_m = params.inverse_mass(geo_i_, point_i);
    updater.write_debug_attribute(DebugAttribute::ContactPointOnPlane, constraint_i, plane_pos);
    updater.write_debug_attribute(DebugAttribute::PointIndex, constraint_i, point_i);
    updater.write_debug_attribute(DebugAttribute::SeparatingAxis, constraint_i, axis);
    updater.write_debug_attribute(
        DebugAttribute::StaticFriction, constraint_i, static_frictions_[constraint_i]);
    updater.write_debug_attribute(
        DebugAttribute::DynamicFriction, constraint_i, dynamic_frictions_[constraint_i]);
    updater.write_debug_attribute(
        DebugAttribute::ColliderMotion, constraint_i, contact_points_motion_[constraint_i]);
    if (inv_m <= 0.0f) {
      /* Points with infinite mass are pinned and don't collide dynamically. */
      updater.write_debug_attribute(DebugAttribute::Active, constraint_i, false);
      updater.write_debug_attribute(DebugAttribute::IsStatic, constraint_i, false);
      updater.write_debug_attribute(
          DebugAttribute::LambdaNormal, constraint_i, lambdas_normal_[constraint_i]);
      return;
    }

    const float3 diff = pos - plane_pos;
    const float normal_distance = math::dot(diff, axis);
    const bool is_active = normal_distance < 0.0f;
    active_states_[constraint_i] = is_active;
    if (!is_active) {
      updater.write_debug_attribute(DebugAttribute::Active, constraint_i, false);
      updater.write_debug_attribute(DebugAttribute::IsStatic, constraint_i, false);
      updater.write_debug_attribute(
          DebugAttribute::LambdaNormal, constraint_i, lambdas_normal_[constraint_i]);
      return;
    }
    updater.write_debug_attribute(DebugAttribute::Active, constraint_i, true);

    /* Positional correction for penetration. */
    float3 offset = float3(0.0f);
    float &lambda_normal = lambdas_normal_[constraint_i];
    if (normal_distance < 0.0f) {
      const float delta_lambda_normal = -normal_distance / (inv_m + compliance_term);
      offset += delta_lambda_normal * inv_m * axis;
      lambda_normal += delta_lambda_normal;
    }
    updater.write_debug_attribute(DebugAttribute::LambdaNormal, constraint_i, lambda_normal);

    /* Apply static friction as a direct positional update. */
    const float3 &prev_pos = params.prev_position(geo_i_, point_i);
    const float3 &collider_velocity = contact_points_motion_[constraint_i];
    const float3 velocity = (pos - prev_pos) - collider_velocity;
    const float3 velocity_tangent = velocity - math::dot(velocity, axis) * axis;
    const float lambda_tangent_sq = math::length_squared(velocity_tangent /
                                                         (inv_m + compliance_term));
    const bool is_static = lambda_tangent_sq <
                           math::square(static_frictions_[constraint_i] * lambda_normal);
    updater.write_debug_attribute(DebugAttribute::IsStatic, constraint_i, is_static);
    if (is_static) {
      offset -= velocity_tangent * inv_m / (inv_m + compliance_term);
    }

    updater.update_position(geo_i_, point_i, offset);
  }

  Vector<IndexMask> generate_independent_masks(IndexMaskMemory &memory) const override
  {
    return unary_constraints_to_independent_masks(points_, memory);
  }

  bke::GeometrySet as_debug_geometry(Vector<bke::GSpanAttributeWriter> &r_attributes) const
  {
    r_attributes.resize(9);
    PointCloud *pointcloud = BKE_pointcloud_new_nomain(points_.size());
    bke::MutableAttributeAccessor attributes = pointcloud->attributes_for_write();
    r_attributes[DebugAttribute::ContactPointOnPlane] =
        attributes.lookup_or_add_for_write_only_span(
            "position", bke::AttrDomain::Point, bke::AttrType::Float3);
    r_attributes[DebugAttribute::PointIndex] = attributes.lookup_or_add_for_write_only_span(
        "point", bke::AttrDomain::Point, bke::AttrType::Int32);
    r_attributes[DebugAttribute::SeparatingAxis] = attributes.lookup_or_add_for_write_only_span(
        "separating_axis", bke::AttrDomain::Point, bke::AttrType::Float3);
    r_attributes[DebugAttribute::Active] = attributes.lookup_or_add_for_write_only_span(
        "active", bke::AttrDomain::Point, bke::AttrType::Bool);
    r_attributes[DebugAttribute::IsStatic] = attributes.lookup_or_add_for_write_only_span(
        "is_static", bke::AttrDomain::Point, bke::AttrType::Bool);
    r_attributes[DebugAttribute::StaticFriction] = attributes.lookup_or_add_for_write_only_span(
        "static_friction", bke::AttrDomain::Point, bke::AttrType::Float);
    r_attributes[DebugAttribute::DynamicFriction] = attributes.lookup_or_add_for_write_only_span(
        "dynamic_friction", bke::AttrDomain::Point, bke::AttrType::Float);
    r_attributes[DebugAttribute::ColliderMotion] = attributes.lookup_or_add_for_write_only_span(
        "collider_motion", bke::AttrDomain::Point, bke::AttrType::Float3);
    r_attributes[DebugAttribute::LambdaNormal] = attributes.lookup_or_add_for_write_only_span(
        "lambda_normal", bke::AttrDomain::Point, bke::AttrType::Float);
    return bke::GeometrySet::from_pointcloud(pointcloud);
  }
};

class FrictionConstraintSet : public TemplatedVelocityConstraintSet<FrictionConstraintSet> {
 private:
  int geo_i_;
  /* Constraint index for each point. */
  Span<int> points_;
  Span<float3> separating_axes_;
  Span<float3> contact_velocities_;
  /* Constraint multiplier lambda for the normal displacement divided by time step. */
  Span<float> dynamic_friction_terms_;
  Span<float> lambdas_normal_;
  MutableSpan<float> lambdas_;

 public:
  static constexpr StringRefNull debug_name = "Friction";

  FrictionConstraintSet(const int geo_i,
                        const Span<int> points,
                        const Span<float3> separating_axes,
                        const Span<float3> contact_velocities,
                        const Span<float> dynamic_friction_terms,
                        const Span<float> lambdas_normal,
                        MutableSpan<float> lambdas)
      : TemplatedVelocityConstraintSet<FrictionConstraintSet>(points.size(), {geo_i}),
        geo_i_(geo_i),
        points_(points),
        separating_axes_(separating_axes),
        contact_velocities_(contact_velocities),
        dynamic_friction_terms_(dynamic_friction_terms),
        lambdas_normal_(lambdas_normal),
        lambdas_(lambdas)
  {
  }

  void reset_force(const int constraint_i) const
  {
    lambdas_[constraint_i] = 0.0f;
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       const ConstraintSetParams &params,
                       const int constraint_i) const
  {
    const int point_i = points_[constraint_i];
    const float3 &axis = separating_axes_[constraint_i];
    const float3 &contact_velocity = contact_velocities_[constraint_i];
    const float3 &velocity = params.velocity(geo_i_, point_i) - contact_velocity;
    const float3 velocity_tangent = velocity - math::dot(velocity, axis) * axis;
    float residual;
    const float3 gradient = math::normalize_and_get_length(velocity_tangent, residual);
    const float delta_lambda = std::min(
        dynamic_friction_terms_[constraint_i] * lambdas_normal_[constraint_i], residual);

    lambdas_[constraint_i] += delta_lambda;
    updater.update_velocity(geo_i_, point_i, -gradient * delta_lambda);
  }

  bke::GeometrySet as_debug_geometry(Vector<bke::GSpanAttributeWriter> & /*r_attributes*/) const
  {
    return {};
  }
};

class MinimumDistanceConstraintSet : public TemplatedConstraintSet<MinimumDistanceConstraintSet> {
 private:
  int geo_i_;
  Span<int2> points_;
  Span<float> min_distances_;
  Span<float> compliance_terms_;
  MutableSpan<float> lambdas_;

 public:
  static constexpr StringRefNull debug_name = "Minimum Distance";

  MinimumDistanceConstraintSet(const int geo_i,
                               const Span<int2> points,
                               const Span<float> min_distances,
                               const Span<float> compliance_terms,
                               MutableSpan<float> lambdas)
      : TemplatedConstraintSet<MinimumDistanceConstraintSet>(points.size(), {geo_i}),
        geo_i_(geo_i),
        points_(points),
        min_distances_(min_distances),
        compliance_terms_(compliance_terms),
        lambdas_(lambdas)
  {
  }

  void reset_force(const int constraint_i) const
  {
    lambdas_[constraint_i] = 0.0f;
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       const ConstraintSetParams &params,
                       const int constraint_i) const
  {
    const int point_i0 = points_[constraint_i][0];
    const int point_i1 = points_[constraint_i][1];
    const float min_distance = min_distances_[constraint_i];
    const float3 &p0 = params.position(geo_i_, point_i0);
    const float3 &p1 = params.position(geo_i_, point_i1);
    const float3 diff = p1 - p0;
    float distance;
    const float3 normalized_dir = math::normalize_and_get_length(diff, distance);
    if (distance >= min_distance) {
      return;
    }
    const float inv_m0 = params.inverse_mass(geo_i_, point_i0);
    const float inv_m1 = params.inverse_mass(geo_i_, point_i1);
    const float length_diff = min_distance - distance;
    if (length_diff < 1e-5f) {
      return;
    }
    const float compliance_term = compliance_terms_[constraint_i];
    float &lambda = lambdas_[constraint_i];
    const float delta_lambda = (length_diff - compliance_term * lambda) /
                               (inv_m0 + inv_m1 + compliance_term);
    lambda += delta_lambda;
    const float3 offset0 = -lambda * inv_m0 * normalized_dir;
    const float3 offset1 = lambda * inv_m1 * normalized_dir;
    updater.update_position(geo_i_, point_i0, offset0);
    updater.update_position(geo_i_, point_i1, offset1);
  }

  Vector<IndexMask> generate_independent_masks(IndexMaskMemory &memory) const override
  {
    return binary_constraints_to_independent_masks(points_, memory);
  }

  bke::GeometrySet as_debug_geometry(Vector<bke::GSpanAttributeWriter> & /*r_attributes*/) const
  {
    return {};
  }
};

class PressureConstraintSet : public TemplatedConstraintSet<PressureConstraintSet> {
 private:
  int geo_i_;
  Span<int3> tris_;
  Span<int> corner_verts_;
  float pressure_;
  float initial_volume_;
  float &lambda_;

 public:
  static constexpr StringRefNull debug_name = "Pressure";

  PressureConstraintSet(const int geo_i,
                        const Span<int3> tris,
                        const Span<int> corner_verts,
                        const float pressure,
                        const float initial_volume,
                        float &lambda)
      : TemplatedConstraintSet<PressureConstraintSet>(1, {geo_i}),
        geo_i_(geo_i),
        tris_(tris),
        corner_verts_(corner_verts),
        pressure_(pressure),
        initial_volume_(initial_volume),
        lambda_(lambda)
  {
  }

  void reset_force(const int /*constraint_i*/) const
  {
    lambda_ = 0.0f;
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       const ConstraintSetParams &params,
                       const int constraint_i) const
  {
    /* This is just a single global constraint. */
    BLI_assert(constraint_i == 0);
    UNUSED_VARS_NDEBUG(constraint_i);

    const Span<float3> positions = params.positions(geo_i_);
    const Span<float> inverse_masses = params.inverse_masses(geo_i_);
    const float current_volume = compute_volume(tris_, corner_verts_, positions);
    const float volume_diff = current_volume - pressure_ * initial_volume_;

    const int points_num = positions.size();
    Array<float3> gradients(points_num, float3(0.0f));
    for (const int tri_i : tris_.index_range()) {
      const int3 &tri = tris_[tri_i];
      const int point_i0 = corner_verts_[tri[0]];
      const int point_i1 = corner_verts_[tri[1]];
      const int v2 = corner_verts_[tri[2]];
      const float3 &p0 = positions[point_i0];
      const float3 &p1 = positions[point_i1];
      const float3 &p2 = positions[v2];
      const float3 c_1_2 = math::cross(p1, p2);
      const float3 c_2_0 = math::cross(p2, p0);
      const float3 c_0_1 = math::cross(p0, p1);
      const float3 c = c_1_2 + c_2_0 + c_0_1;
      gradients[point_i0] += c;
      gradients[point_i1] += c;
      gradients[v2] += c;
    }

    float delta_lambda_divisor = 0.0f;
    for (const int i : IndexRange(points_num)) {
      const float inverse_mass = inverse_masses[i];
      delta_lambda_divisor += math::length_squared(gradients[i]) * inverse_mass;
    }
    const float delta_lambda = math::safe_divide(volume_diff, delta_lambda_divisor);
    lambda_ += delta_lambda;
    threading::parallel_for(IndexRange(points_num), 512, [&](const IndexRange range) {
      for (const int i : range) {
        const float inverse_mass = inverse_masses[i];
        if (inverse_mass <= 0.0f) {
          continue;
        }
        const float3 offset = -delta_lambda * inverse_mass * gradients[i];
        updater.update_position(geo_i_, i, offset);
      }
    });
  }

  static float compute_volume(const Span<int3> tris,
                              const Span<int> corner_verts,
                              const Span<float3> positions)
  {
    const float volume = threading::parallel_deterministic_reduce<float>(
        tris.index_range(),
        512,
        0.0f,
        [&](const IndexRange range, float volume) {
          for (const int tri_i : range) {
            const int3 &tri = tris[tri_i];
            const int point_i0 = corner_verts[tri[0]];
            const int point_i1 = corner_verts[tri[1]];
            const int v2 = corner_verts[tri[2]];
            const float3 &p0 = positions[point_i0];
            const float3 &p1 = positions[point_i1];
            const float3 &p2 = positions[v2];
            volume += math::dot(math::cross(p0, p1), p2);
          }
          return volume;
        },
        [&](const float a, const float b) { return a + b; });
    return volume / 6.0f;
  }

  Vector<IndexMask> generate_independent_masks(IndexMaskMemory & /*memory*/) const override
  {
    return {IndexMask(1)};
  }

  bke::GeometrySet as_debug_geometry(Vector<bke::GSpanAttributeWriter> & /*r_attributes*/) const
  {
    return {};
  }
};

/**
 * Considers a single rod at a time. Tries to enforce that the rotation of the rod is aligned with
 * the actual tangent of the rod. If it is misaligned, it moves the start position, end position
 * and rotation of the frame. At the same time, it enforces a certain length.
 */
class RodStretchAndShearConstraintSet
    : public TemplatedConstraintSet<RodStretchAndShearConstraintSet> {
 private:
  /** Curves that are effected by this constraint set. Each curve is seen as one constraint. */
  IndexRange curves_range_;
  OffsetIndices<int> points_by_curve_;

  /** Indexed by point index. */
  Span<float> rest_lengths_;
  MutableSpan<float3> lambdas_pos_;
  MutableSpan<float3> lambdas_rot_;

  /** Indexed by `point_i - first_point_i_in_constraint_set`. */
  Span<float> compliances_;

 public:
  static constexpr StringRefNull debug_name = "Rod Stretch and Shear";

  RodStretchAndShearConstraintSet(const int geo_i,
                                  const IndexRange curves_range,
                                  const OffsetIndices<int> points_by_curve,
                                  const Span<float> rest_lengths,
                                  const Span<float> compliances,
                                  MutableSpan<float3> lambdas_pos,
                                  MutableSpan<float3> lambdas_rot)
      : TemplatedConstraintSet<RodStretchAndShearConstraintSet>(curves_range.size(), {geo_i}),
        curves_range_(curves_range),
        points_by_curve_(points_by_curve),
        rest_lengths_(rest_lengths),
        lambdas_pos_(lambdas_pos),
        lambdas_rot_(lambdas_rot),
        compliances_(compliances)
  {
  }

  void reset_force(const int constraint_i) const
  {
    const int curve_i = curves_range_[constraint_i];
    const IndexRange points = points_by_curve_[curve_i];
    lambdas_pos_.slice(points).fill(float3(0.0f));
    lambdas_rot_.slice(points).fill(float3(0.0f));
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       const ConstraintSetParams &params,
                       const int constraint_i) const
  {
    const int curve_i = curves_range_[constraint_i];
    const IndexRange points = points_by_curve_[curve_i];
    const int geo_i = affected_geo_indices_[0];
    const int first_point_i_in_constraint_set = points_by_curve_[curves_range_.first()].first();

    /* Could try implementing bilateral interleaving ordering for better stability. */
    for (const int point_i0 : points.drop_back(1)) {
      const int point_i1 = point_i0 + 1;
      const float compliance = compliances_[point_i0 - first_point_i_in_constraint_set];
      const RodStretchAndShearConstraintResult result = evaluate_rod_stretch_and_shear_constraint(
          params.position(geo_i, point_i0),
          params.position(geo_i, point_i1),
          params.rotation(geo_i, point_i0),
          params.inverse_mass(geo_i, point_i0),
          params.inverse_mass(geo_i, point_i1),
          params.moment_of_inertia(geo_i, point_i0),
          rest_lengths_[point_i0],
          compliance * params.compliance_term_factor,
          lambdas_pos_[point_i0],
          lambdas_rot_[point_i0]);
      lambdas_pos_[point_i0] += result.delta_lambda_pos;
      lambdas_rot_[point_i0] += result.delta_lambda_rot;
      updater.update_position(geo_i, point_i0, result.offset0);
      updater.update_position(geo_i, point_i1, result.offset1);
      updater.update_rotation(geo_i, point_i0, result.offset_rot);
    }
  }

  Vector<IndexMask> generate_independent_masks(IndexMaskMemory & /*memory*/) const override
  {
    return all_independent_masks(constraints_num_);
  }

  bke::GeometrySet as_debug_geometry(Vector<bke::GSpanAttributeWriter> & /*r_attributes*/) const
  {
    return {};
  }
};

/** Aligns rotations of two consecutive rods based on a rest rotation. */
class RodBendAndTwistConstraintSet : public TemplatedConstraintSet<RodBendAndTwistConstraintSet> {
 private:
  /** Curves that are effected by this constraint set. Each curve is seen as one constraint. */
  IndexRange curves_range_;
  OffsetIndices<int> points_by_curve_;

  /** Indexed by point index. */
  Span<math::Quaternion> rest_rotations_;
  MutableSpan<float4> lambdas_;

  /** Indexed by `point_i - first_point_i_in_constraint_set`. */
  Span<float> compliances_;

 public:
  static constexpr StringRefNull debug_name = "Rod Bend and Twist";

  RodBendAndTwistConstraintSet(const int geo_i,
                               const IndexRange curves_range,
                               const OffsetIndices<int> points_by_curve,
                               const Span<math::Quaternion> rest_rotations,
                               const Span<float> compliances,
                               MutableSpan<float4> lambdas)
      : TemplatedConstraintSet<RodBendAndTwistConstraintSet>(curves_range.size(), {geo_i}),
        curves_range_(curves_range),
        points_by_curve_(points_by_curve),
        rest_rotations_(rest_rotations),
        lambdas_(lambdas),
        compliances_(compliances)
  {
  }

  void reset_force(const int constraint_i) const
  {
    const int curve_i = curves_range_[constraint_i];
    const IndexRange points = points_by_curve_[curve_i];
    lambdas_.slice(points).fill(float4(0.0f));
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       const ConstraintSetParams &params,
                       const int constraint_i) const
  {
    const int curve_i = curves_range_[constraint_i];
    const IndexRange points = points_by_curve_[curve_i];
    const int geo_i = affected_geo_indices_[0];
    const int first_point_i_in_constraint_set = points_by_curve_[curves_range_.first()].first();

    /* Could implement bilateral interleaving ordering for better stability. */
    /* Note that the last segment does not have this constraint, because the rotation of the last
     * point in the rod is meaningless.*/
    for (const int point_i0 : points.drop_back(2)) {
      const int point_i1 = point_i0 + 1;
      const float compliance = compliances_[point_i0 - first_point_i_in_constraint_set];
      const AlignRotationsConstraintResult result = evaluate_align_rotations_constraint(
          params.rotation(geo_i, point_i0),
          params.rotation(geo_i, point_i1),
          params.moment_of_inertia(geo_i, point_i0),
          params.moment_of_inertia(geo_i, point_i1),
          rest_rotations_[point_i0],
          compliance * params.compliance_term_factor,
          lambdas_[point_i0]);
      lambdas_[point_i0] += result.delta_lambda;
      updater.update_rotation(geo_i, point_i0, result.offset0);
      updater.update_rotation(geo_i, point_i1, result.offset1);
    }
  }

  Vector<IndexMask> generate_independent_masks(IndexMaskMemory & /*memory*/) const override
  {
    return all_independent_masks(constraints_num_);
  }

  bke::GeometrySet as_debug_geometry(Vector<bke::GSpanAttributeWriter> & /*r_attributes*/) const
  {
    return {};
  }
};

class AlignPositionsConstraintSet : public TemplatedConstraintSet<AlignPositionsConstraintSet> {
 private:
  /* Indexed by constraint index. */
  OffsetIndices<int> offsets_;
  Span<float> compliance_terms_;

  /* Indexed by offset indices. */
  Span<int> geo_indices_;
  Span<int> point_indices_;
  MutableSpan<float> lambdas_;

 public:
  static constexpr StringRefNull debug_name = "Align Positions";

  AlignPositionsConstraintSet(OffsetIndices<int> offsets,
                              Span<float> compliance_terms,
                              Span<int> geo_indices,
                              Span<int> point_indices,
                              MutableSpan<float> lambdas)
      : TemplatedConstraintSet<AlignPositionsConstraintSet>(
            point_indices.size(), VectorSet<int>(geo_indices).extract_vector()),
        offsets_(offsets),
        compliance_terms_(compliance_terms),
        geo_indices_(geo_indices),
        point_indices_(point_indices),
        lambdas_(lambdas)
  {
  }

  void reset_force(const int constraint_i) const
  {
    lambdas_[constraint_i] = 0.0f;
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       const ConstraintSetParams &params,
                       const int constraint_i) const
  {
    const IndexRange range = offsets_[constraint_i];
    BLI_assert(!range.is_empty());
    const float3 center = this->compute_center(range, params);

    const float compliance_term = compliance_terms_[constraint_i];
    for (const int i : range) {
      const int point_i = point_indices_[i];
      const int geo_i = geo_indices_[i];
      const float inv_m = params.inverse_mass(geo_i, point_i);
      if (inv_m <= 0.0f) {
        /* Ignored pinned position. */
        continue;
      }
      const float3 &pos = params.position(geo_i, point_i);
      const DistanceConstraintResult result = evaluate_distance_constraint(
          pos, center, inv_m, 0.0f, 0.0f, compliance_term, lambdas_[i]);
      lambdas_[i] += result.delta_lambda;
      updater.update_position(geo_i, point_i, result.offset0);
    }
  }

  float3 compute_center(const IndexRange range, const ConstraintSetParams &params) const
  {
    float3 center_sum = float3(0.0f);
    float mass_sum = 0.0f;
    /* Computed mass weighted center. */
    for (const int i : range) {
      const int point_i = point_indices_[i];
      const int geo_i = geo_indices_[i];
      const float3 &pos = params.position(geo_i, point_i);
      const float inv_m = params.inverse_mass(geo_i, point_i);
      if (inv_m <= 0.0f) {
        /* This position is pinned, so it becomes the center. */
        return pos;
      }
      const float mass = math::rcp(inv_m);
      center_sum += pos * mass;
      mass_sum += mass;
    }
    const float3 center = center_sum / mass_sum;
    return center;
  }

  Vector<IndexMask> generate_independent_masks(IndexMaskMemory &memory) const override
  {
    return n_ary_constraints_to_independent_masks_multi(
        {offsets_, geo_indices_}, {offsets_, point_indices_}, memory);
  }

  bke::GeometrySet as_debug_geometry(Vector<bke::GSpanAttributeWriter> & /*r_attributes*/) const
  {
    return {};
  }
};

class AttachUVSurfaceConstraintSet : public TemplatedConstraintSet<AttachUVSurfaceConstraintSet> {
 private:
  int mesh_geo_i_;
  int points_geo_i_;
  Span<int> indices_;
  Span<int3> triangle_indices_;
  Span<float3> bary_weights_;
  Span<float> compliance_terms_;
  MutableSpan<float3> lambdas_;

 public:
  static constexpr StringRefNull debug_name = "Attach UV Surface";

  AttachUVSurfaceConstraintSet(const int mesh_geo_i,
                               const int points_geo_i,
                               const Span<int> indices,
                               const Span<int3> triangle_indices,
                               const Span<float3> bary_weights,
                               const Span<float> compliance_terms,
                               MutableSpan<float3> lambdas)
      : TemplatedConstraintSet<AttachUVSurfaceConstraintSet>(indices.size(),
                                                             {mesh_geo_i, points_geo_i}),
        mesh_geo_i_(mesh_geo_i),
        points_geo_i_(points_geo_i),
        indices_(indices),
        triangle_indices_(triangle_indices),
        bary_weights_(bary_weights),
        compliance_terms_(compliance_terms),
        lambdas_(lambdas)
  {
  }

  void reset_force(const int constraint_i) const
  {
    lambdas_[constraint_i] = float3(0.0f);
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       const ConstraintSetParams &params,
                       const int constraint_i) const
  {
    const int point_i = indices_[constraint_i];
    const float3 bary_weights = bary_weights_[constraint_i];
    const float compliance_term = compliance_terms_[constraint_i];

    const float inv_mass = params.inverse_mass(points_geo_i_, point_i);
    const float3 &p = params.position(points_geo_i_, point_i);

    const int3 triangle = triangle_indices_[constraint_i];
    const int mesh_i0 = triangle[0];
    const int mesh_i1 = triangle[1];
    const int mesh_i2 = triangle[2];
    const Span<float3> mesh_positions = params.positions(mesh_geo_i_);
    const float3 &mesh_p0 = mesh_positions[mesh_i0];
    const float3 &mesh_p1 = mesh_positions[mesh_i1];
    const float3 &mesh_p2 = mesh_positions[mesh_i2];
    const float mesh_inv_mass0 = params.inverse_mass(mesh_geo_i_, mesh_i0);
    const float mesh_inv_mass1 = params.inverse_mass(mesh_geo_i_, mesh_i1);
    const float mesh_inv_mass2 = params.inverse_mass(mesh_geo_i_, mesh_i2);

    const float effective_weight = inv_mass + pow2f(bary_weights[0]) * mesh_inv_mass0 +
                                   pow2f(bary_weights[1]) * mesh_inv_mass1 +
                                   pow2f(bary_weights[2]) * mesh_inv_mass2;
    if (effective_weight <= 0.0f) {
      /* All points are pinned. */
      return;
    }
    const float3 pin_point = bary_weights[0] * mesh_p0 + bary_weights[1] * mesh_p1 +
                             bary_weights[2] * mesh_p2;
    const float3 diff = p - pin_point;

    const float3 delta_lambda = (-diff - compliance_term * lambdas_[constraint_i]) /
                                (effective_weight + compliance_term);

    lambdas_[constraint_i] += delta_lambda;
    updater.update_position(points_geo_i_, point_i, inv_mass * delta_lambda);
    updater.update_position(
        mesh_geo_i_, mesh_i0, -bary_weights[0] * mesh_inv_mass0 * delta_lambda);
    updater.update_position(
        mesh_geo_i_, mesh_i1, -bary_weights[1] * mesh_inv_mass1 * delta_lambda);
    updater.update_position(
        mesh_geo_i_, mesh_i2, -bary_weights[2] * mesh_inv_mass2 * delta_lambda);
  }

  Vector<IndexMask> generate_independent_masks(IndexMaskMemory &memory) const override
  {
    return detect_independent_constraints<std::pair<int, int>>(
        [&](const int constraint_i) {
          const int3 &tri = triangle_indices_[constraint_i];
          std::array<std::pair<int, int>, 4> affected_points;
          affected_points[0] = {mesh_geo_i_, tri[0]};
          affected_points[1] = {mesh_geo_i_, tri[1]};
          affected_points[2] = {mesh_geo_i_, tri[2]};
          affected_points[3] = {points_geo_i_, indices_[constraint_i]};
          return affected_points;
        },
        indices_.size(),
        memory);
  }

  bke::GeometrySet as_debug_geometry(Vector<bke::GSpanAttributeWriter> & /*r_attributes*/) const
  {
    return {};
  }
};

class LinearDampingConstraintSet
    : public TemplatedVelocityConstraintSet<LinearDampingConstraintSet> {
 private:
  int geo_i_;
  IndexRange points_;
  /** Indexed by constraint index. */
  Span<float> linear_dampings_;
  MutableSpan<float> lambdas_;

 public:
  static constexpr StringRefNull debug_name = "Linear Damping";

  LinearDampingConstraintSet(const int geo_i,
                             const IndexRange points,
                             const Span<float> linear_dampings,
                             MutableSpan<float> lambdas)
      : TemplatedVelocityConstraintSet<LinearDampingConstraintSet>(points.size(), {geo_i}),
        geo_i_(geo_i),
        points_(points),
        linear_dampings_(linear_dampings),
        lambdas_(lambdas)
  {
  }

  void reset_force(const int constraint_i) const
  {
    lambdas_[constraint_i] = 0.0f;
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       const ConstraintSetParams &params,
                       const int constraint_i) const
  {
    const int point_i = points_[constraint_i];
    const float3 &velocity = params.velocity(geo_i_, point_i);
    const float damping = linear_dampings_[constraint_i];
    const float damping_factor = std::clamp(params.delta_time * damping, 0.0f, 1.0f);
    float residual;
    const float3 gradient = math::normalize_and_get_length(velocity, residual);
    const float delta_lambda = -residual * damping_factor - lambdas_[constraint_i];
    const float3 offset = gradient * delta_lambda;
    lambdas_[constraint_i] += delta_lambda;
    updater.update_velocity(geo_i_, point_i, offset);
  }

  bke::GeometrySet as_debug_geometry(Vector<bke::GSpanAttributeWriter> & /*r_attributes*/) const
  {
    return {};
  }
};

class AngularDampingConstraintSet
    : public TemplatedVelocityConstraintSet<AngularDampingConstraintSet> {
 private:
  int geo_i_;
  IndexRange points_;
  /** Indexed by point index. */
  Span<float> angular_dampings_;
  MutableSpan<float> lambdas_;

 public:
  static constexpr StringRefNull debug_name = "Angular Damping";

  AngularDampingConstraintSet(const int geo_i,
                              const IndexRange points,
                              const Span<float> angular_dampings,
                              MutableSpan<float> lambdas)
      : TemplatedVelocityConstraintSet<AngularDampingConstraintSet>(points.size(), {geo_i}),
        geo_i_(geo_i),
        points_(points),
        angular_dampings_(angular_dampings),
        lambdas_(lambdas)
  {
  }

  void reset_force(const int constraint_i) const
  {
    const int point_i = points_[constraint_i];
    lambdas_[point_i] = 0.0f;
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       const ConstraintSetParams &params,
                       const int constraint_i) const
  {
    const int point_i = points_[constraint_i];
    const float3 &angular_velocity = params.angular_velocity(geo_i_, point_i);
    const float damping = angular_dampings_[point_i];
    const float damping_factor = damping * params.delta_time;
    float residual;
    const float3 gradient = math::normalize_and_get_length(angular_velocity, residual);
    const float delta_lambda = -residual * damping_factor - lambdas_[point_i];
    const float3 offset = gradient * delta_lambda;
    lambdas_[point_i] += delta_lambda;
    updater.update_angular_velocity(geo_i_, point_i, offset);
  }

  bke::GeometrySet as_debug_geometry(Vector<bke::GSpanAttributeWriter> & /*r_attributes*/) const
  {
    return {};
  }
};

}  // namespace blender::xpbd
