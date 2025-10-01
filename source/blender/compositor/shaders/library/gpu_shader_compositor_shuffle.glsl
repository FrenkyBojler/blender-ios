/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_common_color_utils.glsl"

float get_channel_value(float4 color, int channel)
{
  /* Channel indices:
   * 0=RED, 1=GREEN, 2=BLUE, 3=ALPHA,
   * 4=HUE, 5=SATURATION, 6=VALUE, 7=LIGHTNESS,
   * 8=Y, 9=U, 10=V, 11=CB, 12=CR,
   * 13=FORWARD_U, 14=FORWARD_V, 15=BACKWARD_U, 16=BACKWARD_V,
   * 17=DEPTH, 18=BLACK, 19=WHITE */

  if (channel == 0) return color.r;
  if (channel == 1) return color.g;
  if (channel == 2) return color.b;
  if (channel == 3) return color.a;
  if (channel == 13) return color.r;
  if (channel == 14) return color.g;
  if (channel == 15) return color.b;
  if (channel == 16) return color.a;
  if (channel == 17) return color.r;
  if (channel == 18) return 0.0f;
  if (channel == 19) return 1.0f;

  if (channel >= 4 && channel <= 7) {
    float4 hsv_or_hsl;
    if (channel == 7) {
      rgb_to_hsl(color, hsv_or_hsl);
      return hsv_or_hsl.z;
    } else {
      rgb_to_hsv(color, hsv_or_hsl);
      if (channel == 4) return hsv_or_hsl.x;
      if (channel == 5) return hsv_or_hsl.y;
      if (channel == 6) return hsv_or_hsl.z;
    }
  }

  if (channel >= 8 && channel <= 10) {
    float4 yuv;
    rgba_to_yuva_itu_709(color, yuv);
    if (channel == 8) return yuv.x;
    if (channel == 9) return yuv.y;
    if (channel == 10) return yuv.z;
  }

  if (channel >= 11 && channel <= 12) {
    float4 ycc;
    rgba_to_ycca_itu_709(color, ycc);
    if (channel == 11) return ycc.y;
    if (channel == 12) return ycc.z;
  }

  return 0.0f;
}

void node_composite_shuffle_rgb(
    float4 color, float red_channel, float green_channel, float blue_channel, float alpha_channel, out float4 result)
{
  result.r = get_channel_value(color, int(red_channel));
  result.g = get_channel_value(color, int(green_channel));
  result.b = get_channel_value(color, int(blue_channel));
  result.a = get_channel_value(color, int(alpha_channel));
}

void node_composite_shuffle_hsv(
    float4 color, float red_channel, float green_channel, float blue_channel, float alpha_channel, out float4 result)
{
  float4 shuffled;
  shuffled.x = get_channel_value(color, int(red_channel));
  shuffled.y = get_channel_value(color, int(green_channel));
  shuffled.z = get_channel_value(color, int(blue_channel));
  shuffled.a = get_channel_value(color, int(alpha_channel));

  hsv_to_rgb(shuffled, result);
}

void node_composite_shuffle_hsl(
    float4 color, float red_channel, float green_channel, float blue_channel, float alpha_channel, out float4 result)
{
  float4 shuffled;
  shuffled.x = get_channel_value(color, int(red_channel));
  shuffled.y = get_channel_value(color, int(green_channel));
  shuffled.z = get_channel_value(color, int(blue_channel));
  shuffled.a = get_channel_value(color, int(alpha_channel));

  hsl_to_rgb(shuffled, result);
  result.rgb = max(result.rgb, float3(0.0f));
}

void node_composite_shuffle_ycc_itu_601(
    float4 color, float red_channel, float green_channel, float blue_channel, float alpha_channel, out float4 result)
{
  float4 shuffled;
  shuffled.x = get_channel_value(color, int(red_channel));
  shuffled.y = get_channel_value(color, int(green_channel));
  shuffled.z = get_channel_value(color, int(blue_channel));
  shuffled.a = get_channel_value(color, int(alpha_channel));

  ycca_to_rgba_itu_601(shuffled, result);
}

void node_composite_shuffle_ycc_itu_709(
    float4 color, float red_channel, float green_channel, float blue_channel, float alpha_channel, out float4 result)
{
  float4 shuffled;
  shuffled.x = get_channel_value(color, int(red_channel));
  shuffled.y = get_channel_value(color, int(green_channel));
  shuffled.z = get_channel_value(color, int(blue_channel));
  shuffled.a = get_channel_value(color, int(alpha_channel));

  ycca_to_rgba_itu_709(shuffled, result);
}

void node_composite_shuffle_ycc_jpeg(
    float4 color, float red_channel, float green_channel, float blue_channel, float alpha_channel, out float4 result)
{
  float4 shuffled;
  shuffled.x = get_channel_value(color, int(red_channel));
  shuffled.y = get_channel_value(color, int(green_channel));
  shuffled.z = get_channel_value(color, int(blue_channel));
  shuffled.a = get_channel_value(color, int(alpha_channel));

  ycca_to_rgba_jpeg(shuffled, result);
}

void node_composite_shuffle_yuv(
    float4 color, float red_channel, float green_channel, float blue_channel, float alpha_channel, out float4 result)
{
  float4 shuffled;
  shuffled.x = get_channel_value(color, int(red_channel));
  shuffled.y = get_channel_value(color, int(green_channel));
  shuffled.z = get_channel_value(color, int(blue_channel));
  shuffled.a = get_channel_value(color, int(alpha_channel));

  yuva_to_rgba_itu_709(shuffled, result);
}

void node_composite_shuffle_motion(
    float4 color, float red_channel, float green_channel, float blue_channel, float alpha_channel, out float4 result)
{
  result.r = get_channel_value(color, int(red_channel));
  result.g = get_channel_value(color, int(green_channel));
  result.b = get_channel_value(color, int(blue_channel));
  result.a = get_channel_value(color, int(alpha_channel));
}

void node_composite_shuffle_depth(
    float4 color, float red_channel, float green_channel, float blue_channel, float alpha_channel, out float4 result)
{
  result.r = get_channel_value(color, int(red_channel));
  result.g = get_channel_value(color, int(green_channel));
  result.b = get_channel_value(color, int(blue_channel));
  result.a = get_channel_value(color, int(alpha_channel));
}