/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"

#include "FN_multi_function_builder.hh"

#include "NOD_geometry_nodes_gizmos.hh"
#include "NOD_multi_function.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {

namespace nodes::node_shader_value_cc {

static PropertySubType sh_value_subtype_from_custom1(const bNode *node)
{
  if (node == nullptr) {
    return PROP_NONE;
  }

  /* Keep in sync with `rna_enum_shader_node_value_subtype_items` in
   * `source/blender/makesrna/intern/rna_nodetree.cc`.
   */
  switch (node->custom1) {
    case PROP_PERCENTAGE:
      return PROP_PERCENTAGE;
    case PROP_FACTOR:
      return PROP_FACTOR;
    case (PROP_MASS & 0x7FFF):
      return PROP_MASS;
    case (PROP_ANGLE & 0x7FFF):
      return PROP_ANGLE;
    case (PROP_TIME & 0x7FFF):
      return PROP_TIME;
    case 50:
      return PROP_TIME_ABSOLUTE;
    case (PROP_DISTANCE & 0x7FFF):
      return PROP_DISTANCE;
    case (PROP_POWER & 0x7FFF):
      return PROP_POWER;
    case (PROP_TEMPERATURE & 0x7FFF):
      return PROP_TEMPERATURE;
    case (PROP_WAVELENGTH & 0x7FFF):
      return PROP_WAVELENGTH;
    case (PROP_COLOR_TEMPERATURE & 0x7FFF):
      return PROP_COLOR_TEMPERATURE;
    case (PROP_FREQUENCY & 0x7FFF):
      return PROP_FREQUENCY;
    default:
      return PROP_NONE;
  }
}

static void sh_node_value_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();
  const PropertySubType subtype = sh_value_subtype_from_custom1(node);

  b.add_output<decl::Float>("Value").subtype(subtype).custom_draw([](CustomSocketDrawParams &params) {
    params.layout.alignment_set(ui::LayoutAlign::Expand);
    ui::Layout &row = params.layout.row(true);
    row.prop(&params.socket_ptr, "default_value", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
    if (gizmos::value_node_has_gizmo(params.tree, params.node)) {
      row.prop(&params.socket_ptr, "pin_gizmo", UI_ITEM_NONE, "", ICON_GIZMO);
    }
  });
}

static void sh_node_value_layout_ex(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  ui::Layout &col = layout.column(false);
  col.use_property_split_set(true);
  col.use_property_decorate_set(false);
  col.prop(ptr, "subtype", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
}

static int gpu_shader_value(GPUMaterial *mat,
                            bNode *node,
                            bNodeExecData * /*execdata*/,
                            GPUNodeStack * /*in*/,
                            GPUNodeStack *out)
{
  const bNodeSocket *socket = static_cast<bNodeSocket *>(node->outputs.first);
  float value = static_cast<bNodeSocketValueFloat *>(socket->default_value)->value;
  return GPU_link(mat, "set_value", GPU_uniform(&value), &out->link);
}

static void sh_node_value_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  const bNodeSocket *bsocket = static_cast<bNodeSocket *>(builder.node().outputs.first);
  const bNodeSocketValueFloat *value = static_cast<const bNodeSocketValueFloat *>(
      bsocket->default_value);
  builder.construct_and_set_matching_fn<mf::CustomMF_Constant<float>>(value->value);
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  NodeItem value = get_output_default("Value", NodeItem::Type::Float);
  return create_node("constant", NodeItem::Type::Float, {{"value", value}});
}
#endif
NODE_SHADER_MATERIALX_END

}  // namespace nodes::node_shader_value_cc

void register_node_type_sh_value()
{
  namespace file_ns = nodes::node_shader_value_cc;

  static bke::bNodeType ntype;

  common_node_type_base(&ntype, "ShaderNodeValue", SH_NODE_VALUE);
  ntype.ui_name = "Value";
  ntype.ui_description = "Input numerical values to other nodes in the tree";
  ntype.enum_name_legacy = "VALUE";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::sh_node_value_declare;
  ntype.gpu_fn = file_ns::gpu_shader_value;
  ntype.build_multi_function = file_ns::sh_node_value_build_multi_function;
  ntype.materialx_fn = file_ns::node_shader_materialx;
  ntype.draw_buttons_ex = file_ns::sh_node_value_layout_ex;

  bke::node_register_type(ntype);
}

}  // namespace blender
