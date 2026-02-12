/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"
#include "node_geometry_util.hh"

#include "NOD_geo_bundle.hh"
#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_rna_define.hh"

#include "RNA_enum_types.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include <fmt/format.h>

#include <algorithm>

namespace blender::nodes::node_geo_get_bundle_item_cc {

NODE_STORAGE_FUNCS(NodeGetBundleItem)

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_default_layout();
  const bNode *node = b.node_or_null();

  b.add_input<decl::Bundle>("Bundle");
  b.add_output<decl::Bundle>("Bundle").align_with_previous().propagate_all().reference_pass_all();
  if (node != nullptr) {
    const NodeGetBundleItem &storage = node_storage(*node);
    const eNodeSocketDatatype socket_type = eNodeSocketDatatype(storage.socket_type);
    auto &decl = b.add_output(socket_type, "Item");
    if (storage.structure_type == NODE_INTERFACE_SOCKET_STRUCTURE_TYPE_AUTO) {
      decl.structure_type(StructureType::Dynamic);
    }
    else {
      decl.structure_type(StructureType(storage.structure_type));
    }
  }
  b.add_output<decl::Bool>("Exists").structure_type(StructureType::Dynamic);
  b.add_input<decl::String>("Path").structure_type(StructureType::Dynamic).optional_label();
  b.add_input<decl::Bool>("Remove").structure_type(StructureType::Dynamic);
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
  auto *storage = MEM_new<NodeGetBundleItem>(__func__);
  storage->socket_type = SOCK_FLOAT;
  node->storage = storage;
}

static ListPtr optimized_list_from_socket_values(Array<bke::SocketValueVariant> &&values,
                                                 const eNodeSocketDatatype data_type)
{
  if (!std::ranges::all_of(values,
                           [](const bke::SocketValueVariant &value) { return value.is_single(); }))
  {
    return List::from_container(std::move(values));
  }

  const CPPType &type = *bke::socket_type_to_geo_nodes_base_cpp_type(data_type);
  GArray<> array(type, values.size(), NoInitialization());
  threading::parallel_for(values.index_range(), 128, [&](const IndexRange range) {
    for (const int list_i : range) {
      void *closure_result = const_cast<void *>(values[list_i].get_single_ptr_raw());
      type.move_construct(closure_result, array[list_i]);
    }
  });
  return List::from_garray(std::move(array));
}

struct GetItemsResult {
  Array<bke::SocketValueVariant> values;
  Array<bool> exists;
};
[[nodiscard]] static GetItemsResult get_items(BundlePtr &bundle,
                                              const bke::bNodeSocketType &socket_type,
                                              const Span<std::string> paths,
                                              const VArray<bool> &remove,
                                              GeoNodeExecParams &params)
{
  /* Check paths are valid. */
  IndexMaskMemory memory;
  const IndexMask valid_paths = IndexMask::from_predicate(
      paths.index_range(), GrainSize(128), memory, [&](const int64_t i) {
        return Bundle::is_valid_path(paths[i]);
      });
  const IndexMask invalid_paths = valid_paths.complement(paths.index_range(), memory);
  if (!invalid_paths.is_empty()) {
    invalid_paths.foreach_index(GrainSize(64), [&](const int64_t i) {
      params.error_message_add(
          NodeWarningType::Warning,
          fmt::format(fmt::runtime(TIP_("Invalid bundle path: {}")), paths[i]));
    });
  }

  /* Look up items from bundle. */
  Array<bool> exists;
  Array<const BundleItemValue *, 16> items(paths.size());
  const IndexMask found_paths = IndexMask::from_predicate(
      valid_paths, GrainSize(64), memory, [&](const int64_t i) {
        items[i] = bundle->lookup_path(paths[i]);
        return items[i] != nullptr;
      });
  if (found_paths.size() != valid_paths.size()) {
    const IndexMask not_found_paths = found_paths.complement(valid_paths, memory);
    if (params.output_is_required("Exists")) {
      exists.reinitialize(paths.size());
      found_paths.to_bools(exists);
    }
    else {
      not_found_paths.foreach_index([&](const int64_t i) {
        params.error_message_add(
            NodeWarningType::Warning,
            fmt::format(fmt::runtime(TIP_("Bundle path not found: {}")), paths[i]));
      });
    }
  }

  /* Check that items are socket values rather than internal data which can't be outputed. */
  const IndexMask socket_value_paths = IndexMask::from_predicate(
      found_paths, GrainSize(2048), memory, [&](const int64_t i) {
        return std::get_if<BundleItemSocketValue>(&items[i]->value);
      });
  if (socket_value_paths.size() != found_paths.size()) {
    const IndexMask not_found_paths = socket_value_paths.complement(found_paths, memory);
    not_found_paths.foreach_index([&](const int64_t i) {
      params.error_message_add(
          NodeWarningType::Warning,
          fmt::format(fmt::runtime(TIP_("Cannot get internal value from bundle: {}")), paths[i]));
    });
  }

  /* Convert socket values to the selected type. */
  Array<bke::SocketValueVariant> output_values(paths.size(), NoInitialization());
  const IndexMask converted_paths = IndexMask::from_predicate(
      socket_value_paths, GrainSize(2048), memory, [&](const int64_t i) {
        const auto &item_value = std::get<BundleItemSocketValue>(items[i]->value);
        std::optional<SocketValueVariant> converted = implicitly_convert_socket_value(
            *item_value.type, item_value.value, socket_type);
        if (!converted) {
          return false;
        }
        new (&output_values[i]) bke::SocketValueVariant(std::move(*converted));
        return true;
      });
  if (converted_paths.size() != socket_value_paths.size()) {
    const IndexMask not_converted_paths = converted_paths.complement(socket_value_paths, memory);
    not_converted_paths.foreach_index([&](const int64_t i) {
      params.error_message_add(
          NodeWarningType::Warning,
          fmt::format(fmt::runtime(TIP_("Cannot convert item to selected type: {}")), paths[i]));
    });
  }

  /* Fill outputs for previous failures with default values. */
  converted_paths.complement(paths.index_range(), memory).foreach_index([&](const int64_t i) {
    new (&output_values[i]) bke::SocketValueVariant(*socket_type.geometry_nodes_default_value);
  });

  const IndexMask remove_mask = IndexMask::from_bools(valid_paths, remove, memory);
  if (!remove_mask.is_empty()) {
    Bundle &bundle_mut = bundle.ensure_mutable_inplace();
    remove_mask.foreach_index([&](const int64_t i) { bundle_mut.remove_path(paths[i]); });
  }

  return GetItemsResult{
      .values = std::move(output_values),
      .exists = std::move(exists),
  };
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const bNode &node = params.node();
  const NodeGetBundleItem &storage = node_storage(node);

  nodes::BundlePtr bundle = params.extract_input<nodes::BundlePtr>("Bundle");
  if (!bundle) {
    params.set_default_remaining_outputs();
    return;
  }

  const bke::bNodeSocketType &socket_type = *bke::node_socket_type_find_static(storage.socket_type,
                                                                               0);

  auto path_value = params.extract_input<bke::SocketValueVariant>("Path");
  auto remove_value = params.extract_input<bke::SocketValueVariant>("Remove");
  if (path_value.is_list()) {
    ListPtr paths_list = path_value.extract<ListPtr>();
    const VArraySpan paths = paths_list->varray<std::string>();
    GetItemsResult result;
    if (remove_value.is_list()) {
      const ListPtr remove_list = remove_value.extract<ListPtr>();
      result = get_items(bundle, socket_type, paths, remove_list->varray<bool>(), params);
    }
    else if (remove_value.is_single()) {
      const bool remove = remove_value.get<bool>();
      result = get_items(
          bundle, socket_type, paths, VArray<bool>::from_single(remove, paths.size()), params);
    }
    params.set_output(
        "Item", optimized_list_from_socket_values(std::move(result.values), socket_type.type));
    if (!result.exists.is_empty()) {
      params.set_output("Exists", List::from_container(std::move(result.exists)));
    }
  }
  else if (path_value.is_single()) {
    params.set_output("Bundle", std::move(bundle));
    params.set_default_remaining_outputs();
    if (remove_value.is_single()) {
      const std::string path = path_value.extract<std::string>();
      const bool remove = remove_value.extract<bool>();
      GetItemsResult result = get_items(
          bundle, socket_type, {path}, VArray<bool>::from_single(remove, 1), params);
      params.set_output("Item", std::move(result.values.first()));
      if (!result.exists.is_empty()) {
        params.set_output("Exists", result.exists.first());
      }
    }
    else {
      params.error_message_add(NodeWarningType::Error, "\"Remove\" must be a single value");
    }
  }
  else {
    params.error_message_add(NodeWarningType::Error, "Path must be a single value or list");
  }

  params.set_output("Bundle", std::move(bundle));
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(srna,
                    "socket_type",
                    "Socket Type",
                    "Value may be implicitly converted if the type does not match",
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

  geo_node_type_base(&ntype, "NodeGetBundleItem");
  ntype.ui_name = "Get Bundle Item";
  ntype.ui_description = "Retrieve a bundle item by path.";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  ntype.draw_buttons_ex = node_layout_ex;
  bke::node_type_storage(
      ntype, "NodeGetBundleItem", node_free_standard_storage, node_copy_standard_storage);
  bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_get_bundle_item_cc
