/* SPDX-FileCopyrightText: 2013 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup cmpnodes
 */

#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector_types.hh"

#include "DNA_node_types.h"

#include "RNA_enum_types.hh"

#include "GPU_shader.hh"
#include "GPU_texture.hh"

#include "BKE_node.hh"
#include "BKE_tracking.h"

#include "MEM_guardedalloc.h"

#include "COM_algorithm_smaa.hh"
#include "COM_domain.hh"
#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_composite_cornerpin_cc {

static void cmp_node_cornerpin_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();

  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic);
  b.add_output<decl::Float>("Plane").structure_type(StructureType::Dynamic);

  b.add_input<decl::Color>("Image")
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .structure_type(StructureType::Dynamic);
  b.add_input<decl::Vector>("Upper Left")
      .subtype(PROP_FACTOR)
      .dimensions(2)
      .default_value({0.0f, 1.0f})
      .min(0.0f)
      .max(1.0f);
  b.add_input<decl::Vector>("Upper Right")
      .subtype(PROP_FACTOR)
      .dimensions(2)
      .default_value({1.0f, 1.0f})
      .min(0.0f)
      .max(1.0f);
  b.add_input<decl::Vector>("Lower Left")
      .subtype(PROP_FACTOR)
      .dimensions(2)
      .default_value({0.0f, 0.0f})
      .min(0.0f)
      .max(1.0f);
  b.add_input<decl::Vector>("Lower Right")
      .subtype(PROP_FACTOR)
      .dimensions(2)
      .default_value({1.0f, 0.0f})
      .min(0.0f)
      .max(1.0f);

  PanelDeclarationBuilder &sampling_panel = b.add_panel("Sampling").default_closed(true);
  sampling_panel.add_input<decl::Menu>("Interpolation")
      .default_value(CMP_NODE_INTERPOLATION_BILINEAR)
      .static_items(rna_enum_node_compositor_interpolation_items)
      .description("Interpolation method");
  sampling_panel.add_input<decl::Menu>("Extension X")
      .default_value(CMP_NODE_EXTENSION_MODE_CLIP)
      .static_items(rna_enum_node_compositor_extension_items)
      .description("The extension mode applied to the X axis");
  sampling_panel.add_input<decl::Menu>("Extension Y")
      .default_value(CMP_NODE_EXTENSION_MODE_CLIP)
      .static_items(rna_enum_node_compositor_extension_items)
      .description("The extension mode applied to the Y axis");
}

static void node_composit_init_cornerpin(bNodeTree * /*ntree*/, bNode *node)
{
  /* Unused, kept for forward compatibility. */
  NodeCornerPinData *data = MEM_callocN<NodeCornerPinData>(__func__);
  node->storage = data;
}

using namespace blender::compositor;

class CornerPinOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const float3x3 homography_matrix = compute_homography_matrix();
    // convert the matrix to translate pixel centers to pixel corners
    const Domain domain = compute_domain();
    float3x3 to_bounds = math::translate(math::from_scale<float3x3>(1.0f / float2(domain.size)),
                                         float2(0.5f));
    float3x3 from_bounds = math::from_scale<float3x3>(float2(domain.size));
    float3x3 imat = from_bounds * homography_matrix * to_bounds;

    const Result &input_image = this->get_input("Image");
    Result &output_image = this->get_result("Image");
    Result &output_mask = this->get_result("Plane");
    if (input_image.is_single_value() || homography_matrix == float3x3::identity()) {
      if (output_image.should_compute()) {
        output_image.share_data(input_image);
      }
      if (output_mask.should_compute()) {
        output_mask.allocate_single_value();
        output_mask.set_single_value(1.0f);
      }
      return;
    }

    if (output_image.should_compute()) {
      if (this->context().use_gpu()) {
        this->compute_plane_gpu(imat);
      }
      else {
        this->compute_plane_cpu(imat);
      }
    }
  }

  void compute_plane_gpu(const float3x3 &imat)
  {
    math::SamplingOptions options = this->get_options();

    // can we use texture() call:
    char shader_name[100];
    strcpy(shader_name, "compositor_plane_deform");
    bool fast = false;  // true means texture() call is being used

    switch (options.sampler) {
      case math::Sampler::Nearest:
        fast = true;
        strcat(shader_name, "_fast");
        break;
      case math::Sampler::Box:
        strcat(shader_name, "_box");
        break;
      case math::Sampler::Bspline:
        strcat(shader_name, "_bspline");
        break;
      case math::Sampler::Anisotropic:
        strcat(shader_name, "_anisotropic");
        break;
    }

    bool masked = false;
    if (!fast && (options.wrap_x == math::InterpWrapMode::Border ||
                  options.wrap_y == math::InterpWrapMode::Border ||
                  options.sampler == math::Sampler::Anisotropic))
    {
      masked = true;
      strcat(shader_name, "_masked");
    }

    gpu::Shader *shader = this->context().get_shader(shader_name);
    GPU_shader_bind(shader);

    const Domain domain = compute_domain();

    if (fast) {
      // make matrix produce texture coordinates
      const float3x3 mat = math::from_scale<float3x3>(1.0f / float2(domain.size)) * imat;
      GPU_shader_uniform_mat3_as_mat4(shader, "imat", mat.ptr());
    }
    else {
      GPU_shader_uniform_mat3_as_mat4(shader, "imat", imat.ptr());
    }

    if (masked) {
      float mx = 1;
      if (options.wrap_x == math::InterpWrapMode::Border) {
        mx = 0;
        options.wrap_x = math::InterpWrapMode::Extend;
      }
      float my = 1;
      if (options.wrap_y == math::InterpWrapMode::Border) {
        my = 0;
        options.wrap_y = math::InterpWrapMode::Extend;
      }
      GPU_shader_uniform_2f(shader, "mask_mult", mx, my);
    }

    Result &input_image = get_input("Image");
    if (options.sampler == math::Sampler::Anisotropic) {
      GPU_texture_mipmap_mode(input_image, true, true);
      GPU_texture_anisotropic_filter(input_image, true);
    }
    else {
      GPU_texture_filter_mode(input_image, false);  // all versions use nearest sampling
      GPU_texture_extend_mode_x(input_image, map_extension_mode_to_extend_mode(options.wrap_x));
      GPU_texture_extend_mode_y(input_image, map_extension_mode_to_extend_mode(options.wrap_y));
    }
    input_image.bind_as_texture(shader, "input_tx");

    Result &output_image = get_result("Image");
    output_image.allocate_texture(domain);
    output_image.bind_as_image(shader, "output_img");

    compute_dispatch_threads_at_least(shader, domain.size);

    input_image.unbind_as_texture();

    output_image.unbind_as_image();
    GPU_shader_unbind();
  }

  void compute_plane_cpu(const float3x3 &imat)
  {
    Result &input = get_input("Image");

    const Domain domain = compute_domain();
    Result &output = get_result("Image");
    output.allocate_texture(domain);

    math::SamplingOptions options = this->get_options();
    const int2 size = domain.size;

    // detect when Nearest sampling works. This will also work for integer translations with no
    // rotation
    if (options.sampler == math::Sampler::Nearest) {
      parallel_for(size, [&](const int2 texel) {
        float3 uvw = imat * float3(texel.x, texel.y, 1.0f);
        float4 sampled_color;
        if (uvw.z <= 0.0f) {
          sampled_color = float4(0.0f);
        }
        else {
          sampled_color = input.sample_nearest(options, uvw.xy() / uvw.z);
        }
        output.store_pixel(texel, sampled_color);
      });
      return;
    }

    bool clip_x = options.sampler == math::Sampler::Anisotropic ||
                  options.wrap_x == math::InterpWrapMode::Border;
    if (clip_x)
      options.wrap_x = math::InterpWrapMode::Extend;
    bool clip_y = options.sampler == math::Sampler::Anisotropic ||
                  options.wrap_y == math::InterpWrapMode::Border;
    if (clip_y)
      options.wrap_y = math::InterpWrapMode::Extend;

    parallel_for(size, [&](const int2 texel) {
      float3 uvw = imat * float3(texel.x, texel.y, 1.0f);

      // Point is at infinity and will be zero when sampled, so early exit.
      // Also negative numbers indicate "behind camera" and should be cropped as well.
      if (uvw.z <= 0.0f) {
        output.store_pixel(texel, float4(0.0f));
        return;
      }

      float iw = 1.0f / uvw.z;  // 1/w

      // compute derivative of source location
      const float3 &m0 = imat[0];
      float2 dPdx = (m0.xy() - uvw.xy() * m0.z * iw) * iw;
      const float3 &m1 = imat[1];
      float2 dPdy = (m1.xy() - uvw.xy() * m1.z * iw) * iw;

      float m = 1;

      // antialias the horizon line
      float dw = hypotf(m0.z, m1.z);
      if (dw > uvw.z)
        m = uvw.z / dw;

      float2 uv = uvw.xy() * iw;

      if (clip_x || clip_y) {
        if (m < 1)
          m = 0;  // remove artifacts at horizon
        else {
          const float2 wh = math::hypot2(dPdx, dPdy);
          if (clip_x)
            m = math::clamp(std::min(uv.x, size.x - uv.x) / wh.x + 0.5f, 0.0f, 1.0f);
          if (clip_y)
            m *= math::clamp(std::min(uv.y, size.y - uv.y) / wh.y + 0.5f, 0.0f, 1.0f);
        }
        if (m <= 0.0f) {
          output.store_pixel(texel, float4(0.0f));
          return;
        }
      }

      float4 sampled_color = m * input.sample_area(options, uv, dPdx, dPdy);
      output.store_pixel(texel, sampled_color);
    });
  }

  float3x3 compute_homography_matrix()
  {
    float2 lower_left = get_input("Lower Left").get_single_value_default(float2(0.0f));
    float2 lower_right = get_input("Lower Right").get_single_value_default(float2(0.0f));
    float2 upper_right = get_input("Upper Right").get_single_value_default(float2(0.0f));
    float2 upper_left = get_input("Upper Left").get_single_value_default(float2(0.0f));

    /* The inputs are invalid because the plane is not convex, fall back to an identity operation
     * in that case. */
    if (!is_quad_convex_v2(lower_left, lower_right, upper_right, upper_left)) {
      return float3x3::identity();
    }

    /* Compute a 2D projection matrix that projects from the corners of the image in normalized
     * coordinates into the corners of the input plane. */
    float3x3 homography_matrix;
    float corners[4][2] = {{lower_left.x, lower_left.y},
                           {lower_right.x, lower_right.y},
                           {upper_right.x, upper_right.y},
                           {upper_left.x, upper_left.y}};
    float identity_corners[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    BKE_tracking_homography_between_two_quads(corners, identity_corners, homography_matrix.ptr());
    return homography_matrix;
  }

  math::SamplingOptions get_options() const
  {
    math::SamplingOptions ret;

    switch (static_cast<CMPNodeInterpolation>(
        this->get_input("Interpolation")
            .get_single_value_default(MenuValue(CMP_NODE_INTERPOLATION_BILINEAR))
            .value))
    {
      case CMP_NODE_INTERPOLATION_ANISOTROPIC:
        ret.sampler = math::Sampler::Anisotropic;
        break;
      case CMP_NODE_INTERPOLATION_NEAREST:
        ret.sampler = math::Sampler::Nearest;
        break;
      default:  // CMP_NODE_INTERPOLATION_BILINEAR
        ret.sampler = math::Sampler::Box;
        break;
      case CMP_NODE_INTERPOLATION_BICUBIC:
        ret.sampler = math::Sampler::Bspline;
        break;
    }

    switch (static_cast<CMPExtensionMode>(
        this->get_input("Extension X")
            .get_single_value_default(MenuValue(CMP_NODE_EXTENSION_MODE_CLIP))
            .value))
    {
      default:  // case CMP_NODE_EXTENSION_MODE_CLIP:
        ret.wrap_x = math::InterpWrapMode::Border;
        break;
      case CMP_NODE_EXTENSION_MODE_REPEAT:
        ret.wrap_x = math::InterpWrapMode::Repeat;
        break;
      case CMP_NODE_EXTENSION_MODE_EXTEND:
        ret.wrap_x = math::InterpWrapMode::Extend;
        break;
    }

    switch (static_cast<CMPExtensionMode>(
        this->get_input("Extension Y")
            .get_single_value_default(MenuValue(CMP_NODE_EXTENSION_MODE_CLIP))
            .value))
    {
      default:  // case CMP_NODE_EXTENSION_MODE_CLIP:
        ret.wrap_y = math::InterpWrapMode::Border;
        break;
      case CMP_NODE_EXTENSION_MODE_REPEAT:
        ret.wrap_y = math::InterpWrapMode::Repeat;
        break;
      case CMP_NODE_EXTENSION_MODE_EXTEND:
        ret.wrap_y = math::InterpWrapMode::Extend;
        break;
    }

    return ret;
  }

  bool should_compute_mask()
  {
    Result &output_mask = this->get_result("Plane");
    math::SamplingOptions options = this->get_options();
    const bool is_clipped_x = options.wrap_x == math::InterpWrapMode::Border;
    const bool is_clipped_y = options.wrap_y == math::InterpWrapMode::Border;
    const bool output_needed = output_mask.should_compute();
    const bool use_anisotropic = options.sampler == math::Sampler::Anisotropic;

    return is_clipped_x || is_clipped_y || output_needed || use_anisotropic;
  }

  Domain compute_domain() override
  {
    Domain domain = this->get_input("Image").domain();
    /* Reset the location of the domain such that translations take effect, this will result in
     * clipping but is more expected for the user. */
    domain.transformation.location() = float2(0.0f);
    return domain;
  }
};

static NodeOperation *get_compositor_operation(Context &context, DNode node)
{
  return new CornerPinOperation(context, node);
}

}  // namespace blender::nodes::node_composite_cornerpin_cc

static void register_node_type_cmp_cornerpin()
{
  namespace file_ns = blender::nodes::node_composite_cornerpin_cc;

  static blender::bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeCornerPin", CMP_NODE_CORNERPIN);
  ntype.ui_name = "Corner Pin";
  ntype.ui_description = "Plane warp transformation using explicit corner values";
  ntype.enum_name_legacy = "CORNERPIN";
  ntype.nclass = NODE_CLASS_DISTORT;
  ntype.declare = file_ns::cmp_node_cornerpin_declare;
  ntype.initfunc = file_ns::node_composit_init_cornerpin;
  ntype.get_compositor_operation = file_ns::get_compositor_operation;
  blender::bke::node_type_storage(
      ntype, "NodeCornerPinData", node_free_standard_storage, node_copy_standard_storage);
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(register_node_type_cmp_cornerpin)
