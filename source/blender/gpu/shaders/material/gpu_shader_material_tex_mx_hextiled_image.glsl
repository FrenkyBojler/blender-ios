/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* MaterialX-compatible hextiled image sampling, adapted from MaterialX's
 * genglsl hextiledimage implementation. */

float2 mx_hextile_hash(float2 p)
{
  float3 p3 = fract(float3(p.x, p.y, p.x) * float3(0.1031f, 0.1030f, 0.0973f));
  p3 += dot(p3, float3(p3.y, p3.z, p3.x) + 33.33f);
  return fract((float2(p3.x, p3.x) + float2(p3.y, p3.z)) * float2(p3.z, p3.y));
}

float mx_hextile_schlick_gain(float x, float r)
{
  float rr = clamp(r, 0.001f, 0.999f);
  float a = (1.0f / rr - 2.0f) * (1.0f - 2.0f * x);
  return (x < 0.5f) ? x / (a + 1.0f) : (a - x) / (a - 1.0f);
}

float3 mx_hextile_normalize_weights(float3 w)
{
  return w / dot(w, float3(1.0f));
}

float3 mx_hextile_compute_blend_weights(float3 luminance_weights, float3 tile_weights, float falloff)
{
  float3 w = luminance_weights * pow(tile_weights, float3(7.0f));
  w = mx_hextile_normalize_weights(w);
  if (falloff != 0.5f) {
    w = float3(mx_hextile_schlick_gain(w.x, falloff),
               mx_hextile_schlick_gain(w.y, falloff),
               mx_hextile_schlick_gain(w.z, falloff));
    w = mx_hextile_normalize_weights(w);
  }
  return w;
}

float2 mx_hextile_rotate2d(float2 point, float angle)
{
  float s = sin(angle);
  float c = cos(angle);
  return float2(c * point.x - s * point.y, s * point.x + c * point.y);
}

float2 mx_hextile_to_texture_space(float2 coord)
{
  return float2(coord.x, 1.0f - coord.y);
}

float2 mx_hextile_from_texture_space(float2 coord)
{
  return float2(coord.x, 1.0f - coord.y);
}

float2 mx_hextile_deriv_from_texture_space(float2 deriv)
{
  return float2(deriv.x, -deriv.y);
}

void mx_hextile_coord(float2 coord,
                      float rotation,
                      float2 rotation_range,
                      float scale,
                      float2 scale_range,
                      float offset,
                      float2 offset_range,
                      out float2 coord1,
                      out float2 coord2,
                      out float2 coord3,
                      out float2 ddx1,
                      out float2 ddx2,
                      out float2 ddx3,
                      out float2 ddy1,
                      out float2 ddy2,
                      out float2 ddy3,
                      out float3 weights)
{
  float sqrt3_2 = sqrt(3.0f) * 2.0f;
  float2 st = coord * sqrt3_2;
  float2 st_skewed = float2(st.x - 0.57735027f * st.y, 1.15470054f * st.y);
  float2 st_frac = fract(st_skewed);
  float3 temp = float3(st_frac.x, st_frac.y, 1.0f - st_frac.x - st_frac.y);

  float s = step(0.0f, -temp.z);
  float s2 = 2.0f * s - 1.0f;
  weights = float3(-temp.z * s2, s - temp.y * s2, s - temp.x * s2);

  float2 base_id = floor(st_skewed);
  float2 id1 = base_id + float2(s, s);
  float2 id2 = base_id + float2(s, 1.0f - s);
  float2 id3 = base_id + float2(1.0f - s, s);

  float2 ctr1 = float2(id1.x / sqrt3_2 + 0.5f * id1.y / sqrt3_2,
                       0.8660254f * id1.y / sqrt3_2);
  float2 ctr2 = float2(id2.x / sqrt3_2 + 0.5f * id2.y / sqrt3_2,
                       0.8660254f * id2.y / sqrt3_2);
  float2 ctr3 = float2(id3.x / sqrt3_2 + 0.5f * id3.y / sqrt3_2,
                       0.8660254f * id3.y / sqrt3_2);

  float2 seed_offset = float2(0.12345f);
  float2 rand1 = mx_hextile_hash(id1 + seed_offset);
  float2 rand2 = mx_hextile_hash(id2 + seed_offset);
  float2 rand3 = mx_hextile_hash(id3 + seed_offset);

  float2 rr = rotation_range * (M_PI / 180.0f);
  float3 rotations = mix(float3(rr.x), float3(rr.y), float3(rand1.x, rand2.x, rand3.x) * rotation);
  float3 scales = mix(float3(1.0f),
                      mix(float3(scale_range.x), float3(scale_range.y), float3(rand1.y, rand2.y, rand3.y)),
                      scale);
  scales = max(scales, float3(1e-6f));

  float2 offset1 = mix(float2(offset_range.x), float2(offset_range.y), rand1 * offset);
  float2 offset2 = mix(float2(offset_range.x), float2(offset_range.y), rand2 * offset);
  float2 offset3 = mix(float2(offset_range.x), float2(offset_range.y), rand3 * offset);

  coord1 = mx_hextile_rotate2d(coord - ctr1, rotations.x) / scales.x + ctr1 + offset1;
  coord2 = mx_hextile_rotate2d(coord - ctr2, rotations.y) / scales.y + ctr2 + offset2;
  coord3 = mx_hextile_rotate2d(coord - ctr3, rotations.z) / scales.z + ctr3 + offset3;

  float2 dx = gpu_dfdx(coord) * texture_lod_bias_get();
  float2 dy = gpu_dfdy(coord) * texture_lod_bias_get();
  ddx1 = mx_hextile_rotate2d(dx, rotations.x) / scales.x;
  ddx2 = mx_hextile_rotate2d(dx, rotations.y) / scales.y;
  ddx3 = mx_hextile_rotate2d(dx, rotations.z) / scales.z;
  ddy1 = mx_hextile_rotate2d(dy, rotations.x) / scales.x;
  ddy2 = mx_hextile_rotate2d(dy, rotations.y) / scales.y;
  ddy3 = mx_hextile_rotate2d(dy, rotations.z) / scales.z;
}

[[node]]
void node_tex_mx_hextiled_image(float3 co,
                                float3 tiling,
                                float rotation,
                                float3 rotation_range,
                                float scale,
                                float3 scale_range,
                                float offset,
                                float3 offset_range,
                                float falloff,
                                float falloff_contrast,
                                float3 lumacoeffs,
                                sampler2D ima,
                                float4 &color,
                                float &alpha)
{
  float2 coord = mx_hextile_to_texture_space(co.xy * tiling.xy);
  float2 coord1, coord2, coord3, ddx1, ddx2, ddx3, ddy1, ddy2, ddy3;
  float3 tile_weights;
  mx_hextile_coord(coord,
                   rotation,
                   rotation_range.xy,
                   scale,
                   scale_range.xy,
                   offset,
                   offset_range.xy,
                   coord1,
                   coord2,
                   coord3,
                   ddx1,
                   ddx2,
                   ddx3,
                   ddy1,
                   ddy2,
                   ddy3,
                   tile_weights);

  float4 c1 = textureGrad(ima,
                           mx_hextile_from_texture_space(coord1),
                           mx_hextile_deriv_from_texture_space(ddx1),
                           mx_hextile_deriv_from_texture_space(ddy1));
  float4 c2 = textureGrad(ima,
                           mx_hextile_from_texture_space(coord2),
                           mx_hextile_deriv_from_texture_space(ddx2),
                           mx_hextile_deriv_from_texture_space(ddy2));
  float4 c3 = textureGrad(ima,
                           mx_hextile_from_texture_space(coord3),
                           mx_hextile_deriv_from_texture_space(ddx3),
                           mx_hextile_deriv_from_texture_space(ddy3));

  float3 cw = float3(dot(c1.rgb, lumacoeffs), dot(c2.rgb, lumacoeffs), dot(c3.rgb, lumacoeffs));
  const float falloff_contrast_weight = falloff_contrast * 0.5f;
  cw = mix(float3(1.0f), cw, float3(falloff_contrast_weight));
  float3 w = mx_hextile_compute_blend_weights(cw, tile_weights, falloff);
  float3 aw = mx_hextile_compute_blend_weights(float3(1.0f), tile_weights, falloff);

  color = float4(w.x * c1.rgb + w.y * c2.rgb + w.z * c3.rgb,
                 aw.x * c1.a + aw.y * c2.a + aw.z * c3.a);
  alpha = color.a;
}
