/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_bounds_types.hh"
#include "BLI_math_vector_types.hh"

#include "GPU_shader.hh"

#include "NOD_composite.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

namespace blender::nodes::node_composite_group_output_cc {

using namespace blender::compositor;

class GroupOutputOperation : public NodeOperation {
 public:
  GroupOutputOperation(Context &context, DNode node) : NodeOperation(context, node)
  {
    for (const bNodeSocket *input : node->input_sockets()) {
      if (!is_socket_available(input)) {
        continue;
      }

      /* Force implicit conversion to color this is the only supported compositor output type as of
       * now. */
      InputDescriptor &descriptor = this->get_input_descriptor(input->identifier);
      descriptor.type = ResultType::Color;
    }
  }

  void execute() override
  {
    if (!this->context().is_valid_compositing_region()) {
      return;
    }

    const bNodeSocket *input_socket = this->get_input_socket();
    if (!input_socket) {
      return;
    }

    const Result &image = this->get_input(input_socket->identifier);
    if (image.is_single_value()) {
      this->execute_clear(image);
    }
    else {
      this->execute_copy(image);
    }
  }

  /* Returns the first available input socket to be written to the output. The rest of the sockets
   * are ignored. */
  const bNodeSocket *get_input_socket()
  {
    for (const bNodeSocket *input_socket : this->node()->input_sockets()) {
      if (is_socket_available(input_socket)) {
        return input_socket;
      }
    }

    return nullptr;
  }

  void execute_clear(const Result &image)
  {
    float4 color = image.get_single_value<float4>();

    const Domain domain = this->compute_domain();
    Result output = this->context().get_output_result();
    if (this->context().use_gpu()) {
      GPU_texture_clear(output, GPU_DATA_FLOAT, color);
    }
    else {
      parallel_for(domain.size, [&](const int2 texel) { output.store_pixel(texel, color); });
    }
  }

  void execute_copy(const Result &image)
  {
    if (this->context().use_gpu()) {
      this->execute_copy_gpu(image);
    }
    else {
      this->execute_copy_cpu(image);
    }
  }

  void execute_copy_gpu(const Result &image)
  {
    const Domain domain = this->compute_domain();
    Result output = this->context().get_output_result();

    GPUShader *shader = this->context().get_shader("compositor_write_output", output.precision());
    GPU_shader_bind(shader);

    const Bounds<int2> bounds = this->get_output_bounds();
    GPU_shader_uniform_2iv(shader, "lower_bound", bounds.min);
    GPU_shader_uniform_2iv(shader, "upper_bound", bounds.max);

    image.bind_as_texture(shader, "input_tx");

    output.bind_as_image(shader, "output_img");

    compute_dispatch_threads_at_least(shader, domain.size);

    image.unbind_as_texture();
    output.unbind_as_image();
    GPU_shader_unbind();
  }

  void execute_copy_cpu(const Result &image)
  {
    const Domain domain = this->compute_domain();
    Result output = this->context().get_output_result();

    const Bounds<int2> bounds = this->get_output_bounds();
    parallel_for(domain.size, [&](const int2 texel) {
      const int2 output_texel = texel + bounds.min;
      if (output_texel.x > bounds.max.x || output_texel.y > bounds.max.y) {
        return;
      }
      output.store_pixel(texel + bounds.min, image.load_pixel<float4>(texel));
    });
  }

  /* Returns the bounds of the area of the compositing region. Only write into the compositing
   * region, which might be limited to a smaller region of the output result. */
  Bounds<int2> get_output_bounds()
  {
    const rcti compositing_region = this->context().get_compositing_region();
    return Bounds<int2>(int2(compositing_region.xmin, compositing_region.ymin),
                        int2(compositing_region.xmax, compositing_region.ymax));
  }

  /* The operation domain has the same size as the compositing region without any transformations
   * applied. */
  Domain compute_domain() override
  {
    return Domain(this->context().get_compositing_region_size());
  }
};

}  // namespace blender::nodes::node_composite_group_output_cc

namespace blender::nodes {

compositor::NodeOperation *get_group_output_compositor_operation(compositor::Context &context,
                                                                 DNode node)
{
  return new node_composite_group_output_cc::GroupOutputOperation(context, node);
}

}  // namespace blender::nodes
