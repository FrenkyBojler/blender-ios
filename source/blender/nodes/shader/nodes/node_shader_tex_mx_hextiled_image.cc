/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"
#include "node_util.hh"

#include "BKE_image.hh"
#include "BKE_node_runtime.hh"
#include "BKE_texture.h"

#include "IMB_colormanagement.hh"

#include "DEG_depsgraph_query.hh"

namespace blender {

namespace nodes::node_shader_tex_mx_hextiled_image_cc {

static void sh_node_tex_mx_hextiled_image_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Vector>("Vector"_ustr).implicit_field(NODE_DEFAULT_INPUT_POSITION_FIELD);
  b.add_input<decl::Vector>("Tiling"_ustr).default_value(float3(1.0f, 1.0f, 0.0f));
  b.add_input<decl::Float>("Rotation"_ustr).default_value(1.0f);
  b.add_input<decl::Vector>("Rotation Range"_ustr).default_value(float3(0.0f, 360.0f, 0.0f));
  b.add_input<decl::Float>("Scale"_ustr).default_value(1.0f);
  b.add_input<decl::Vector>("Scale Range"_ustr).default_value(float3(0.5f, 2.0f, 0.0f));
  b.add_input<decl::Float>("Offset"_ustr).default_value(1.0f);
  b.add_input<decl::Vector>("Offset Range"_ustr).default_value(float3(0.0f, 1.0f, 0.0f));
  b.add_input<decl::Float>("Falloff"_ustr).default_value(0.5f);
  b.add_input<decl::Float>("Falloff Contrast"_ustr).default_value(0.5f);
  b.add_input<decl::Color>("Luma Coeffs"_ustr).default_value({0.2722287f, 0.6740818f, 0.0536895f, 1.0f});
  b.add_output<decl::Color>("Color"_ustr).no_muted_links();
  b.add_output<decl::Float>("Alpha"_ustr).no_muted_links();
}

static void node_shader_init_tex_mx_hextiled_image(bNodeTree * /*ntree*/, bNode *node)
{
  NodeTexImage *tex = MEM_new<NodeTexImage>(__func__);
  BKE_texture_mapping_default(&tex->base.tex_mapping, TEXMAP_TYPE_POINT);
  BKE_texture_colormapping_default(&tex->base.color_mapping);
  BKE_imageuser_default(&tex->iuser);
  tex->extension = SHD_IMAGE_EXTENSION_REPEAT;
  tex->interpolation = SHD_INTERP_LINEAR;
  tex->projection = SHD_PROJ_FLAT;

  node->storage = tex;
}

static int node_shader_gpu_tex_mx_hextiled_image(GPUMaterial *mat,
                                                 bNode *node,
                                                 bNodeExecData * /*execdata*/,
                                                 GPUNodeStack *in,
                                                 GPUNodeStack *out)
{
  Image *ima = id_cast<Image *>(node->id);
  NodeTexImage *tex = static_cast<NodeTexImage *>(node->storage);
  bNode *node_original = node->runtime->original ? node->runtime->original : node;
  NodeTexImage *tex_original = static_cast<NodeTexImage *>(node_original->storage);
  ImageUser *iuser = &tex_original->iuser;

  if (!ima) {
    return GPU_stack_link(mat, node, "node_tex_image_empty", in, out);
  }

  GPUNodeLink **texco = &in[0].link;
  if (!*texco) {
    *texco = GPU_attribute(mat, CD_AUTO_FROM_NAME, "");
    node_shader_gpu_bump_tex_coord(mat, node, texco);
  }

  GPUSamplerState sampler_state = GPUSamplerState::default_sampler();
  switch (tex->extension) {
    case SHD_IMAGE_EXTENSION_EXTEND:
      sampler_state.extend_x = GPU_SAMPLER_EXTEND_MODE_EXTEND;
      sampler_state.extend_yz = GPU_SAMPLER_EXTEND_MODE_EXTEND;
      break;
    case SHD_IMAGE_EXTENSION_REPEAT:
      sampler_state.extend_x = GPU_SAMPLER_EXTEND_MODE_REPEAT;
      sampler_state.extend_yz = GPU_SAMPLER_EXTEND_MODE_REPEAT;
      break;
    case SHD_IMAGE_EXTENSION_CLIP:
      sampler_state.extend_x = GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER;
      sampler_state.extend_yz = GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER;
      break;
    case SHD_IMAGE_EXTENSION_MIRROR:
      sampler_state.extend_x = GPU_SAMPLER_EXTEND_MODE_MIRRORED_REPEAT;
      sampler_state.extend_yz = GPU_SAMPLER_EXTEND_MODE_MIRRORED_REPEAT;
      break;
    default:
      break;
  }
  if (tex->interpolation != SHD_INTERP_CLOSEST) {
    sampler_state.filtering = GPU_SAMPLER_FILTERING_ANISOTROPIC_ENABLE |
                              GPU_SAMPLER_FILTERING_LINEAR | GPU_SAMPLER_FILTERING_MIPMAP;
  }

  GPUNodeLink *gpu_image = GPU_image(mat, ima, iuser, sampler_state);
  GPU_stack_link(mat, node, "node_tex_mx_hextiled_image", in, out, gpu_image);

  if (out[0].hasoutput) {
    if (ELEM(ima->alpha_mode, IMA_ALPHA_IGNORE, IMA_ALPHA_CHANNEL_PACKED) ||
        IMB_colormanagement_space_name_is_data(ima->colorspace_settings.name))
    {
      GPU_link(mat, "color_alpha_clear", out[0].link, &out[0].link);
    }
    else if (ima->alpha_mode == IMA_ALPHA_PREMUL) {
      if (out[1].hasoutput) {
        GPU_link(mat, "color_alpha_unpremultiply", out[0].link, &out[0].link);
      }
      else {
        GPU_link(mat, "color_alpha_clear", out[0].link, &out[0].link);
      }
    }
    else if (out[1].hasoutput) {
      GPU_link(mat, "color_alpha_clear", out[0].link, &out[0].link);
    }
    else {
      GPU_link(mat, "color_alpha_premultiply", out[0].link, &out[0].link);
    }
  }

  return true;
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  return val(MaterialX::Color4(1.0f, 0.0f, 1.0f, 1.0f));
}
#endif
NODE_SHADER_MATERIALX_END

}  // namespace nodes::node_shader_tex_mx_hextiled_image_cc

void register_node_type_sh_tex_mx_hextiled_image()
{
  namespace file_ns = nodes::node_shader_tex_mx_hextiled_image_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeMxHextiledImage"_ustr, SH_NODE_TEX_MX_HEXTILED_IMAGE);
  ntype.ui_name = "MaterialX Hextiled Image";
  ntype.ui_description = "Sample an image using MaterialX hextile randomization";
  ntype.enum_name_legacy = "TEX_MX_HEXTILED_IMAGE";
  ntype.nclass = NODE_CLASS_TEXTURE;
  ntype.declare = file_ns::sh_node_tex_mx_hextiled_image_declare;
  ntype.initfunc = file_ns::node_shader_init_tex_mx_hextiled_image;
  bke::node_type_storage(
      ntype, "NodeTexImage", node_free_standard_storage, node_copy_standard_storage);
  ntype.gpu_fn = file_ns::node_shader_gpu_tex_mx_hextiled_image;
  ntype.labelfunc = node_image_label;
  bke::node_type_size_preset(ntype, bke::eNodeSizePreset::Large);
  ntype.materialx_fn = file_ns::node_shader_materialx;

  bke::node_register_type(ntype);
}

}  // namespace blender
