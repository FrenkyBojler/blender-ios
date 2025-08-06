/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <fmt/format.h>

#include "BLI_listbase.h"

#include "BKE_context.hh"

#include "BLO_read_write.hh"

#include "NOD_geo_viewer.hh"
#include "NOD_node_extra_info.hh"
#include "NOD_rna_define.hh"
#include "NOD_socket_items_blend.hh"
#include "NOD_socket_items_ops.hh"
#include "NOD_socket_items_ui.hh"
#include "NOD_socket_search_link.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "ED_node.hh"
#include "ED_viewer_path.hh"

#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_viewer_cc {

NODE_STORAGE_FUNCS(NodeGeometryViewer)

static bool draw_from_viewer_log_value(CustomSocketDrawParams &params,
                                       const geo_eval_log::ViewerNodeLog::Item &viewer_item)
{

  switch (viewer_item.type->type) {
    case SOCK_FLOAT: {
      const auto &value_variant = *static_cast<bke::SocketValueVariant *>(viewer_item.data);
      if (!value_variant.is_single()) {
        return false;
      }
      const float value = value_variant.get<float>();
      const std::string label = fmt::format("{:.5f}", value);
      params.layout.label(label, ICON_NONE);
      break;
    }
    case SOCK_INT: {
      const auto &value_variant = *static_cast<bke::SocketValueVariant *>(viewer_item.data);
      if (!value_variant.is_single()) {
        return false;
      }
      const int value = value_variant.get<int>();
      const std::string label = fmt::format("{}", value);
      params.layout.label(label, ICON_NONE);
      break;
    }
    case SOCK_VECTOR: {
      const auto &value_variant = *static_cast<bke::SocketValueVariant *>(viewer_item.data);
      if (!value_variant.is_single()) {
        return false;
      }
      const float3 value = value_variant.get<float3>();
      uiLayout &col = params.layout.column(true);
      col.label(fmt::format("{}: {:.5f}", IFACE_("X"), value.x), ICON_NONE);
      col.label(fmt::format("{}: {:.5f}", IFACE_("Y"), value.y), ICON_NONE);
      col.label(fmt::format("{}: {:.5f}", IFACE_("Z"), value.z), ICON_NONE);
      break;
    }
    case SOCK_STRING: {
      const auto &value_variant = *static_cast<bke::SocketValueVariant *>(viewer_item.data);
      if (!value_variant.is_single()) {
        return false;
      }
      std::string value = value_variant.get<std::string>();
      /* The node doesn't get wider than that anyway. */
      const int max_display_length = 200;
      if (value.size() > max_display_length) {
        value.resize(max_display_length);
        value.append("...");
      }
      params.layout.label(value, ICON_NONE);
      break;
    }
    case SOCK_BOOLEAN: {
      const auto &value_variant = *static_cast<bke::SocketValueVariant *>(viewer_item.data);
      if (!value_variant.is_single()) {
        return false;
      }
      const bool value = value_variant.get<bool>();
      params.layout.label(value ? IFACE_("True") : IFACE_("False"), ICON_NONE);
      break;
    }
    default: {
      return false;
    }
  }
  return true;
}

static void draw_input_socket(CustomSocketDrawParams &params)
{
  params.r_use_standard_drawing = true;
  SpaceNode *snode = CTX_wm_space_node(&params.C);
  if (!snode) {
    return;
  }
  snode->edittree->ensure_topology_cache();
  const bNodeSocket &socket = params.socket;
  if (!socket.is_directly_linked()) {
    return;
  }
  const geo_eval_log::ContextualGeoTreeLogs geo_tree_logs =
      geo_eval_log::GeoNodesLog::get_contextual_tree_logs(*snode);
  geo_eval_log::GeoTreeLog *tree_log = geo_tree_logs.get_main_tree_log(params.node);
  if (!tree_log) {
    return;
  }
  tree_log->ensure_viewer_node_logs();
  geo_eval_log::ViewerNodeLog *viewer_log = tree_log->viewer_node_logs.lookup_default(
      params.node.identifier, nullptr);
  if (!viewer_log) {
    return;
  }
  const int socket_index = socket.index();
  if (socket_index >= viewer_log->items.size()) {
    return;
  }
  const geo_eval_log::ViewerNodeLog::Item &viewer_item = viewer_log->items[socket_index];
  if (!draw_from_viewer_log_value(params, viewer_item)) {
    return;
  }
  params.r_use_standard_drawing = false;
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  const bNode *node = b.node_or_null();
  const bNodeTree *tree = b.tree_or_null();

  if (!node || !tree) {
    return;
  }

  const NodeGeometryViewer &storage = node_storage(*node);
  for (const int i : IndexRange(storage.items_num)) {
    const NodeGeometryViewerItem &item = storage.items[i];
    const eNodeSocketDatatype socket_type = eNodeSocketDatatype(item.socket_type);
    const StringRef name = item.name ? item.name : "";
    const std::string identifier = GeoViewerItemsAccessor::socket_identifier_for_item(item);
    auto &input_decl = b.add_input(socket_type, name, identifier)
                           .socket_name_ptr(
                               &tree->id, GeoViewerItemsAccessor::item_srna, &item, "name");
    if (socket_type_supports_fields(socket_type)) {
      input_decl.supports_field();
    }
    input_decl.structure_type(StructureType::Dynamic);
    input_decl.custom_draw([](CustomSocketDrawParams &params) { draw_input_socket(params); });
  }

  b.add_input<decl::Extend>("", "__extend__").structure_type(StructureType::Dynamic);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryViewer *data = MEM_callocN<NodeGeometryViewer>(__func__);
  data->data_type = CD_PROP_FLOAT;
  data->domain = int8_t(AttrDomain::Auto);
  node->storage = data;
}

static void node_layout(uiLayout * /*layout*/, bContext * /*C*/, PointerRNA * /*ptr*/) {}

static void node_layout_ex(uiLayout *layout, bContext *C, PointerRNA *ptr)
{
  bNode &node = *ptr->data_as<bNode>();
  bNodeTree &ntree = *reinterpret_cast<bNodeTree *>(ptr->owner_id);

  if (uiLayout *panel = layout->panel(C, "viewer_items", false, IFACE_("Viewer Items"))) {
    socket_items::ui::draw_items_list_with_operators<GeoViewerItemsAccessor>(
        C, panel, ntree, node);
    socket_items::ui::draw_active_item_props<GeoViewerItemsAccessor>(
        ntree, node, [&](PointerRNA *item_ptr) {
          panel->use_property_split_set(true);
          panel->use_property_decorate_set(false);
          panel->prop(item_ptr, "socket_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        });
  }
}

static void node_gather_link_searches(GatherLinkSearchOpParams & /*params*/)
{
  // TODO
}

static void node_extra_info(NodeExtraInfoParams &params)
{
  const auto data_type = eCustomDataType(node_storage(params.node).data_type);
  if (ELEM(data_type, CD_PROP_QUATERNION, CD_PROP_FLOAT4X4)) {
    NodeExtraInfoRow row;
    row.icon = ICON_INFO;
    row.text = TIP_("No color overlay");
    row.tooltip = TIP_(
        "Rotation values can only be displayed with the text overlay in the 3D view");
    params.rows.append(std::move(row));
  }
}

static void node_operators()
{
  socket_items::ops::make_common_operators<GeoViewerItemsAccessor>();
}

static bool node_insert_link(bke::NodeInsertLinkParams &params)
{
  return socket_items::try_add_item_via_any_extend_socket<GeoViewerItemsAccessor>(
      params.ntree, params.node, params.node, params.link);
}

static void node_blend_write(const bNodeTree & /*tree*/, const bNode &node, BlendWriter &writer)
{
  socket_items::blend_write<GeoViewerItemsAccessor>(&writer, node);
}

static void node_blend_read(bNodeTree & /*tree*/, bNode &node, BlendDataReader &reader)
{
  socket_items::blend_read_data<GeoViewerItemsAccessor>(&reader, node);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeViewer", GEO_NODE_VIEWER);
  ntype.ui_name = "Viewer";
  ntype.ui_description = "Display the input data in the Spreadsheet Editor";
  ntype.enum_name_legacy = "VIEWER";
  ntype.nclass = NODE_CLASS_OUTPUT;
  blender::bke::node_type_storage(
      ntype, "NodeGeometryViewer", node_free_standard_storage, node_copy_standard_storage);
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.draw_buttons = node_layout;
  ntype.draw_buttons_ex = node_layout_ex;
  ntype.insert_link = node_insert_link;
  ntype.gather_link_search_ops = node_gather_link_searches;
  ntype.no_muting = true;
  ntype.register_operators = node_operators;
  ntype.get_extra_info = node_extra_info;
  ntype.blend_write_storage_content = node_blend_write;
  ntype.blend_data_read_storage_content = node_blend_read;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_viewer_cc

namespace blender::nodes {

StructRNA *GeoViewerItemsAccessor::item_srna = &RNA_NodeGeometryViewerItem;

void GeoViewerItemsAccessor::blend_write_item(BlendWriter *writer,
                                              const NodeGeometryViewerItem &item)
{
  BLO_write_string(writer, item.name);
}

void GeoViewerItemsAccessor::blend_read_data_item(BlendDataReader *reader,
                                                  NodeGeometryViewerItem &item)
{
  BLO_read_string(reader, &item.name);
}

}  // namespace blender::nodes
