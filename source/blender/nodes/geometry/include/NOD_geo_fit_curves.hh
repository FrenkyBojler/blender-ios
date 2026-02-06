/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "DNA_node_types.h"

#include "NOD_socket_items.hh"

namespace blender::nodes {

/**
 * Makes it possible to use various functions (e.g. the ones in `NOD_socket_items.hh`) for field
 * to grid items.
 */
struct FitCurvesItemsAccessor : public socket_items::SocketItemsAccessorDefaults {
  using ItemT = GeometryNodeFitCurvesItem;
  static StructRNA **item_srna;
  static int node_type;
  static constexpr StringRefNull node_idname = "GeometryNodeFitCurves";
  static constexpr bool has_type = true;
  static constexpr bool has_name = true;
  struct operator_idnames {
    static constexpr StringRefNull add_item = "NODE_OT_fit_curves_item_add";
    static constexpr StringRefNull remove_item = "NODE_OT_fit_curves_item_remove";
    static constexpr StringRefNull move_item = "NODE_OT_fit_curves_item_move";
  };
  struct ui_idnames {
    static constexpr StringRefNull list = "NODE_UL_fit_curves_items";
  };
  struct rna_names {
    static constexpr StringRefNull items = "field_items";
    static constexpr StringRefNull active_index = "active_index";
  };

  static socket_items::SocketItemsRef<GeometryNodeFitCurvesItem> get_items_from_node(bNode &node)
  {
    auto &storage = *static_cast<GeometryNodeFitCurves *>(node.storage);
    return {&storage.items, &storage.items_num, &storage.active_index};
  }

  static void copy_item(const GeometryNodeFitCurvesItem &src, GeometryNodeFitCurvesItem &dst)
  {
    dst = src;
    dst.name = BLI_strdup_null(dst.name);
  }

  static void destruct_item(GeometryNodeFitCurvesItem *item)
  {
    MEM_SAFE_DELETE(item->name);
  }

  static void blend_write_item(BlendWriter *writer, const ItemT &item);
  static void blend_read_data_item(BlendDataReader *reader, ItemT &item);

  static eNodeSocketDatatype get_socket_type(const ItemT &item)
  {
    return eNodeSocketDatatype(item.data_type);
  }

  static bool supports_socket_type(const eNodeSocketDatatype socket_type, const int /*ntree_type*/)
  {
    return ELEM(socket_type, SOCK_FLOAT, SOCK_VECTOR, SOCK_RGBA);
  }

  static char **get_name(GeometryNodeFitCurvesItem &item)
  {
    return &item.name;
  }

  static void init_with_socket_type_and_name(bNode &node,
                                             GeometryNodeFitCurvesItem &item,
                                             const eNodeSocketDatatype socket_type,
                                             const char *name)
  {
    auto *storage = static_cast<GeometryNodeFitCurves *>(node.storage);
    item.data_type = socket_type;
    item.identifier = storage->next_identifier++;
    socket_items::set_item_name_and_make_unique<FitCurvesItemsAccessor>(node, item, name);
  }

  static std::string socket_identifier_for_item(const GeometryNodeFitCurvesItem &item)
  {
    return "Field_" + std::to_string(item.identifier);
  }
};

}  // namespace blender::nodes
