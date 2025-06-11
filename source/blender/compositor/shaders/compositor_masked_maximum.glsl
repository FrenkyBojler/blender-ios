/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_compositor_texture_utilities.glsl"
#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_math_vector_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

bool is_in_unit_rounded_square(float2 coord, const float roundness)
{
  if (roundness == 1.0f) {
    return square(coord.x) + square(coord.y) <= 1.0f;
  }

  /* Remap coord into first octand. This can be done because the rounded square mask is symmetric
   * to both the X and Y axes. */
  coord = abs(coord);
  coord = float2(max(coord.x, coord.y), min(coord.x, coord.y));

  if (roundness == 0.0f) {
    return coord.x <= 1.0f;
  }

  return ((coord.x <= 1.0f) && (coord.y <= (1.0f - roundness))) ||
         (square(coord.x - 1.0f + roundness) + square(coord.y - 1.0f + roundness) <=
          square(roundness));
}

float compute_rounded_square_radius(float2 coord, const float roundness)
{
  float l_coord = sqrt(square(coord.x) + square(coord.y));

  if (roundness == 1.0f) {
    return l_coord;
  }

  /* Remap coord into first octand. This can be done because the rounded square mask is symmetric
   * to both the X and Y axes. */
  coord = abs(coord);
  coord = float2(max(coord.x, coord.y), min(coord.x, coord.y));

  if (roundness == 0.0f) {
    return coord.x;
  }

  float angle_bisector_A_coord = atan(coord.y / coord.x);
  float angle_bisector_A_bevel_start = atan(1.0f - roundness);
  if (angle_bisector_A_coord > angle_bisector_A_bevel_start) {
    /* Regular rounded part. */
    float coord_A_segment_divider = M_PI_4 - angle_bisector_A_coord;
    float l_circle_center = M_SQRT2 * (1.0f - roundness);
    float l_coord_R_l_bevel_start = cos(coord_A_segment_divider) * l_circle_center +
                                    sqrt(square(cos(coord_A_segment_divider) * l_circle_center) +
                                         square(roundness) - square(l_circle_center));

    return l_coord / l_coord_R_l_bevel_start;
  }
  else {
    /* Regular straight part. */
    return l_coord * cos(angle_bisector_A_coord);
  }
}

float compute_rounded_square_mask(float2 coord,
                                  float2 size,
                                  const float roundness,
                                  const float falloff)
{
  /* Swap x and y names if size.y > size.x. This is done because the following code excpects
   * size.x to be greater or equal to size.y. This makes sure that the falloff is calculated
   * based on the larger size, making the Falloff input an upper limit to the falloff range. */
  if (size.y > size.x) {
    swap(coord.x, coord.y);
    swap(size.x, size.y);
  }

  if (size.y == 0.0f) {
    if (size.x == 0.0f) {
      if ((falloff == 0.0f) ||
          (!is_in_unit_rounded_square(coord / (float2(falloff, falloff)), roundness)))
      {
        /* coord is outside of the mask. */
        return 0.0f;
      }
      else {
        /* coord is in the linear falloff part of the mask. */
        return inverse_mix(falloff, 0.0f, compute_rounded_square_radius(coord, roundness));
      }
    }
    else {
      /* Mask is a 1 dimensional line. */
      if ((coord.y != 0.0f) || (abs(coord.x) > (size.x + falloff))) {
        /* coord is outside of the mask. */
        return 0.0f;
      }
      else if (abs(coord.x) <= (size.x)) {
        /* coord is in the constant part of the mask. */
        return 1.0f;
      }
      else {
        /* coord is in the linear falloff part of the mask. */
        return inverse_mix(size.x + falloff, size.x, abs(coord.x));
      }
    }
  }
  else {
    if (is_in_unit_rounded_square(coord / size, roundness)) {
      /* coord is in the constant part of the mask. */
      return 1.0f;
    }
    else if ((falloff == 0.0f) ||
             !is_in_unit_rounded_square(
                 coord / (size + float2(falloff, falloff * size.y / size.x)), roundness))
    {
      /* coord is outside of the mask. */
      return 0.0f;
    }
    else {
      /* coord is in the linear falloff part of the mask. */
      return inverse_mix(
          size.x + falloff,
          size.x,
          compute_rounded_square_radius(float2(coord.x, coord.y * size.x / size.y), roundness));
    }
  }
}

float2 rotate_vector_2d(float2 vector, float angle)
{
  return float2(vector.x * cos(angle) - vector.y * sin(angle),
                vector.x * sin(angle) + vector.y * cos(angle));
}

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);

  float4 size = max(texture_load(input_size_tx, texel), float4(0.0f, 0.0f, 0.0f, 0.0f));
  float rotation = texture_load(input_rotation_tx, texel).x;
  float4 translation = texture_load(input_translation_tx, texel);
  float roundness = clamp(texture_load(input_roundness_tx, texel).x, 0.0f, 1.0f);
  float falloff = max(texture_load(input_falloff_tx, texel).x, 0.0f);

  /* Calculate top right and bottom left corner of the bounding box of the rounded square mask.
   */
  float2 bounding_box_top_right_corner_float;
  if (size.x == size.y) {
    bounding_box_top_right_corner_float = float2(ceil(size.x + falloff), ceil(size.y + falloff));
  }
  else if (size.x == 0.0f) {
    bounding_box_top_right_corner_float = float2(0.0f, ceil(size.y + falloff));
  }
  else if (size.y == 0.0f) {
    bounding_box_top_right_corner_float = float2(ceil(size.x + falloff), 0.0f);
  }
  else {
    bounding_box_top_right_corner_float = float2(
        ceil(size.x + (falloff * min(size.x / size.y, 1.0f))),
        ceil(size.y + (falloff * min(size.y / size.x, 1.0f))));
  }
  if (rotation != 0.0f) {
    float2 rotated_top_right_corner = rotate_vector_2d(
        float2(bounding_box_top_right_corner_float.x, bounding_box_top_right_corner_float.y),
        rotation);
    float2 rotated_bottom_right_corner = rotate_vector_2d(
        float2(bounding_box_top_right_corner_float.x, -bounding_box_top_right_corner_float.y),
        rotation);
    bounding_box_top_right_corner_float = float2(
        max(ceil(abs(rotated_top_right_corner.x)), ceil(abs(rotated_bottom_right_corner.x))),
        max(ceil(abs(rotated_top_right_corner.y)), ceil(abs(rotated_bottom_right_corner.y))));
  }
  int2 bounding_box_top_right_corner = int2(bounding_box_top_right_corner_float);
  int2 bounding_box_bottom_left_corner = -bounding_box_top_right_corner;
  /* Crop away parts of the computation window that are outside of the domain. */
  bounding_box_top_right_corner += texel;
  bounding_box_bottom_left_corner += texel;
  bounding_box_top_right_corner = min(bounding_box_top_right_corner, domain_size - int2(1, 1));
  bounding_box_bottom_left_corner = max(bounding_box_bottom_left_corner, int2(0, 0));
  bounding_box_top_right_corner -= texel;
  bounding_box_bottom_left_corner -= texel;
  float masked_maximum = -FLT_MAX;
  for (int y = bounding_box_bottom_left_corner.y; y <= bounding_box_top_right_corner.y; y++) {
    for (int x = bounding_box_bottom_left_corner.x; x <= bounding_box_top_right_corner.x; x++) {
      float2 coord = float2(x, y) - float2(translation.x, translation.y);
      if (rotation != 0.0f) {
        coord = rotate_vector_2d(coord, -rotation);
      }
      masked_maximum = max(
          masked_maximum,
          compute_rounded_square_mask(coord, float2(size.x, size.y), roundness, falloff) *
              max(texture_load(input_mask_tx, texel + int2(x, y)).x, 0.0f));
    }
  }

  imageStore(output_mask_img, texel, float4(masked_maximum));
}
