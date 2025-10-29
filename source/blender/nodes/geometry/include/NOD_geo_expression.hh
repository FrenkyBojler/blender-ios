/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <optional>

#include "DNA_node_types.h"

#include "NOD_socket_items.hh"

#include "RNA_access.hh"

namespace blender::nodes {

struct ExpressionInputItemsAccessor : public socket_items::SocketItemsAccessorDefaults {
  using ItemT = NodeExpressionInputItem;
  static StructRNA *item_srna;
  static int node_type;
  static constexpr StringRefNull node_idname = "NodeExpression";
  static constexpr bool has_type = true;
  static constexpr bool has_name = true;
  struct operator_idnames {
    static constexpr StringRefNull add_item = "NODE_OT_expression_input_item_add";
    static constexpr StringRefNull remove_item = "NODE_OT_expression_input_item_remove";
    static constexpr StringRefNull move_item = "NODE_OT_expression_input_item_move";
  };
  struct ui_idnames {
    static constexpr StringRefNull list = "DATA_UL_expression_input_state";
  };
  struct rna_names {
    static constexpr StringRefNull items = "input_items";
    static constexpr StringRefNull active_index = "active_input_index";
  };

  static socket_items::SocketItemsRef<NodeExpressionInputItem> get_items_from_node(bNode &node)
  {
    auto *storage = static_cast<NodeExpression *>(node.storage);
    return {&storage->input_items.items,
            &storage->input_items.items_num,
            &storage->input_items.active_index};
  }

  static void copy_item(const NodeExpressionInputItem &src, NodeExpressionInputItem &dst)
  {
    dst = src;
    dst.name = BLI_strdup_null(dst.name);
  }

  static void destruct_item(NodeExpressionInputItem *item)
  {
    MEM_SAFE_FREE(item->name);
  }

  static void blend_write_item(BlendWriter *writer, const ItemT &item);
  static void blend_read_data_item(BlendDataReader *reader, ItemT &item);

  static eNodeSocketDatatype get_socket_type(const NodeExpressionInputItem &item)
  {
    return eNodeSocketDatatype(item.socket_type);
  }

  static char **get_name(NodeExpressionInputItem &item)
  {
    return &item.name;
  }

  static bool supports_socket_type(const eNodeSocketDatatype socket_type, const int ntree_type)
  {
    switch (ntree_type) {
      case NTREE_GEOMETRY:
      case NTREE_SHADER:
        return ELEM(socket_type,
                    SOCK_FLOAT,
                    SOCK_VECTOR,
                    SOCK_RGBA,
                    SOCK_BOOLEAN,
                    SOCK_ROTATION,
                    SOCK_MATRIX,
                    SOCK_INT,
                    SOCK_STRING);
      default:
        return false;
    }
  }

  static void init_with_socket_type_and_name(bNode &node,
                                             NodeExpressionInputItem &item,
                                             const eNodeSocketDatatype socket_type,
                                             const char *name)
  {
    auto *storage = static_cast<NodeExpression *>(node.storage);
    item.socket_type = socket_type;
    item.identifier = storage->input_items.next_identifier++;
    socket_items::set_item_name_and_make_unique<ExpressionInputItemsAccessor>(node, item, name);
  }

  static std::string socket_identifier_for_item(const NodeExpressionInputItem &item)
  {
    return "InputItem_" + std::to_string(item.identifier);
  }
};

struct ExpressionOutputItemsAccessor : public socket_items::SocketItemsAccessorDefaults {
  using ItemT = NodeExpressionOutputItem;
  static StructRNA *item_srna;
  static int node_type;
  static constexpr StringRefNull node_idname = "NodeExpression";
  static constexpr bool has_type = true;
  static constexpr bool has_name = false;
  struct operator_idnames {
    static constexpr StringRefNull add_item = "NODE_OT_expression_output_item_add";
    static constexpr StringRefNull remove_item = "NODE_OT_expression_output_item_remove";
    static constexpr StringRefNull move_item = "NODE_OT_expression_output_item_move";
  };
  struct ui_idnames {
    static constexpr StringRefNull list = "DATA_UL_expression_output_items";
  };
  struct rna_names {
    static constexpr StringRefNull items = "output_items";
    static constexpr StringRefNull active_index = "active_output_index";
  };

  static socket_items::SocketItemsRef<NodeExpressionOutputItem> get_items_from_node(bNode &node)
  {
    auto *storage = static_cast<NodeExpression *>(node.storage);
    return {&storage->output_items.items,
            &storage->output_items.items_num,
            &storage->output_items.active_index};
  }

  static void copy_item(const NodeExpressionOutputItem &src, NodeExpressionOutputItem &dst)
  {
    dst = src;
  }

  static void destruct_item(NodeExpressionOutputItem * /*item*/) {}

  static void blend_write_item(BlendWriter *writer, const ItemT &item);
  static void blend_read_data_item(BlendDataReader *reader, ItemT &item);

  static eNodeSocketDatatype get_socket_type(const NodeExpressionOutputItem &item)
  {
    return eNodeSocketDatatype(item.socket_type);
  }

  static char **get_name(NodeExpressionOutputItem &item)
  {
    return &item.name;
  }

  static bool supports_socket_type(const eNodeSocketDatatype socket_type, const int ntree_type)
  {
    return ExpressionInputItemsAccessor::supports_socket_type(socket_type, ntree_type);
  }

  static void init_with_socket_type_and_name(bNode &node,
                                             NodeExpressionOutputItem &item,
                                             const eNodeSocketDatatype socket_type,
                                             const char *name)
  {
    auto *storage = static_cast<NodeExpression *>(node.storage);
    item.socket_type = socket_type;
    item.identifier = storage->output_items.next_identifier++;
    socket_items::set_item_name_and_make_unique<ExpressionOutputItemsAccessor>(node, item, name);
  }

  static std::string socket_identifier_for_item(const NodeExpressionOutputItem &item)
  {
    return "ExpressionItem_" + std::to_string(item.identifier);
  }
};

}  // namespace blender::nodes
