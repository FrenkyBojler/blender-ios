/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "NOD_node_extra_info.hh"
#include "NOD_rna_define.hh"

#include "BLI_string.h"

#include "RNA_access.hh"
#include "RNA_enum_types.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_context_input_cc {

NODE_STORAGE_FUNCS(NodeGeometryContextInput)

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();

  if (node != nullptr) {
    const eNodeSocketDatatype data_type = eNodeSocketDatatype(node_storage(*node).socket_type);
    b.add_output(data_type, "Value");
  }
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryContextInput *data = MEM_cnew<NodeGeometryContextInput>(__func__);
  data->socket_type = SOCK_FLOAT;
  node->storage = data;
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiLayout *col = uiLayoutColumn(layout, false);
  uiItemR(col, ptr, "socket_type", UI_ITEM_NONE, "", ICON_NONE);
  uiItemFullR(col,
              ptr,
              RNA_struct_find_property(ptr, "context_identifier"),
              -1,
              0,
              UI_ITEM_NONE,
              "",
              ICON_NONE,
              IFACE_("Identifier"));
  uiItemFullR(col,
              ptr,
              RNA_struct_find_property(ptr, "context_name"),
              -1,
              0,
              UI_ITEM_NONE,
              "",
              ICON_NONE,
              IFACE_("Name"));
  uiItemFullR(col,
              ptr,
              RNA_struct_find_property(ptr, "context_description"),
              -1,
              0,
              UI_ITEM_NONE,
              "",
              ICON_NONE,
              IFACE_("Description"));
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  const NodeGeometryContextInput &src_storage = node_storage(*src_node);
  auto *dst_storage = MEM_cnew<NodeGeometryContextInput>(__func__, src_storage);
  dst_storage->context_identifier = BLI_strdup_null(dst_storage->context_identifier);
  dst_storage->context_name = BLI_strdup_null(dst_storage->context_name);
  dst_storage->context_description = BLI_strdup_null(dst_storage->context_description);
  dst_node->storage = dst_storage;
}

static void node_free_storage(bNode *node)
{
  NodeGeometryContextInput &storage = node_storage(*node);
  MEM_SAFE_FREE(storage.context_identifier);
  MEM_SAFE_FREE(storage.context_name);
  MEM_SAFE_FREE(storage.context_description);
  MEM_freeN(&storage);
}

static void node_extra_info(NodeExtraInfoParams &params)
{
  const NodeGeometryContextInput &storage = node_storage(params.node);
  if (StringRef(storage.context_identifier).startswith(".")) {
    NodeExtraInfoRow row;
    row.text = IFACE_("Invalid Identifier");
    row.tooltip = TIP_("Identifiers with a dot a prefix are reserved for internal use.");
    row.icon = ICON_ERROR;
    params.rows.append(std::move(row));
  }
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeContextInput", GEO_NODE_CONTEXT_INPUT);
  blender::bke::node_type_storage(
      &ntype, "NodeGeometryContextInput", node_free_storage, node_copy_storage);
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.draw_buttons = node_layout;
  ntype.ui_name = "Context Input";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.no_muting = true;
  ntype.get_extra_info = node_extra_info;
  bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_context_input_cc
