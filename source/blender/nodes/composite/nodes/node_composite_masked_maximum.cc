/* SPDX-FileCopyrightText: 2026 Blender Authors
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

#include "RNA_enum_types.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"

/* **************** Masked Maximum ******************** */

namespace blender::nodes::node_composite_masked_maximum_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_input<decl::Float>("Image"_ustr)
      .default_value(0.5f)
      .hide_value()
      .compositor_domain_priority(0)
      .description("The input image")
      .structure_type(StructureType::Dynamic);
  b.add_output<decl::Float>("Image"_ustr)
      .description("The output image")
      .structure_type(StructureType::Dynamic)
      .align_with_previous();
  b.add_output<decl::Vector>("Chosen Image Pixel"_ustr)
      .dimensions(2)
      .description(
          "The normalized coordinates of the pixel of the input image that was chosen during the "
          "operation")
      .structure_type(StructureType::Dynamic);
  b.add_output<decl::Vector>("Chosen Mask Pixel"_ustr)
      .dimensions(2)
      .description(
          "The normalized coordinates of the mask at the pixel of the input image that was chosen "
          "during the operation")
      .structure_type(StructureType::Dynamic);

  b.add_input<decl::Float>("Mask"_ustr)
      .default_value(1.0f)
      .hide_value()
      .compositor_domain_priority(1)
      .description("The input mask")
      .structure_type(StructureType::Dynamic);
  b.add_input<decl::Vector>("Mask Size"_ustr)
      .dimensions(2)
      .default_value({0.0f, 0.0f})
      .compositor_domain_priority(2)
      .description(
          "Size from the center of the mask to its boundaries. If Mask Size is negative in "
          "any dimension, the operation is performed on the inverted input image and the result "
          "of that operation is also inverted")
      .structure_type(StructureType::Dynamic);

  PanelDeclarationBuilder &mask_transform_panel =
      b.add_panel("Mask Transform"_ustr).default_closed(true);
  mask_transform_panel.add_input<decl::Float>("Rotation"_ustr)
      .default_value(0.0f)
      .subtype(PROP_ANGLE)
      .compositor_domain_priority(3)
      .description("Angle to rotate the mask by")
      .structure_type(StructureType::Dynamic);
  mask_transform_panel.add_input<decl::Vector>("Translation"_ustr)
      .dimensions(2)
      .default_value({0.0f, 0.0f})
      .compositor_domain_priority(4)
      .description("Translation of the mask")
      .structure_type(StructureType::Dynamic);

  PanelDeclarationBuilder &mask_modification_panel =
      b.add_panel("Mask Modification"_ustr).default_closed(true);
  mask_modification_panel.add_input<decl::Float>("Rounding"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .compositor_domain_priority(5)
      .description(
          "Rounding of the mask. Increasing this value makes the mask rounder by cutting off the "
          "corners of the input mask. A value of 0 results in the entire input mask being used "
          "while a value of 1 results in an circular cutout of the input mask being used")
      .structure_type(StructureType::Dynamic);

  PanelDeclarationBuilder &falloff_panel =
      mask_modification_panel.add_panel("Falloff"_ustr).default_closed(true);
  falloff_panel.add_input<decl::Float>("Hardness"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .compositor_domain_priority(6)
      .description("How close the mask falloff starts from the edge of the mask")
      .structure_type(StructureType::Dynamic);

  PanelDeclarationBuilder &falloff_shape_panel =
      falloff_panel.add_panel("Falloff Shape"_ustr).default_closed(true);
  falloff_shape_panel.add_input<decl::Float>("Ellipse Height"_ustr)
      .min(0.0f)
      .max(1.0f)
      .default_value(0.5f)
      .subtype(PROP_FACTOR)
      .compositor_domain_priority(7)
      .description(
          "Height of the elliptical segments of the elliptical step function, which is used to "
          "control the shape of the falloff. A higher value results in a smoother falloff.")
      .structure_type(StructureType::Dynamic);
  falloff_shape_panel.add_input<decl::Float>("Ellipse Width"_ustr)
      .min(0.0f)
      .max(1.0f)
      .default_value(0.5f)
      .subtype(PROP_FACTOR)
      .compositor_domain_priority(8)
      .description(
          "Width of the elliptical segments of the elliptical step function, which is used to "
          "control the shape of the falloff. A higher value results in a rounder falloff")
      .structure_type(StructureType::Dynamic);
  falloff_shape_panel.add_input<decl::Float>("Inflection Midpoint"_ustr)
      .min(0.0f)
      .max(1.0f)
      .default_value(0.5f)
      .subtype(PROP_FACTOR)
      .compositor_domain_priority(9)
      .description(
          "Position of the inflection midpoint of the elliptical step function, which is used to "
          "control the shape of the falloff. It controls how big the two elliptical segments are "
          "relative to each other")
      .structure_type(StructureType::Dynamic);

  PanelDeclarationBuilder &operation_properties_panel =
      b.add_panel("Operation Properties"_ustr).default_closed(true);
  operation_properties_panel.add_input<decl::Float>("Value Boundary"_ustr)
      .default_value(0.0f)
      .compositor_domain_priority(10)
      .description(
          "Value by which the values of the input image are offset before they are multiplied by "
          "the values of the mask. If Mask Size is negative in any dimension, 1 - Value Boundary "
          "is an upper boundary to the possible output image values, otherwise Value Boundary is "
          "a lower boundary to the possible output image values")
      .structure_type(StructureType::Dynamic);
  operation_properties_panel.add_input<decl::Menu>("Mask Interpolation"_ustr)
      .default_value(CMP_NODE_INTERPOLATION_BILINEAR)
      .static_items(rna_enum_node_compositor_interpolation_items)
      .optional_label()
      .description("Interpolation method of the input mask");
  operation_properties_panel.add_input<decl::Bool>("Keep Seamless"_ustr)
      .default_value(false)
      .description(
          "When enabled, the operation keeps the output image seamless for a seamless input "
          "image.");
}

using namespace blender::compositor;

class MaskedMaximumOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    Result &output_image = this->get_result("Image");
    Result &output_chosen_image_pixel = this->get_result("Chosen Image Pixel");
    Result &output_chosen_mask_pixel = this->get_result("Chosen Mask Pixel");

    const Domain domain = compute_domain();
    if (output_image.should_compute()) {
      output_image.allocate_texture(domain);
    }
    if (output_chosen_image_pixel.should_compute()) {
      output_chosen_image_pixel.allocate_texture(domain);
    }
    if (output_chosen_mask_pixel.should_compute()) {
      output_chosen_mask_pixel.allocate_texture(domain);
    }

    if (this->context().use_gpu()) {
      this->execute_gpu(domain, output_image, output_chosen_image_pixel, output_chosen_mask_pixel);
    }
    else {
      this->execute_cpu(domain, output_image, output_chosen_image_pixel, output_chosen_mask_pixel);
    }
  }

  void execute_gpu(const Domain domain,
                   Result &output_image,
                   Result &output_chosen_image_pixel,
                   Result &output_chosen_mask_pixel)
  {
    const Interpolation mask_interpolation = this->get_mask_interpolation();
    gpu::Shader *shader = context().get_shader(this->get_shader_name(mask_interpolation));
    GPU_shader_bind(shader);

    GPU_shader_uniform_1b(shader, "output_image_should_compute", output_image.should_compute());
    GPU_shader_uniform_1b(shader,
                          "output_chosen_image_pixel_should_compute",
                          output_chosen_image_pixel.should_compute());
    GPU_shader_uniform_1b(shader,
                          "output_chosen_mask_pixel_should_compute",
                          output_chosen_mask_pixel.should_compute());

    GPU_shader_uniform_2iv(shader, "domain_data_size", domain.data_size);

    const Result &input_mask = get_input("Mask");
    GPU_shader_uniform_2iv(shader, "input_mask_domain_data_size", input_mask.domain().data_size);

    GPU_shader_uniform_1b(
        shader, "keep_seamless", get_input("Keep Seamless").get_single_value_default<bool>());

    const Result &input_image = get_input("Image");
    input_image.bind_as_texture(shader, "input_image_tx");

    GPU_texture_filter_mode(input_mask, mask_interpolation != Interpolation::Nearest);
    /* Extension mode is set to Extend to ensure that in the case where all pixels of the input
     * mask have the same value, it behaves the same as a single value input with that value. */
    /* This also covers the case where the input_mask is actually a single value, as single values
     * are currently treated the same as 1x1 textures.
     * TODO: Properly handle single values once methods become available. */
    GPU_texture_extend_mode_x(input_mask, map_extension_mode_to_extend_mode(Extension::Extend));
    GPU_texture_extend_mode_y(input_mask, map_extension_mode_to_extend_mode(Extension::Extend));
    input_mask.bind_as_texture(shader, "input_mask_tx");

    const Result &input_mask_size = get_input("Mask Size");
    input_mask_size.bind_as_texture(shader, "input_mask_size_tx");

    const Result &input_rotation = get_input("Rotation");
    input_rotation.bind_as_texture(shader, "input_rotation_tx");

    const Result &input_translation = get_input("Translation");
    input_translation.bind_as_texture(shader, "input_translation_tx");

    const Result &input_rounding = get_input("Rounding");
    input_rounding.bind_as_texture(shader, "input_rounding_tx");

    const Result &input_hardness = get_input("Hardness");
    input_hardness.bind_as_texture(shader, "input_hardness_tx");

    const Result &input_ellipse_height = get_input("Ellipse Height");
    input_ellipse_height.bind_as_texture(shader, "input_ellipse_height_tx");

    const Result &input_ellipse_width = get_input("Ellipse Width");
    input_ellipse_width.bind_as_texture(shader, "input_ellipse_width_tx");

    const Result &input_inflection_midpoint = get_input("Inflection Midpoint");
    input_inflection_midpoint.bind_as_texture(shader, "input_inflection_midpoint_tx");

    const Result &input_value_boundary = get_input("Value Boundary");
    input_value_boundary.bind_as_texture(shader, "input_value_boundary_tx");

    if (output_image.should_compute()) {
      output_image.bind_as_image(shader, "output_image_img");
    }

    if (output_chosen_image_pixel.should_compute()) {
      output_chosen_image_pixel.bind_as_image(shader, "output_chosen_image_pixel_img");
    }

    if (output_chosen_mask_pixel.should_compute()) {
      output_chosen_mask_pixel.bind_as_image(shader, "output_chosen_mask_pixel_img");
    }

    compute_dispatch_threads_at_least(shader, domain.data_size);

    GPU_shader_unbind();
    input_image.unbind_as_texture();
    input_mask.unbind_as_texture();
    input_mask_size.unbind_as_texture();
    input_rotation.unbind_as_texture();
    input_translation.unbind_as_texture();
    input_rounding.unbind_as_texture();
    input_hardness.unbind_as_texture();
    input_ellipse_height.unbind_as_texture();
    input_ellipse_width.unbind_as_texture();
    input_inflection_midpoint.unbind_as_texture();
    input_value_boundary.unbind_as_texture();
    if (output_image.should_compute()) {
      output_image.unbind_as_image();
    }
    if (output_chosen_image_pixel.should_compute()) {
      output_chosen_image_pixel.unbind_as_image();
    }
    if (output_chosen_mask_pixel.should_compute()) {
      output_chosen_mask_pixel.unbind_as_image();
    }
  }

  char const *get_shader_name(const Interpolation &mask_interpolation)
  {
    switch (mask_interpolation) {
      case Interpolation::Anisotropic:
      case Interpolation::Bicubic:
        return "compositor_masked_maximum_bicubic";
      case Interpolation::Bilinear:
      case Interpolation::Nearest:
        return "compositor_masked_maximum";
    }

    return "compositor_masked_maximum";
  }

  Interpolation get_mask_interpolation()
  {
    const CMPNodeInterpolation mask_interpolation = CMPNodeInterpolation(
        this->get_input("Mask Interpolation").get_single_value_default<MenuValue>().value);
    switch (mask_interpolation) {
      case CMP_NODE_INTERPOLATION_NEAREST:
        return Interpolation::Nearest;
      case CMP_NODE_INTERPOLATION_BILINEAR:
        return Interpolation::Bilinear;
      case CMP_NODE_INTERPOLATION_ANISOTROPIC:
      case CMP_NODE_INTERPOLATION_BICUBIC:
        return Interpolation::Bicubic;
    }

    return Interpolation::Nearest;
  }

  void execute_cpu(const Domain domain,
                   Result &output_image,
                   Result &output_chosen_image_pixel,
                   Result &output_chosen_mask_pixel)
  {
    const Interpolation mask_interpolation = this->get_mask_interpolation();
    const bool keep_seamless = get_input("Keep Seamless").get_single_value_default<bool>();

    const Result &input_image = get_input("Image");
    const Result &input_mask = get_input("Mask");
    const Result &input_mask_size = get_input("Mask Size");
    const Result &input_rotation = get_input("Rotation");
    const Result &input_translation = get_input("Translation");
    const Result &input_rounding = get_input("Rounding");
    const Result &input_hardness = get_input("Hardness");
    const Result &input_ellipse_height = get_input("Ellipse Height");
    const Result &input_ellipse_width = get_input("Ellipse Width");
    const Result &input_inflection_midpoint = get_input("Inflection Midpoint");
    const Result &input_value_boundary = get_input("Value Boundary");

    parallel_for(domain.data_size, [&](const int2 texel) {
      float2 mask_size = input_mask_size.load_pixel_zero<float2, true>(texel);
      float domain_diagonal_length = math::sqrt(math::square(float(domain.data_size.x)) +
                                                math::square(float(domain.data_size.y)));
      /* In principle, absolute mask_size values greater than domain_diagonal_length can
       * still result in different outputs, however, to prevent extremely long computation times,
       * they are clamped.
       */
      mask_size = math::clamp(mask_size,
                              float2(-math::ceil(domain_diagonal_length)),
                              float2(math::ceil(domain_diagonal_length)));
      float2 abs_mask_size = math::abs(mask_size);
      float rotation = input_rotation.load_pixel_zero<float, true>(texel);
      float2 translation = input_translation.load_pixel_zero<float2, true>(texel);
      float rounding = math::clamp(input_rounding.load_pixel_zero<float, true>(texel), 0.0f, 1.0f);
      float hardness = math::clamp(input_hardness.load_pixel_zero<float, true>(texel), 0.0f, 1.0f);
      float ellipse_height = math::clamp(
          input_ellipse_height.load_pixel_zero<float, true>(texel), 0.0f, 1.0f);
      float ellipse_width = math::clamp(
          input_ellipse_width.load_pixel_zero<float, true>(texel), 0.0f, 1.0f);
      float inflection_midpoint = math::clamp(
          input_inflection_midpoint.load_pixel_zero<float, true>(texel), 0.0f, 1.0f);
      float value_boundary = input_value_boundary.load_pixel_zero<float, true>(texel);

      /* Calculate the top right and bottom left corners of the bounding box of the rounded square
       * mask. */
      float2 bounding_box_top_right_corner_relative_to_pixel = math::ceil(abs_mask_size);
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

      /* The output variables are initialized with the pixel that is outside the bounding box and
       * closest to the pixel that the operation is evaluated on. It follows that if the pixel that
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
      float2 mask_center_coordinates = float2(texel) + float2(translation);
      float2 chosen_mask_pixel = compute_normalized_mask_coordinates(
          rotate_vector_2d(chosen_pixel_coordinates - mask_center_coordinates, -rotation),
          abs_mask_size,
          input_mask.domain().data_size);

      bool is_dilate = (mask_size.x >= 0.0f) && (mask_size.y >= 0.0f);
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
          float2 normalized_mask_coordinates = compute_normalized_mask_coordinates(
              pixel_coordinates_relative_to_mask_center,
              abs_mask_size,
              input_mask.domain().data_size);
          float mask_value = compute_rounded_square_mask(pixel_coordinates_relative_to_mask_center,
                                                         abs_mask_size,
                                                         rounding,
                                                         hardness,
                                                         ellipse_height,
                                                         ellipse_width,
                                                         inflection_midpoint) *
                             input_mask.sample<float, true>(
                                 normalized_mask_coordinates,
                                 mask_interpolation,
                                 /* Extension mode is set to Extend to ensure that in the case
                                    where all pixels of the input mask have the same value, it
                                    behaves the same as a single value input with that value. */
                                 Extension::Extend,
                                 Extension::Extend);

          int2 image_sampling_coordinates = int2(
              math::floored_mod(pixel_coordinates, float2(domain.data_size)));
          float iteration_masked_maximum =
              (mask_value *
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
            chosen_mask_pixel = normalized_mask_coordinates;
            chosen_pixel_coordinates = pixel_coordinates;
            masked_maximum = iteration_masked_maximum;
          }
        }
      }

      if (output_chosen_mask_pixel.should_compute()) {
        output_chosen_mask_pixel.store_pixel(texel, chosen_mask_pixel);
      }
      if (output_chosen_image_pixel.should_compute()) {
        /* Output the pixel coordinates that are inside the domain. */
        chosen_pixel_coordinates = math::floored_mod(chosen_pixel_coordinates,
                                                     float2(domain.data_size));
        /* Add float2(0.5f, 0.5f) to chosen_pixel_coordinates to align with pixel centers. */
        float2 chosen_image_pixel = (chosen_pixel_coordinates + float2(0.5f, 0.5f)) /
                                    float2(domain.data_size);
        output_chosen_image_pixel.store_pixel(texel, chosen_image_pixel);
      }
      if (output_image.should_compute()) {
        output_image.store_pixel(texel, is_dilate ? masked_maximum : (1.0f - masked_maximum));
      }
    });
  }

  float2 compute_normalized_mask_coordinates(float2 pixel_coordinates_relative_to_mask_center,
                                             float2 abs_mask_size,
                                             int2 input_mask_data_size)
  {
    float2 normalized_mask_coordinates = float2(
        (abs_mask_size.x == 0.0f) ?
            0.5f :
            (0.5f * (pixel_coordinates_relative_to_mask_center.x / abs_mask_size.x) + 0.5f),
        (abs_mask_size.y == 0.0f) ?
            0.5f :
            (0.5f * (pixel_coordinates_relative_to_mask_center.y / abs_mask_size.y) + 0.5f));
    /* Align normalized_mask_coordinates with pixel centers.
     * For this, normalized_mask_coordinates is remapped from [0, 1] x [0, 1] to
     * [0.5/input_mask_data_size.x, (input_mask_data_size.x-0.5)/input_mask_data_size.x] x
     * [0.5/input_mask_data_size.y, (input_mask_data_size.y-0.5)/input_mask_data_size.y]. */
    return (normalized_mask_coordinates * float2(input_mask_data_size - int2(1, 1)) +
            float2(0.5f, 0.5f)) /
           float2(input_mask_data_size);
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
                                    float2 abs_mask_size,
                                    const float roundness,
                                    const float hardness,
                                    const float ellipse_height,
                                    const float ellipse_width,
                                    const float inflection_midpoint)
  {
    /* Swap x and y names if abs_mask_size.y > abs_mask_size.x. This is done
     * because the following code expects abs_mask_size.x to be greater or equal to
     * abs_mask_size.y. */
    if (abs_mask_size.y > abs_mask_size.x) {
      std::swap(coord.x, coord.y);
      std::swap(abs_mask_size.x, abs_mask_size.y);
    }

    if (abs_mask_size.y == 0.0f) {
      if (abs_mask_size.x == 0.0f) {
        /* Mask is a 0 dimensional point. */
        return ((coord.x == 0.0f) && (coord.y == 0.0f)) ? 1.0f : 0.0f;
      }
      else {
        /* Mask is a 1 dimensional line. */
        if ((coord.y != 0.0f) || (math::abs(coord.x) > abs_mask_size.x)) {
          /* coord is outside of the mask. */
          return 0.0f;
        }
        else if (math::abs(coord.x) <= (hardness * abs_mask_size.x)) {
          /* coord is in the constant part of the mask. */
          return 1.0f;
        }
        else {
          /* coord is in the falloff part of the mask. */
          return elliptical_unit_step_without_constant_part(
              math::inverse_mix(abs_mask_size.x, hardness * abs_mask_size.x, math::abs(coord.x)),
              ellipse_height,
              ellipse_width,
              1.0f - inflection_midpoint);
        }
      }
    }
    else {
      /* Mask is a 2 dimensional rounded square. */
      if (is_in_unit_rounded_square(coord / (hardness * abs_mask_size), roundness)) {
        /* coord is in the constant part of the mask. */
        return 1.0f;
      }
      else if ((hardness == 1.0f) || !is_in_unit_rounded_square(coord / abs_mask_size, roundness))
      {
        /* coord is outside of the mask. */
        return 0.0f;
      }
      else {
        /* coord is in the falloff part of the mask. */
        return elliptical_unit_step_without_constant_part(
            math::inverse_mix(
                abs_mask_size.x,
                hardness * abs_mask_size.x,
                compute_rounded_square_radius(
                    float2(coord.x, coord.y * abs_mask_size.x / abs_mask_size.y), roundness)),
            ellipse_height,
            ellipse_width,
            1.0f - inflection_midpoint);
      }
    }
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new MaskedMaximumOperation(context, node);
}

static void register_node_type_cmp_masked_maximum()
{
  static bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeMaskedMaximum"_ustr);
  ntype.ui_name = "Masked Maximum";
  ntype.ui_description = "Masked Maximum";
  ntype.nclass = NODE_CLASS_MATTE;
  ntype.declare = node_declare;
  ntype.flag |= NODE_PREVIEW;
  ntype.get_compositor_operation = get_compositor_operation;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(register_node_type_cmp_masked_maximum)

}  // namespace blender::nodes::node_composite_masked_maximum_cc
