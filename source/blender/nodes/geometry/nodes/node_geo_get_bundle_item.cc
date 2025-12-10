/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "ED_screen.hh"

#include "NOD_geo_bundle.hh"
#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_rna_define.hh"
#include "NOD_socket_items_blend.hh"
#include "NOD_socket_items_ui.hh"
#include "NOD_sync_sockets.hh"

#include "BKE_idprop.hh"

#include "BLO_read_write.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_enum_types.hh"

#include <fmt/format.h>

namespace blender::nodes::node_geo_get_bundle_item_cc {

NODE_STORAGE_FUNCS(NodeGeometryGetBundleItem)

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_default_layout();
  const bNode *node = b.node_or_null();

  b.add_input<decl::Bundle>("Bundle");
  b.add_output<decl::Bundle>("Bundle").align_with_previous();
  if (node != nullptr) {
    const NodeGeometryGetBundleItem &storage = node_storage(*node);
    const eNodeSocketDatatype data_type = eNodeSocketDatatype(storage.data_type);
    b.add_output(data_type, "Item");
  }
  b.add_output<decl::Bool>("Exists");
  b.add_input<decl::String>("Name").optional_label();
  b.add_input<decl::Bool>("Remove");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);
  layout.prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryGetBundleItem *data = MEM_callocN<NodeGeometryGetBundleItem>(__func__);
  data->data_type = SOCK_GEOMETRY;
  node->storage = data;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const bNode &node = params.node();
  const NodeGeometryGetBundleItem &storage = node_storage(node);

  nodes::BundlePtr bundle = params.extract_input<nodes::BundlePtr>("Bundle");
  if (!bundle) {
    params.set_default_remaining_outputs();
    return;
  }

  const std::string name = params.extract_input<std::string>("Name");
  const bool remove = params.extract_input<bool>("Remove");

  if (name.empty()) {
    params.set_output("Bundle", std::move(bundle));
    params.set_default_remaining_outputs();
    return;
  }

  const BundleItemValue *value = bundle->lookup_path(name);
  if (!value) {
    params.set_output("Bundle", std::move(bundle));
    params.set_default_remaining_outputs();
    return;
  }
  const auto *socket_value = std::get_if<BundleItemSocketValue>(&value->value);
  if (!socket_value) {
    params.error_message_add(
        NodeWarningType::Error,
        fmt::format("{}: \"{}\"", TIP_("Cannot get internal value from bundle"), name));
    params.set_output("Bundle", std::move(bundle));
    params.set_default_remaining_outputs();
    return;
  }

  const bke::bNodeSocketType *stype = bke::node_socket_type_find_static(storage.data_type, 0);
  SocketValueVariant output_value = std::move(socket_value->value);
  if (socket_value->type->type != stype->type) {
    params.set_output("Bundle", std::move(bundle));
    params.set_default_remaining_outputs();
    return;
  }

  if (remove) {
    if (!bundle->is_mutable()) {
      bundle = bundle->copy();
    }
    bundle->tag_ensured_mutable();
    const_cast<Bundle &>(*bundle).remove(name);
  }

  params.set_output("Bundle", std::move(bundle));
  params.set_output("Item", std::move(output_value));
  params.set_output("Exists", true);
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(
      srna,
      "data_type",
      "Data Type",
      "",
      rna_enum_node_socket_data_type_items,
      NOD_storage_enum_accessors(data_type),
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

  geo_node_type_base(&ntype, "NodeGetBundleItem");
  ntype.ui_name = "Get Bundle Item";
  ntype.ui_description = "Retrieve a bundle item by name and data type.";
  ntype.nclass = NODE_CLASS_CONVERTER;
  blender::bke::node_type_storage(
      ntype, "NodeGeometryGetBundleItem", node_free_standard_storage, node_copy_standard_storage);
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_get_bundle_item_cc
