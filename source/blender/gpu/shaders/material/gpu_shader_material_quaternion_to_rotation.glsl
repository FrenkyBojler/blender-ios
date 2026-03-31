/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_math_vector_compare_lib.glsl"

[[node]]
void quaternion_to_rotation(float w, float x, float y, float z, out float4 rotation)
{
  const float4 q = float4(w, x, y, z);
  if (!is_zero(q)) {
    rotation = normalize(q);
  }
  else {
    rotation = float4(1.0f, 0.0f, 0.0f, 0.0f);
  }
}
