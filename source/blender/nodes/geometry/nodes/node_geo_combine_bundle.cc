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

namespace blender::nodes::node_geo_combine_bundle_cc {

NODE_STORAGE_FUNCS(NodeGeometryCombineBundle);

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNodeTree *tree = b.tree_or_null();
  const bNode *node = b.node_or_null();
  if (tree && node) {
    const NodeGeometryCombineBundle &storage = node_storage(*node);
    for (const int i : IndexRange(storage.items_num)) {
      const NodeGeometryCombineBundleItem &item = storage.items[i];
      const eNodeSocketDatatype socket_type = eNodeSocketDatatype(item.socket_type);
      const StringRef name = item.name ? item.name : "";
      const std::string identifier = CombineBundleItemsAccessor::socket_identifier_for_item(item);
      b.add_input(socket_type, name, identifier)
          .socket_name_ptr(&tree->id, CombineBundleItemsAccessor::item_srna, &item, "name");
    }
  }
  b.add_input<decl::Extend>("", "__extend__");
  b.add_output<decl::Bundle>("Bundle").propagate_all().reference_pass_all();
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  auto *storage = MEM_cnew<NodeGeometryCombineBundle>(__func__);
  node->storage = storage;
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  const NodeGeometryCombineBundle &src_storage = node_storage(*src_node);
  auto *dst_storage = MEM_cnew<NodeGeometryCombineBundle>(__func__, src_storage);
  dst_node->storage = dst_storage;

  socket_items::copy_array<CombineBundleItemsAccessor>(*src_node, *dst_node);
}

static void node_free_storage(bNode *node)
{
  socket_items::destruct_array<CombineBundleItemsAccessor>(*node);
  MEM_freeN(node->storage);
}

static bool node_insert_link(bNodeTree *tree, bNode *node, bNodeLink *link)
{
  return socket_items::try_add_item_via_any_extend_socket<CombineBundleItemsAccessor>(
      *tree, *node, *node, *link);
}

static void node_layout_ex(uiLayout *layout, bContext *C, PointerRNA *node_ptr)
{
  bNodeTree &ntree = *reinterpret_cast<bNodeTree *>(node_ptr->owner_id);
  bNode &node = *static_cast<bNode *>(node_ptr->data);

  if (uiLayout *panel = uiLayoutPanel(C, layout, "bundle_items", false, TIP_("Bundle Items"))) {
    socket_items::ui::draw_items_list_with_operators<CombineBundleItemsAccessor>(
        C, panel, ntree, node);
  }
}

static void node_operators()
{
  socket_items::ops::make_common_operators<CombineBundleItemsAccessor>();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  if (!U.experimental.use_bundle_and_closure_nodes) {
    params.set_default_remaining_outputs();
    return;
  }

  const bNode &node = params.node();
  const NodeGeometryCombineBundle &storage = node_storage(node);

  std::shared_ptr<bke::SocketListSignature> socket_list =
      std::make_shared<bke::SocketListSignature>();

  for (const int i : IndexRange(storage.items_num)) {
    const NodeGeometryCombineBundleItem &item = storage.items[i];
    const StringRefNull idname = *bke::node_static_socket_type(item.socket_type, 0);
    const bke::bNodeSocketType *stype = bke::node_socket_type_find(idname);
    socket_list->items.append({stype, item.name ? item.name : ""});
  }

  std::shared_ptr<bke::BundleSignature> bundle_signature = std::make_shared<bke::BundleSignature>(
      std::move(socket_list));

  const int64_t size_in_bytes = bundle_signature->size_in_bytes();
  void *data = MEM_mallocN(size_in_bytes, __func__);

  for (const int i : IndexRange(storage.items_num)) {
    const int64_t offset = bundle_signature->offset(i);
    const CPPType &type = bundle_signature->cpp_type(i);
    void *input_ptr = params.lazy_function_params().try_get_input_data_ptr(i);
    BLI_assert(input_ptr);
    type.move_construct(input_ptr, POINTER_OFFSET(data, offset));
  }

  bke::BundlePtr bundle = bke::BundlePtr(
      MEM_new<bke::Bundle>(__func__, std::move(bundle_signature), data));
  params.set_output("Bundle", std::move(bundle));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeCombineBundle", GEO_NODE_COMBINE_BUNDLE);
  ntype.ui_name = "Combine Bundle";
  ntype.ui_description = "Combine multiple socket values into one.";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.insert_link = node_insert_link;
  ntype.draw_buttons_ex = node_layout_ex;
  ntype.register_operators = node_operators;
  bke::node_type_storage(
      &ntype, "NodeGeometryCombineBundle", node_free_storage, node_copy_storage);
  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_combine_bundle_cc

namespace blender::nodes {

StructRNA *CombineBundleItemsAccessor::item_srna = &RNA_NodeGeometryCombineBundleItem;
int CombineBundleItemsAccessor::node_type = GEO_NODE_COMBINE_BUNDLE;
int CombineBundleItemsAccessor::item_dna_type = SDNA_TYPE_FROM_STRUCT(
    NodeGeometryCombineBundleItem);

void CombineBundleItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  BLO_write_string(writer, item.name);
}

void CombineBundleItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

}  // namespace blender::nodes
