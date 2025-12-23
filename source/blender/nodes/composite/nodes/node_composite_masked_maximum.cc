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
      .min(0.0f)
      .max(1.0f)
      .compositor_domain_priority(0)
      .structure_type(StructureType::Dynamic);
  b.add_output<decl::Float>("Image").structure_type(StructureType::Dynamic);

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
  falloff_panel.add_input<decl::Float>("Width")
      .default_value(1.0f)
      .min(0.0f)
      .compositor_domain_priority(3)
      .description(
          "Maximal width of the falloff part of the rounded square mask starting at the "
          "boundaries of its constant part")
      .structure_type(StructureType::Dynamic);
  falloff_panel.add_input<decl::Float>("Boundary Value")
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .compositor_domain_priority(4)
      .description("Value at the outer boundary of the falloff part of the rounded square mask")
      .structure_type(StructureType::Dynamic);

  PanelDeclarationBuilder &falloff_shape_panel =
      falloff_panel.add_panel("Falloff Shape").default_closed(true);
  falloff_shape_panel.add_input<decl::Float>("Ellipse Height")
      .min(0.0f)
      .max(1.0f)
      .default_value(0.5f)
      .subtype(PROP_FACTOR)
      .compositor_domain_priority(5)
      .description(
          "Height of the elliptical segments of the elliptical step function, which is used to "
          "control the shape of the falloff. A higher value results in a smoother falloff.");
  falloff_shape_panel.add_input<decl::Float>("Ellipse Width")
      .min(0.0f)
      .max(1.0f)
      .default_value(0.5f)
      .subtype(PROP_FACTOR)
      .compositor_domain_priority(6)
      .description(
          "Width of the elliptical segments of the elliptical step function, which is used to "
          "control the shape of the falloff. A higher value results in a rounder falloff");
  falloff_shape_panel.add_input<decl::Float>("Inflection Midpoint")
      .min(0.0f)
      .max(1.0f)
      .default_value(0.5f)
      .subtype(PROP_FACTOR)
      .compositor_domain_priority(7)
      .description(
          "Position of the inflection midpoint of the elliptical step function, which is used to "
          "control the shape of the falloff. It controls how big the two elliptical segments are "
          "relative to each other");

  PanelDeclarationBuilder &transform_panel = b.add_panel("Mask Transform").default_closed(true);
  transform_panel.add_input<decl::Float>("Rotation")
      .default_value(0.0f)
      .subtype(PROP_ANGLE)
      .compositor_domain_priority(8)
      .description("Angle to rotate the rounded square mask by")
      .structure_type(StructureType::Dynamic);
  transform_panel.add_input<decl::Vector>("Translation")
      .dimensions(2)
      .default_value({0.0f, 0.0f, 0.0f})
      .compositor_domain_priority(9)
      .description("Translation of the rounded square mask")
      .structure_type(StructureType::Dynamic);
}

using namespace blender::compositor;

class MaskedMaximumOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &input_image = this->get_input("Image");
    const Result &input_size = get_input("Constant Part Size");
    const Result &input_falloff_width = get_input("Width");
    const Result &input_translation = get_input("Translation");
    Result &output_image = this->get_result("Image");

    if (input_translation.is_single_value()) {
      if (math::floored_mod(input_translation.get_single_value<float2>(), float2(1.0f, 1.0f)) ==
          float2(0.0f, 0.0f))
      {
        if (input_image.is_single_value()) {
          /* Operation does nothing and the input can be passed through. */
          output_image.share_data(input_image);
          return;
        }
        if (input_size.is_single_value() && input_falloff_width.is_single_value()) {
          float2 abs_input_size = math::abs(input_size.get_single_value<float2>());
          float rounded_square_mask_support_size =
              math::max(abs_input_size.x, abs_input_size.y) +
              math::max(input_falloff_width.get_single_value<float>(), 0.0f);
          if (rounded_square_mask_support_size < 1.0f) {
            /* Operation does nothing and the input can be passed through. */
            output_image.share_data(input_image);
            return;
          }
        }
      }
    }

    if (this->context().use_gpu()) {
      this->execute_gpu(input_image, output_image);
    }
    else {
      this->execute_cpu(input_image, output_image);
    }
  }

  void execute_gpu(const Result &input_image, Result &output_image)
  {
    gpu::Shader *shader = context().get_shader("compositor_masked_maximum");
    GPU_shader_bind(shader);

    const Domain domain = compute_domain();

    GPU_shader_uniform_2iv(shader, "domain_data_size", domain.data_size);
    GPU_shader_uniform_1b(
        shader, "keep_seamless", get_input("Keep Seamless").get_single_value_default<bool>());

    input_image.bind_as_texture(shader, "input_image_tx");

    const Result &input_size = get_input("Constant Part Size");
    input_size.bind_as_texture(shader, "input_size_tx");

    const Result &input_roundness = get_input("Roundness");
    input_roundness.bind_as_texture(shader, "input_roundness_tx");

    const Result &input_falloff_width = get_input("Width");
    input_falloff_width.bind_as_texture(shader, "input_falloff_width_tx");

    const Result &input_falloff_boundary_value = get_input("Boundary Value");
    input_falloff_boundary_value.bind_as_texture(shader, "input_falloff_boundary_value_tx");

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

    output_image.allocate_texture(domain);
    output_image.bind_as_image(shader, "output_image_img");

    compute_dispatch_threads_at_least(shader, domain.data_size);

    GPU_shader_unbind();
    input_image.unbind_as_texture();
    input_size.unbind_as_texture();
    input_roundness.unbind_as_texture();
    input_falloff_width.unbind_as_texture();
    input_falloff_boundary_value.unbind_as_texture();
    input_ellipse_height.unbind_as_texture();
    input_ellipse_width.unbind_as_texture();
    input_inflection_midpoint.unbind_as_texture();
    input_rotation.unbind_as_texture();
    input_translation.unbind_as_texture();
    output_image.unbind_as_image();
  }

  void execute_cpu(const Result &input_image, Result &output_image)
  {
    Domain domain = this->compute_domain();
    output_image.allocate_texture(domain);

    parallel_for(domain.data_size, [&](const int2 texel) {
      float2 size = get_input("Constant Part Size").load_pixel_zero<float2, true>(texel);
      bool is_dilate = (size.x >= 0.0f) && (size.y >= 0.0f);
      float2 abs_size = math::abs(size);
      float roundness = math::clamp(
          get_input("Roundness").load_pixel_zero<float, true>(texel), 0.0f, 1.0f);
      float falloff_width = math::max(get_input("Width").load_pixel_zero<float, true>(texel),
                                      0.0f);
      float falloff_boundary_value = math::clamp(
          get_input("Boundary Value").load_pixel_zero<float, true>(texel), 0.0f, 1.0f);
      float ellipse_height = math::clamp(
          get_input("Ellipse Height").load_pixel_zero<float, true>(texel), 0.0f, 1.0f);
      float ellipse_width = math::clamp(
          get_input("Ellipse Width").load_pixel_zero<float, true>(texel), 0.0f, 1.0f);
      float inflection_midpoint = math::clamp(
          get_input("Inflection Midpoint").load_pixel_zero<float, true>(texel), 0.0f, 1.0f);
      float rotation = get_input("Rotation").load_pixel_zero<float, true>(texel);
      float2 translation = get_input("Translation").load_pixel_zero<float2, true>(texel);

      /* Calculate top right and bottom left corner of the bounding box of the rounded square mask.
       */
      float2 bounding_box_top_right_corner_float;
      if (abs_size.x == abs_size.y) {
        bounding_box_top_right_corner_float = float2(math::ceil(abs_size.x + falloff_width),
                                                     math::ceil(abs_size.y + falloff_width));
      }
      else if (abs_size.x == 0.0f) {
        bounding_box_top_right_corner_float = float2(0.0f, math::ceil(abs_size.y + falloff_width));
      }
      else if (abs_size.y == 0.0f) {
        bounding_box_top_right_corner_float = float2(math::ceil(abs_size.x + falloff_width), 0.0f);
      }
      else {
        bounding_box_top_right_corner_float = float2(
            math::ceil(abs_size.x + (falloff_width * math::min(abs_size.x / abs_size.y, 1.0f))),
            math::ceil(abs_size.y + (falloff_width * math::min(abs_size.y / abs_size.x, 1.0f))));
      }
      if (rotation != 0.0f) {
        float2 rotated_top_right_corner = rotate_vector_2d(
            float2(bounding_box_top_right_corner_float.x, bounding_box_top_right_corner_float.y),
            rotation);
        float2 rotated_bottom_right_corner = rotate_vector_2d(
            float2(bounding_box_top_right_corner_float.x, -bounding_box_top_right_corner_float.y),
            rotation);
        bounding_box_top_right_corner_float = float2(
            math::max(math::ceil(math::abs(rotated_top_right_corner.x)),
                      math::ceil(math::abs(rotated_bottom_right_corner.x))),
            math::max(math::ceil(math::abs(rotated_top_right_corner.y)),
                      math::ceil(math::abs(rotated_bottom_right_corner.y))));
      }
      float2 bounding_box_bottom_left_corner_float = -bounding_box_top_right_corner_float;
      /* Translate bounding box. */
      bounding_box_top_right_corner_float = float2(
          math::ceil(bounding_box_top_right_corner_float.x + translation.x),
          math::ceil(bounding_box_top_right_corner_float.y + translation.y));
      bounding_box_bottom_left_corner_float = float2(
          math::floor(bounding_box_bottom_left_corner_float.x + translation.x),
          math::floor(bounding_box_bottom_left_corner_float.y + translation.y));

      int2 bounding_box_top_right_corner = int2(bounding_box_top_right_corner_float);
      int2 bounding_box_bottom_left_corner = int2(bounding_box_bottom_left_corner_float);
      if (!get_input("Keep Seamless").get_single_value_default<bool>())
      { /* Crop away parts of the bounding box that are outside of the domain. */
        bounding_box_top_right_corner += texel;
        bounding_box_bottom_left_corner += texel;
        bounding_box_top_right_corner = math::min(bounding_box_top_right_corner,
                                                  domain.data_size - int2(1, 1));
        bounding_box_bottom_left_corner = math::max(bounding_box_bottom_left_corner, int2(0, 0));
        bounding_box_top_right_corner -= texel;
        bounding_box_bottom_left_corner -= texel;
      }
      float masked_maximum = -FLT_MAX;
      if (is_dilate) {
        for (int y = bounding_box_bottom_left_corner.y; y <= bounding_box_top_right_corner.y; y++)
        {
          for (int x = bounding_box_bottom_left_corner.x; x <= bounding_box_top_right_corner.x;
               x++)
          {
            float2 coord = float2(x, y) - float2(translation.x, translation.y);
            if (rotation != 0.0f) {
              coord = rotate_vector_2d(coord, -rotation);
            }
            float rounded_square_mask = compute_rounded_square_mask(coord,
                                                                    abs_size,
                                                                    roundness,
                                                                    falloff_width,
                                                                    falloff_boundary_value,
                                                                    ellipse_height,
                                                                    ellipse_width,
                                                                    inflection_midpoint);
            /* Only operate on the support of the rounded square mask. */
            if (rounded_square_mask != 0.0f) {
              masked_maximum = math::max(
                  masked_maximum,
                  rounded_square_mask *
                      input_image.load_pixel_zero<float, true>(int2(math::floored_mod(
                          float2(texel + int2(x, y)), float2(domain.data_size)))));
            }
          }
        }
        output_image.store_pixel(texel, masked_maximum);
      }
      else {
        for (int y = bounding_box_bottom_left_corner.y; y <= bounding_box_top_right_corner.y; y++)
        {
          for (int x = bounding_box_bottom_left_corner.x; x <= bounding_box_top_right_corner.x;
               x++)
          {
            float2 coord = float2(x, y) - float2(translation.x, translation.y);
            if (rotation != 0.0f) {
              coord = rotate_vector_2d(coord, -rotation);
            }
            float rounded_square_mask = compute_rounded_square_mask(coord,
                                                                    abs_size,
                                                                    roundness,
                                                                    falloff_width,
                                                                    falloff_boundary_value,
                                                                    ellipse_height,
                                                                    ellipse_width,
                                                                    inflection_midpoint);
            /* Only operate on the support of the rounded square mask. */
            if (rounded_square_mask != 0.0f) {
              masked_maximum = math::max(
                  masked_maximum,
                  rounded_square_mask *
                      (1.0f - input_image.load_pixel_zero<float, true>(
                                  math::floored_mod(texel + int2(x, y), domain.data_size))));
            }
          }
        }
        output_image.store_pixel(texel, 1.0f - masked_maximum);
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
                                    float2 abs_size,
                                    const float roundness,
                                    const float falloff_width,
                                    const float falloff_boundary_value,
                                    const float ellipse_height,
                                    const float ellipse_width,
                                    const float inflection_midpoint)
  {
    /* Swap x and y names if abs_size.y > abs_size.x. This is done because the following code
     * expects abs_size.x to be greater or equal to abs_size.y. This makes sure that the falloff is
     * calculated based on the larger abs_size, making the Width input an upper limit to the
     * falloff width. */
    if (abs_size.y > abs_size.x) {
      std::swap(coord.x, coord.y);
      std::swap(abs_size.x, abs_size.y);
    }

    if (abs_size.y == 0.0f) {
      if (abs_size.x == 0.0f) {
        if ((coord.x == 0.0f) && (coord.y == 0.0f)) {
          /* coord is in the constant part of the mask. */
          return 1.0f;
        }
        else if ((falloff_width == 0.0f) ||
                 (!is_in_unit_rounded_square(coord / (float2(falloff_width, falloff_width)),
                                             roundness)))
        {
          /* coord is outside of the mask. */
          return 0.0f;
        }
        else {
          /* coord is in the falloff part of the mask. */
          return math::interpolate(
              1.0f,
              falloff_boundary_value,
              elliptical_unit_step_without_constant_part(
                  math::inverse_mix(
                      0.0f, falloff_width, compute_rounded_square_radius(coord, roundness)),
                  ellipse_height,
                  ellipse_width,
                  inflection_midpoint));
        }
      }
      else {
        /* Mask is a 1 dimensional line. */
        if ((coord.y != 0.0f) || (math::abs(coord.x) > (abs_size.x + falloff_width))) {
          /* coord is outside of the mask. */
          return 0.0f;
        }
        else if (math::abs(coord.x) <= (abs_size.x)) {
          /* coord is in the constant part of the mask. */
          return 1.0f;
        }
        else {
          /* coord is in the falloff part of the mask. */
          return math::interpolate(
              1.0f,
              falloff_boundary_value,
              elliptical_unit_step_without_constant_part(
                  math::inverse_mix(abs_size.x, abs_size.x + falloff_width, math::abs(coord.x)),
                  ellipse_height,
                  ellipse_width,
                  inflection_midpoint));
        }
      }
    }
    else {
      if (is_in_unit_rounded_square(coord / abs_size, roundness)) {
        /* coord is in the constant part of the mask. */
        return 1.0f;
      }
      else if ((falloff_width == 0.0f) ||
               !is_in_unit_rounded_square(
                   coord /
                       (abs_size + float2(falloff_width, falloff_width * abs_size.y / abs_size.x)),
                   roundness))
      {
        /* coord is outside of the mask. */
        return 0.0f;
      }
      else {
        /* coord is in the falloff part of the mask. */
        return math::interpolate(
            1.0f,
            falloff_boundary_value,
            elliptical_unit_step_without_constant_part(
                math::inverse_mix(
                    abs_size.x,
                    abs_size.x + falloff_width,
                    compute_rounded_square_radius(
                        float2(coord.x, coord.y * abs_size.x / abs_size.y), roundness)),
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
