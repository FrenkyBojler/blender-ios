/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_math_vector_safe_lib.glsl"

[[node]]
void node_curvature(float radius, const float sample_count, float &result_curvature)
{
  result_curvature = ambient_occlusion_eval(float3(0.0, 0.0, 1.0), radius, 0.0, sample_count);
}
