/* SPDX-FileCopyrightText: 2006 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup cmpnodes
 */

#include "BLI_assert.h"
#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"

#include "RNA_access.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "GPU_shader.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"

/* **************** Masked Maximum ******************** */

namespace blender::nodes::node_composite_masked_maximum_cc {

static void cmp_node_masked_maximum_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_input<decl::Float>("Image")
      .default_value(0.5f)
      .hide_value()
      .compositor_domain_priority(0)
      .structure_type(StructureType::Dynamic);
  b.add_output<decl::Float>("Image").structure_type(StructureType::Dynamic).align_with_previous();
  b.add_output<decl::Vector>("Chosen Pixel")
      .dimensions(2)
      .description("The integer coordinates of the pixel that was chosen during the operation")
      .structure_type(StructureType::Dynamic);
  b.add_output<decl::Float>("Chosen Mask Value")
      .description(
          "The value of the rounded square mask at the pixel that was chosen during the operation")
      .structure_type(StructureType::Dynamic);

  b.add_input<decl::Bool>("Keep Seamless")
      .default_value(false)
      .description(
          "When enabled, the operation keeps the output mask seamless for a seamless input mask.");
  b.add_input<decl::Vector>("Constant Part Size")
      .dimensions(2)
      .default_value({0.0f, 0.0f, 0.0f})
      .compositor_domain_priority(1)
      .description(
          "Size from the center of the constant part of the rounded square mask to its "
          "boundaries. If the Size value is negative in any dimension, an erosion is "
          "performed instead of a dilation")
      .structure_type(StructureType::Dynamic);
  b.add_input<decl::Float>("Roundness")
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .compositor_domain_priority(2)
      .description("Roundness of the rounded square mask")
      .structure_type(StructureType::Dynamic);

  PanelDeclarationBuilder &falloff_panel = b.add_panel("Falloff Part").default_closed(false);
  falloff_panel.add_input<decl::Float>("Size Boundary")
      .default_value(1.0f)
      .min(0.0f)
      .compositor_domain_priority(3)
      .description(
          "Maximal size of the falloff part of the rounded square mask starting at the edges of "
          "its constant part. This is also an upper boundary to where the falloff gradient can "
          "reach from a given pixel")
      .structure_type(StructureType::Dynamic);
  falloff_panel.add_input<decl::Float>("Value Boundary")
      .default_value(0.0f)
      .compositor_domain_priority(4)
      .description(
          "Value that the falloff gradient may fall off to. When performing a dilation, Value "
          "Boundary is a lower boundary to the possible output image values. When performing an "
          "erosion, 1 - Value Boundary is an upper boundary to the possible output image values")
      .structure_type(StructureType::Dynamic);
  falloff_panel.add_input<decl::Float>("Aggressiveness")
      .min(0.0f)
      .max(1.0f)
      .default_value(0.0f)
      .subtype(PROP_FACTOR)
      .compositor_domain_priority(5)
      .description(
          "Value of the falloff part at the outer edge of the rounded square mask. A higher value "
          "results in a more aggressive effect")
      .structure_type(StructureType::Dynamic);

  PanelDeclarationBuilder &falloff_shape_panel =
      falloff_panel.add_panel("Falloff Shape").default_closed(true);
  falloff_shape_panel.add_input<decl::Float>("Ellipse Height")
      .min(0.0f)
      .max(1.0f)
      .default_value(0.5f)
      .subtype(PROP_FACTOR)
      .compositor_domain_priority(6)
      .description(
          "Height of the elliptical segments of the elliptical step function, which is used to "
          "control the shape of the falloff. A higher value results in a smoother falloff.")
      .structure_type(StructureType::Dynamic);
  falloff_shape_panel.add_input<decl::Float>("Ellipse Width")
      .min(0.0f)
      .max(1.0f)
      .default_value(0.5f)
      .subtype(PROP_FACTOR)
      .compositor_domain_priority(7)
      .description(
          "Width of the elliptical segments of the elliptical step function, which is used to "
          "control the shape of the falloff. A higher value results in a rounder falloff")
      .structure_type(StructureType::Dynamic);
  falloff_shape_panel.add_input<decl::Float>("Inflection Midpoint")
      .min(0.0f)
      .max(1.0f)
      .default_value(0.5f)
      .subtype(PROP_FACTOR)
      .compositor_domain_priority(8)
      .description(
          "Position of the inflection midpoint of the elliptical step function, which is used to "
          "control the shape of the falloff. It controls how big the two elliptical segments are "
          "relative to each other")
      .structure_type(StructureType::Dynamic);

  PanelDeclarationBuilder &transform_panel = b.add_panel("Mask Transform").default_closed(true);
  transform_panel.add_input<decl::Float>("Rotation")
      .default_value(0.0f)
      .subtype(PROP_ANGLE)
      .compositor_domain_priority(9)
      .description("Angle to rotate the rounded square mask by")
      .structure_type(StructureType::Dynamic);
  transform_panel.add_input<decl::Vector>("Translation")
      .dimensions(2)
      .default_value({0.0f, 0.0f, 0.0f})
      .compositor_domain_priority(10)
      .description("Translation of the rounded square mask")
      .structure_type(StructureType::Dynamic);
}

using namespace blender::compositor;

class MaskedMaximumOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    Result &output_image = this->get_result("Image");
    Result &output_chosen_pixel = this->get_result("Chosen Pixel");
    Result &output_chosen_mask_value = this->get_result("Chosen Mask Value");

    const Domain domain = compute_domain();
    if (output_image.should_compute()) {
      output_image.allocate_texture(domain);
    }
    if (output_chosen_pixel.should_compute()) {
      output_chosen_pixel.allocate_texture(domain);
    }
    if (output_chosen_mask_value.should_compute()) {
      output_chosen_mask_value.allocate_texture(domain);
    }

    if (this->context().use_gpu()) {
      this->execute_gpu(domain, output_image, output_chosen_pixel, output_chosen_mask_value);
    }
    else {
      this->execute_cpu(domain, output_image, output_chosen_pixel, output_chosen_mask_value);
    }
  }

  void execute_gpu(const Domain domain,
                   Result &output_image,
                   Result &output_chosen_pixel,
                   Result &output_chosen_mask_value)
  {
    gpu::Shader *shader = context().get_shader("compositor_masked_maximum");
    GPU_shader_bind(shader);

    GPU_shader_uniform_1b(shader, "output_image_should_compute", output_image.should_compute());
    GPU_shader_uniform_1b(
        shader, "output_chosen_pixel_should_compute", output_chosen_pixel.should_compute());
    GPU_shader_uniform_1b(shader,
                          "output_chosen_mask_value_should_compute",
                          output_chosen_mask_value.should_compute());

    GPU_shader_uniform_2iv(shader, "domain_data_size", domain.data_size);
    GPU_shader_uniform_1b(
        shader, "keep_seamless", get_input("Keep Seamless").get_single_value_default<bool>());

    const Result &input_image = get_input("Image");
    input_image.bind_as_texture(shader, "input_image_tx");

    const Result &input_constant_part_size = get_input("Constant Part Size");
    input_constant_part_size.bind_as_texture(shader, "input_constant_part_size_tx");

    const Result &input_roundness = get_input("Roundness");
    input_roundness.bind_as_texture(shader, "input_roundness_tx");

    const Result &input_size_boundary = get_input("Size Boundary");
    input_size_boundary.bind_as_texture(shader, "input_size_boundary_tx");

    const Result &input_value_boundary = get_input("Value Boundary");
    input_value_boundary.bind_as_texture(shader, "input_value_boundary_tx");

    const Result &input_aggressiveness = get_input("Aggressiveness");
    input_aggressiveness.bind_as_texture(shader, "input_aggressiveness_tx");

    const Result &input_ellipse_height = get_input("Ellipse Height");
    input_ellipse_height.bind_as_texture(shader, "input_ellipse_height_tx");

    const Result &input_ellipse_width = get_input("Ellipse Width");
    input_ellipse_width.bind_as_texture(shader, "input_ellipse_width_tx");

    const Result &input_inflection_midpoint = get_input("Inflection Midpoint");
    input_inflection_midpoint.bind_as_texture(shader, "input_inflection_midpoint_tx");

    const Result &input_rotation = get_input("Rotation");
    input_rotation.bind_as_texture(shader, "input_rotation_tx");

    const Result &input_translation = get_input("Translation");
    input_translation.bind_as_texture(shader, "input_translation_tx");

    if (output_image.should_compute()) {
      output_image.bind_as_image(shader, "output_image_img");
    }

    if (output_chosen_pixel.should_compute()) {
      output_chosen_pixel.bind_as_image(shader, "output_chosen_pixel_img");
    }

    if (output_chosen_mask_value.should_compute()) {
      output_chosen_mask_value.bind_as_image(shader, "output_chosen_mask_value_img");
    }

    compute_dispatch_threads_at_least(shader, domain.data_size);

    GPU_shader_unbind();
    input_image.unbind_as_texture();
    input_constant_part_size.unbind_as_texture();
    input_roundness.unbind_as_texture();
    input_size_boundary.unbind_as_texture();
    input_value_boundary.unbind_as_texture();
    input_aggressiveness.unbind_as_texture();
    input_ellipse_height.unbind_as_texture();
    input_ellipse_width.unbind_as_texture();
    input_inflection_midpoint.unbind_as_texture();
    input_rotation.unbind_as_texture();
    input_translation.unbind_as_texture();
    if (output_image.should_compute()) {
      output_image.unbind_as_image();
    }
    if (output_chosen_pixel.should_compute()) {
      output_chosen_pixel.unbind_as_image();
    }
    if (output_chosen_mask_value.should_compute()) {
      output_chosen_mask_value.unbind_as_image();
    }
  }

  void execute_cpu(const Domain domain,
                   Result &output_image,
                   Result &output_chosen_pixel,
                   Result &output_chosen_mask_value)
  {
    const bool keep_seamless = get_input("Keep Seamless").get_single_value_default<bool>();
    const Result &input_image = get_input("Image");
    const Result &input_constant_part_size = get_input("Constant Part Size");
    const Result &input_roundness = get_input("Roundness");
    const Result &input_size_boundary = get_input("Size Boundary");
    const Result &input_value_boundary = get_input("Value Boundary");
    const Result &input_aggressiveness = get_input("Aggressiveness");
    const Result &input_ellipse_height = get_input("Ellipse Height");
    const Result &input_ellipse_width = get_input("Ellipse Width");
    const Result &input_inflection_midpoint = get_input("Inflection Midpoint");
    const Result &input_rotation = get_input("Rotation");
    const Result &input_translation = get_input("Translation");

    parallel_for(domain.data_size, [&](const int2 texel) {
      float2 constant_part_size = input_constant_part_size.load_pixel_zero<float2, true>(texel);
      float domain_diagonal_length = math::sqrt(math::square(float(domain.data_size.x)) +
                                                math::square(float(domain.data_size.y)));
      /* In principle, absolute constant_part_size values greater than domain_diagonal_length can
       * still result in different outputs, however, to prevent extremely long computation times,
       * they are clamped.
       */
      constant_part_size = math::clamp(constant_part_size,
                                       float2(-math::ceil(domain_diagonal_length)),
                                       float2(math::ceil(domain_diagonal_length)));
      float2 abs_constant_part_size = math::abs(constant_part_size);
      float roundness = math::clamp(
          input_roundness.load_pixel_zero<float, true>(texel), 0.0f, 1.0f);
      /* In principle, size_boundary values greater than domain_diagonal_length can still result in
       * different outputs, however, to prevent extremely long computation times, they are clamped.
       */
      float size_boundary = math::clamp(input_size_boundary.load_pixel_zero<float, true>(texel),
                                        0.0f,
                                        math::ceil(domain_diagonal_length));
      float value_boundary = input_value_boundary.load_pixel_zero<float, true>(texel);
      float aggressiveness = math::clamp(
          input_aggressiveness.load_pixel_zero<float, true>(texel), 0.0f, 1.0f);
      float ellipse_height = math::clamp(
          input_ellipse_height.load_pixel_zero<float, true>(texel), 0.0f, 1.0f);
      float ellipse_width = math::clamp(
          input_ellipse_width.load_pixel_zero<float, true>(texel), 0.0f, 1.0f);
      float inflection_midpoint = math::clamp(
          input_inflection_midpoint.load_pixel_zero<float, true>(texel), 0.0f, 1.0f);
      float rotation = input_rotation.load_pixel_zero<float, true>(texel);
      float2 translation = input_translation.load_pixel_zero<float2, true>(texel);

      /* Calculate the top right and bottom left corners of the bounding box of the rounded square
       * mask. */
      float2 bounding_box_top_right_corner_relative_to_pixel;
      if (abs_constant_part_size.x == abs_constant_part_size.y) {
        bounding_box_top_right_corner_relative_to_pixel = float2(
            math::ceil(abs_constant_part_size.x + size_boundary),
            math::ceil(abs_constant_part_size.y + size_boundary));
      }
      else if (abs_constant_part_size.x == 0.0f) {
        bounding_box_top_right_corner_relative_to_pixel = float2(
            0.0f, math::ceil(abs_constant_part_size.y + size_boundary));
      }
      else if (abs_constant_part_size.y == 0.0f) {
        bounding_box_top_right_corner_relative_to_pixel = float2(
            math::ceil(abs_constant_part_size.x + size_boundary), 0.0f);
      }
      else {
        bounding_box_top_right_corner_relative_to_pixel = float2(
            math::ceil(abs_constant_part_size.x +
                       (size_boundary *
                        math::min(abs_constant_part_size.x / abs_constant_part_size.y, 1.0f))),
            math::ceil(abs_constant_part_size.y +
                       (size_boundary *
                        math::min(abs_constant_part_size.y / abs_constant_part_size.x, 1.0f))));
      }
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
            math::max(math::ceil(math::abs(rotated_top_right_corner.x)),
                      math::ceil(math::abs(rotated_bottom_right_corner.x))),
            math::max(math::ceil(math::abs(rotated_top_right_corner.y)),
                      math::ceil(math::abs(rotated_bottom_right_corner.y))));
      }
      float2 bounding_box_bottom_left_corner_relative_to_pixel =
          -bounding_box_top_right_corner_relative_to_pixel;
      /* Translate bounding box. */
      bounding_box_top_right_corner_relative_to_pixel = math::ceil(
          bounding_box_top_right_corner_relative_to_pixel + translation);
      bounding_box_bottom_left_corner_relative_to_pixel = math::floor(
          bounding_box_bottom_left_corner_relative_to_pixel + translation);

      int2 bounding_box_top_right_corner = texel +
                                           int2(bounding_box_top_right_corner_relative_to_pixel);
      int2 bounding_box_bottom_left_corner =
          texel + int2(bounding_box_bottom_left_corner_relative_to_pixel);
      if (!keep_seamless) {
        /* Crop away parts of the bounding box that are outside the domain. */
        bounding_box_top_right_corner = math::min(bounding_box_top_right_corner,
                                                  domain.data_size - int2(1, 1));
        bounding_box_bottom_left_corner = math::max(bounding_box_bottom_left_corner, int2(0, 0));
      }

      /* Initialize the output variables by evaluating the pixel that is outside the bounding box
       * and closest to the pixel that the operation is evaluated on. Therefore, if the pixel that
       * the operation is evaluated on is outside the bounding box, it is used to initialize the
       * output variables. */
      float masked_maximum = value_boundary;
      float2 chosen_pixel_coordinates = float2(texel);
      if ((texel.x >= bounding_box_bottom_left_corner.x) &&
          (texel.y >= bounding_box_bottom_left_corner.y) &&
          (texel.x <= bounding_box_top_right_corner.x) &&
          (texel.y <= bounding_box_top_right_corner.y))
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
          if ((bounding_box_top_right_corner.x <= (domain.data_size.x - 2)) &&
              (smallest_distance > (bounding_box_top_right_corner.x + 1 - texel.x)))
          {
            chosen_pixel_coordinates = float2(bounding_box_top_right_corner.x + 1, texel.y);
            smallest_distance = bounding_box_top_right_corner.x + 1 - texel.x;
          }

          /* Check above the bounding box. */
          if ((bounding_box_top_right_corner.y <= (domain.data_size.y - 2)) &&
              (smallest_distance > (bounding_box_top_right_corner.y + 1 - texel.y)))
          {
            chosen_pixel_coordinates = float2(texel.x, bounding_box_top_right_corner.y + 1);
          }
        }
      }
      float chosen_mask_value = 0.0f;

      bool is_dilate = (constant_part_size.x >= 0.0f) && (constant_part_size.y >= 0.0f);
      float2 mask_center_coordinates = float2(texel) + float2(translation);
      for (int y = bounding_box_bottom_left_corner.y; y <= bounding_box_top_right_corner.y; y++) {
        for (int x = bounding_box_bottom_left_corner.x; x <= bounding_box_top_right_corner.x; x++)
        {
          float2 pixel_coordinates = float2(x, y);
          float2 pixel_coordinates_relative_to_mask_center = pixel_coordinates -
                                                             mask_center_coordinates;
          if (rotation != 0.0f) {
            pixel_coordinates_relative_to_mask_center = rotate_vector_2d(
                pixel_coordinates_relative_to_mask_center, -rotation);
          }
          float rounded_square_mask = compute_rounded_square_mask(
              pixel_coordinates_relative_to_mask_center,
              abs_constant_part_size,
              roundness,
              size_boundary,
              aggressiveness,
              ellipse_height,
              ellipse_width,
              inflection_midpoint);

          int2 image_sampling_coordinates = int2(
              math::floored_mod(pixel_coordinates, float2(domain.data_size)));
          float iteration_masked_maximum =
              (rounded_square_mask *
               ((is_dilate ? input_image.load_pixel_zero<float, true>(image_sampling_coordinates) :
                             (1.0f - input_image.load_pixel_zero<float, true>(
                                         image_sampling_coordinates))) -
                value_boundary)) +
              value_boundary;

          if ((iteration_masked_maximum > masked_maximum) ||
              ((iteration_masked_maximum == masked_maximum) &&
               (math::dot(pixel_coordinates - float2(texel), pixel_coordinates - float2(texel)) <
                math::dot(chosen_pixel_coordinates - float2(texel),
                          chosen_pixel_coordinates - float2(texel)))))
          {
            chosen_mask_value = rounded_square_mask;
            chosen_pixel_coordinates = pixel_coordinates;
            masked_maximum = iteration_masked_maximum;
          }
        }
      }

      if (output_chosen_mask_value.should_compute()) {
        output_chosen_mask_value.store_pixel(texel, chosen_mask_value);
      }
      if (output_chosen_pixel.should_compute()) {
        /* Output the pixel coordinates that are inside the domain. */
        chosen_pixel_coordinates = math::floored_mod(chosen_pixel_coordinates,
                                                     float2(domain.data_size));
        output_chosen_pixel.store_pixel(texel, chosen_pixel_coordinates);
      }
      if (output_image.should_compute()) {
        output_image.store_pixel(texel, is_dilate ? masked_maximum : (1.0f - masked_maximum));
      }
    });
  }

  float2 rotate_vector_2d(float2 vector, float angle)
  {
    return float2(vector.x * cos(angle) - vector.y * sin(angle),
                  vector.x * sin(angle) + vector.y * cos(angle));
  }

  float elliptical_ramp_without_constant_part(float value,
                                              float ellipse_height,
                                              float ellipse_width)
  {
    if (value < ellipse_width + ellipse_height * (1.0f - ellipse_width)) {
      return (ellipse_height *
              (value * ellipse_height * (1.0f - ellipse_width) + math::square(ellipse_width) -
               ellipse_width *
                   math::sqrt(math::square(ellipse_width) - math::square(value) +
                              2.0f * value * ellipse_height * (1.0f - ellipse_width)))) /
             (math::square(ellipse_height * (1.0f - ellipse_width)) + math::square(ellipse_width));
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
      return math::square(coord.x) + math::square(coord.y) <= 1.0f;
    }

    /* Remap coord into first octand. This can be done because the rounded square mask is symmetric
     * to both the X and Y axes. */
    coord = math::abs(coord);
    coord = float2(math::max(coord.x, coord.y), math::min(coord.x, coord.y));

    if (roundness == 0.0f) {
      return coord.x <= 1.0f;
    }

    return ((coord.x <= 1.0f) && (coord.y <= (1.0f - roundness))) ||
           (math::square(coord.x - 1.0f + roundness) + math::square(coord.y - 1.0f + roundness) <=
            math::square(roundness));
  }

  float compute_rounded_square_radius(float2 coord, const float roundness)
  {
    float l_coord = math::sqrt(math::square(coord.x) + math::square(coord.y));

    if (roundness == 1.0f) {
      return l_coord;
    }

    /* Remap coord into first octand. This can be done because the rounded square mask is symmetric
     * to both the X and Y axes. */
    coord = math::abs(coord);
    coord = float2(math::max(coord.x, coord.y), math::min(coord.x, coord.y));

    if (roundness == 0.0f) {
      return coord.x;
    }

    float angle_bisector_A_coord = math::atan(coord.y / coord.x);
    float angle_bisector_A_bevel_start = math::atan(1.0f - roundness);
    if (angle_bisector_A_coord > angle_bisector_A_bevel_start) {
      /* Regular rounded part. */
      float coord_A_segment_divider = M_PI_4 - angle_bisector_A_coord;
      float l_circle_center = M_SQRT2 * (1.0f - roundness);
      float l_coord_R_l_bevel_start =
          math::cos(coord_A_segment_divider) * l_circle_center +
          math::sqrt(math::square(math::cos(coord_A_segment_divider) * l_circle_center) +
                     math::square(roundness) - math::square(l_circle_center));

      return l_coord / l_coord_R_l_bevel_start;
    }
    else {
      /* Regular straight part. */
      return l_coord * math::cos(angle_bisector_A_coord);
    }
  }

  float compute_rounded_square_mask(float2 coord,
                                    float2 abs_constant_part_size,
                                    const float roundness,
                                    const float size_boundary,
                                    const float aggressiveness,
                                    const float ellipse_height,
                                    const float ellipse_width,
                                    const float inflection_midpoint)
  {
    /* Swap x and y names if abs_constant_part_size.y > abs_constant_part_size.x. This is done
     * because the following code expects abs_constant_part_size.x to be greater or equal to
     * abs_constant_part_size.y. This makes sure that the falloff is calculated based on the larger
     * abs_constant_part_size, making the Width input an upper limit to the falloff width. */
    if (abs_constant_part_size.y > abs_constant_part_size.x) {
      std::swap(coord.x, coord.y);
      std::swap(abs_constant_part_size.x, abs_constant_part_size.y);
    }

    if (abs_constant_part_size.y == 0.0f) {
      if (abs_constant_part_size.x == 0.0f) {
        if ((coord.x == 0.0f) && (coord.y == 0.0f)) {
          /* coord is in the constant part of the mask. */
          return 1.0f;
        }
        else if ((size_boundary == 0.0f) ||
                 (!is_in_unit_rounded_square(coord / (float2(size_boundary, size_boundary)),
                                             roundness)))
        {
          /* coord is outside of the mask. */
          return 0.0f;
        }
        else {
          /* coord is in the falloff part of the mask. */
          return math::interpolate(
              1.0f,
              aggressiveness,
              elliptical_unit_step_without_constant_part(
                  math::inverse_mix(
                      0.0f, size_boundary, compute_rounded_square_radius(coord, roundness)),
                  ellipse_height,
                  ellipse_width,
                  inflection_midpoint));
        }
      }
      else {
        /* Mask is a 1 dimensional line. */
        if ((coord.y != 0.0f) || (math::abs(coord.x) > (abs_constant_part_size.x + size_boundary)))
        {
          /* coord is outside of the mask. */
          return 0.0f;
        }
        else if (math::abs(coord.x) <= (abs_constant_part_size.x)) {
          /* coord is in the constant part of the mask. */
          return 1.0f;
        }
        else {
          /* coord is in the falloff part of the mask. */
          return math::interpolate(1.0f,
                                   aggressiveness,
                                   elliptical_unit_step_without_constant_part(
                                       math::inverse_mix(abs_constant_part_size.x,
                                                         abs_constant_part_size.x + size_boundary,
                                                         math::abs(coord.x)),
                                       ellipse_height,
                                       ellipse_width,
                                       inflection_midpoint));
        }
      }
    }
    else {
      if (is_in_unit_rounded_square(coord / abs_constant_part_size, roundness)) {
        /* coord is in the constant part of the mask. */
        return 1.0f;
      }
      else if ((size_boundary == 0.0f) ||
               !is_in_unit_rounded_square(
                   coord /
                       (abs_constant_part_size + float2(size_boundary,
                                                        size_boundary * abs_constant_part_size.y /
                                                            abs_constant_part_size.x)),
                   roundness))
      {
        /* coord is outside of the mask. */
        return 0.0f;
      }
      else {
        /* coord is in the falloff part of the mask. */
        return math::interpolate(
            1.0f,
            aggressiveness,
            elliptical_unit_step_without_constant_part(
                math::inverse_mix(
                    abs_constant_part_size.x,
                    abs_constant_part_size.x + size_boundary,
                    compute_rounded_square_radius(
                        float2(coord.x,
                               coord.y * abs_constant_part_size.x / abs_constant_part_size.y),
                        roundness)),
                ellipse_height,
                ellipse_width,
                inflection_midpoint));
      }
    }
  }
};

static NodeOperation *get_compositor_operation(Context &context, DNode node)
{
  return new MaskedMaximumOperation(context, node);
}

}  // namespace blender::nodes::node_composite_masked_maximum_cc

static void register_node_type_cmp_masked_maximum()
{
  namespace file_ns = blender::nodes::node_composite_masked_maximum_cc;

  static blender::bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeMaskedMaximum");
  ntype.ui_name = "Masked Maximum";
  ntype.ui_description = "Masked Maximum";
  ntype.nclass = NODE_CLASS_MATTE;
  ntype.declare = file_ns::cmp_node_masked_maximum_declare;
  ntype.flag |= NODE_PREVIEW;
  ntype.get_compositor_operation = file_ns::get_compositor_operation;

  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(register_node_type_cmp_masked_maximum)
