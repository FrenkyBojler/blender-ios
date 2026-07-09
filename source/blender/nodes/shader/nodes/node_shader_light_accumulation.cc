/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

namespace blender {

namespace nodes::node_shader_light_accumulation_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>("Diffuse"_ustr);
  b.add_input<decl::Color>("Glossy"_ustr);
  b.add_input<decl::Color>("Transmission"_ustr);
  b.add_output<decl::Shader>("Shader"_ustr);
}

static int node_shader_gpu_light_accumulation(GPUMaterial *mat,
                                              bNode *node,
                                              bNodeExecData * /*execdata*/,
                                              GPUNodeStack *in,
                                              GPUNodeStack *out)
{
  return GPU_stack_link(mat, node, "node_light_accumulation", in, out);
}

}  // namespace nodes::node_shader_light_accumulation_cc

/* node type definition */
void register_node_type_sh_light_accumulation()
{
  namespace file_ns = nodes::node_shader_light_accumulation_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeLightAccumulation"_ustr, SH_NODE_LIGHT_ACCUMULATION);
  ntype.ui_name = "Light Accumulation";
  ntype.ui_description = "Light Accumulation";
  ntype.enum_name_legacy = "LIGHT ACCUMULATION";
  ntype.nclass = NODE_CLASS_SHADER;
  ntype.declare = file_ns::node_declare;
  ntype.gpu_fn = file_ns::node_shader_gpu_light_accumulation;
  // ntype.materialx_fn = file_ns::node_shader_materialx;

  bke::node_register_type(ntype);
}

}  // namespace blender
