/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "DNA_node_types.h"

#include "NOD_socket_items.hh"

#include "BKE_customdata.hh"

namespace blender::nodes {

struct AttributeToListItemsAccessor : public socket_items::SocketItemsAccessorDefaults {
  using ItemT = NodeGeometryAttributeToListItem;
  static StructRNA **item_srna;
  static int node_type;
  static constexpr StringRefNull node_idname = "GeometryNodeAttributeToList";
  static constexpr bool has_type = true;
  static constexpr bool has_name = true;
  struct operator_idnames {
    static constexpr StringRefNull add_item = "NODE_OT_attribute_to_list_item_add";
    static constexpr StringRefNull remove_item = "NODE_OT_attribute_to_list_item_remove";
    static constexpr StringRefNull move_item = "NODE_OT_attribute_to_list_item_move";
  };
  struct ui_idnames {
    static constexpr StringRefNull list = "NODE_UL_attribute_to_list_items_list";
  };
  struct rna_names {
    static constexpr StringRefNull items = "attribute_to_list_items";
    static constexpr StringRefNull active_index = "active_index";
  };

  static socket_items::SocketItemsRef<NodeGeometryAttributeToListItem> get_items_from_node(
      bNode &node)
  {
    auto *storage = static_cast<NodeGeometryAttributeToList *>(node.storage);
    return {&storage->items, &storage->items_num, &storage->active_index};
  }

  static void copy_item(const NodeGeometryAttributeToListItem &src,
                        NodeGeometryAttributeToListItem &dst)
  {
    dst = src;
    dst.name = BLI_strdup_null(dst.name);
  }

  static void destruct_item(NodeGeometryAttributeToListItem *item)
  {
    MEM_SAFE_DELETE(item->name);
  }

  static void blend_write_item(BlendWriter *writer, const ItemT &item);
  static void blend_read_data_item(BlendDataReader *reader, ItemT &item);

  static eNodeSocketDatatype get_socket_type(const NodeGeometryAttributeToListItem &item)
  {
    return eNodeSocketDatatype(item.socket_type);
  }

  static char **get_name(NodeGeometryAttributeToListItem &item)
  {
    return &item.name;
  }

  static bool supports_socket_type(const eNodeSocketDatatype socket_type, const int /*ntree_type*/)
  {
    return socket_type_supports_fields(socket_type);
  }

  static void init_with_socket_type_and_name(bNode &node,
                                             NodeGeometryAttributeToListItem &item,
                                             const eNodeSocketDatatype socket_type,
                                             const char *name)
  {
    auto *storage = static_cast<NodeGeometryAttributeToList *>(node.storage);
    item.socket_type = socket_type;
    item.identifier = storage->next_identifier++;
    socket_items::set_item_name_and_make_unique<AttributeToListItemsAccessor>(node, item, name);
  }

  static std::string socket_identifier_for_item(const NodeGeometryAttributeToListItem &item)
  {
    return "Item_" + std::to_string(item.identifier);
  }
};

}  // namespace blender::nodes
