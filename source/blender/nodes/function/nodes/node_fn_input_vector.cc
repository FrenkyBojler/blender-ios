/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_function_util.hh"

#include "NOD_geometry_nodes_gizmos.hh"

#include "BKE_node_tree_update.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_fn_input_vector_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();
  PropertySubType subtype = PROP_XYZ;
  int dimensions = 3;
  if (node != nullptr && node->storage != nullptr) {
    const NodeInputVector &storage = *static_cast<const NodeInputVector *>(node->storage);
    subtype = PropertySubType(storage.subtype);
    dimensions = int(storage.vector_socket_dimensions);
    if (dimensions == 0) {
      dimensions = 3;
    }
    CLAMP(dimensions, 2, 4);
  }

  b.add_output<decl::Vector>("Vector").subtype(subtype).dimensions(dimensions).custom_draw(
      [](CustomSocketDrawParams &params) {
    params.layout.alignment_set(ui::LayoutAlign::Expand);
    ui::Layout &row = params.layout.row(true);
    row.column(true).prop(
        &params.socket_ptr, "default_value", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
    if (gizmos::value_node_has_gizmo(params.tree, params.node)) {
      row.prop(&params.socket_ptr, "pin_gizmo", UI_ITEM_NONE, "", ICON_GIZMO);
    }
  });
}

static void node_layout_ex(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  ui::Layout &col = layout.column(false);
  col.use_property_split_set(true);
  col.use_property_decorate_set(false);
  col.prop(ptr, "subtype", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  col.prop(ptr, "vector_socket_dimensions", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  const bNode &bnode = builder.node();
  const bNodeSocket *socket = static_cast<const bNodeSocket *>(bnode.outputs.first);
  const bNodeSocketValueVector *value =
      (socket != nullptr) ? static_cast<const bNodeSocketValueVector *>(socket->default_value) :
                            nullptr;
  const float3 vector = (value != nullptr) ? float3(value->value) : float3(0.0f);
  builder.construct_and_set_matching_fn<mf::CustomMF_Constant<float3>>(vector);
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  NodeInputVector *storage = static_cast<NodeInputVector *>(node->storage);
  bNodeSocket *socket = static_cast<bNodeSocket *>(node->outputs.first);
  if (storage == nullptr || socket == nullptr) {
    return;
  }

  bNodeSocketValueVector *value = static_cast<bNodeSocketValueVector *>(socket->default_value);
  if (value == nullptr) {
    return;
  }

  const bool socket_is_default = value->value[0] == 0.0f && value->value[1] == 0.0f &&
                                 value->value[2] == 0.0f && value->value[3] == 0.0f;
  const bool storage_is_non_default =
      storage->vector[0] != 0.0f || storage->vector[1] != 0.0f || storage->vector[2] != 0.0f;

  /* One-time migration: older files stored the value in node storage, not in the output socket. */
  if (socket_is_default && storage_is_non_default) {
    value->value[0] = storage->vector[0];
    value->value[1] = storage->vector[1];
    value->value[2] = storage->vector[2];
    BKE_ntree_update_tag_node_property(ntree, node);
  }
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeInputVector *data = MEM_new<NodeInputVector>(__func__);
  data->subtype = PROP_XYZ;
  data->vector_socket_dimensions = 3;
  node->storage = data;
}

static void node_register()
{
  static bke::bNodeType ntype;

  fn_cmp_node_type_base(&ntype, "FunctionNodeInputVector", FN_NODE_INPUT_VECTOR);
  ntype.ui_name = "Vector";
  ntype.ui_description = "Provide a vector value that can be connected to other nodes in the tree";
  ntype.enum_name_legacy = "INPUT_VECTOR";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;
  bke::node_type_storage(
      ntype, "NodeInputVector", node_free_standard_storage, node_copy_standard_storage);
  ntype.build_multi_function = node_build_multi_function;
  ntype.draw_buttons_ex = node_layout_ex;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_fn_input_vector_cc
