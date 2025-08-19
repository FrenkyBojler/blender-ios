/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_base.h"
#include "GEO_xpbd_constraint_solver.hh"

#include "BLI_math_vector.hh"

namespace blender::geometry::xpbd_constraint_solver {

class PinConstraintEvaluator : public TemplatedConstraintSetEvaluator<PinConstraintEvaluator> {
 private:
  Span<int> points_ref_indices_;
  Span<int> indices_;
  Span<float3> pin_positions_;

 public:
  PinConstraintEvaluator(const Span<int> points_ref_indices,
                         const Span<int> indices,
                         const Span<float3> pin_positions)
      : points_ref_indices_(points_ref_indices), indices_(indices), pin_positions_(pin_positions)
  {
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       const Span<PointsRef> points_refs,
                       const int constraint_i) const
  {
    const int points_ref_i = points_ref_indices_[constraint_i];
    const int i = indices_[constraint_i];
    const float3 &pin_position = pin_positions_[constraint_i];
    const float3 &p = points_refs[points_ref_i].positions[i];
    const float3 offset = pin_position - p;
    updater.update_position(points_ref_i, i, offset);
  }
};

class DistanceConstraintEvaluator
    : public TemplatedConstraintSetEvaluator<DistanceConstraintEvaluator> {
 private:
  Span<int2> points_ref_indices_;
  Span<float> inverse_masses_;
  Span<int2> point_pairs_;
  Span<float> distances_;
  Span<float> compliance_terms_;

 public:
  DistanceConstraintEvaluator(const Span<int2> points_ref_indices,
                              const Span<float> inverse_masses,
                              const Span<int2> point_pairs,
                              const Span<float> distances,
                              const Span<float> compliance_terms)
      : points_ref_indices_(points_ref_indices),
        inverse_masses_(inverse_masses),
        point_pairs_(point_pairs),
        distances_(distances),
        compliance_terms_(compliance_terms)
  {
    BLI_assert(point_pairs.size() == distances.size());
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       const Span<PointsRef> points_refs,
                       const int constraint_i) const
  {
    const int2 &points_ref_pair = points_ref_indices_[constraint_i];
    const int points_ref_i0 = points_ref_pair[0];
    const int points_ref_i1 = points_ref_pair[1];
    const int2 &point_pair = point_pairs_[constraint_i];
    const float target_distance = distances_[constraint_i];
    const int v0 = point_pair[0];
    const int v1 = point_pair[1];
    const float3 &p0 = points_refs[points_ref_i0].positions[v0];
    const float3 &p1 = points_refs[points_ref_i1].positions[v1];
    const float inv_m0 = inverse_masses_[v0];
    const float inv_m1 = inverse_masses_[v1];
    const float compliance_term = compliance_terms_[constraint_i];

    if (inv_m0 == 0.0f && inv_m1 == 0.0f) {
      return;
    }

    const float3 p_diff = p1 - p0;
    float length;
    const float3 normalized_dir = math::normalize_and_get_length(p_diff, length);
    const float length_diff = length - target_distance;
    const float lambda = length_diff / (inv_m0 + inv_m1 + compliance_term);

    const float3 offset0 = lambda * inv_m0 * normalized_dir;
    const float3 offset1 = -lambda * inv_m1 * normalized_dir;
    updater.update_position(points_ref_i0, v0, offset0);
    updater.update_position(points_ref_i1, v1, offset1);
  }
};

class CollisionPlaneConstraintEvaluator
    : public TemplatedConstraintSetEvaluator<CollisionPlaneConstraintEvaluator> {
 private:
  Span<int> points_ref_indices_;
  Span<int> points_;
  Span<float3> plane_positions_;
  Span<float3> plane_normals_;

 public:
  CollisionPlaneConstraintEvaluator(const Span<int> points_ref_indices,
                                    const Span<int> points,
                                    const Span<float3> plane_positions,
                                    const Span<float3> plane_normals)
      : points_ref_indices_(points_ref_indices),
        points_(points),
        plane_positions_(plane_positions),
        plane_normals_(plane_normals)
  {
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       const Span<PointsRef> points_refs,
                       const int constraint_i) const
  {
    const int points_ref_i = points_ref_indices_[constraint_i];
    const int v = points_[constraint_i];
    const float3 pos = points_refs[points_ref_i].positions[v];
    const float3 plane_pos = plane_positions_[constraint_i];
    const float3 plane_normal = plane_normals_[constraint_i];
    BLI_assert(math::is_unit(plane_normal));

    const float3 diff = pos - plane_pos;
    const float distance = math::dot(diff, plane_normal);
    if (distance >= 0.0f) {
      return;
    }
    const float3 offset = plane_normal * -distance;
    updater.update_position(points_ref_i, v, offset);
  }
};

class MinimumDistanceConstraintEvaluator
    : public TemplatedConstraintSetEvaluator<MinimumDistanceConstraintEvaluator> {
 private:
  Span<int2> points_ref_indices_;
  Span<int2> points_;
  Span<float> min_distances_;
  Span<float> inverse_masses_;
  Span<float> compliance_terms_;

 public:
  MinimumDistanceConstraintEvaluator(const Span<int2> points_ref_indices,
                                     const Span<int2> points,
                                     const Span<float> min_distances,
                                     const Span<float> inverse_masses,
                                     const Span<float> compliance_terms)
      : points_ref_indices_(points_ref_indices),
        points_(points),
        min_distances_(min_distances),
        inverse_masses_(inverse_masses),
        compliance_terms_(compliance_terms)
  {
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       const Span<PointsRef> points_refs,
                       const int constraint_i) const
  {
    const int2 &points_ref_pair = points_ref_indices_[constraint_i];
    const int points_ref_i0 = points_ref_pair[0];
    const int points_ref_i1 = points_ref_pair[1];
    const int v0 = points_[constraint_i][0];
    const int v1 = points_[constraint_i][1];
    const float min_distance = min_distances_[constraint_i];
    const float3 &p0 = points_refs[points_ref_i0].positions[v0];
    const float3 &p1 = points_refs[points_ref_i1].positions[v1];
    const float3 diff = p1 - p0;
    float distance;
    const float3 normalized_dir = math::normalize_and_get_length(diff, distance);
    if (distance >= min_distance) {
      return;
    }
    const float inv_m0 = inverse_masses_[v0];
    const float inv_m1 = inverse_masses_[v1];
    const float length_diff = min_distance - distance;
    if (length_diff < 1e-5f) {
      return;
    }
    const float compliance_term = compliance_terms_[constraint_i];
    const float lambda = length_diff / (inv_m0 + inv_m1 + compliance_term);
    const float3 offset0 = -lambda * inv_m0 * normalized_dir;
    const float3 offset1 = lambda * inv_m1 * normalized_dir;
    updater.update_position(points_ref_i0, v0, offset0);
    updater.update_position(points_ref_i1, v1, offset1);
  }
};

class OverpressureConstraintEvaluator
    : public TemplatedConstraintSetEvaluator<OverpressureConstraintEvaluator> {
 private:
  int points_ref_i_;
  Span<int3> tris_;
  Span<int> corner_verts_;
  Span<float> inverse_masses_;
  float overpressure_;
  float initial_volume_;

 public:
  OverpressureConstraintEvaluator(const int points_ref_i,
                                  const Span<int3> tris,
                                  const Span<int> corner_verts,
                                  const Span<float> inverse_masses,
                                  const float overpressure,
                                  const float initial_volume)
      : points_ref_i_(points_ref_i),
        tris_(tris),
        corner_verts_(corner_verts),
        inverse_masses_(inverse_masses),
        overpressure_(overpressure),
        initial_volume_(initial_volume)
  {
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       const Span<PointsRef> points_refs,
                       const int /*constraint_i*/) const
  {
    const Span<float3> positions = points_refs[points_ref_i_].positions;
    const float current_volume = compute_volume(tris_, corner_verts_, positions);
    const float volume_diff = current_volume - overpressure_ * initial_volume_;

    const int points_num = positions.size();
    Array<float3> gradients(points_num, float3(0.0f));
    for (const int tri_i : tris_.index_range()) {
      const int3 &tri = tris_[tri_i];
      const int v0 = corner_verts_[tri[0]];
      const int v1 = corner_verts_[tri[1]];
      const int v2 = corner_verts_[tri[2]];
      const float3 &p0 = positions[v0];
      const float3 &p1 = positions[v1];
      const float3 &p2 = positions[v2];
      const float3 c_1_2 = math::cross(p1, p2);
      const float3 c_2_0 = math::cross(p2, p0);
      const float3 c_0_1 = math::cross(p0, p1);
      const float3 c = c_1_2 + c_2_0 + c_0_1;
      gradients[v0] += c;
      gradients[v1] += c;
      gradients[v2] += c;
    }

    float lambda_divisor = 0.0f;
    for (const int i : IndexRange(points_num)) {
      const float inverse_mass = inverse_masses_[i];
      lambda_divisor += math::length_squared(gradients[i]) * inverse_mass;
    }
    const float lambda = math::safe_divide(volume_diff, lambda_divisor);
    threading::parallel_for(IndexRange(points_num), 512, [&](const IndexRange range) {
      for (const int i : range) {
        const float inverse_mass = inverse_masses_[i];
        if (inverse_mass <= 0.0f) {
          continue;
        }
        const float3 offset = -lambda * inverse_mass * gradients[i];
        updater.update_position(points_ref_i_, i, offset);
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
            const int v0 = corner_verts[tri[0]];
            const int v1 = corner_verts[tri[1]];
            const int v2 = corner_verts[tri[2]];
            const float3 &p0 = positions[v0];
            const float3 &p1 = positions[v1];
            const float3 &p2 = positions[v2];
            volume += math::dot(math::cross(p0, p1), p2);
          }
          return volume;
        },
        [&](const float a, const float b) { return a + b; });
    return volume / 6.0f;
  }
};

/**
 * Considers a single rod at a time. Tries to enforce that the rotation of the rod is aligned with
 * the actual tangent of the rod. If it is misaligned, it moves the start position, end position
 * and rotation of the frame. At the same time, it enforces a certain length.
 */
class RodStretchAndShearConstraintEvaluator
    : public TemplatedConstraintSetEvaluator<RodStretchAndShearConstraintEvaluator> {
 private:
  Span<int2> points_ref_indices_;
  Span<int2> point_pairs_;
  Span<float> inverse_masses_;
  Span<float3> inertias_;
  Span<float> rest_lengths_;
  Span<float> compliance_terms_;

 public:
  RodStretchAndShearConstraintEvaluator(const Span<int2> points_ref_indices,
                                        const Span<int2> point_pairs,
                                        const Span<float> inverse_masses,
                                        const Span<float3> inertias,
                                        const Span<float> rest_lengths,
                                        const Span<float> compliance_terms)
      : points_ref_indices_(points_ref_indices),
        point_pairs_(point_pairs),
        inverse_masses_(inverse_masses),
        inertias_(inertias),
        rest_lengths_(rest_lengths),
        compliance_terms_(compliance_terms)
  {
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       const Span<PointsRef> points_refs,
                       const int constraint_i) const
  {
    const int2 &points_ref_pair = points_ref_indices_[constraint_i];
    const int2 &point_pair = point_pairs_[constraint_i];
    const int points_ref_i0 = points_ref_pair[0];
    const int points_ref_i1 = points_ref_pair[1];
    const int v0 = point_pair[0];
    const int v1 = point_pair[1];

    /* TODO: Check if it makes sense to use the rotation of the "following point" to better support
     * trees of rods. */
    const int rotation_point_index = 0;
    const int rotation_points_ref_i = points_ref_pair[rotation_point_index];
    const int rotation_v = point_pair[rotation_point_index];

    const float3 &p0 = points_refs[points_ref_i0].positions[v0];
    const float3 &p1 = points_refs[points_ref_i1].positions[v1];
    const math::Quaternion &rot = points_refs[rotation_points_ref_i].rotations[rotation_v];
    const float inv_m0 = inverse_masses_[v0];
    const float inv_m1 = inverse_masses_[v1];
    const float3 &inertia = inertias_[constraint_i];
    const float compliance_term = 0.0f;  // compliance_terms_[constraint_i];
    const float rest_length = rest_lengths_[constraint_i];

    /* Lumped weight for the rotation influence. The higher the inertia, the lower the change of
     * the rotation should be compared to the change in point positions. */
    const float inv_lumped_inertia = math::safe_rcp(0.5f * (inertia.x + inertia.y + inertia.z));

    if (inv_m0 == 0.0f && inv_m1 == 0.0f && inv_lumped_inertia == 0.0f) {
      /* Everything is pinned, so the constraint can't do anything. */
      return;
    }
    if (rest_length <= 0.0f) {
      /* Can't enforce a specific rod orientation with zero length. Could still just constraint the
       * length though. */
      return;
    }

    /* Current non-normalized tangent of the rod. */
    const float3 p_diff = p1 - p0;
    /* Expected non-normalized tangent of the rod based on the rotation. */
    const float3 forward = math::transform_point(rot, float3(0.0f, 0.0f, rest_length));
    /* How much the rod is stretched and sheared. */
    const float3 residual = p_diff - forward;

    /* Based on "Position and Orientation Based Cosserat Rods" (Kugelstadt, Schömer, 2016). */
    const float3 lambda = residual /
                          (inv_m0 + inv_m1 + 4.0f * inv_lumped_inertia * pow2f(rest_length) +
                           compliance_term);

    const float3 offset0 = lambda * inv_m0;
    const float3 offset1 = -lambda * inv_m1;
    const math::Quaternion offset_rot = math::Quaternion(
                                            0.0f, lambda * inv_lumped_inertia * rest_length) *
                                        rot * math::Quaternion(0, 0, 0, -1);

    updater.update_position(points_ref_i0, v0, offset0);
    updater.update_position(points_ref_i1, v1, offset1);
    updater.update_rotation(rotation_points_ref_i, rotation_v, offset_rot);
  }
};

/* Aligns rotations of two consecutive rods based on a rest rotation. */
class RodBendAndTwistConstraintEvaluator
    : public TemplatedConstraintSetEvaluator<RodBendAndTwistConstraintEvaluator> {
 private:
  Span<int2> points_ref_indices_;
  Span<int2> point_pairs_;
  Span<float3> inertias_;
  Span<math::Quaternion> rest_rotations_;
  Span<float> compliance_terms_;

 public:
  RodBendAndTwistConstraintEvaluator(const Span<int2> points_ref_indices,
                                     const Span<int2> point_pairs,
                                     const Span<float3> inertias,
                                     const Span<math::Quaternion> rest_rotations,
                                     const Span<float> compliance_terms)
      : points_ref_indices_(points_ref_indices),
        point_pairs_(point_pairs),
        inertias_(inertias),
        rest_rotations_(rest_rotations),
        compliance_terms_(compliance_terms)
  {
  }

  template<typename UpdaterT>
  void evaluate_single(UpdaterT &updater,
                       const Span<PointsRef> points_refs,
                       const int constraint_i) const
  {
    const int2 &points_ref_pair = points_ref_indices_[constraint_i];
    const int2 &point_pair = point_pairs_[constraint_i];
    const int points_ref_i0 = points_ref_pair[0];
    const int points_ref_i1 = points_ref_pair[1];
    const int v0 = point_pair[0];
    const int v1 = point_pair[1];

    const math::Quaternion &r0 = points_refs[points_ref_i0].rotations[v0];
    const math::Quaternion &r1 = points_refs[points_ref_i1].rotations[v1];
    const float3 &inertia0 = inertias_[v0];
    const float3 &inertia1 = inertias_[v1];

    const float inv_lumped_inertia0 = math::safe_rcp(0.5f *
                                                     (inertia0.x + inertia0.y + inertia0.z));
    const float inv_lumped_inertia1 = math::safe_rcp(0.5f *
                                                     (inertia1.x + inertia1.y + inertia1.z));
    if (inv_lumped_inertia0 == 0.0f && inv_lumped_inertia1 == 0.0f) {
      /* Everything is pinned, so the constraint can't do anything. */
      return;
    }

    const float compliance_term = compliance_terms_[constraint_i];
    const math::Quaternion &rest_rot = rest_rotations_[constraint_i];
    const float4 rest_rot_f = float4(rest_rot);

    /* Note In "Position and Orientation Based Cosserat Rods" (Kugelstadt, Schoemer) the W
     * component of the Darboux vector is ignored. In "Sag-Free Initialization for Strand-Based
     * Hybrid Hair Simulation" (Hsu et al.) it is included to improve stability in cases where the
     * hair is bent at nearly 180 degrees. */
    const math::Quaternion &rot_diff = math::invert_normalized(r0) * r1;
    const float4 rot_diff_f = float4(rot_diff);

    const float4 residual_neg = rest_rot_f - rot_diff_f;
    const float4 residual_pos = rest_rot_f + rot_diff_f;
    const float4 residual = math::length_squared(residual_neg) <
                                    math::length_squared(residual_pos) ?
                                residual_neg :
                                residual_pos;

    const float4 lambda = residual / (inv_lumped_inertia0 + inv_lumped_inertia1 + compliance_term);

    const math::Quaternion offset0 = r1 * math::Quaternion(lambda * inv_lumped_inertia0);
    const math::Quaternion offset1 = r0 * math::Quaternion(lambda * inv_lumped_inertia1);

    updater.update_rotation(points_ref_i0, v0, offset0);
    updater.update_rotation(points_ref_i1, v1, offset1);
  }
};

}  // namespace blender::geometry::xpbd_constraint_solver
