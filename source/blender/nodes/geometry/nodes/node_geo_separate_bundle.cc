/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "NOD_geo_bundle.hh"
#include "NOD_socket_items_ops.hh"
#include "NOD_socket_items_ui.hh"

#include "BLO_read_write.hh"

#include "BKE_geometry_nodes_bundle.hh"

#include "UI_interface.hh"

namespace blender::nodes::node_geo_separate_bundle_cc {

NODE_STORAGE_FUNCS(NodeGeometrySeparateBundle);

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Bundle>("Bundle");
  const bNodeTree *tree = b.tree_or_null();
  const bNode *node = b.node_or_null();
  if (tree && node) {
    const NodeGeometrySeparateBundle &storage = node_storage(*node);
    for (const int i : IndexRange(storage.items_num)) {
      const NodeGeometrySeparateBundleItem &item = storage.items[i];
      const eNodeSocketDatatype socket_type = eNodeSocketDatatype(item.socket_type);
      const StringRef name = item.name ? item.name : "";
      const std::string identifier = SeparateBundleItemsAccessor::socket_identifier_for_item(item);
      b.add_output(socket_type, name, identifier)
          .socket_name_ptr(&tree->id, SeparateBundleItemsAccessor::item_srna, &item, "name")
          .propagate_all()
          .reference_pass_all();
    }
  }
  b.add_output<decl::Extend>("", "__extend__");
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  auto *storage = MEM_cnew<NodeGeometrySeparateBundle>(__func__);
  node->storage = storage;
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  const NodeGeometrySeparateBundle &src_storage = node_storage(*src_node);
  auto *dst_storage = MEM_cnew<NodeGeometrySeparateBundle>(__func__, src_storage);
  dst_node->storage = dst_storage;

  socket_items::copy_array<SeparateBundleItemsAccessor>(*src_node, *dst_node);
}

static void node_free_storage(bNode *node)
{
  socket_items::destruct_array<SeparateBundleItemsAccessor>(*node);
  MEM_freeN(node->storage);
}

static bool node_insert_link(bNodeTree *tree, bNode *node, bNodeLink *link)
{
  return socket_items::try_add_item_via_any_extend_socket<SeparateBundleItemsAccessor>(
      *tree, *node, *node, *link);
}

static void node_layout_ex(uiLayout *layout, bContext *C, PointerRNA *node_ptr)
{
  bNodeTree &ntree = *reinterpret_cast<bNodeTree *>(node_ptr->owner_id);
  bNode &node = *static_cast<bNode *>(node_ptr->data);

  if (uiLayout *panel = uiLayoutPanel(C, layout, "bundle_items", false, TIP_("Bundle Items"))) {
    socket_items::ui::draw_items_list_with_operators<SeparateBundleItemsAccessor>(
        C, panel, ntree, node);
  }
}

static void node_operators()
{
  socket_items::ops::make_common_operators<SeparateBundleItemsAccessor>();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  bke::BundlePtr bundle = params.extract_input<bke::BundlePtr>("Bundle");
  if (!bundle) {
    params.set_default_remaining_outputs();
    return;
  }

  const bNode &node = params.node();
  const NodeGeometrySeparateBundle &storage = node_storage(node);

  bke::SocketListSignature socket_list;
  for (const int i : IndexRange(storage.items_num)) {
    const NodeGeometrySeparateBundleItem &item = storage.items[i];
    const StringRefNull idname = *bke::node_static_socket_type(item.socket_type, 0);
    const bke::bNodeSocketType *stype = bke::node_socket_type_find(idname);
    socket_list.items.append({stype, item.name ? item.name : ""});
  }

  Array<std::optional<int>> mapping(storage.items_num);
  bke::get_socket_list_signature_map(bundle->signature().sockets(), socket_list, mapping);

  lf::Params &lf_params = params.lazy_function_params();

  for (const int i : IndexRange(storage.items_num)) {
    const std::optional<int> &src_index_opt = mapping[i];
    if (!src_index_opt.has_value()) {
      set_default_value_for_output_socket(lf_params, i, node.output_socket(i));
      continue;
    }
    const int src_index = *src_index_opt;
    const CPPType &type = bundle->signature().cpp_type(src_index);
    const void *input_ptr = bundle->data(src_index);
    void *output_ptr = lf_params.get_output_data_ptr(i);
    type.copy_construct(input_ptr, output_ptr);
    lf_params.output_set(i);
  }
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSeparateBundle", GEO_NODE_SEPARATE_BUNDLE);
  ntype.ui_name = "Separate Bundle";
  ntype.ui_description = "Split a bundle into multiple sockets.";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.insert_link = node_insert_link;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons_ex = node_layout_ex;
  ntype.register_operators = node_operators;
  bke::node_type_storage(
      &ntype, "NodeGeometrySeparateBundle", node_free_storage, node_copy_storage);
  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_separate_bundle_cc

namespace blender::nodes {

StructRNA *SeparateBundleItemsAccessor::item_srna = &RNA_NodeGeometrySeparateBundleItem;
int SeparateBundleItemsAccessor::node_type = GEO_NODE_SEPARATE_BUNDLE;
int SeparateBundleItemsAccessor::item_dna_type = SDNA_TYPE_FROM_STRUCT(
    NodeGeometrySeparateBundleItem);

void SeparateBundleItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  BLO_write_string(writer, item.name);
}

void SeparateBundleItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

}  // namespace blender::nodes
