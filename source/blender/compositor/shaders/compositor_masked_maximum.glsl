/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_masked_maximum_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_masked_maximum)

#include "gpu_shader_compositor_texture_utilities.glsl"
#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_math_constants_lib.glsl"
#include "gpu_shader_math_vector_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

float2 rotate_vector_2d(float2 vector, float angle)
{
  return float2(vector.x * cos(angle) - vector.y * sin(angle),
                vector.x * sin(angle) + vector.y * cos(angle));
}

float elliptical_ramp_without_constant_part(float value, float ellipse_height, float ellipse_width)
{
  if (value < ellipse_width + ellipse_height * (1.0f - ellipse_width)) {
    return (ellipse_height *
            (value * ellipse_height * (1.0f - ellipse_width) + square(ellipse_width) -
             ellipse_width * sqrt(square(ellipse_width) - square(value) +
                                  2.0f * value * ellipse_height * (1.0f - ellipse_width)))) /
           (square(ellipse_height * (1.0f - ellipse_width)) + square(ellipse_width));
  }
  else {
    return (ellipse_width == 1.0f) ? ellipse_height :
                                     (value - ellipse_width) / (1.0f - ellipse_width);
  }
}

float elliptical_unit_step_without_constant_part(float value,
                                                 float ellipse_height,
                                                 float ellipse_width,
                                                 float inflection_midpoint)
{
  if (ellipse_width == 0.0f) {
    return value;
  }
  else if (inflection_midpoint == 0.0f) {
    return 1.0f -
           elliptical_ramp_without_constant_part(1.0f - value, ellipse_height, ellipse_width);
  }
  else if (inflection_midpoint == 1.0f) {
    return elliptical_ramp_without_constant_part(value, ellipse_height, ellipse_width);
  }
  else {
    return (value < inflection_midpoint) ?
               inflection_midpoint *
                   elliptical_ramp_without_constant_part(
                       value / inflection_midpoint, ellipse_height, ellipse_width) :
               1.0f - (1.0f - inflection_midpoint) *
                          elliptical_ramp_without_constant_part((1.0f - value) /
                                                                    (1.0f - inflection_midpoint),
                                                                ellipse_height,
                                                                ellipse_width);
  }
}

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
                                  float2 abs_mask_size,
                                  const float mask_roundness,
                                  const float hardness,
                                  const float ellipse_height,
                                  const float ellipse_width,
                                  const float inflection_midpoint)
{
  /* Swap x and y names if abs_mask_size.y > abs_mask_size.x. This is done
   * because the following code expects abs_mask_size.x to be greater or equal to
   * abs_mask_size.y. */
  if (abs_mask_size.y > abs_mask_size.x) {
    swap(coord.x, coord.y);
    swap(abs_mask_size.x, abs_mask_size.y);
  }

  if (abs_mask_size.y == 0.0f) {
    if (abs_mask_size.x == 0.0f) {
      /* Mask is a 0 dimensional point. */
      return ((coord.x == 0.0f) && (coord.y == 0.0f)) ? 1.0f : 0.0f;
    }
    else {
      /* Mask is a 1 dimensional line. */
      if ((coord.y != 0.0f) || (abs(coord.x) > abs_mask_size.x)) {
        /* coord is outside of the mask. */
        return 0.0f;
      }
      else if (abs(coord.x) <= (hardness * abs_mask_size.x)) {
        /* coord is in the constant part of the mask. */
        return 1.0f;
      }
      else {
        /* coord is in the falloff part of the mask. */
        return elliptical_unit_step_without_constant_part(
            inverse_mix(abs_mask_size.x, hardness * abs_mask_size.x, abs(coord.x)),
            ellipse_height,
            ellipse_width,
            1.0f - inflection_midpoint);
      }
    }
  }
  else {
    /* Mask is a 2 dimensional rounded square. */
    if (is_in_unit_rounded_square(coord / (hardness * abs_mask_size), mask_roundness)) {
      /* coord is in the constant part of the mask. */
      return 1.0f;
    }
    else if ((hardness == 1.0f) ||
             !is_in_unit_rounded_square(coord / abs_mask_size, mask_roundness))
    {
      /* coord is outside of the mask. */
      return 0.0f;
    }
    else {
      /* coord is in the falloff part of the mask. */
      return elliptical_unit_step_without_constant_part(
          inverse_mix(
              abs_mask_size.x,
              hardness * abs_mask_size.x,
              compute_rounded_square_radius(
                  float2(coord.x, coord.y * abs_mask_size.x / abs_mask_size.y), mask_roundness)),
          ellipse_height,
          ellipse_width,
          1.0f - inflection_midpoint);
    }
  }
}

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);

  float2 mask_size = texture_load(input_mask_size_tx, texel).xy;
  float domain_diagonal_length = sqrt(square(float(domain_data_size.x)) +
                                      square(float(domain_data_size.y)));
  /* In principle, absolute mask_size values greater than domain_diagonal_length can still
   * result in different outputs, however, to prevent extremely long computation times, they are
   * clamped. */
  mask_size = clamp(
      mask_size, float2(-ceil(domain_diagonal_length)), float2(ceil(domain_diagonal_length)));
  float2 abs_mask_size = abs(mask_size);
  float mask_roundness = clamp(texture_load(input_mask_roundness_tx, texel).x, 0.0f, 1.0f);
  float rotation = texture_load(input_rotation_tx, texel).x;
  float2 translation = texture_load(input_translation_tx, texel).xy;
  float hardness = clamp(texture_load(input_hardness_tx, texel).x, 0.0f, 1.0f);
  float value_boundary = texture_load(input_value_boundary_tx, texel).x;
  float ellipse_height = clamp(texture_load(input_ellipse_height_tx, texel).x, 0.0f, 1.0f);
  float ellipse_width = clamp(texture_load(input_ellipse_width_tx, texel).x, 0.0f, 1.0f);
  float inflection_midpoint = clamp(
      texture_load(input_inflection_midpoint_tx, texel).x, 0.0f, 1.0f);

  /* Calculate the top right and bottom left corners of the bounding box of the rounded square
   * mask. */
  float2 bounding_box_top_right_corner_relative_to_pixel = ceil(abs_mask_size);
  if (rotation != 0.0f) {
    /* Rotate bounding box. */
    float2 rotated_top_right_corner = rotate_vector_2d(
        float2(bounding_box_top_right_corner_relative_to_pixel.x,
               bounding_box_top_right_corner_relative_to_pixel.y),
        rotation);
    float2 rotated_bottom_right_corner = rotate_vector_2d(
        float2(bounding_box_top_right_corner_relative_to_pixel.x,
               -bounding_box_top_right_corner_relative_to_pixel.y),
        rotation);
    bounding_box_top_right_corner_relative_to_pixel = float2(
        max(ceil(abs(rotated_top_right_corner.x)), ceil(abs(rotated_bottom_right_corner.x))),
        max(ceil(abs(rotated_top_right_corner.y)), ceil(abs(rotated_bottom_right_corner.y))));
  }
  float2 bounding_box_bottom_left_corner_relative_to_pixel =
      -bounding_box_top_right_corner_relative_to_pixel;
  /* Translate bounding box. */
  bounding_box_top_right_corner_relative_to_pixel = ceil(
      bounding_box_top_right_corner_relative_to_pixel + translation);
  bounding_box_bottom_left_corner_relative_to_pixel = floor(
      bounding_box_bottom_left_corner_relative_to_pixel + translation);

  int2 bounding_box_top_right_corner = texel +
                                       int2(bounding_box_top_right_corner_relative_to_pixel);
  int2 bounding_box_bottom_left_corner = texel +
                                         int2(bounding_box_bottom_left_corner_relative_to_pixel);
  if (!keep_seamless) {
    /* Crop away parts of the bounding box that are outside the domain. */
    bounding_box_top_right_corner = min(bounding_box_top_right_corner,
                                        domain_data_size - int2(1, 1));
    bounding_box_bottom_left_corner = max(bounding_box_bottom_left_corner, int2(0, 0));
  }

  /* Initialize the output variables by evaluating the pixel that is outside the bounding box
   * and closest to the pixel that the operation is evaluated on. Therefore, if the pixel that
   * the operation is evaluated on is outside the bounding box, it is used to initialize the
   * output variables. */
  float masked_maximum = value_boundary;
  float2 chosen_pixel_coordinates = float2(texel);
  if ((texel.x >= bounding_box_bottom_left_corner.x) &&
      (texel.y >= bounding_box_bottom_left_corner.y) &&
      (texel.x <= bounding_box_top_right_corner.x) && (texel.y <= bounding_box_top_right_corner.y))
  {
    /* The pixel that the operation is evaluated on is inside the bounding box. */
    if (keep_seamless) {
      /* Start left of the bounding box. */
      chosen_pixel_coordinates = float2(bounding_box_bottom_left_corner.x - 1, texel.y);
      int smallest_distance = texel.x - bounding_box_bottom_left_corner.x + 1;

      /* Check below the bounding box. */
      if (smallest_distance > (texel.y - bounding_box_bottom_left_corner.y + 1)) {
        chosen_pixel_coordinates = float2(texel.x, bounding_box_bottom_left_corner.y - 1);
        smallest_distance = texel.y - bounding_box_bottom_left_corner.y + 1;
      }

      /* Check right of the bounding box. */
      if (smallest_distance > (bounding_box_top_right_corner.x + 1 - texel.x)) {
        chosen_pixel_coordinates = float2(bounding_box_top_right_corner.x + 1, texel.y);
        smallest_distance = bounding_box_top_right_corner.x + 1 - texel.x;
      }

      /* Check above the bounding box. */
      if (smallest_distance > (bounding_box_top_right_corner.y + 1 - texel.y)) {
        chosen_pixel_coordinates = float2(texel.x, bounding_box_top_right_corner.y + 1);
      }
    }
    else {
      /* Initial pixel must be inside the domain. */
      int smallest_distance = INT_MAX;
      /* Check left of the bounding box. */
      if (bounding_box_bottom_left_corner.x >= 1) {
        chosen_pixel_coordinates = float2(bounding_box_bottom_left_corner.x - 1, texel.y);
        smallest_distance = texel.x - bounding_box_bottom_left_corner.x + 1;
      }

      /* Check below the bounding box. */
      if ((bounding_box_bottom_left_corner.y >= 1) &&
          (smallest_distance > (texel.y - bounding_box_bottom_left_corner.y + 1)))
      {
        chosen_pixel_coordinates = float2(texel.x, bounding_box_bottom_left_corner.y - 1);
        smallest_distance = texel.y - bounding_box_bottom_left_corner.y + 1;
      }

      /* Check right of the bounding box. */
      if ((bounding_box_top_right_corner.x <= (domain_data_size.x - 2)) &&
          (smallest_distance > (bounding_box_top_right_corner.x + 1 - texel.x)))
      {
        chosen_pixel_coordinates = float2(bounding_box_top_right_corner.x + 1, texel.y);
        smallest_distance = bounding_box_top_right_corner.x + 1 - texel.x;
      }

      /* Check above the bounding box. */
      if ((bounding_box_top_right_corner.y <= (domain_data_size.y - 2)) &&
          (smallest_distance > (bounding_box_top_right_corner.y + 1 - texel.y)))
      {
        chosen_pixel_coordinates = float2(texel.x, bounding_box_top_right_corner.y + 1);
      }
    }
  }
  float chosen_mask_value = 0.0f;

  bool is_dilate = (mask_size.x >= 0.0f) && (mask_size.y >= 0.0f);
  float2 mask_center_coordinates = float2(texel) + float2(translation);
  for (int y = bounding_box_bottom_left_corner.y; y <= bounding_box_top_right_corner.y; y++) {
    for (int x = bounding_box_bottom_left_corner.x; x <= bounding_box_top_right_corner.x; x++) {
      float2 pixel_coordinates = float2(x, y);
      float2 pixel_coordinates_relative_to_mask_center = pixel_coordinates -
                                                         mask_center_coordinates;
      if (rotation != 0.0f) {
        pixel_coordinates_relative_to_mask_center = rotate_vector_2d(
            pixel_coordinates_relative_to_mask_center, -rotation);
      }
      float rounded_square_mask = compute_rounded_square_mask(
          pixel_coordinates_relative_to_mask_center,
          abs_mask_size,
          mask_roundness,
          hardness,
          ellipse_height,
          ellipse_width,
          inflection_midpoint);

      int2 image_sampling_coordinates = int2(
          floored_mod(pixel_coordinates, float2(domain_data_size)));
      float iteration_masked_maximum =
          (rounded_square_mask *
           ((is_dilate ? texture_load(input_image_tx, image_sampling_coordinates).x :
                         (1.0f - texture_load(input_image_tx, image_sampling_coordinates).x)) -
            value_boundary)) +
          value_boundary;

      if ((iteration_masked_maximum > masked_maximum) ||
          ((iteration_masked_maximum == masked_maximum) &&
           (dot(pixel_coordinates - float2(texel), pixel_coordinates - float2(texel)) <
            dot(chosen_pixel_coordinates - float2(texel),
                chosen_pixel_coordinates - float2(texel)))))
      {
        chosen_mask_value = rounded_square_mask;
        chosen_pixel_coordinates = pixel_coordinates;
        masked_maximum = iteration_masked_maximum;
      }
    }
  }

  if (output_chosen_mask_value_should_compute) {
    imageStore(output_chosen_mask_value_img, texel, float4(chosen_mask_value));
  }
  if (output_chosen_pixel_should_compute) {
    /* Output the pixel coordinates that are inside the domain. */
    chosen_pixel_coordinates = floored_mod(chosen_pixel_coordinates, float2(domain_data_size));
    imageStore(output_chosen_pixel_img,
               texel,
               float4(chosen_pixel_coordinates.x, chosen_pixel_coordinates.y, 0.0f, 0.0f));
  }
  if (output_image_should_compute) {
    imageStore(
        output_image_img, texel, float4(is_dilate ? masked_maximum : (1.0f - masked_maximum)));
  }
}
