/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "NOD_geo_bundle.hh"
#include "NOD_socket_items_blend.hh"
#include "NOD_socket_items_ops.hh"
#include "NOD_socket_items_ui.hh"
#include "NOD_socket_search_link.hh"
#include "NOD_sync_sockets.hh"

#include "BKE_idprop.hh"

#include "BLO_read_write.hh"

#include "NOD_geometry_nodes_bundle.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "NOD_rna_define.hh"
#include "RNA_enum_types.hh"

namespace blender::nodes::node_geo_store_bundle_item_cc {

NODE_STORAGE_FUNCS(NodeStoreBundleItem)

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_default_layout();
  const bNode *node = b.node_or_null();

  b.add_input<decl::Bundle>("Bundle");
  b.add_output<decl::Bundle>("Bundle").align_with_previous();
  b.add_input<decl::String>("Name").optional_label();

  if (node != nullptr) {
    const NodeStoreBundleItem &storage = node_storage(*node);
    const eNodeSocketDatatype socket_type = eNodeSocketDatatype(storage.socket_type);
    b.add_input(socket_type, "Item");
  }
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);
  layout.prop(ptr, "socket_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeStoreBundleItem *data = MEM_callocN<NodeStoreBundleItem>(__func__);
  data->socket_type = SOCK_GEOMETRY;
  node->storage = data;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const bNode &bnode = params.node();
  const NodeStoreBundleItem &storage = node_storage(params.node());

  BundlePtr bundle_ptr = params.extract_input<nodes::BundlePtr>("Bundle");
  if (!bundle_ptr) {
    bundle_ptr = Bundle::create();
  }
  if (!bundle_ptr->is_mutable()) {
    bundle_ptr = bundle_ptr->copy();
  }

  Bundle &bundle = const_cast<Bundle &>(*bundle_ptr);

  const std::string name = params.extract_input<std::string>("Name");
  if (name.empty()) {
    params.set_output("Bundle", std::move(bundle_ptr));
    return;
  }

  bke::SocketValueVariant value = params.extract_input<bke::SocketValueVariant>("Item");
  const bNodeSocket *item_sock = bnode.input_by_identifier("Item");
  if (!item_sock) {
    params.set_output("Bundle", std::move(bundle_ptr));
    return;
  }

  const bke::bNodeSocketType *stype = bke::node_socket_type_find_static(storage.socket_type, 0);
  if (!stype || !stype->geometry_nodes_default_value) {
    params.set_output("Bundle", std::move(bundle_ptr));
    return;
  }

  bundle.add_path_override(name, BundleItemSocketValue{stype, std::move(value)});

  params.set_output("Bundle", std::move(bundle_ptr));
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(
      srna,
      "socket_type",
      "Data Type",
      "",
      rna_enum_node_socket_data_type_items,
      NOD_storage_enum_accessors(socket_type),
      SOCK_GEOMETRY,
      [](bContext * /*C*/, PointerRNA * /*ptr*/, PropertyRNA * /*prop*/, bool *r_free) {
        *r_free = true;
        return enum_items_filter(rna_enum_node_socket_data_type_items,
                                 [](const EnumPropertyItem &item) -> bool {
                                   return ELEM(item.value,
                                               SOCK_FLOAT,
                                               SOCK_INT,
                                               SOCK_BOOLEAN,
                                               SOCK_ROTATION,
                                               SOCK_MATRIX,
                                               SOCK_VECTOR,
                                               SOCK_STRING,
                                               SOCK_RGBA,
                                               SOCK_GEOMETRY,
                                               SOCK_OBJECT,
                                               SOCK_COLLECTION,
                                               SOCK_MATERIAL,
                                               SOCK_IMAGE,
                                               SOCK_MENU,
                                               SOCK_BUNDLE,
                                               SOCK_CLOSURE);
                                 });
      });
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "NodeStoreBundleItem");
  ntype.ui_name = "Store Bundle Item";
  ntype.ui_description = "Store a bundle item by name and data type.";
  ntype.nclass = NODE_CLASS_CONVERTER;
  blender::bke::node_type_storage(ntype,
                                  "NodeStoreBundleItem",
                                  node_free_standard_storage,
                                  node_copy_standard_storage);
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_store_bundle_item_cc
