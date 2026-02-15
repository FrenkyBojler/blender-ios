/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_vector.hh"

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

}  // namespace blender::xpbd
