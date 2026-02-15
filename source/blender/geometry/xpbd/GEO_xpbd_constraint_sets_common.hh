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

    if (inv_m <= 0.0f) {
      /* Points with infinite mass are pinned and don't collide dynamically. */
      return;
    }

    const float3 diff = pos - plane_pos;
    const float normal_distance = math::dot(diff, axis);
    const bool is_active = normal_distance < 0.0f;
    active_states_[constraint_i] = is_active;
    if (!is_active) {
      return;
    }

    /* Positional correction for penetration. */
    float3 offset = float3(0.0f);
    float &lambda_normal = lambdas_normal_[constraint_i];
    if (normal_distance < 0.0f) {
      const float delta_lambda_normal = -normal_distance / (inv_m + compliance_term);
      offset += delta_lambda_normal * inv_m * axis;
      lambda_normal += delta_lambda_normal;
    }

    /* Apply static friction as a direct positional update. */
    const float3 &prev_pos = params.prev_position(geo_i_, point_i);
    const float3 &collider_velocity = contact_points_motion_[constraint_i];
    const float3 velocity = (pos - prev_pos) - collider_velocity;
    const float3 velocity_tangent = velocity - math::dot(velocity, axis) * axis;
    const float lambda_tangent_sq = math::length_squared(velocity_tangent /
                                                         (inv_m + compliance_term));
    const bool is_static = lambda_tangent_sq <
                           math::square(static_frictions_[constraint_i] * lambda_normal);
    if (is_static) {
      offset -= velocity_tangent * inv_m / (inv_m + compliance_term);
    }

    updater.update_position(geo_i_, point_i, offset);
  }

  Vector<IndexMask> generate_independent_masks(IndexMaskMemory &memory) const override
  {
    return unary_constraints_to_independent_masks(points_, memory);
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
};

class AngularDampingConstraintSet
    : public TemplatedVelocityConstraintSet<AngularDampingConstraintSet> {
 private:
  int geo_i_;
  IndexRange points_;
  /** Indexed by constraint index. */
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
    lambdas_[constraint_i] = 0.0f;
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       const ConstraintSetParams &params,
                       const int constraint_i) const
  {
    const int point_i = points_[constraint_i];
    const float3 &angular_velocity = params.angular_velocity(geo_i_, point_i);
    const float damping = angular_dampings_[constraint_i];
    const float damping_factor = damping * params.delta_time;
    float residual;
    const float3 gradient = math::normalize_and_get_length(angular_velocity, residual);
    const float delta_lambda = -residual * damping_factor - lambdas_[constraint_i];
    const float3 offset = gradient * delta_lambda;
    lambdas_[constraint_i] += delta_lambda;
    updater.update_angular_velocity(geo_i_, point_i, offset);
  }
};

}  // namespace blender::xpbd
