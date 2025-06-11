/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"

#include "COM_algorithm_jump_flooding.hh"
#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_composite_signed_distance_field_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("Mask").hide_value();

  b.add_output<decl::Float>("Signed Distance Field")
      .description(
          "The distance in pixel to the nearest pixel at the boundary of the mask. The distance "
          "is negative inside the mask");
  b.add_output<decl::Vector>("Nearest Pixel")
      .dimensions(2)
      .description("The integer coordinates of the nearest pixel at the boundary of the mask");
}

using namespace blender::compositor;

class SignedDistanceFieldOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &input = this->get_input("Mask");
    Result &distance_output = this->get_result("Signed Distance Field");

    Result &nearest_pixel_output = this->get_result("Nearest Pixel");
    nearest_pixel_output.set_type(ResultType::Int2);
    nearest_pixel_output.set_precision(ResultPrecision::Half);

    if (input.is_single_value()) {
      if (distance_output.should_compute()) {
        distance_output.allocate_single_value();
        distance_output.set_single_value(0.0f);
      }
      if (nearest_pixel_output.should_compute()) {
        nearest_pixel_output.allocate_single_value();
        distance_output.set_single_value(int2(0));
      }
      return;
    }

    Result mask_boundary = this->compute_boundary();

    Result flooded_boundary = this->context().create_result(ResultType::Int2,
                                                            ResultPrecision::Half);
    jump_flooding(this->context(), mask_boundary, flooded_boundary);
    mask_boundary.release();

    if (distance_output.should_compute()) {
      this->compute_signed_distance(flooded_boundary);
    }

    if (nearest_pixel_output.should_compute()) {
      nearest_pixel_output.set_type(ResultType::Int2);
      nearest_pixel_output.set_precision(ResultPrecision::Half);
      nearest_pixel_output.steal_data(flooded_boundary);
    }
    else {
      flooded_boundary.release();
    }
  }

  /* Compute an image that marks the boundary pixels of the mask region as seed pixels for
   * the jump flooding algorithm. */
  Result compute_boundary()
  {
    if (this->context().use_gpu()) {
      return this->compute_boundary_gpu();
    }

    return this->compute_boundary_cpu();
  }

  Result compute_boundary_gpu()
  {
    GPUShader *shader = context().get_shader("compositor_signed_distance_field_compute_boundary",
                                             ResultPrecision::Half);
    GPU_shader_bind(shader);

    const Result &input = this->get_input("Mask");
    input.bind_as_texture(shader, "input_tx");

    Result boundary = this->context().create_result(ResultType::Int2, ResultPrecision::Half);
    const Domain domain = input.domain();
    boundary.allocate_texture(domain);
    boundary.bind_as_image(shader, "boundary_img");

    compute_dispatch_threads_at_least(shader, domain.size);

    input.unbind_as_texture();
    boundary.unbind_as_image();
    GPU_shader_unbind();

    return boundary;
  }

  Result compute_boundary_cpu()
  {
    const Result &input = this->get_input("Mask");

    Result boundary = this->context().create_result(ResultType::Int2, ResultPrecision::Half);
    const Domain domain = input.domain();
    boundary.allocate_texture(domain);

    /* The signed distance field operation uses a jump flood algorithm to flood the region to be
     * distance transformed with the pixels at its boundary. The algorithms expects an input image
     * whose values are those returned by the initialize_jump_flooding_value function, given the
     * texel location and a boolean specifying if the pixel is a boundary one.
     *
     * Technically, we needn't restrict the output to just the boundary pixels, since the algorithm
     * can still operate if the interior of the region was also included. However, the algorithm
     * operates more accurately when the number of pixels to be flooded is minimum. */
    parallel_for(domain.size, [&](const int2 texel) {
      /* Identify if any of the 8 neighbors around the center pixel are zero. */
      bool has_zero_neighbors = false;
      for (int j = -1; j <= 1; j++) {
        for (int i = -1; i <= 1; i++) {
          const int2 offset = int2(i, j);

          /* Exempt the center pixel. */
          if (offset == int2(0)) {
            continue;
          }

          if (input.load_pixel_extended<float>(texel + offset) == 0.0f) {
            has_zero_neighbors = true;
            break;
          }
        }
      }

      /* The pixels at the boundary are those that are non-zero and have zero neighbors. */
      const bool is_non_zero = input.load_pixel<float>(texel) != 0.0f;
      const bool is_boundary_pixel = is_non_zero && has_zero_neighbors;

      /* Encode the boundary information in the format expected by the jump flooding algorithm. */
      const int2 jump_flooding_value = initialize_jump_flooding_value(texel, is_boundary_pixel);

      boundary.store_pixel(texel, jump_flooding_value);
    });

    return boundary;
  }

  void compute_signed_distance(const Result &flooded_boundary)
  {
    if (this->context().use_gpu()) {
      this->compute_signed_distance_gpu(flooded_boundary);
    }
    else {
      this->compute_signed_distance_cpu(flooded_boundary);
    }
  }

  void compute_signed_distance_gpu(const Result &flooded_boundary)
  {
    GPUShader *shader = this->context().get_shader(
        "compositor_signed_distance_field_compute_distance");
    GPU_shader_bind(shader);

    const Result &input = this->get_input("Mask");
    input.bind_as_texture(shader, "input_tx");

    flooded_boundary.bind_as_texture(shader, "flooded_boundary_tx");

    const Domain domain = input.domain();
    Result &distance_output = this->get_result("Signed Distance Field");
    distance_output.allocate_texture(domain);
    distance_output.bind_as_image(shader, "distance_img");

    compute_dispatch_threads_at_least(shader, domain.size);

    input.unbind_as_texture();
    flooded_boundary.unbind_as_texture();
    distance_output.unbind_as_image();
    GPU_shader_unbind();
  }

  void compute_signed_distance_cpu(const Result &flooded_boundary)
  {
    const Result &input = this->get_input("Mask");

    const Domain domain = input.domain();
    Result &distance_output = this->get_result("Signed Distance Field");
    distance_output.allocate_texture(domain);

    parallel_for(domain.size, [&](const int2 texel) {
      const bool is_inside_mask = input.load_pixel<float>(texel) != 0.0f;
      const int2 closest_boundary_texel = flooded_boundary.load_pixel<int2>(texel);
      const float distance_to_boundary = math::distance(float2(texel),
                                                        float2(closest_boundary_texel));
      const float signed_distance = is_inside_mask ? -distance_to_boundary : distance_to_boundary;

      distance_output.store_pixel(texel, signed_distance);
    });
  }
};

static NodeOperation *get_compositor_operation(Context &context, DNode node)
{
  return new SignedDistanceFieldOperation(context, node);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeSignedDistanceField");
  ntype.ui_name = "Signed Distance Field";
  ntype.ui_description = "Computes a signed distance field from the given mask";
  ntype.nclass = NODE_CLASS_OP_FILTER;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;

  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_composite_signed_distance_field_cc
