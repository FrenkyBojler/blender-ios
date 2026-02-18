/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "NOD_geo_bundle.hh"
#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_geometry_nodes_list.hh"
#include "NOD_rna_define.hh"

#include "RNA_enum_types.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_geo_store_bundle_item_cc {

NODE_STORAGE_FUNCS(NodeStoreBundleItem)

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_default_layout();
  const bNode *node = b.node_or_null();

  b.add_input<decl::Bundle>("Bundle");
  b.add_output<decl::Bundle>("Bundle").align_with_previous().propagate_all().reference_pass_all();
  b.add_input<decl::String>("Path").structure_type(StructureType::Dynamic).optional_label();

  if (node != nullptr) {
    const NodeStoreBundleItem &storage = node_storage(*node);
    const eNodeSocketDatatype socket_type = eNodeSocketDatatype(storage.socket_type);
    auto &decl = b.add_input(socket_type, "Item");
    if (storage.structure_type == NODE_INTERFACE_SOCKET_STRUCTURE_TYPE_AUTO) {
      decl.structure_type(StructureType::Dynamic);
    }
    else {
      decl.structure_type(StructureType(storage.structure_type));
    }
  }
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);
  layout.prop(ptr, "socket_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_layout_ex(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);
  layout.prop(ptr, "structure_type", UI_ITEM_NONE, IFACE_("Shape"), ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  auto *storage = MEM_new<NodeStoreBundleItem>(__func__);
  storage->socket_type = SOCK_FLOAT;
  node->storage = storage;
}

static void store_if_valid(Bundle &bundle,
                           const bke::bNodeSocketType &socket_type,
                           const StringRef path,
                           const GPointer item,
                           GeoNodeExecParams &params)
{
  if (!Bundle::is_valid_path(path)) {
    if (!path.is_empty()) {
      params.error_message_add(NodeWarningType::Warning,
                               fmt::format(fmt::runtime(TIP_("Invalid bundle path: {}")), path));
    }
    return;
  }
  if (item.type()->is<bke::SocketValueVariant>()) {
    bundle.add_path_override(path,
                             BundleItemSocketValue{.type = &socket_type,
                                                   .value = *item.get<bke::SocketValueVariant>()});
  }
  else {
    bke::SocketValueVariant value;
    item.type()->copy_construct(item.get(), value.allocate_single(socket_type.type));
    bundle.add_path_override(
        path, BundleItemSocketValue{.type = &socket_type, .value = std::move(value)});
  }
}

static void store_if_valid(Bundle &bundle,
                           const bke::bNodeSocketType &socket_type,
                           const StringRef path,
                           const GMutablePointer item,
                           GeoNodeExecParams &params)
{
  if (!Bundle::is_valid_path(path)) {
    if (!path.is_empty()) {
      params.error_message_add(NodeWarningType::Warning,
                               fmt::format(fmt::runtime(TIP_("Invalid bundle path: {}")), path));
    }
    return;
  }
  if (item.type()->is<bke::SocketValueVariant>()) {
    bundle.add_path_override(
        path,
        BundleItemSocketValue{.type = &socket_type,
                              .value = std::move(*item.get<bke::SocketValueVariant>())});
  }
  else {
    bke::SocketValueVariant value;
    item.type()->move_construct(item.get(), value.allocate_single(socket_type.type));
    bundle.add_path_override(
        path, BundleItemSocketValue{.type = &socket_type, .value = std::move(value)});
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const bNode &bnode = params.node();
  const NodeStoreBundleItem &storage = node_storage(bnode);
  const bke::bNodeSocketType &socket_type = *bke::node_socket_type_find_static(storage.socket_type,
                                                                               0);

  BundlePtr bundle_ptr = params.extract_input<nodes::BundlePtr>("Bundle");
  if (!bundle_ptr) {
    bundle_ptr = Bundle::create();
  }
  Bundle &bundle = bundle_ptr.ensure_mutable_inplace();

  auto path_value = params.extract_input<bke::SocketValueVariant>("Path");
  auto item_value = params.extract_input<bke::SocketValueVariant>("Item");
  if (item_value.is_list()) {
    ListPtr item_list = item_value.extract<ListPtr>();
    const CPPType &cpp_type = item_list->cpp_type();
    if (path_value.is_list()) {
      ListPtr paths_list = create_repeated_list(path_value.extract<ListPtr>(), item_list->size());
      const VArraySpan paths = paths_list->varray<std::string>();

      const auto item_list_data = item_list->data();
      if (const auto *array_data = std::get_if<nodes::List::ArrayData>(&item_list_data)) {
        if (item_list->is_mutable() && array_data->sharing_info->is_mutable()) {
          GMutableSpan items(cpp_type, const_cast<void *>(array_data->data), item_list->size());
          for (const int i : paths.index_range()) {
            store_if_valid(
                bundle, socket_type, paths[i], GMutablePointer(items.type(), items[i]), params);
          }
        }
        else {
          const GSpan items(cpp_type, array_data->data, item_list->size());
          for (const int i : paths.index_range()) {
            store_if_valid(
                bundle, socket_type, paths[i], GPointer(items.type(), items[i]), params);
          }
        }
      }
      else if (const auto *single_data = std::get_if<nodes::List::SingleData>(&item_list_data)) {
        if (item_list->is_mutable() && single_data->sharing_info->is_mutable() &&
            item_list->size() == 1)
        {
          for (const int i : paths.index_range()) {
            store_if_valid(
                bundle,
                socket_type,
                paths[i],
                GMutablePointer(item_list->cpp_type(), const_cast<void *>(single_data->value)),
                params);
          }
        }
        else {
          for (const int i : paths.index_range()) {
            store_if_valid(bundle,
                           socket_type,
                           paths[i],
                           GPointer(item_list->cpp_type(), single_data->value),
                           params);
          }
        }
      }
    }
    else {
      params.error_message_add(NodeWarningType::Error,
                               "\"Path\" must be a list if \"Item\" is a list");
      return;
    }
  }
  else if (item_value.is_single()) {
    if (path_value.is_single()) {
      const std::string path = path_value.extract<std::string>();
      store_if_valid(bundle, socket_type, path, GMutablePointer(&item_value), params);
    }
    else {
      params.error_message_add(NodeWarningType::Error, "\"Path\" must be a single value");
      return;
    }
  }

  params.set_output("Bundle", std::move(bundle_ptr));
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(srna,
                    "socket_type",
                    "Socket Type",
                    "",
                    rna_enum_node_socket_data_type_items,
                    NOD_storage_enum_accessors(socket_type),
                    SOCK_FLOAT,
                    [](bContext * /*C*/, PointerRNA *ptr, PropertyRNA * /*prop*/, bool *r_free) {
                      *r_free = true;
                      const bNodeTree &ntree = *id_cast<const bNodeTree *>(ptr->owner_id);
                      return enum_items_filter(rna_enum_node_socket_data_type_items,
                                               [&](const EnumPropertyItem &item) -> bool {
                                                 return socket_type_supported_in_bundle(
                                                     eNodeSocketDatatype(item.value), ntree.type);
                                               });
                    });
  RNA_def_node_enum(srna,
                    "structure_type",
                    "Structure Type",
                    "What kind of higher order types are expected to flow through this socket",
                    rna_enum_node_socket_structure_type_items,
                    NOD_storage_enum_accessors(structure_type));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "NodeStoreBundleItem");
  ntype.ui_name = "Store Bundle Item";
  ntype.ui_description = "Store a bundle item by path and data type.";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  ntype.draw_buttons_ex = node_layout_ex;
  bke::node_type_storage(
      ntype, "NodeStoreBundleItem", node_free_standard_storage, node_copy_standard_storage);
  bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_store_bundle_item_cc
