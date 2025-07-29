/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_compositor_texture_utilities.glsl"

void convert_int_to_int2()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, int4(int_to_int2(value.x), int2(0)));
}

void convert_int_to_float()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(int_to_float(value.x), float3(0.0f)));
}

void convert_int_to_float2()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(int_to_float2(value.x), float2(0.0f)));
}

void convert_int_to_float3()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(int_to_float3(value.x), 0.0f));
}

void convert_int_to_color()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(int_to_color(value.x)));
}

void convert_int_to_float4()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(int_to_float4(value.x)));
}

void convert_int_to_bool()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, int4(int_to_bool(value.x)));
}

void convert_int2_to_int()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, int4(int2_to_int(value.xy), int3(0)));
}

void convert_int2_to_float()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(int2_to_float(value.xy), float3(0.0f)));
}

void convert_int2_to_float2()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(int2_to_float2(value.xy), float2(0.0f)));
}

void convert_int2_to_float3()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(int2_to_float3(value.xy), 0.0f));
}

void convert_int2_to_color()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(int2_to_color(value.xy)));
}

void convert_int2_to_float4()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(int2_to_float4(value.xy)));
}

void convert_int2_to_bool()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, int4(int2_to_bool(value.xy)));
}

void convert_bool_to_float()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(bool_to_float(bool(value.x)), float3(0.0f)));
}

void convert_bool_to_int()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, int4(bool_to_int(bool(value.x)), int3(0)));
}

void convert_bool_to_int2()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, int4(bool_to_int2(bool(value.x)), int2(0)));
}

void convert_bool_to_float2()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(bool_to_float2(bool(value.x)), float2(0.0f)));
}

void convert_bool_to_float3()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(bool_to_float3(bool(value.x)), 0.0f));
}

void convert_bool_to_color()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(bool_to_color(bool(value.x))));
}

void convert_bool_to_float4()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(bool_to_float4(bool(value.x))));
}

void convert_float_to_int()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, int4(float_to_int(value.x), int3(0)));
}

void convert_float_to_int2()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, int4(float_to_int2(value.x), int2(0)));
}

void convert_float_to_float2()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(float_to_float2(value.x), float2(0.0f)));
}

void convert_float_to_float3()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(float_to_float3(value.x), 0.0f));
}

void convert_float_to_color()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(float_to_color(value.x)));
}

void convert_float_to_float4()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(float_to_float4(value.x)));
}

void convert_float_to_bool()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, int4(float_to_bool(value.x)));
}

void convert_float2_to_float()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(float2_to_float(value.xy), float3(0.0f)));
}

void convert_float2_to_int()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, int4(float2_to_int(value.xy), int3(0)));
}

void convert_float2_to_int2()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, int4(float2_to_int2(value.xy), int2(0)));
}

void convert_float2_to_float3()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(float2_to_float3(value.xy), 0.0f));
}

void convert_float2_to_color()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(float2_to_color(value.xy)));
}

void convert_float2_to_float4()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(float2_to_float4(value.xy)));
}

void convert_float2_to_bool()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, int4(float2_to_bool(value.xy)));
}

void convert_float3_to_float()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(float3_to_float(value.xyz), float3(0.0f)));
}

void convert_float3_to_int()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, int4(float3_to_int(value.xyz), int3(0)));
}

void convert_float3_to_int2()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, int4(float3_to_int2(value.xyz), int2(0)));
}

void convert_float3_to_float2()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(float3_to_float2(value.xyz), float2(0.0f)));
}

void convert_float3_to_color()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(float3_to_color(value.xyz)));
}

void convert_float3_to_float4()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(float3_to_float4(value.xyz)));
}

void convert_float3_to_bool()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, int4(float3_to_bool(value.xyz)));
}

void convert_float4_to_float()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(float4_to_float(value), float3(0.0f)));
}

void convert_float4_to_int()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, int4(float4_to_int(value), int3(0)));
}

void convert_float4_to_int2()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, int4(float4_to_int2(value), int2(0)));
}

void convert_float4_to_float2()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(float4_to_float2(value), float2(0.0f)));
}

void convert_float4_to_float3()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(float4_to_float3(value), 0.0f));
}

void convert_float4_to_color()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(float4_to_color(value)));
}

void convert_float4_to_bool()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, int4(float4_to_bool(value)));
}

void convert_color_to_float()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(color_to_float(value, luminance_coefficients_u)));
}

void convert_color_to_int()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, int4(color_to_int(value, luminance_coefficients_u)));
}

void convert_color_to_int2()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, int4(color_to_int2(value), int2(0)));
}

void convert_color_to_float2()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(color_to_float2(value), float2(0.0f)));
}

void convert_color_to_float3()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(color_to_float3(value), 0.0f));
}

void convert_color_to_float4()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(color_to_float4(value)));
}

void convert_color_to_bool()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, int4(color_to_bool(value, luminance_coefficients_u)));
}

void convert_color_to_alpha()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, float4(value.a));
}
