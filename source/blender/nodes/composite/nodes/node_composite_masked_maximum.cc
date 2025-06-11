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
  b.add_input<decl::Float>("Mask").default_value(0.5f).min(0.0f).compositor_domain_priority(0);
  b.add_input<decl::Vector>("Size")
      .dimensions(2)
      .default_value({0.0f, 0.0f, 0.0f})
      .min(0.0f)
      .compositor_domain_priority(1)
      .description(
          "Size from the center of the constant part of the rounded square mask to its "
          "boundaries");
  b.add_input<decl::Float>("Rotation")
      .default_value(0.0f)
      .subtype(PROP_ANGLE)
      .compositor_domain_priority(2)
      .description("Angle to rotate the rounded square mask by");
  b.add_input<decl::Vector>("Translation")
      .dimensions(2)
      .default_value({0.0f, 0.0f, 0.0f})
      .compositor_domain_priority(3)
      .description("Translation of the rounded square mask");
  b.add_input<decl::Float>("Roundness")
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .compositor_domain_priority(4)
      .description("Roundness of the rounded square mask");
  b.add_input<decl::Float>("Falloff")
      .default_value(0.0f)
      .min(0.0f)
      .compositor_domain_priority(5)
      .description(
          "Maximal range of the linear falloff starting at the boundaries of the constant part of "
          "the rounded square mask");

  b.add_output<decl::Float>("Mask");
}

using namespace blender::compositor;

class MaskedMaximumOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &input_mask = this->get_input("Mask");
    Result &output_mask = this->get_result("Mask");
    float3 size_single_value = get_input("Size").get_single_value_default(
        float3(1.0f, 1.0f, 0.0f));
    float3 translation_single_value =
        get_input("Translation").get_single_value_default(float3(1.0f, 1.0f, 0.0f));

    if (input_mask.is_single_value() ||
        ((size_single_value.x <= 0.0f) && (size_single_value.y <= 0.0f) &&
         (translation_single_value.x == 0.0f) && (translation_single_value.y == 0.0f) &&
         (get_input("Falloff").get_single_value_default(1.0f) <= 0.0f)))
    {
      /* Operation does nothing and the input can be passed through. */
      output_mask.share_data(input_mask);
      return;
    }

    if (this->context().use_gpu()) {
      this->execute_gpu(input_mask, output_mask);
    }
    else {
      this->execute_cpu(input_mask, output_mask);
    }
  }

  void execute_gpu(const Result &input_mask, Result &output_mask)
  {
    GPUShader *shader = context().get_shader("compositor_masked_maximum");
    GPU_shader_bind(shader);

    const Domain domain = compute_domain();

    GPU_shader_uniform_2iv(shader, "domain_size", domain.size);

    input_mask.bind_as_texture(shader, "input_mask_tx");

    const Result &input_size = get_input("Size");
    input_size.bind_as_texture(shader, "input_size_tx");

    const Result &input_rotation = get_input("Rotation");
    input_rotation.bind_as_texture(shader, "input_rotation_tx");

    const Result &input_translation = get_input("Translation");
    input_translation.bind_as_texture(shader, "input_translation_tx");

    const Result &input_roundness = get_input("Roundness");
    input_roundness.bind_as_texture(shader, "input_roundness_tx");

    const Result &input_falloff = get_input("Falloff");
    input_falloff.bind_as_texture(shader, "input_falloff_tx");

    output_mask.allocate_texture(domain);
    output_mask.bind_as_image(shader, "output_mask_img");

    compute_dispatch_threads_at_least(shader, domain.size);

    GPU_shader_unbind();
    input_mask.unbind_as_texture();
    input_size.unbind_as_texture();
    input_rotation.unbind_as_texture();
    input_translation.unbind_as_texture();
    input_roundness.unbind_as_texture();
    input_falloff.unbind_as_texture();
    output_mask.unbind_as_image();
  }

  void execute_cpu(const Result &input_mask, Result &output_mask)
  {
    Domain domain = this->compute_domain();
    output_mask.allocate_texture(domain);

    parallel_for(domain.size, [&](const int2 texel) {
      float3 size = math::max(get_input("Size").load_pixel_zero<float3, true>(texel),
                              float3(0.0f, 0.0f, 0.0f));
      float rotation = get_input("Rotation").load_pixel_zero<float, true>(texel);
      float3 translation = get_input("Translation").load_pixel_zero<float3, true>(texel);
      float roundness = math::clamp(
          get_input("Roundness").load_pixel_zero<float, true>(texel), 0.0f, 1.0f);
      float falloff = math::max(get_input("Falloff").load_pixel_zero<float, true>(texel), 0.0f);

      /* Calculate top right and bottom left corner of the bounding box of the rounded square mask.
       */
      float2 bounding_box_top_right_corner_float;
      if (size.x == size.y) {
        bounding_box_top_right_corner_float = float2(math::ceil(size.x + falloff),
                                                     math::ceil(size.y + falloff));
      }
      else if (size.x == 0.0f) {
        bounding_box_top_right_corner_float = float2(0.0f, math::ceil(size.y + falloff));
      }
      else if (size.y == 0.0f) {
        bounding_box_top_right_corner_float = float2(math::ceil(size.x + falloff), 0.0f);
      }
      else {
        bounding_box_top_right_corner_float = float2(
            math::ceil(size.x + (falloff * math::min(size.x / size.y, 1.0f))),
            math::ceil(size.y + (falloff * math::min(size.y / size.x, 1.0f))));
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
      /* Crop away parts of the bounding box that are outside of the domain. */
      bounding_box_top_right_corner += texel;
      bounding_box_bottom_left_corner += texel;
      bounding_box_top_right_corner = math::min(bounding_box_top_right_corner,
                                                domain.size - int2(1, 1));
      bounding_box_bottom_left_corner = math::max(bounding_box_bottom_left_corner, int2(0, 0));
      bounding_box_top_right_corner -= texel;
      bounding_box_bottom_left_corner -= texel;
      float masked_maximum = -FLT_MAX;
      for (int y = bounding_box_bottom_left_corner.y; y <= bounding_box_top_right_corner.y; y++) {
        for (int x = bounding_box_bottom_left_corner.x; x <= bounding_box_top_right_corner.x; x++)
        {
          float2 coord = float2(x, y) - float2(translation.x, translation.y);
          if (rotation != 0.0f) {
            coord = rotate_vector_2d(coord, -rotation);
          }
          masked_maximum = math::max(
              masked_maximum,
              compute_rounded_square_mask(coord, float2(size.x, size.y), roundness, falloff) *
                  math::max(input_mask.load_pixel_zero<float, true>(texel + int2(x, y)), 0.0f));
        }
      }

      output_mask.store_pixel(texel, masked_maximum);
    });
  }

  float2 rotate_vector_2d(float2 vector, float angle)
  {
    return float2(vector.x * cos(angle) - vector.y * sin(angle),
                  vector.x * sin(angle) + vector.y * cos(angle));
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
                                    float2 size,
                                    const float roundness,
                                    const float falloff)
  {
    /* Swap x and y names if size.y > size.x. This is done because the following code excpects
     * size.x to be greater or equal to size.y. This makes sure that the falloff is calculated
     * based on the larger size, making the Falloff input an upper limit to the falloff range. */
    if (size.y > size.x) {
      std::swap(coord.x, coord.y);
      std::swap(size.x, size.y);
    }

    if (size.y == 0.0f) {
      if (size.x == 0.0f) {
        if ((coord.x == 0.0f) && (coord.y == 0.0f)) {
          /* coord is in the constant part of the mask. */
          return 1.0f;
        }
        else if ((falloff == 0.0f) ||
                 (!is_in_unit_rounded_square(coord / (float2(falloff, falloff)), roundness)))
        {
          /* coord is outside of the mask. */
          return 0.0f;
        }
        else {
          /* coord is in the linear falloff part of the mask. */
          return math::inverse_mix(falloff, 0.0f, compute_rounded_square_radius(coord, roundness));
        }
      }
      else {
        /* Mask is a 1 dimensional line. */
        if ((coord.y != 0.0f) || (math::abs(coord.x) > (size.x + falloff))) {
          /* coord is outside of the mask. */
          return 0.0f;
        }
        else if (math::abs(coord.x) <= (size.x)) {
          /* coord is in the constant part of the mask. */
          return 1.0f;
        }
        else {
          /* coord is in the linear falloff part of the mask. */
          return math::inverse_mix(size.x + falloff, size.x, math::abs(coord.x));
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
        return math::inverse_mix(
            size.x + falloff,
            size.x,
            compute_rounded_square_radius(float2(coord.x, coord.y * size.x / size.y), roundness));
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
