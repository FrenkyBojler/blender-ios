/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "DNA_node_types.h"

#include "NOD_socket_items.hh"

namespace blender::nodes {

/**
 * Makes it possible to use various functions (e.g. the ones in `NOD_socket_items.hh`).
 */
struct ForeachBundleReduceItemsAccessor : public socket_items::SocketItemsAccessorDefaults {
  using ItemT = NodeForeachBundleReduceItem;
  static StructRNA **item_srna;
  static int node_type;
  static constexpr StringRefNull node_idname = "NodeForeachBundleOutput";
  static constexpr bool has_type = true;
  static constexpr bool has_name = true;
  struct operator_idnames {
    static constexpr StringRefNull add_item = "NODE_OT_foreach_bundle_reduce_item_add";
    static constexpr StringRefNull remove_item = "NODE_OT_foreach_bundle_reduce_item_remove";
    static constexpr StringRefNull move_item = "NODE_OT_foreach_bundle_reduce_item_move";
  };
  struct ui_idnames {
    static constexpr StringRefNull list = "DATA_UL_foreach_bundle_reduce_items";
  };
  struct rna_names {
    static constexpr StringRefNull items = "reduce_items";
    static constexpr StringRefNull active_index = "active_reduce_index";
  };

  static socket_items::SocketItemsRef<NodeForeachBundleReduceItem> get_items_from_node(bNode &node)
  {
    auto *storage = static_cast<NodeForeachBundleOutput *>(node.storage);
    return {&storage->reduce_items.items,
            &storage->reduce_items.items_num,
            &storage->reduce_items.active_index};
  }

  static void copy_item(const NodeForeachBundleReduceItem &src, NodeForeachBundleReduceItem &dst)
  {
    dst = src;
    dst.name = BLI_strdup_null(dst.name);
  }

  static void destruct_item(NodeForeachBundleReduceItem *item)
  {
    MEM_SAFE_FREE(item->name);
  }

  static void blend_write_item(BlendWriter *writer, const ItemT &item);
  static void blend_read_data_item(BlendDataReader *reader, ItemT &item);

  static eNodeSocketDatatype get_socket_type(const NodeForeachBundleReduceItem &item)
  {
    return eNodeSocketDatatype(item.socket_type);
  }

  static char **get_name(NodeForeachBundleReduceItem &item)
  {
    return &item.name;
  }

  static bool supports_socket_type(const eNodeSocketDatatype socket_type, const int ntree_type)
  {
    return bke::node_tree_type_supports_socket_type_static(ntree_type, socket_type);
  }

  static void init_with_socket_type_and_name(bNode &node,
                                             NodeForeachBundleReduceItem &item,
                                             const eNodeSocketDatatype socket_type,
                                             const char *name)
  {
    auto *storage = static_cast<NodeForeachBundleOutput *>(node.storage);
    item.socket_type = socket_type;
    item.identifier = storage->reduce_items.next_identifier++;
    socket_items::set_item_name_and_make_unique<ForeachBundleReduceItemsAccessor>(
        node, item, name);
  }

  static std::string socket_identifier_for_item(const NodeForeachBundleReduceItem &item)
  {
    return "Reduce_" + std::to_string(item.identifier);
  }
};

struct ForeachBundleGatherItemsAccessor : public socket_items::SocketItemsAccessorDefaults {
  using ItemT = NodeForeachBundleGatherItem;
  static StructRNA **item_srna;
  static int node_type;
  static constexpr StringRefNull node_idname = "NodeForeachBundleOutput";
  static constexpr bool has_type = true;
  static constexpr bool has_name = true;
  struct operator_idnames {
    static constexpr StringRefNull add_item = "NODE_OT_foreach_bundle_gather_item_add";
    static constexpr StringRefNull remove_item = "NODE_OT_foreach_bundle_gather_item_remove";
    static constexpr StringRefNull move_item = "NODE_OT_foreach_bundle_gather_item_move";
  };
  struct ui_idnames {
    static constexpr StringRefNull list = "DATA_UL_foreach_bundle_gather_items";
  };
  struct rna_names {
    static constexpr StringRefNull items = "gather_items";
    static constexpr StringRefNull active_index = "active_gather_index";
  };

  static socket_items::SocketItemsRef<NodeForeachBundleGatherItem> get_items_from_node(bNode &node)
  {
    auto *storage = static_cast<NodeForeachBundleOutput *>(node.storage);
    return {&storage->gather_items.items,
            &storage->gather_items.items_num,
            &storage->gather_items.active_index};
  }

  static void copy_item(const NodeForeachBundleGatherItem &src, NodeForeachBundleGatherItem &dst)
  {
    dst = src;
    dst.name = BLI_strdup_null(dst.name);
  }

  static void destruct_item(NodeForeachBundleGatherItem *item)
  {
    MEM_SAFE_FREE(item->name);
  }

  static void blend_write_item(BlendWriter *writer, const ItemT &item);
  static void blend_read_data_item(BlendDataReader *reader, ItemT &item);

  static eNodeSocketDatatype get_socket_type(const NodeForeachBundleGatherItem &item)
  {
    return eNodeSocketDatatype(item.socket_type);
  }

  static char **get_name(NodeForeachBundleGatherItem &item)
  {
    return &item.name;
  }

  static bool supports_socket_type(const eNodeSocketDatatype socket_type, const int ntree_type)
  {
    return bke::node_tree_type_supports_socket_type_static(ntree_type, socket_type);
  }

  static void init_with_socket_type_and_name(bNode &node,
                                             NodeForeachBundleGatherItem &item,
                                             const eNodeSocketDatatype socket_type,
                                             const char *name)
  {
    auto *storage = static_cast<NodeForeachBundleOutput *>(node.storage);
    item.socket_type = socket_type;
    item.identifier = storage->gather_items.next_identifier++;
    socket_items::set_item_name_and_make_unique<ForeachBundleGatherItemsAccessor>(
        node, item, name);
  }

  static std::string socket_identifier_for_item(const NodeForeachBundleGatherItem &item)
  {
    return "Gather_" + std::to_string(item.identifier);
  }
};

}  // namespace blender::nodes
