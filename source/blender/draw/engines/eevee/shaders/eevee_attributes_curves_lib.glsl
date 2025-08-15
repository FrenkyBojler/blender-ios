/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "draw_object_infos_info.hh"

#ifdef GPU_LIBRARY_SHADER
#  define CURVES_SHADER
#  define DRW_HAIR_INFO
#endif

SHADER_LIBRARY_CREATE_INFO(draw_modelmat)
SHADER_LIBRARY_CREATE_INFO(draw_curves)

#include "draw_curves_lib.glsl"
#include "draw_model_lib.glsl"
#include "draw_object_infos_lib.glsl"
#include "gpu_shader_codegen_lib.glsl"
#include "gpu_shader_math_matrix_lib.glsl"
#include "gpu_shader_math_vector_lib.glsl"

#ifdef GPU_VERTEX_SHADER

/* -------------------------------------------------------------------- */
/** \name Curve
 *
 * Curve objects loads attributes from buffers through sampler buffers.
 * Per attribute scope follows loading order.
 * \{ */

#  ifdef OBINFO_LIB
float3 attr_load_orco(float4 orco, int index)
{
  float3 P = curves::get_curve_root_pos();
  float3 lP = transform_point(drw_modelinv(), P);
  return drw_object_orco(lP);
}
#  endif

/* Return the index to use for looking up the attribute value in the sampler
 * based on the attribute scope (point or spline). */
int curves_attribute_element_id(int index)
{
  if (drw_curves.is_point_attribute[index][0] != 0u) {
    return int(curve_interp.point_id);
  }
  return curve_interp_flat.strand_id;
}

float4 attr_load_tangent(samplerBuffer cd_buf, int index)
{
  /* Not supported for the moment. */
  return float4(0.0f, 0.0f, 0.0f, 1.0f);
}
float3 attr_load_uv(samplerBuffer cd_buf, int index)
{
  return texelFetch(cd_buf, curve_interp_flat.strand_id).rgb;
}
float4 attr_load_color(samplerBuffer cd_buf, int index)
{
  return texelFetch(cd_buf, curve_interp_flat.strand_id).rgba;
}
float4 attr_load_vec4(samplerBuffer cd_buf, int index)
{
  return texelFetch(cd_buf, curves_attribute_element_id(index)).rgba;
}
float3 attr_load_vec3(samplerBuffer cd_buf, int index)
{
  return texelFetch(cd_buf, curves_attribute_element_id(index)).rgb;
}
float2 attr_load_vec2(samplerBuffer cd_buf, int index)
{
  return texelFetch(cd_buf, curves_attribute_element_id(index)).rg;
}
float attr_load_float(samplerBuffer cd_buf, int index)
{
  return texelFetch(cd_buf, curves_attribute_element_id(index)).r;
}

/** \} */

#endif
