/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

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

  template<typename SolverT>
  void evaluate_single(SolverT &solver,
                       const Span<PointsRef> points_refs,
                       const int constraint_i) const
  {
    const int points_ref_i = points_ref_indices_[constraint_i];
    const int i = indices_[constraint_i];
    const float3 &pin_position = pin_positions_[constraint_i];
    const float3 &p = points_refs[points_ref_i].positions[i];
    const float3 offset = pin_position - p;
    solver.update_position(points_ref_i, i, offset);
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

  template<typename SolverT>
  void evaluate_single(SolverT &solver,
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

    const float3 p_diff = p1 - p0;
    float length;
    const float3 normalized_dir = math::normalize_and_get_length(p_diff, length);
    const float length_diff = length - target_distance;
    const float lambda = length_diff / (inv_m0 + inv_m1 + compliance_term);

    const float3 offset0 = lambda * inv_m0 * normalized_dir;
    const float3 offset1 = -lambda * inv_m1 * normalized_dir;
    solver.update_position(points_ref_i0, v0, offset0);
    solver.update_position(points_ref_i1, v1, offset1);
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

  template<typename SolverT>
  void evaluate_single(SolverT &solver,
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
    solver.update_position(points_ref_i, v, offset);
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

  template<typename SolverT>
  void evaluate_single(SolverT &solver,
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
    solver.update_position(points_ref_i0, v0, offset0);
    solver.update_position(points_ref_i1, v1, offset1);
  }
};

}  // namespace blender::geometry::xpbd_constraint_solver
