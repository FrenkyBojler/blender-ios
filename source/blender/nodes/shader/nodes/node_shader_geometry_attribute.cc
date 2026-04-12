/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

#include "node_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"

namespace blender {

namespace nodes::node_shader_geometry_attribute_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();
  if (node == nullptr) {
    return;
  }
  const auto data_type = eCustomDataType(node->custom1);
  b.add_output(data_type, "Value"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);
  layout.prop(ptr, "node_tree", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
}

static int node_shader_gpu_attribute(GPUMaterial *mat,
                                     bNode *node,
                                     bNodeExecData * /*execdata*/,
                                     GPUNodeStack *in,
                                     GPUNodeStack *out)
{
  NodeShaderAttribute *attr = static_cast<NodeShaderAttribute *>(node->storage);
  bool is_varying = attr->type == SHD_ATTRIBUTE_GEOMETRY;
  float attr_hash = 0.0f;

  GPUNodeLink *cd_attr;

  if (is_varying) {
    cd_attr = GPU_attribute(mat, CD_AUTO_FROM_NAME, attr->name);

    if (STREQ(attr->name, "color")) {
      GPU_link(mat, "node_attribute_color", cd_attr, &cd_attr);
    }
    else if (STREQ(attr->name, "temperature")) {
      GPU_link(mat, "node_attribute_temperature", cd_attr, &cd_attr);
    }
  }
  else if (attr->type == SHD_ATTRIBUTE_VIEW_LAYER) {
    cd_attr = GPU_layer_attribute(mat, attr->name);
  }
  else {
    cd_attr = GPU_uniform_attribute(mat,
                                    attr->name,
                                    attr->type == SHD_ATTRIBUTE_INSTANCER,
                                    reinterpret_cast<uint32_t *>(&attr_hash));

    GPU_link(mat, "node_attribute_uniform", cd_attr, GPU_constant(&attr_hash), &cd_attr);
  }

  GPU_stack_link(mat, node, "node_attribute", in, out, cd_attr);

  if (is_varying) {

    for (const auto [i, sock] : node->outputs.enumerate()) {
      node_shader_gpu_bump_tex_coord(mat, node, &out[i].link);
    }
  }

  return 1;
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = CD_PROP_FLOAT;
  node->id = nullptr;
}

}  // namespace nodes::node_shader_geometry_attribute_cc

/* node type definition */
void register_node_type_sh_geometry_attribute()
{
  namespace file_ns = nodes::node_shader_geometry_attribute_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeGeometryAttribute"_ustr);
  ntype.ui_name = "Geometry Attribute";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.initfunc = file_ns::node_init;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_layout;
  bke::node_type_storage(
      ntype, "ShaderNodeGeometryAttribute", node_free_standard_storage, node_copy_standard_storage);
  ntype.gpu_fn = file_ns::node_shader_gpu_attribute;

  bke::node_register_type(ntype);
}

}  // namespace blender
