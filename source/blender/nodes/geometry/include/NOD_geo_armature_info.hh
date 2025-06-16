/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "DNA_node_types.h"

#include "NOD_socket_items.hh"

namespace blender::nodes {

/**
 * Makes it possible to use various functions (e.g. the ones in `NOD_socket_items.hh`) for index
 * switch items.
 */
struct ArmatureInfoItemsAccessor : public socket_items::SocketItemsAccessorDefaults {
  using ItemT = ArmatureInfoItem;
  static StructRNA *item_srna;
  static int node_type;
  static constexpr StringRefNull node_idname = "GeometryNodeArmatureInfo";
  static constexpr bool has_type = false;
  static constexpr bool has_name = false;

  static socket_items::SocketItemsRef<ArmatureInfoItem> get_items_from_node(bNode &node)
  {
    auto &storage = *static_cast<NodeGeometryArmatureInfo *>(node.storage);
    return {&storage.items, &storage.items_num, nullptr};
  }

  static void copy_item(const ArmatureInfoItem &src, ArmatureInfoItem &dst)
  {
    dst = src;
  }

  static void destruct_item(ArmatureInfoItem * /*item*/) {}

  static void blend_write_item(BlendWriter *writer, const ItemT &item);
  static void blend_read_data_item(BlendDataReader *reader, ItemT &item);

  static void init(bNode &node, ArmatureInfoItem &item)
  {
    auto &storage = *static_cast<NodeGeometryArmatureInfo *>(node.storage);
    item.identifier = storage.next_identifier++;
  }

  static std::string socket_identifier_for_item(const ArmatureInfoItem &item)
  {
    return "Item_" + std::to_string(item.identifier);
  }
};

}  // namespace blender::nodes
