/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "DNA_node_types.h"

#include "NOD_socket_items.hh"

namespace blender::nodes {

inline bool socket_type_supported_in_memory_input(const eNodeSocketDatatype socket_type,
                                                  const int ntree_type)
{
  return (ELEM(socket_type,
              SOCK_BOOLEAN,
              SOCK_MENU,
              SOCK_INT,
              SOCK_INT_VECTOR,
              SOCK_FLOAT,
              SOCK_VECTOR,
              SOCK_ROTATION,
              SOCK_RGBA,
              SOCK_STRING) ||
              ELEM(socket_type,
              SOCK_OBJECT,
              SOCK_IMAGE,
              SOCK_COLLECTION,
              SOCK_TEXTURE,
              SOCK_MATERIAL,
              SOCK_MATRIX,
              SOCK_BUNDLE,
              SOCK_FONT,
              SOCK_SCENE,
              SOCK_TEXT_ID,
              SOCK_SOUND)) && bke::node_tree_type_supports_socket_type_static(ntree_type, socket_type);
}

inline bool socket_type_supported_in_closure_in_memory_output(const eNodeSocketDatatype socket_type,
                                                              const int ntree_type)
{
  return bke::node_tree_type_supports_socket_type_static(ntree_type, socket_type);
}

struct MemoryZoneInputItemsAccessor : public socket_items::SocketItemsAccessorDefaults {
  using ItemT = NodeMemoryZoneInputItem;
  static StructRNA **item_srna;
  static int node_type;
  static constexpr StringRefNull node_idname = "GeometryNodeMemoryZoneOutput";
  static constexpr bool has_type = true;
  static constexpr bool has_name = true;
  struct operator_idnames {
    static constexpr StringRefNull add_item = "NODE_OT_memory_zone_input_item_add";
    static constexpr StringRefNull remove_item = "NODE_OT_memory_zone_input_item_remove";
    static constexpr StringRefNull move_item = "NODE_OT_memory_zone_input_item_move";
  };
  struct ui_idnames {
    static constexpr StringRefNull list = "DATA_UL_memory_zone_input_state";
  };
  struct rna_names {
    static constexpr StringRefNull items = "input_items";
    static constexpr StringRefNull active_index = "active_input_index";
  };

  static socket_items::SocketItemsRef<ItemT> get_items_from_node(bNode &node)
  {
    auto *storage = static_cast<NodeGeometryMemoryZoneOutput *>(node.storage);
    NodeMemoryZoneInputItems &inputs = storage->input_items;
    return {&inputs.items, &inputs.items_num, &inputs.active_index};
  }

  static void copy_item(const ItemT &src, ItemT &dst)
  {
    dst = src;
    dst.name = BLI_strdup_null(dst.name);
  }

  static void destruct_item(ItemT *item)
  {
    MEM_SAFE_DELETE(item->name);
  }

  static void blend_write_item(BlendWriter *writer, const ItemT &item);
  static void blend_read_data_item(BlendDataReader *reader, ItemT &item);

  static eNodeSocketDatatype get_socket_type(const ItemT &item)
  {
    return item.socket_type;
  }

  static char **get_name(ItemT &item)
  {
    return &item.name;
  }

  static bool supports_socket_type(const eNodeSocketDatatype socket_type, const int ntree_type)
  {
    return socket_type_supported_in_memory_input(socket_type, ntree_type);
  }

  static void init_with_socket_type_and_name(bNode &node,
                                             ItemT &item,
                                             const eNodeSocketDatatype socket_type,
                                             const char *name)
  {
    auto *storage = static_cast<NodeGeometryMemoryZoneOutput *>(node.storage);
    NodeMemoryZoneInputItems &inputs = storage->input_items;
    item.socket_type = socket_type;
    item.identifier = inputs.next_identifier++;
    socket_items::set_item_name_and_make_unique<MemoryZoneInputItemsAccessor>(node, item, name);
  }

  static std::string socket_identifier_for_item(const ItemT &item)
  {
    return "Item_" + std::to_string(item.identifier);
  }
};

struct MemoryZoneOutputItemsAccessor : public socket_items::SocketItemsAccessorDefaults {
  using ItemT = NodeMemoryZoneOutputItem;
  static StructRNA **item_srna;
  static int node_type;
  static constexpr StringRefNull node_idname = "GeometryNodeMemoryZoneOutput";
  static constexpr bool has_type = true;
  static constexpr bool has_name = true;
  struct operator_idnames {
    static constexpr StringRefNull add_item = "NODE_OT_memory_zone_output_item_add";
    static constexpr StringRefNull remove_item = "NODE_OT_memory_zone_output_item_remove";
    static constexpr StringRefNull move_item = "NODE_OT_memory_zone_output_item_move";
  };
  struct ui_idnames {
    static constexpr StringRefNull list = "DATA_UL_memory_zone_output_state";
  };
  struct rna_names {
    static constexpr StringRefNull items = "output_items";
    static constexpr StringRefNull active_index = "active_output_index";
  };

  static socket_items::SocketItemsRef<ItemT> get_items_from_node(bNode &node)
  {
    auto *storage = static_cast<NodeGeometryMemoryZoneOutput *>(node.storage);
    NodeMemoryZoneOutputItems &outputs = storage->output_items;
    return {&outputs.items, &outputs.items_num, &outputs.active_index};
  }

  static void copy_item(const ItemT &src, ItemT &dst)
  {
    dst = src;
    dst.name = BLI_strdup_null(dst.name);
  }

  static void destruct_item(ItemT *item)
  {
    MEM_SAFE_DELETE(item->name);
  }

  static void blend_write_item(BlendWriter *writer, const ItemT &item);
  static void blend_read_data_item(BlendDataReader *reader, ItemT &item);

  static eNodeSocketDatatype get_socket_type(const ItemT &item)
  {
    return item.socket_type;
  }

  static char **get_name(ItemT &item)
  {
    return &item.name;
  }

  static bool supports_socket_type(const eNodeSocketDatatype socket_type, const int ntree_type)
  {
    return socket_type_supported_in_closure_in_memory_output(socket_type, ntree_type);
  }

  static void init_with_socket_type_and_name(bNode &node,
                                             ItemT &item,
                                             const eNodeSocketDatatype socket_type,
                                             const char *name)
  {
    auto *storage = static_cast<NodeGeometryMemoryZoneOutput *>(node.storage);
    NodeMemoryZoneOutputItems &outputs = storage->output_items;
    item.socket_type = socket_type;
    item.identifier = outputs.next_identifier++;
    socket_items::set_item_name_and_make_unique<MemoryZoneOutputItemsAccessor>(node, item, name);
  }

  static std::string socket_identifier_for_item(const ItemT &item)
  {
    return "Item_" + std::to_string(item.identifier);
  }
};


}  // namespace blender::nodes
