/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Point/AA bits similar to #gpu_shader_2D_point_uniform_size_aa_vert */

#include "gpu_shader_common_color_utils.glsl"
#include "infos/gpu_shader_sequencer_info.hh"

VERTEX_SHADER_CREATE_INFO(gpu_shader_sequencer_scope)

/* Match eSpaceSeq_RegionType */
#define SEQ_DRAW_IMG_WAVEFORM 2
#define SEQ_DRAW_IMG_VECTORSCOPE 3
#define SEQ_DRAW_IMG_HISTOGRAM 4
#define SEQ_DRAW_IMG_RGBPARADE 5

void main()
{
  /* Fetch pixel from the input image, corresponding to current point. */
  int2 texel = int2(gl_VertexID % image_width, gl_VertexID / image_width);
  float4 color = texelFetch(image, texel, 0);
  if (img_premultiplied) {
    color_alpha_unpremultiply(color, color);
  }

  float2 pos = float2(0.0);
  if (scope_mode == SEQ_DRAW_IMG_WAVEFORM) {
    /* Waveform: pixel height based on luminance. */
    pos.x = texel.x - image_width / 2;
    pos.y = (get_luminance(color.rgb, luma_coeffs) - 0.5) * image_height;
  }
  else if (scope_mode == SEQ_DRAW_IMG_RGBPARADE) {
    /* RGB parade: similar to waveform, except three different "bands"
     * for each R/G/B intensity. */
    int channel = texel.x % 3;
    int column = texel.x / 3;

    /* Use a bit desaturated color, and blend in a bit of original pixel color. */
    float other_channels = 0.6;
    float factor = 0.4;
    if (channel == 0) {
      pos.x = column - image_width / 2;
      pos.y = (color.r - 0.5) * image_height;
      color.rgb = mix(color.rgb, float3(1, other_channels, other_channels), factor);
    }
    if (channel == 1) {
      pos.x = column - image_width / 2 + image_width / 3;
      pos.y = (color.g - 0.5) * image_height;
      color.rgb = mix(color.rgb, float3(other_channels, 1, other_channels), factor);
    }
    if (channel == 2) {
      pos.x = column - image_width / 2 + image_width * 2 / 3;
      pos.y = (color.b - 0.5) * image_height;
      color.rgb = mix(color.rgb, float3(other_channels, other_channels, 1), factor);
    }
  }
  else if (scope_mode == SEQ_DRAW_IMG_VECTORSCOPE) {
    /* Vectorscope: pixel position is based on U,V of the color. */
    float4 yuva;
    rgba_to_yuva_itu_709(color, yuva);
    float vec_size = min(image_width, image_height);
    /* Multiplier to map YUV U,V range (+-0.436, +-0.615) to +-0.5 on both axes. */
    float2 uv_scale = float2(0.5f / 0.436f, 0.5f / 0.615f);
    pos = yuva.yz * vec_size * uv_scale;
  }

  /* Determine final point color: we want to keep the hue, desaturate it a bit,
   * and use full brightness. */
  float4 hsv;
  rgb_to_hsv(color, hsv);
  if (scope_mode != SEQ_DRAW_IMG_RGBPARADE) {
    /* Saturation adjustments for parade mode are already done above. */
    hsv.y *= 0.5;
  }
  hsv.z = 1.0;
  hsv_to_rgb(hsv, color);

  finalColor.rgb = color.rgb;
  finalColor.a = 0.2;

  float size = scope_point_size * 4.0;
  if (size < 2.0) {
    /* If point size becomes very small, keep it at minimum size and instead
     * fade points out. */
    finalColor.a *= size / 2.0;
    finalColor.a = max(1.0 / 255.0, finalColor.a);
    size = 2.0;
  }

  gl_Position = ModelViewProjectionMatrix * float4(pos, 0.0f, 1.0f);
  gl_PointSize = size;

  /* calculate concentric radii in pixels */
  float radius = 0.5f * size;

  /* start at the outside and progress toward the center */
  radii[0] = radius;
  radii[1] = radius - 1.0f;

  /* convert to PointCoord units */
  radii /= size;
}
