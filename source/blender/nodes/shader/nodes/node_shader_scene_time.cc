/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

#include "node_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"

namespace blender {

namespace nodes::node_shader_scene_time_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Seconds");
  b.add_output<decl::Float>("Frame");
}

static int node_shader_gpu(GPUMaterial *mat,
                           bNode *node,
                           bNodeExecData * /*execdata*/,
                           GPUNodeStack *in,
                           GPUNodeStack *out)
{
  GPU_stack_link(mat, node, "node_scene_time", in, out);
  return 1;
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  return get_output_default(socket_out_->identifier, NodeItem::Type::Any);
}
#endif
NODE_SHADER_MATERIALX_END

}  // namespace nodes::node_shader_scene_time_cc

/* node type definition */
void register_node_type_sh_scene_time()
{
  namespace file_ns = nodes::node_shader_scene_time_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeSceneTime", SH_NODE_SCENE_TIME);
  ntype.ui_name = "Scene Time";
  ntype.ui_description = "Input the current scene time in seconds or frames";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.gpu_fn = file_ns::node_shader_gpu;
  ntype.materialx_fn = file_ns::node_shader_materialx;

  bke::node_register_type(ntype);
}

}  // namespace blender
