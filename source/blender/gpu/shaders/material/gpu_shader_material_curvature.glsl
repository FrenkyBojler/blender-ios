/* SPDX-FileCopyrightText: 2019-2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_curvature(float radius,
                    const float sample_count,
                    float bias,
                    float &result_curvature,
                    float &result_convexity,
                    float &result_concavity,
                    float4 &result_both)
{
  result_curvature = 0.5f;
  result_convexity = 0.0f;
  result_concavity = 0.0f;
  result_both = float4(result_convexity, result_concavity, 0.0f, 1.0f);
}
