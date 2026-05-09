/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_math_vector_safe_lib.glsl"

[[node]]
void node_curvature(float4 color,
                    float dist,
                    float3 normal,
                    const float sample_count,
                    float4 &result_color,
                    float &result_curvature)
{
  result_curvature = ambient_occlusion_eval(safe_normalize(normal), dist, 0.0, sample_count);
  result_color = result_curvature * color;
}
