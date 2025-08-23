/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_base.h"
#include "GEO_xpbd_constraint_utils.hh"

#include "BLI_math_vector.hh"

namespace blender::xpbd {

struct DistanceConstraintResult {
  float3 offset0 = float3(0.0f);
  float3 offset1 = float3(0.0f);
};

inline DistanceConstraintResult evaluate_distance_constraint(const float3 &p0,
                                                             const float3 &p1,
                                                             const float inv_m0,
                                                             const float inv_m1,
                                                             const float rest_distance,
                                                             const float compliance_term)
{
  if (inv_m0 == 0.0f && inv_m1 == 0.0f) {
    return {};
  }

  const float3 p_diff = p1 - p0;
  float length;
  const float3 normalized_dir = math::normalize_and_get_length(p_diff, length);
  const float length_diff = length - rest_distance;
  const float lambda = length_diff / (inv_m0 + inv_m1 + compliance_term);

  const float3 offset0 = lambda * inv_m0 * normalized_dir;
  const float3 offset1 = -lambda * inv_m1 * normalized_dir;

  return {offset0, offset1};
}

struct AlignRotationsConstraintResult {
  math::Quaternion offset0 = math::Quaternion(0.0f, 0.0f, 0.0f, 0.0f);
  math::Quaternion offset1 = math::Quaternion(0.0f, 0.0f, 0.0f, 0.0f);
};

inline AlignRotationsConstraintResult evaluate_align_rotations_constraint(
    const math::Quaternion &r0,
    const math::Quaternion &r1,
    const float3 &inertia0,
    const float3 &inertia1,
    const math::Quaternion &rest_rotation,
    const float compliance_term)
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

  const float4 lambda = -residual / (inv_lumped_inertia0 + inv_lumped_inertia1 + compliance_term);

  const math::Quaternion offset0 = r1 *
                                   math::conjugate(math::Quaternion(lambda * inv_lumped_inertia0));
  const math::Quaternion offset1 = r0 * math::Quaternion(lambda * inv_lumped_inertia1);

  return {offset0, offset1};
}

struct RodStretchAndShearConstraintResult {
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
    const float compliance_term)
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
  const float3 lambda_pos = residual_pos / (weight_sum + compliance_term);
  const float3 lambda_rot = residual_rot / (weight_sum_rot + compliance_term);

  RodStretchAndShearConstraintResult result;
  result.offset0 = lambda_pos * inv_m0;
  result.offset1 = -lambda_pos * inv_m1;
  result.offset_rot = math::Quaternion(0.0f, lambda_rot * inv_lumped_inertia * p_len) * rot *
                      math::Quaternion(0, 0, 0, -1);
  return result;
}

class PinnedPositionConstraintSet : public TemplatedConstraintSet<PinnedPositionConstraintSet> {
 private:
  int geo_i_;
  Span<int> indices_;
  Span<float3> pin_positions_;
  Span<float> compliance_terms_;

 public:
  PinnedPositionConstraintSet(const int geo_i,
                              const Span<int> indices,
                              const Span<float3> pin_positions,
                              const Span<float> compliance_terms)
      : TemplatedConstraintSet<PinnedPositionConstraintSet>(indices.size(), {geo_i}),
        geo_i_(geo_i),
        indices_(indices),
        pin_positions_(pin_positions),
        compliance_terms_(compliance_terms)
  {
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       ConstraintSetParams &params,
                       const int constraint_i) const
  {
    const int point_i = indices_[constraint_i];
    const DistanceConstraintResult result = evaluate_distance_constraint(
        params.position(geo_i_, point_i),
        pin_positions_[constraint_i],
        params.inverse_mass(geo_i_, point_i),
        0.0f,
        0.0f,
        compliance_terms_[constraint_i]);
    updater.update_position(geo_i_, point_i, result.offset0);
  }

  Vector<IndexMask> generate_independent_masks(IndexMaskMemory &memory) const override
  {
    return unary_constraints_to_independent_masks(indices_, memory);
  }
};

class PinRotationConstraintSet : public TemplatedConstraintSet<PinRotationConstraintSet> {
 private:
  int geo_i_;
  Span<int> indices_;
  Span<math::Quaternion> pin_rotations_;
  Span<float> compliance_terms_;

 public:
  PinRotationConstraintSet(const int geo_i,
                           const Span<int> indices,
                           const Span<math::Quaternion> pin_rotations,
                           const Span<float> compliance_terms)
      : TemplatedConstraintSet<PinRotationConstraintSet>(indices.size(), {geo_i}),
        geo_i_(geo_i),
        indices_(indices),
        pin_rotations_(pin_rotations),
        compliance_terms_(compliance_terms)
  {
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       ConstraintSetParams &params,
                       const int constraint_i) const
  {
    const int point_i = indices_[constraint_i];
    const AlignRotationsConstraintResult result = evaluate_align_rotations_constraint(
        params.rotation(geo_i_, point_i),
        pin_rotations_[constraint_i],
        params.inertia(geo_i_, point_i),
        float3(std::numeric_limits<float>::infinity()),
        math::Quaternion::identity(),
        compliance_terms_[constraint_i]);
    updater.update_rotation(geo_i_, point_i, result.offset0);
  }

  Vector<IndexMask> generate_independent_masks(IndexMaskMemory &memory) const override
  {
    return unary_constraints_to_independent_masks(indices_, memory);
  }
};

class DistanceConstraintSet : public TemplatedConstraintSet<DistanceConstraintSet> {
 private:
  int geo_i_;
  Span<int2> point_pairs_;
  Span<float> distances_;
  Span<float> compliance_terms_;

 public:
  DistanceConstraintSet(const int geo_i,
                        const Span<int2> point_pairs,
                        const Span<float> distances,
                        const Span<float> compliance_terms)
      : TemplatedConstraintSet<DistanceConstraintSet>(point_pairs.size(), {geo_i}),
        geo_i_(geo_i),
        point_pairs_(point_pairs),
        distances_(distances),
        compliance_terms_(compliance_terms)
  {
    BLI_assert(point_pairs.size() == distances.size());
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       ConstraintSetParams &params,
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
        compliance_terms_[constraint_i]);
    updater.update_position(geo_i_, point_i0, result.offset0);
    updater.update_position(geo_i_, point_i1, result.offset1);
  }

  Vector<IndexMask> generate_independent_masks(IndexMaskMemory &memory) const override
  {
    return binary_constraints_to_independent_masks(point_pairs_, memory);
  }
};

class CollisionPlaneConstraintSet : public TemplatedConstraintSet<CollisionPlaneConstraintSet> {
 private:
  int geo_i_;
  Span<int> points_;
  Span<float3> plane_positions_;
  Span<float3> plane_normals_;

 public:
  CollisionPlaneConstraintSet(const int geo_i,
                              const Span<int> points,
                              const Span<float3> plane_positions,
                              const Span<float3> plane_normals)
      : TemplatedConstraintSet<CollisionPlaneConstraintSet>(points.size(), {geo_i}),
        geo_i_(geo_i),
        points_(points),
        plane_positions_(plane_positions),
        plane_normals_(plane_normals)
  {
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       ConstraintSetParams &params,
                       const int constraint_i) const
  {
    const int point_i = points_[constraint_i];
    const float3 &pos = params.position(geo_i_, point_i);
    const float3 &plane_pos = plane_positions_[constraint_i];
    const float3 &plane_normal = plane_normals_[constraint_i];
    BLI_assert(math::is_unit(plane_normal));

    const float3 diff = pos - plane_pos;
    const float distance = math::dot(diff, plane_normal);
    if (distance >= 0.0f) {
      return;
    }
    const float3 offset = plane_normal * -distance;
    updater.update_position(geo_i_, point_i, offset);
  }

  Vector<IndexMask> generate_independent_masks(IndexMaskMemory &memory) const override
  {
    return unary_constraints_to_independent_masks(points_, memory);
  }
};

class MinimumDistanceConstraintSet : public TemplatedConstraintSet<MinimumDistanceConstraintSet> {
 private:
  int geo_i_;
  Span<int2> points_;
  Span<float> min_distances_;
  Span<float> compliance_terms_;

 public:
  MinimumDistanceConstraintSet(const int geo_i,
                               const Span<int2> points,
                               const Span<float> min_distances,
                               const Span<float> compliance_terms)
      : TemplatedConstraintSet<MinimumDistanceConstraintSet>(points.size(), {geo_i}),
        geo_i_(geo_i),
        points_(points),
        min_distances_(min_distances),
        compliance_terms_(compliance_terms)
  {
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       ConstraintSetParams &params,
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
    const float lambda = length_diff / (inv_m0 + inv_m1 + compliance_term);
    const float3 offset0 = -lambda * inv_m0 * normalized_dir;
    const float3 offset1 = lambda * inv_m1 * normalized_dir;
    updater.update_position(geo_i_, point_i0, offset0);
    updater.update_position(geo_i_, point_i1, offset1);
  }

  Vector<IndexMask> generate_independent_masks(IndexMaskMemory &memory) const override
  {
    return binary_constraints_to_independent_masks(points_, memory);
  }
};

class PressureConstraintSet : public TemplatedConstraintSet<PressureConstraintSet> {
 private:
  int geo_i_;
  Span<int3> tris_;
  Span<int> corner_verts_;
  float pressure_;
  float initial_volume_;

 public:
  PressureConstraintSet(const int geo_i,
                        const Span<int3> tris,
                        const Span<int> corner_verts,
                        const float pressure,
                        const float initial_volume)
      : TemplatedConstraintSet<PressureConstraintSet>(1, {geo_i}),
        geo_i_(geo_i),
        tris_(tris),
        corner_verts_(corner_verts),
        pressure_(pressure),
        initial_volume_(initial_volume)
  {
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       ConstraintSetParams &params,
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

    float lambda_divisor = 0.0f;
    for (const int i : IndexRange(points_num)) {
      const float inverse_mass = inverse_masses[i];
      lambda_divisor += math::length_squared(gradients[i]) * inverse_mass;
    }
    const float lambda = math::safe_divide(volume_diff, lambda_divisor);
    threading::parallel_for(IndexRange(points_num), 512, [&](const IndexRange range) {
      for (const int i : range) {
        const float inverse_mass = inverse_masses[i];
        if (inverse_mass <= 0.0f) {
          continue;
        }
        const float3 offset = -lambda * inverse_mass * gradients[i];
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
};

/**
 * Considers a single rod at a time. Tries to enforce that the rotation of the rod is aligned with
 * the actual tangent of the rod. If it is misaligned, it moves the start position, end position
 * and rotation of the frame. At the same time, it enforces a certain length.
 */
class RodStretchAndShearCurveLocalConstraintSet
    : public TemplatedCurveLocalConstraintSet<RodStretchAndShearCurveLocalConstraintSet> {
 private:
  Span<float> rest_lengths_;
  Span<float> compliance_terms_;

 public:
  RodStretchAndShearCurveLocalConstraintSet(const int geo_i,
                                            const OffsetIndices<int> points_by_curve,
                                            const Span<float> rest_lengths,
                                            const Span<float> compliance_terms)
      : TemplatedCurveLocalConstraintSet<RodStretchAndShearCurveLocalConstraintSet>(
            geo_i, points_by_curve),
        rest_lengths_(rest_lengths),
        compliance_terms_(compliance_terms)
  {
  }

  template<typename UpdaterT>
  void evaluate_curve(UpdaterT &updater, ConstraintSetParams &params, const int curve_i) const
  {
    const IndexRange points = points_by_curve_[curve_i];

    /* TODO: Implement bilateral interleaving ordering for better stability. */
    for (const int point_i0 : points.drop_back(1)) {
      const int point_i1 = point_i0 + 1;
      const RodStretchAndShearConstraintResult result = evaluate_rod_stretch_and_shear_constraint(
          params.position(geo_i_, point_i0),
          params.position(geo_i_, point_i1),
          params.rotation(geo_i_, point_i0),
          params.inverse_mass(geo_i_, point_i0),
          params.inverse_mass(geo_i_, point_i1),
          params.inertia(geo_i_, point_i0),
          rest_lengths_[point_i0],
          compliance_terms_[point_i0]);
      updater.update_position(geo_i_, point_i0, result.offset0);
      updater.update_position(geo_i_, point_i1, result.offset1);
      updater.update_rotation(geo_i_, point_i0, result.offset_rot);
    }
  }
};

/** Aligns rotations of two consecutive rods based on a rest rotation. */
class RodBendAndTwistCurveLocalConstraintSet
    : public TemplatedCurveLocalConstraintSet<RodBendAndTwistCurveLocalConstraintSet> {
 private:
  Span<math::Quaternion> rest_rotations_;
  Span<float> compliance_terms_;

 public:
  RodBendAndTwistCurveLocalConstraintSet(const int geo_i,
                                         const OffsetIndices<int> points_by_curve,
                                         const Span<math::Quaternion> rest_rotations,
                                         const Span<float> compliance_terms)
      : TemplatedCurveLocalConstraintSet<RodBendAndTwistCurveLocalConstraintSet>(geo_i,
                                                                                 points_by_curve),
        rest_rotations_(rest_rotations),
        compliance_terms_(compliance_terms)
  {
  }

  template<typename UpdaterT>
  void evaluate_curve(UpdaterT &updater, ConstraintSetParams &params, const int curve_i) const
  {
    const IndexRange points = points_by_curve_[curve_i];

    /* TODO: Implement bilateral interleaving ordering for better stability. */
    /* Note that the last segment does not have this constraint, because the rotation of the last
     * point in the rod is meaningless.*/
    for (const int point_i0 : points.drop_back(2)) {
      const int point_i1 = point_i0 + 1;
      const AlignRotationsConstraintResult result = evaluate_align_rotations_constraint(
          params.rotation(geo_i_, point_i0),
          params.rotation(geo_i_, point_i1),
          params.inertia(geo_i_, point_i0),
          params.inertia(geo_i_, point_i1),
          rest_rotations_[point_i0],
          compliance_terms_[point_i0]);
      updater.update_rotation(geo_i_, point_i0, result.offset0);
      updater.update_rotation(geo_i_, point_i1, result.offset1);
    }
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

 public:
  AlignPositionsConstraintSet(OffsetIndices<int> offsets,
                              Span<float> compliance_terms,
                              Span<int> geo_indices,
                              Span<int> point_indices)
      : TemplatedConstraintSet<AlignPositionsConstraintSet>(
            point_indices.size(), VectorSet<int>(geo_indices).extract_vector()),
        offsets_(offsets),
        compliance_terms_(compliance_terms),
        geo_indices_(geo_indices),
        point_indices_(point_indices)
  {
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       ConstraintSetParams &params,
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
          pos, center, inv_m, 0.0f, 0.0f, compliance_term);
      updater.update_position(geo_i, point_i, result.offset0);
    }
  }

  float3 compute_center(const IndexRange range, ConstraintSetParams &params) const
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
};

class AttachUVSurfaceConstraintSet : public TemplatedConstraintSet<AttachUVSurfaceConstraintSet> {
 private:
  int mesh_geo_i_;
  int points_geo_i_;
  Span<int> indices_;
  Span<int3> triangle_indices_;
  Span<float3> bary_weights_;
  Span<float> compliance_terms_;

 public:
  AttachUVSurfaceConstraintSet(const int mesh_geo_i,
                               const int points_geo_i,
                               const Span<int> indices,
                               const Span<int3> triangle_indices,
                               const Span<float3> bary_weights,
                               const Span<float> compliance_terms)
      : TemplatedConstraintSet<AttachUVSurfaceConstraintSet>(indices.size(),
                                                             {mesh_geo_i, points_geo_i}),
        mesh_geo_i_(mesh_geo_i),
        points_geo_i_(points_geo_i),
        indices_(indices),
        triangle_indices_(triangle_indices),
        bary_weights_(bary_weights),
        compliance_terms_(compliance_terms)
  {
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       ConstraintSetParams &params,
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

    const float3 lambda = -diff / (effective_weight + compliance_term);

    updater.update_position(points_geo_i_, point_i, inv_mass * lambda);
    updater.update_position(mesh_geo_i_, mesh_i0, -bary_weights[0] * mesh_inv_mass0 * lambda);
    updater.update_position(mesh_geo_i_, mesh_i1, -bary_weights[1] * mesh_inv_mass1 * lambda);
    updater.update_position(mesh_geo_i_, mesh_i2, -bary_weights[2] * mesh_inv_mass2 * lambda);
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
};

}  // namespace blender::xpbd
