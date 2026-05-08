/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/svm/image.h"
#include "kernel/svm/node_types.h"
#include "kernel/svm/util.h"

#include "util/math_dual.h"

CCL_NAMESPACE_BEGIN

ccl_device_inline float2 mx_hextile_hash(const float2 p)
{
  float3 p3 = make_float3(fractf(p.x * 0.1031f), fractf(p.y * 0.1030f), fractf(p.x * 0.0973f));
  p3 += make_float3(dot(p3, make_float3(p3.y, p3.z, p3.x) + make_float3(33.33f)));
  const float2 value = (make_float2(p3.x, p3.x) + make_float2(p3.y, p3.z)) *
                       make_float2(p3.z, p3.y);
  return make_float2(fractf(value.x), fractf(value.y));
}

ccl_device_inline float mx_hextile_schlick_gain(const float x, const float r)
{
  const float rr = clamp(r, 0.001f, 0.999f);
  const float a = (1.0f / rr - 2.0f) * (1.0f - 2.0f * x);
  return (x < 0.5f) ? x / (a + 1.0f) : (a - x) / (a - 1.0f);
}

ccl_device_inline float3 mx_hextile_normalize_weights(const float3 weights)
{
  return weights / dot(weights, one_float3());
}

ccl_device_inline float3 mx_hextile_compute_blend_weights(const float3 luminance_weights,
                                                          const float3 tile_weights,
                                                          const float falloff)
{
  float3 w = luminance_weights * power(tile_weights, 7.0f);
  w = mx_hextile_normalize_weights(w);
  if (falloff != 0.5f) {
    w = make_float3(mx_hextile_schlick_gain(w.x, falloff),
                    mx_hextile_schlick_gain(w.y, falloff),
                    mx_hextile_schlick_gain(w.z, falloff));
    w = mx_hextile_normalize_weights(w);
  }
  return w;
}

ccl_device_inline float2 mx_hextile_rotate2d(const float2 point, const float angle)
{
  const float sine = sinf(angle);
  const float cosine = cosf(angle);
  return make_float2(cosine * point.x - sine * point.y, sine * point.x + cosine * point.y);
}

ccl_device_inline dual2 mx_hextile_rotate2d(const dual2 point, const float angle)
{
  const float sine = sinf(angle);
  const float cosine = cosf(angle);
  return make_float2(point.x() * cosine - point.y() * sine,
                     point.x() * sine + point.y() * cosine);
}

ccl_device_inline float2 mx_hextile_to_texture_space(const float2 coord)
{
  return make_float2(coord.x, 1.0f - coord.y);
}

ccl_device_inline dual2 mx_hextile_to_texture_space(const dual2 coord)
{
  return make_float2(coord.x(), 1.0f - coord.y());
}

ccl_device_inline float2 mx_hextile_from_texture_space(const float2 coord)
{
  return make_float2(coord.x, 1.0f - coord.y);
}

ccl_device_inline dual2 mx_hextile_from_texture_space(const dual2 coord)
{
  return make_float2(coord.x(), 1.0f - coord.y());
}

ccl_device_inline float2 mx_hextile_base_value(const float2 v)
{
  return v;
}

ccl_device_inline float2 mx_hextile_base_value(const dual2 v)
{
  return v.val;
}

template<class Float2Type>
ccl_device_inline void mx_hextile_coord(const Float2Type coord,
                                        const float rotation,
                                        const float2 rotation_range,
                                        const float scale,
                                        const float2 scale_range,
                                        const float offset,
                                        const float2 offset_range,
                                        ccl_private Float2Type coords[3],
                                        ccl_private float3 *weights)
{
  const float sqrt3_2 = sqrtf(3.0f) * 2.0f;
  const float2 coord_value = mx_hextile_base_value(coord);
  const float2 st = coord_value * sqrt3_2;
  const float2 st_skewed = make_float2(st.x - 0.57735027f * st.y, 1.15470054f * st.y);
  const float2 st_frac = make_float2(fractf(st_skewed.x), fractf(st_skewed.y));
  const float tx = st_frac.x;
  const float ty = st_frac.y;
  const float tz = 1.0f - tx - ty;

  const float s = (-tz < 0.0f) ? 0.0f : 1.0f;
  const float s2 = 2.0f * s - 1.0f;
  *weights = make_float3(-tz * s2, s - ty * s2, s - tx * s2);

  const float2 base_id = floor(st_skewed);
  const float2 id1 = base_id + make_float2(s, s);
  const float2 id2 = base_id + make_float2(s, 1.0f - s);
  const float2 id3 = base_id + make_float2(1.0f - s, s);

  const float2 ctr1 = make_float2(id1.x / sqrt3_2 + 0.5f * id1.y / sqrt3_2,
                                  0.8660254f * id1.y / sqrt3_2);
  const float2 ctr2 = make_float2(id2.x / sqrt3_2 + 0.5f * id2.y / sqrt3_2,
                                  0.8660254f * id2.y / sqrt3_2);
  const float2 ctr3 = make_float2(id3.x / sqrt3_2 + 0.5f * id3.y / sqrt3_2,
                                  0.8660254f * id3.y / sqrt3_2);

  const float2 seed_offset = make_float2(0.12345f);
  const float2 rand1 = mx_hextile_hash(id1 + seed_offset);
  const float2 rand2 = mx_hextile_hash(id2 + seed_offset);
  const float2 rand3 = mx_hextile_hash(id3 + seed_offset);

  const float2 rr = rotation_range * (M_PI_F / 180.0f);
  const float3 rotations = make_float3(interp(rr.x, rr.y, rand1.x * rotation),
                                       interp(rr.x, rr.y, rand2.x * rotation),
                                       interp(rr.x, rr.y, rand3.x * rotation));
  const float3 random_scales = make_float3(interp(scale_range.x, scale_range.y, rand1.y),
                                           interp(scale_range.x, scale_range.y, rand2.y),
                                           interp(scale_range.x, scale_range.y, rand3.y));
  const float3 scales = max(interp(one_float3(), random_scales, scale), make_float3(1e-6f));

  const float2 offset1 = make_float2(interp(offset_range.x, offset_range.y, rand1.x * offset),
                                     interp(offset_range.x, offset_range.y, rand1.y * offset));
  const float2 offset2 = make_float2(interp(offset_range.x, offset_range.y, rand2.x * offset),
                                     interp(offset_range.x, offset_range.y, rand2.y * offset));
  const float2 offset3 = make_float2(interp(offset_range.x, offset_range.y, rand3.x * offset),
                                     interp(offset_range.x, offset_range.y, rand3.y * offset));

  coords[0] = mx_hextile_rotate2d(coord - ctr1, rotations.x) * (1.0f / scales.x) + ctr1 +
              offset1;
  coords[1] = mx_hextile_rotate2d(coord - ctr2, rotations.y) * (1.0f / scales.y) + ctr2 +
              offset2;
  coords[2] = mx_hextile_rotate2d(coord - ctr3, rotations.z) * (1.0f / scales.z) + ctr3 +
              offset3;
}

template<class Float3Type>
ccl_device_noinline void svm_node_tex_mx_hextiled_image(
    KernelGlobals kg,
    ccl_private ShaderData *sd,
    ccl_private float *ccl_restrict stack,
    const ccl_global SVMNodeTexMxHextiledImage &ccl_restrict node)
{
  const Float3Type vector = stack_load<Float3Type>(stack, node.vector_offset);
  const float3 tiling = stack_load(stack, node.tiling);
  const float rotation = stack_load(stack, node.rotation);
  const float3 rotation_range = stack_load(stack, node.rotation_range);
  const float scale = stack_load(stack, node.scale);
  const float3 scale_range = stack_load(stack, node.scale_range);
  const float offset = stack_load(stack, node.offset);
  const float3 offset_range = stack_load(stack, node.offset_range);
  const float falloff = stack_load(stack, node.falloff);
  const float falloff_contrast = stack_load(stack, node.falloff_contrast);
  const float3 luma_coeffs = stack_load(stack, node.luma_coeffs);

  auto coord = mx_hextile_to_texture_space(make_float2(vector) * make_float2(tiling.x, tiling.y));
  decltype(coord) coords[3];
  float3 tile_weights;
  mx_hextile_coord(coord,
                   rotation,
                   make_float2(rotation_range.x, rotation_range.y),
                   scale,
                   make_float2(scale_range.x, scale_range.y),
                   offset,
                   make_float2(offset_range.x, offset_range.y),
                   coords,
                   &tile_weights);

  const float4 c1 = svm_image_texture(
      kg, sd, node.id, dual2(mx_hextile_from_texture_space(coords[0])), node.flags);
  const float4 c2 = svm_image_texture(
      kg, sd, node.id, dual2(mx_hextile_from_texture_space(coords[1])), node.flags);
  const float4 c3 = svm_image_texture(
      kg, sd, node.id, dual2(mx_hextile_from_texture_space(coords[2])), node.flags);

  const float falloff_contrast_weight = falloff_contrast * 0.5f;
  const float3 cw = interp(one_float3(),
                           make_float3(dot(make_float3(c1), luma_coeffs),
                                       dot(make_float3(c2), luma_coeffs),
                                       dot(make_float3(c3), luma_coeffs)),
                           falloff_contrast_weight);
  const float3 w = mx_hextile_compute_blend_weights(cw, tile_weights, falloff);
  const float3 aw = mx_hextile_compute_blend_weights(one_float3(), tile_weights, falloff);
  const float4 result = make_float4(w.x * make_float3(c1) + w.y * make_float3(c2) + w.z * make_float3(c3),
                                    aw.x * c1.w + aw.y * c2.w + aw.z * c3.w);

  if (stack_valid(node.out_offset)) {
    stack_store_float3(stack, node.out_offset, make_float3(result));
  }
  if (stack_valid(node.alpha_offset)) {
    stack_store_float(stack, node.alpha_offset, result.w);
  }
}

CCL_NAMESPACE_END
