/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

#include "BLI_math_vector.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {

namespace nodes::node_shader_raycast_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Vector>("Position"_ustr).hide_value();
  b.add_input<decl::Vector>("Direction"_ustr).hide_value();
  b.add_input<decl::Vector>("Offset"_ustr);
  b.add_input<decl::Float>("Length"_ustr).default_value(1.0);
  b.add_output<decl::Float>("Is Hit"_ustr);
  b.add_output<decl::Float>("Self Hit"_ustr);
  b.add_output<decl::Float>("Hit Distance"_ustr);
  b.add_output<decl::Vector>("Hit Position"_ustr);
  b.add_output<decl::Vector>("Hit Normal"_ustr);
}

static void node_shader_init(bNodeTree * /*ntree*/, bNode *node)
{
  node->custom1 = 0;                      /* Only Local */
  node->custom2 = SHD_RAYCAST_MODE_WORLD; /* Mode */
}

static void node_shader_buts(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "mode", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  layout.prop(ptr, "only_local", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
}

static void node_shader_update(bNodeTree *ntree, bNode *node)
{
  bNodeSocket *position = bke::node_find_socket(*node, SOCK_IN, "Position");
  bNodeSocket *direction = bke::node_find_socket(*node, SOCK_IN, "Direction");
  bNodeSocket *offset = bke::node_find_socket(*node, SOCK_IN, "Offset");

  bke::node_set_socket_availability(*ntree, *position, node->custom2 == SHD_RAYCAST_MODE_WORLD);
  bke::node_set_socket_availability(*ntree, *direction, node->custom2 == SHD_RAYCAST_MODE_WORLD);
  bke::node_set_socket_availability(*ntree, *offset, node->custom2 == SHD_RAYCAST_MODE_OFFSET);
}

static int node_shader_gpu(GPUMaterial *mat,
                           bNode *node,
                           bNodeExecData * /*execdata*/,
                           GPUNodeStack *in,
                           GPUNodeStack *out)
{
  GPU_material_flag_set(mat, GPU_MATFLAG_RAYCAST);

  if (!in[0].link) {
    GPU_link(mat, "world_position_get", &in[0].link);
  }

  if (!in[1].link) {
    GPU_link(mat, "world_normals_get", &in[1].link);
  }

  const bool only_local = node->custom1;
  const char *function_name = node->custom2 == SHD_RAYCAST_MODE_WORLD ?
                                  (only_local ? "node_raycast_world_only_local" :
                                                "node_raycast_world") :
                                  (only_local ? "node_raycast_offset_only_local" :
                                                "node_raycast_offset");

  return GPU_stack_link(mat, node, function_name, in, out);
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  return get_output_default(socket_out_->identifier, NodeItem::Type::Any);
}
#endif
NODE_SHADER_MATERIALX_END

}  // namespace nodes::node_shader_raycast_cc

/* node type definition */
void register_node_type_sh_raycast()
{
  namespace file_ns = blender::nodes::node_shader_raycast_cc;

  static blender::bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeRaycast", SH_NODE_RAYCAST);
  ntype.ui_name = "Raycast";
  ntype.ui_description = "Cast rays and retrieve information from the hit point";
  ntype.enum_name_legacy = "MATERIAL_RAYCAST";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.initfunc = file_ns::node_shader_init;
  ntype.add_ui_poll = object_shader_nodes_poll;
  ntype.draw_buttons = file_ns::node_shader_buts;
  ntype.updatefunc = file_ns::node_shader_update;
  ntype.declare = file_ns::node_declare;
  ntype.gpu_fn = file_ns::node_shader_gpu;
  ntype.materialx_fn = file_ns::node_shader_materialx;

  blender::bke::node_register_type(ntype);
}

}  // namespace blender
