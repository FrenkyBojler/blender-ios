/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "GEO_xpbd_constraint_solver.hh"

#include "BLI_math_vector.hh"
#include "BLI_virtual_array.hh"

namespace blender::geometry::xpbd_constraint_solver {

class PinConstraintEvaluator : public TemplatedConstraintSetEvaluator<PinConstraintEvaluator> {
 private:
  int geo_i_;
  Span<float3> positions_;
  Span<int> indices_;
  Span<float3> pin_positions_;

 public:
  PinConstraintEvaluator(const int geo_i,
                         const Span<float3> positions,
                         const Span<int> indices,
                         const Span<float3> pin_positions)
      : geo_i_(geo_i), positions_(positions), indices_(indices), pin_positions_(pin_positions)
  {
  }

  template<typename SolverT> void evaluate_single(SolverT &solver, const int constraint_i) const
  {
    const int i = indices_[constraint_i];
    const float3 &pin_position = pin_positions_[constraint_i];
    const float3 &p = positions_[i];
    const float3 offset = pin_position - p;
    solver.update_position(geo_i_, i, offset);
  }
};

class DistanceConstraintEvaluator
    : public TemplatedConstraintSetEvaluator<DistanceConstraintEvaluator> {
 private:
  int geo_i_;
  Span<float3> positions_;
  Span<float> inverse_masses_;
  Span<int2> point_pairs_;
  Span<float> distances_;
  Span<float> compliance_terms_;

 public:
  DistanceConstraintEvaluator(const int geo_i,
                              const Span<float3> positions,
                              const Span<float> inverse_masses,
                              const Span<int2> point_pairs,
                              const Span<float> distances,
                              const Span<float> compliance_terms)
      : geo_i_(geo_i),
        positions_(positions),
        inverse_masses_(inverse_masses),
        point_pairs_(point_pairs),
        distances_(distances),
        compliance_terms_(compliance_terms)
  {
    BLI_assert(point_pairs.size() == distances.size());
  }

  template<typename SolverT> void evaluate_single(SolverT &solver, const int constraint_i) const
  {
    const int2 &point_pair = point_pairs_[constraint_i];
    const float target_distance = distances_[constraint_i];
    const int v0 = point_pair[0];
    const int v1 = point_pair[1];
    const float3 &p0 = positions_[v0];
    const float3 &p1 = positions_[v1];
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
    solver.update_position(geo_i_, v0, offset0);
    solver.update_position(geo_i_, v1, offset1);
  }
};

class CollisionPlaneConstraintEvaluator
    : public TemplatedConstraintSetEvaluator<CollisionPlaneConstraintEvaluator> {
 private:
  int geo_i_;
  Span<int> points_;
  Span<float3> positions_;
  Span<float3> plane_positions_;
  Span<float3> plane_normals_;

 public:
  CollisionPlaneConstraintEvaluator(const int geo_i,
                                    Span<int> points,
                                    Span<float3> positions,
                                    Span<float3> plane_positions,
                                    Span<float3> plane_normals)
      : geo_i_(geo_i),
        points_(points),
        positions_(positions),
        plane_positions_(plane_positions),
        plane_normals_(plane_normals)
  {
  }

  template<typename SolverT> void evaluate_single(SolverT &solver, const int constraint_i) const
  {
    const int v = points_[constraint_i];
    const float3 pos = positions_[v];
    const float3 plane_pos = plane_positions_[constraint_i];
    const float3 plane_normal = plane_normals_[constraint_i];
    BLI_assert(math::is_unit(plane_normal));

    const float3 diff = pos - plane_pos;
    const float distance = math::dot(diff, plane_normal);
    if (distance >= 0.0f) {
      return;
    }
    const float3 offset = plane_normal * -distance;
    solver.update_position(geo_i_, v, offset);
  }
};

}  // namespace blender::geometry::xpbd_constraint_solver
