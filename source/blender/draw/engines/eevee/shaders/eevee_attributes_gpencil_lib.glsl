/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "draw_object_infos_info.hh"

#ifdef GPU_LIBRARY_SHADER
SHADER_LIBRARY_CREATE_INFO(draw_modelmat)
#endif

#include "draw_model_lib.glsl"
#include "draw_object_infos_lib.glsl"
#include "eevee_nodetree_lib.glsl"
#include "gpu_shader_codegen_lib.glsl"
#include "gpu_shader_math_matrix_lib.glsl"
#include "gpu_shader_math_vector_lib.glsl"

/* -------------------------------------------------------------------- */
/** \name Grease Pencil
 *
 * Grease Pencil objects have one uv and one color attribute layer.
 * \{ */

/* Globals to feed the load functions. */
packed_float2 g_uvs;
packed_float4 g_color;

#ifdef OBINFO_LIB
float3 attr_load_orco(GPencilPoint point, float4 orco, int index)
{
  float3 lP = drw_point_world_to_object(interp.P);
  return drw_object_orco(lP);
}
#endif
float4 attr_load_tangent(GPencilPoint point, float4 tangent, int index)
{
  return float4(0.0f, 0.0f, 0.0f, 1.0f);
}
float3 attr_load_uv(GPencilPoint point, float3 dummy, int index)
{
  return float3(g_uvs, 0.0f);
}
float4 attr_load_color(GPencilPoint point, float4 dummy, int index)
{
  return g_color;
}
float4 attr_load_vec4(GPencilPoint point, float4 attr, int index)
{
  return float4(0.0f);
}
float3 attr_load_vec3(GPencilPoint point, float3 attr, int index)
{
  return float3(0.0f);
}
float2 attr_load_vec2(GPencilPoint point, float2 attr, int index)
{
  return float2(0.0f);
}
float attr_load_float(GPencilPoint point, float attr, int index)
{
  return 0.0f;
}

/** \} */
