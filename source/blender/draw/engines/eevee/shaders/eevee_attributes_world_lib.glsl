/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_codegen_lib.glsl"
#include "gpu_shader_math_matrix_lib.glsl"
#include "gpu_shader_math_vector_lib.glsl"

/* -------------------------------------------------------------------- */
/** \name World
 *
 * World has no attributes other than orco.
 * \{ */

float3 attr_load_orco(float4 orco, int index)
{
  return -g_data.N;
}
float4 attr_load_tangent(float4 tangent, int index)
{
  return float4(0);
}
float4 attr_load_vec4(float4 attr, int index)
{
  return float4(0);
}
float3 attr_load_vec3(float3 attr, int index)
{
  return float3(0);
}
float2 attr_load_vec2(float2 attr, int index)
{
  return float2(0);
}
float attr_load_float(float attr, int index)
{
  return 0.0f;
}
float4 attr_load_color(float4 attr, int index)
{
  return float4(0);
}
float3 attr_load_uv(float3 attr, int index)
{
  return float3(0);
}

/** \} */
