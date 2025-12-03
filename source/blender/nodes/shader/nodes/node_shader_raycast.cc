/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

#include "BLI_math_vector.h"

namespace blender::nodes::node_shader_raycast_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Vector>("Position").hide_value();
  b.add_input<decl::Vector>("Direction").hide_value();
  b.add_input<decl::Float>("Length").default_value(1.0);
  b.add_output<decl::Float>("Is Hit");
  b.add_output<decl::Vector>("Hit Position");
  b.add_output<decl::Float>("Hit Distance");
}

static int node_shader_gpu_raycast(GPUMaterial *mat,
                                   bNode *node,
                                   bNodeExecData * /*execdata*/,
                                   GPUNodeStack *in,
                                   GPUNodeStack *out)
{
  if (!in[0].link) {
    GPU_link(mat, "world_position_get", &in[0].link);
  }

  if (!in[1].link) {
    GPU_link(mat, "world_normals_get", &in[1].link);
  }

  return GPU_stack_link(mat, node, "node_raycast", in, out);
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  return get_output_default(socket_out_->identifier, NodeItem::Type::Any);
}
#endif
NODE_SHADER_MATERIALX_END

}  // namespace blender::nodes::node_shader_raycast_cc

/* node type definition */
void register_node_type_sh_raycast()
{
  namespace file_ns = blender::nodes::node_shader_raycast_cc;

  static blender::bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeRaycast", SH_NODE_RAYCAST);
  ntype.ui_name = "Raycast";
  ntype.ui_description = "Cast rays and retrieve information from the hit point";
  ntype.enum_name_legacy = "RAYCAST";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.add_ui_poll = object_shader_nodes_poll;
  ntype.declare = file_ns::node_declare;
  ntype.gpu_fn = file_ns::node_shader_gpu_raycast;
  ntype.materialx_fn = file_ns::node_shader_materialx;

  blender::bke::node_register_type(ntype);
}
