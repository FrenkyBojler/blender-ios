/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_convert_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_convert_float2_to_color)

#include "gpu_shader_compositor_texture_utilities.glsl"
#include "gpu_shader_compositor_type_conversion.glsl"
#include "gpu_shader_math_matrix_construct_lib.glsl"
#include "gpu_shader_math_rotation_conversion_lib.glsl"

/* --------------------------------------------------------------------
 * Float to other.
 */

void convert_float_to_int()
{
  auto &sampler_in = sampler_get(compositor_convert_float_to_int, input_tx);
  auto &image_out = image_get(compositor_convert_float_to_int, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(float_to_int(value.x), int3(0)));
}

void convert_float_to_int2()
{
  auto &sampler_in = sampler_get(compositor_convert_float_to_int2, input_tx);
  auto &image_out = image_get(compositor_convert_float_to_int2, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(float_to_int2(value.x), int2(0)));
}

void convert_float_to_int3()
{
  auto &sampler_in = sampler_get(compositor_convert_float_to_int3, input_tx);
  auto &image_out = image_get(compositor_convert_float_to_int3, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(float_to_int3(value.x), 0));
}

void convert_float_to_float2()
{
  auto &sampler_in = sampler_get(compositor_convert_float_to_float2, input_tx);
  auto &image_out = image_get(compositor_convert_float_to_float2, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(float_to_float2(value.x), float2(0.0f)));
}

void convert_float_to_float3()
{
  auto &sampler_in = sampler_get(compositor_convert_float_to_float3, input_tx);
  auto &image_out = image_get(compositor_convert_float_to_float3, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(float_to_float3(value.x), 0.0f));
}

void convert_float_to_color()
{
  auto &sampler_in = sampler_get(compositor_convert_float_to_color, input_tx);
  auto &image_out = image_get(compositor_convert_float_to_color, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(float_to_color(value.x)));
}

void convert_float_to_float4()
{
  auto &sampler_in = sampler_get(compositor_convert_float_to_float4, input_tx);
  auto &image_out = image_get(compositor_convert_float_to_float4, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(float_to_float4(value.x)));
}

void convert_float_to_bool()
{
  auto &sampler_in = sampler_get(compositor_convert_float_to_bool, input_tx);
  auto &image_out = image_get(compositor_convert_float_to_bool, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(float_to_bool(value.x)));
}

void convert_float_to_quaternion()
{
  auto &sampler_in = sampler_get(compositor_convert_float_to_quaternion, input_tx);
  auto &image_out = image_get(compositor_convert_float_to_quaternion, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);

  EulerXYZ eul;
  eul.x = value.x;
  eul.y = value.x;
  eul.z = value.x;
  Quaternion quat = to_quaternion(eul);

  imageStore(image_out, texel, float4(quat.x, quat.y, quat.z, quat.w));
}

/* --------------------------------------------------------------------
 * Float2 to other.
 */

void convert_float2_to_float()
{
  auto &sampler_in = sampler_get(compositor_convert_float2_to_float, input_tx);
  auto &image_out = image_get(compositor_convert_float2_to_float, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(float2_to_float(value.xy), float3(0.0f)));
}

void convert_float2_to_int()
{
  auto &sampler_in = sampler_get(compositor_convert_float2_to_int, input_tx);
  auto &image_out = image_get(compositor_convert_float2_to_int, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(float2_to_int(value.xy), int3(0)));
}

void convert_float2_to_int2()
{
  auto &sampler_in = sampler_get(compositor_convert_float2_to_int2, input_tx);
  auto &image_out = image_get(compositor_convert_float2_to_int2, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(float2_to_int2(value.xy), int2(0)));
}

void convert_float2_to_int3()
{
  auto &sampler_in = sampler_get(compositor_convert_float2_to_int3, input_tx);
  auto &image_out = image_get(compositor_convert_float2_to_int3, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(float2_to_int3(value.xy), 0));
}

void convert_float2_to_float3()
{
  auto &sampler_in = sampler_get(compositor_convert_float2_to_float3, input_tx);
  auto &image_out = image_get(compositor_convert_float2_to_float3, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(float2_to_float3(value.xy), 0.0f));
}

void convert_float2_to_color()
{
  auto &sampler_in = sampler_get(compositor_convert_float2_to_color, input_tx);
  auto &image_out = image_get(compositor_convert_float2_to_color, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(float2_to_color(value.xy)));
}

void convert_float2_to_float4()
{
  auto &sampler_in = sampler_get(compositor_convert_float2_to_float4, input_tx);
  auto &image_out = image_get(compositor_convert_float2_to_float4, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(float2_to_float4(value.xy)));
}

void convert_float2_to_bool()
{
  auto &sampler_in = sampler_get(compositor_convert_float2_to_bool, input_tx);
  auto &image_out = image_get(compositor_convert_float2_to_bool, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(float2_to_bool(value.xy)));
}

/* --------------------------------------------------------------------
 * Float3 to other.
 */

void convert_float3_to_float()
{
  auto &sampler_in = sampler_get(compositor_convert_float3_to_float, input_tx);
  auto &image_out = image_get(compositor_convert_float3_to_float, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(float3_to_float(value.xyz), float3(0.0f)));
}

void convert_float3_to_int()
{
  auto &sampler_in = sampler_get(compositor_convert_float3_to_int, input_tx);
  auto &image_out = image_get(compositor_convert_float3_to_int, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(float3_to_int(value.xyz), int3(0)));
}

void convert_float3_to_int2()
{
  auto &sampler_in = sampler_get(compositor_convert_float3_to_int2, input_tx);
  auto &image_out = image_get(compositor_convert_float3_to_int2, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(float3_to_int2(value.xyz), int2(0)));
}

void convert_float3_to_int3()
{
  auto &sampler_in = sampler_get(compositor_convert_float3_to_int3, input_tx);
  auto &image_out = image_get(compositor_convert_float3_to_int3, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(float3_to_int3(value.xyz), 0));
}

void convert_float3_to_float2()
{
  auto &sampler_in = sampler_get(compositor_convert_float3_to_float2, input_tx);
  auto &image_out = image_get(compositor_convert_float3_to_float2, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(float3_to_float2(value.xyz), float2(0.0f)));
}

void convert_float3_to_color()
{
  auto &sampler_in = sampler_get(compositor_convert_float3_to_color, input_tx);
  auto &image_out = image_get(compositor_convert_float3_to_color, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(float3_to_color(value.xyz)));
}

void convert_float3_to_float4()
{
  auto &sampler_in = sampler_get(compositor_convert_float3_to_float4, input_tx);
  auto &image_out = image_get(compositor_convert_float3_to_float4, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(float3_to_float4(value.xyz)));
}

void convert_float3_to_bool()
{
  auto &sampler_in = sampler_get(compositor_convert_float3_to_bool, input_tx);
  auto &image_out = image_get(compositor_convert_float3_to_bool, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(float3_to_bool(value.xyz)));
}

void convert_float3_to_quaternion()
{
  auto &sampler_in = sampler_get(compositor_convert_float3_to_quaternion, input_tx);
  auto &image_out = image_get(compositor_convert_float3_to_quaternion, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);

  EulerXYZ eul;
  eul.x = value.x;
  eul.y = value.y;
  eul.z = value.z;
  Quaternion quat = to_quaternion(eul);

  imageStore(image_out, texel, float4(quat.x, quat.y, quat.z, quat.w));
}

/* --------------------------------------------------------------------
 * Float4 to other.
 */

void convert_float4_to_float()
{
  auto &sampler_in = sampler_get(compositor_convert_float4_to_float, input_tx);
  auto &image_out = image_get(compositor_convert_float4_to_float, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(float4_to_float(value), float3(0.0f)));
}

void convert_float4_to_int()
{
  auto &sampler_in = sampler_get(compositor_convert_float4_to_int, input_tx);
  auto &image_out = image_get(compositor_convert_float4_to_int, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(float4_to_int(value), int3(0)));
}

void convert_float4_to_int2()
{
  auto &sampler_in = sampler_get(compositor_convert_float4_to_int2, input_tx);
  auto &image_out = image_get(compositor_convert_float4_to_int2, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(float4_to_int2(value), int2(0)));
}

void convert_float4_to_int3()
{
  auto &sampler_in = sampler_get(compositor_convert_float4_to_int3, input_tx);
  auto &image_out = image_get(compositor_convert_float4_to_int3, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(float4_to_int3(value), 0));
}

void convert_float4_to_float2()
{
  auto &sampler_in = sampler_get(compositor_convert_float4_to_float2, input_tx);
  auto &image_out = image_get(compositor_convert_float4_to_float2, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(float4_to_float2(value), float2(0.0f)));
}

void convert_float4_to_float3()
{
  auto &sampler_in = sampler_get(compositor_convert_float4_to_float3, input_tx);
  auto &image_out = image_get(compositor_convert_float4_to_float3, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(float4_to_float3(value), 0.0f));
}

void convert_float4_to_color()
{
  auto &sampler_in = sampler_get(compositor_convert_float4_to_color, input_tx);
  auto &image_out = image_get(compositor_convert_float4_to_color, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(float4_to_color(value)));
}

void convert_float4_to_bool()
{
  auto &sampler_in = sampler_get(compositor_convert_float4_to_bool, input_tx);
  auto &image_out = image_get(compositor_convert_float4_to_bool, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(float4_to_bool(value)));
}

/* --------------------------------------------------------------------
 * Color to other.
 */

void convert_color_to_float()
{
  auto &sampler_in = sampler_get(compositor_convert_color_to_float, input_tx);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  auto &image_out = image_get(compositor_convert_color_to_float, output_img);
  auto &luma_coefs = push_constant_get(compositor_convert_color_to_float,
                                       luminance_coefficients_u);
  imageStore(image_out, texel, float4(color_to_float(value, luma_coefs)));
}

void convert_color_to_int()
{
  auto &sampler_in = sampler_get(compositor_convert_color_to_int, input_tx);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  auto &image_out = image_get(compositor_convert_color_to_int, output_img);
  auto &luma_coefs = push_constant_get(compositor_convert_color_to_int, luminance_coefficients_u);
  imageStore(image_out, texel, int4(color_to_int(value, luma_coefs)));
}

void convert_color_to_int2()
{
  auto &sampler_in = sampler_get(compositor_convert_color_to_int2, input_tx);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  auto &image_out = image_get(compositor_convert_color_to_int2, output_img);
  imageStore(image_out, texel, int4(color_to_int2(value), int2(0)));
}

void convert_color_to_int3()
{
  auto &sampler_in = sampler_get(compositor_convert_color_to_int3, input_tx);
  auto &image_out = image_get(compositor_convert_color_to_int3, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(color_to_int3(value), 0));
}

void convert_color_to_float2()
{
  auto &sampler_in = sampler_get(compositor_convert_color_to_float2, input_tx);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  auto &image_out = image_get(compositor_convert_color_to_float2, output_img);
  imageStore(image_out, texel, float4(color_to_float2(value), float2(0.0f)));
}

void convert_color_to_float3()
{
  auto &sampler_in = sampler_get(compositor_convert_color_to_float3, input_tx);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  auto &image_out = image_get(compositor_convert_color_to_float3, output_img);
  imageStore(image_out, texel, float4(color_to_float3(value), 0.0f));
}

void convert_color_to_float4()
{
  auto &sampler_in = sampler_get(compositor_convert_color_to_float4, input_tx);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  auto &image_out = image_get(compositor_convert_color_to_float4, output_img);
  imageStore(image_out, texel, float4(color_to_float4(value)));
}

void convert_color_to_bool()
{
  auto &sampler_in = sampler_get(compositor_convert_color_to_bool, input_tx);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  auto &image_out = image_get(compositor_convert_color_to_bool, output_img);
  auto &luma_coefs = push_constant_get(compositor_convert_color_to_bool, luminance_coefficients_u);
  imageStore(image_out, texel, int4(color_to_bool(value, luma_coefs)));
}

void convert_color_to_alpha()
{
  auto &sampler_in = sampler_get(compositor_convert_color_to_alpha, input_tx);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);
  auto &image_out = image_get(compositor_convert_color_to_alpha, output_img);
  imageStore(image_out, texel, float4(value.a));
}

/* --------------------------------------------------------------------
 * Int to other.
 */

void convert_int_to_int2()
{
  auto &sampler_in = sampler_get(compositor_convert_int_to_int2, input_tx);
  auto &image_out = image_get(compositor_convert_int_to_int2, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(int_to_int2(value.x), int2(0)));
}

void convert_int_to_int3()
{
  auto &sampler_in = sampler_get(compositor_convert_int_to_int3, input_tx);
  auto &image_out = image_get(compositor_convert_int_to_int3, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(int_to_int3(value.x), 0));
}

void convert_int_to_float()
{
  auto &sampler_in = sampler_get(compositor_convert_int_to_float, input_tx);
  auto &image_out = image_get(compositor_convert_int_to_float, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(int_to_float(value.x), float3(0.0f)));
}

void convert_int_to_float2()
{
  auto &sampler_in = sampler_get(compositor_convert_int_to_float2, input_tx);
  auto &image_out = image_get(compositor_convert_int_to_float2, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(int_to_float2(value.x), float2(0.0f)));
}

void convert_int_to_float3()
{
  auto &sampler_in = sampler_get(compositor_convert_int_to_float3, input_tx);
  auto &image_out = image_get(compositor_convert_int_to_float3, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(int_to_float3(value.x), 0.0f));
}

void convert_int_to_color()
{
  auto &sampler_in = sampler_get(compositor_convert_int_to_color, input_tx);
  auto &image_out = image_get(compositor_convert_int_to_color, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(int_to_color(value.x)));
}

void convert_int_to_float4()
{
  auto &sampler_in = sampler_get(compositor_convert_int_to_float4, input_tx);
  auto &image_out = image_get(compositor_convert_int_to_float4, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(int_to_float4(value.x)));
}

void convert_int_to_bool()
{
  auto &sampler_in = sampler_get(compositor_convert_int_to_bool, input_tx);
  auto &image_out = image_get(compositor_convert_int_to_bool, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(int_to_bool(value.x)));
}

/* --------------------------------------------------------------------
 * Int2 to other.
 */

void convert_int2_to_int()
{
  auto &sampler_in = sampler_get(compositor_convert_int2_to_int, input_tx);
  auto &image_out = image_get(compositor_convert_int2_to_int, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(int2_to_int(value.xy), int3(0)));
}

void convert_int2_to_int3()
{
  auto &sampler_in = sampler_get(compositor_convert_int2_to_int3, input_tx);
  auto &image_out = image_get(compositor_convert_int2_to_int3, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(int2_to_int3(value.xy), 0));
}

void convert_int2_to_float()
{
  auto &sampler_in = sampler_get(compositor_convert_int2_to_float, input_tx);
  auto &image_out = image_get(compositor_convert_int2_to_float, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(int2_to_float(value.xy), float3(0.0f)));
}

void convert_int2_to_float2()
{
  auto &sampler_in = sampler_get(compositor_convert_int2_to_float2, input_tx);
  auto &image_out = image_get(compositor_convert_int2_to_float2, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(int2_to_float2(value.xy), float2(0.0f)));
}

void convert_int2_to_float3()
{
  auto &sampler_in = sampler_get(compositor_convert_int2_to_float3, input_tx);
  auto &image_out = image_get(compositor_convert_int2_to_float3, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(int2_to_float3(value.xy), 0.0f));
}

void convert_int2_to_color()
{
  auto &sampler_in = sampler_get(compositor_convert_int2_to_color, input_tx);
  auto &image_out = image_get(compositor_convert_int2_to_color, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(int2_to_color(value.xy)));
}

void convert_int2_to_float4()
{
  auto &sampler_in = sampler_get(compositor_convert_int2_to_float4, input_tx);
  auto &image_out = image_get(compositor_convert_int2_to_float4, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(int2_to_float4(value.xy)));
}

void convert_int2_to_bool()
{
  auto &sampler_in = sampler_get(compositor_convert_int2_to_bool, input_tx);
  auto &image_out = image_get(compositor_convert_int2_to_bool, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(int2_to_bool(value.xy)));
}

/* --------------------------------------------------------------------
 * Int3 to other.
 */

void convert_int3_to_int()
{
  auto &sampler_in = sampler_get(compositor_convert_int3_to_int, input_tx);
  auto &image_out = image_get(compositor_convert_int3_to_int, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(int3_to_int(value.xyz), int3(0)));
}

void convert_int3_to_int2()
{
  auto &sampler_in = sampler_get(compositor_convert_int3_to_int2, input_tx);
  auto &image_out = image_get(compositor_convert_int3_to_int2, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(int3_to_int2(value.xyz), int2(0)));
}

void convert_int3_to_float()
{
  auto &sampler_in = sampler_get(compositor_convert_int3_to_float, input_tx);
  auto &image_out = image_get(compositor_convert_int3_to_float, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(int3_to_float(value.xyz), float3(0.0f)));
}

void convert_int3_to_float2()
{
  auto &sampler_in = sampler_get(compositor_convert_int3_to_float2, input_tx);
  auto &image_out = image_get(compositor_convert_int3_to_float2, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(int3_to_float2(value.xyz), float2(0.0f)));
}

void convert_int3_to_float3()
{
  auto &sampler_in = sampler_get(compositor_convert_int3_to_float3, input_tx);
  auto &image_out = image_get(compositor_convert_int3_to_float3, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(int3_to_float3(value.xyz), 0.0f));
}

void convert_int3_to_color()
{
  auto &sampler_in = sampler_get(compositor_convert_int3_to_color, input_tx);
  auto &image_out = image_get(compositor_convert_int3_to_color, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(int3_to_color(value.xyz)));
}

void convert_int3_to_float4()
{
  auto &sampler_in = sampler_get(compositor_convert_int3_to_float4, input_tx);
  auto &image_out = image_get(compositor_convert_int3_to_float4, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(int3_to_float4(value.xyz)));
}

void convert_int3_to_bool()
{
  auto &sampler_in = sampler_get(compositor_convert_int3_to_bool, input_tx);
  auto &image_out = image_get(compositor_convert_int3_to_bool, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(int3_to_bool(value.xyz)));
}

void convert_int3_to_quaternion()
{
  auto &sampler_in = sampler_get(compositor_convert_int3_to_quaternion, input_tx);
  auto &image_out = image_get(compositor_convert_int3_to_quaternion, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);

  EulerXYZ eul;
  eul.x = float(value.x);
  eul.y = float(value.y);
  eul.z = float(value.z);
  Quaternion quat = to_quaternion(eul);

  imageStore(image_out, texel, float4(quat.x, quat.y, quat.z, quat.w));
}

/* --------------------------------------------------------------------
 * Bool to other.
 */

void convert_bool_to_float()
{
  auto &sampler_in = sampler_get(compositor_convert_bool_to_float, input_tx);
  auto &image_out = image_get(compositor_convert_bool_to_float, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(bool_to_float(bool(value.x)), float3(0.0f)));
}

void convert_bool_to_int()
{
  auto &sampler_in = sampler_get(compositor_convert_bool_to_int, input_tx);
  auto &image_out = image_get(compositor_convert_bool_to_int, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(bool_to_int(bool(value.x)), int3(0)));
}

void convert_bool_to_int2()
{
  auto &sampler_in = sampler_get(compositor_convert_bool_to_int2, input_tx);
  auto &image_out = image_get(compositor_convert_bool_to_int2, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(bool_to_int2(bool(value.x)), int2(0)));
}

void convert_bool_to_int3()
{
  auto &sampler_in = sampler_get(compositor_convert_bool_to_int3, input_tx);
  auto &image_out = image_get(compositor_convert_bool_to_int3, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, int4(bool_to_int3(bool(value.x)), 0));
}

void convert_bool_to_float2()
{
  auto &sampler_in = sampler_get(compositor_convert_bool_to_float2, input_tx);
  auto &image_out = image_get(compositor_convert_bool_to_float2, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(bool_to_float2(bool(value.x)), float2(0.0f)));
}

void convert_bool_to_float3()
{
  auto &sampler_in = sampler_get(compositor_convert_bool_to_float3, input_tx);
  auto &image_out = image_get(compositor_convert_bool_to_float3, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(bool_to_float3(bool(value.x)), 0.0f));
}

void convert_bool_to_color()
{
  auto &sampler_in = sampler_get(compositor_convert_bool_to_color, input_tx);
  auto &image_out = image_get(compositor_convert_bool_to_color, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(bool_to_color(bool(value.x))));
}

void convert_bool_to_float4()
{
  auto &sampler_in = sampler_get(compositor_convert_bool_to_float4, input_tx);
  auto &image_out = image_get(compositor_convert_bool_to_float4, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(sampler_in, texel);
  imageStore(image_out, texel, float4(bool_to_float4(bool(value.x))));
}

/* --------------------------------------------------------------------
 * Float4x4 to other.
 */

void convert_float4x4_to_quaternion()
{
  auto &sampler_in = sampler_get(compositor_convert_float4x4_to_quaternion, input_tx);
  auto &image_out = image_get(compositor_convert_float4x4_to_quaternion, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);

  float4x4 mat_4x4 = texture_load_float4x4(sampler_in, texel);
  float3x3 mat_3x3 = float3x3(mat_4x4[0].xyz, mat_4x4[1].xyz, mat_4x4[2].xyz);
  EulerXYZ euler = to_euler(mat_3x3);
  Quaternion quat = to_quaternion(euler);

  imageStore(image_out, texel, float4(quat.x, quat.y, quat.z, quat.w));
}

/* --------------------------------------------------------------------
 * Quaternion to other.
 */

void convert_quaternion_to_float3()
{
  auto &sampler_in = sampler_get(compositor_convert_quaternion_to_float3, input_tx);
  auto &image_out = image_get(compositor_convert_quaternion_to_float3, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);

  Quaternion quat;
  quat.x = value.x;
  quat.y = value.y;
  quat.z = value.z;
  quat.w = value.w;
  float3 result = to_euler(from_rotation(quat)).as_float3();

  imageStore(image_out, texel, float4(result, 0.0f));
}

void convert_quaternion_to_int3()
{
  auto &sampler_in = sampler_get(compositor_convert_quaternion_to_int3, input_tx);
  auto &image_out = image_get(compositor_convert_quaternion_to_int3, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);

  Quaternion quat;
  quat.x = value.x;
  quat.y = value.y;
  quat.z = value.z;
  quat.w = value.w;
  float3 result = to_euler(from_rotation(quat)).as_float3();

  imageStore(image_out, texel, int4(int3(result), 0));
}

void convert_quaternion_to_float4x4()
{
  auto &sampler_in = sampler_get(compositor_convert_quaternion_to_float4x4, input_tx);
  auto &image_out = image_get(compositor_convert_quaternion_to_float4x4, output_img);
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(sampler_in, texel);

  Quaternion quat;
  quat.x = value.x;
  quat.y = value.y;
  quat.z = value.z;
  quat.w = value.w;
  float3x3 mat_3x3 = from_rotation(quat);

  float4 col0 = float4(mat_3x3[0], 0.0f);
  float4 col1 = float4(mat_3x3[1], 0.0f);
  float4 col2 = float4(mat_3x3[2], 0.0f);
  float4 col3 = float4(0.0f, 0.0f, 0.0f, 1.0f);

  imageStore(image_out, int3(texel, 0), col0);
  imageStore(image_out, int3(texel, 1), col1);
  imageStore(image_out, int3(texel, 2), col2);
  imageStore(image_out, int3(texel, 3), col3);
}
