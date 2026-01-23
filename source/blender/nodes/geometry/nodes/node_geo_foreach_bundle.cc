/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_string_utf8.h"

#include "BLO_read_write.hh"

#include "NOD_geo_foreach_bundle.hh"
#include "NOD_socket.hh"
#include "NOD_socket_items_blend.hh"
#include "NOD_socket_items_ops.hh"
#include "NOD_socket_items_ui.hh"
#include "NOD_socket_search_link.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "node_geometry_util.hh"

namespace blender::nodes {

namespace node_geo_foreach_bundle_cc {

namespace input_node {

NODE_STORAGE_FUNCS(NodeForeachBundleInput);

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();

  const bNode *node = b.node_or_null();
  const bNodeTree *tree = b.tree_or_null();

  b.allow_any_socket_order();

  b.add_output<decl::Bundle>("Subbundle");
  b.add_output<decl::String>("Path");

  b.add_input<decl::Bundle>("Bundle");
  {
    auto &p = b.add_panel("Reduction", 0).default_closed(true);
    if (node && tree) {
      const NodeForeachBundleInput &storage = node_storage(*node);
      if (const bNode *output_node = tree->node_by_id(storage.output_node_id)) {
        const auto &output_storage = *static_cast<const NodeForeachBundleOutput *>(
            output_node->storage);
        for (const int i : IndexRange(output_storage.reduce_items.items_num)) {
          const NodeForeachBundleReduceItem &item = output_storage.reduce_items.items[i];
          const eNodeSocketDatatype socket_type = eNodeSocketDatatype(item.socket_type);
          const StringRef name = item.name ? item.name : "";
          const std::string identifier =
              ForeachBundleReduceItemsAccessor::socket_identifier_for_item(item);
          auto &input_decl = p.add_input(socket_type, name, identifier)
                                 .socket_name_ptr(&tree->id,
                                                  *ForeachBundleReduceItemsAccessor::item_srna,
                                                  &item,
                                                  "name");
          auto &output_decl = p.add_output(socket_type, name, identifier).align_with_previous();
          if (socket_type_supports_fields(socket_type)) {
            input_decl.supports_field();
            output_decl.dependent_field({input_decl.index()});
          }
          input_decl.structure_type(StructureType::Dynamic);
          output_decl.structure_type(StructureType::Dynamic);
        }
      }
      p.add_input<decl::Extend>("", "__extend__").structure_type(StructureType::Dynamic);
      p.add_output<decl::Extend>("", "__extend__")
          .structure_type(StructureType::Dynamic)
          .align_with_previous();
    }
  }
  {
    auto &p = b.add_panel("Filter", 1).default_closed(true);
    p.add_input<decl::String>("Type");
  }
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  auto *storage = MEM_new_for_free<NodeForeachBundleInput>(__func__);
  node->storage = storage;
}

static void node_label(const bNodeTree * /*ntree*/,
                       const bNode * /*node*/,
                       char *label,
                       const int label_maxncpy)
{
  BLI_strncpy_utf8(
      label, CTX_IFACE_(BLT_I18NCONTEXT_ID_NODETREE, "For Each Bundle"), label_maxncpy);
}

static bool node_insert_link(bke::NodeInsertLinkParams &params)
{
  bNode *output_node = params.ntree.node_by_id(node_storage(params.node).output_node_id);
  if (!output_node) {
    return true;
  }
  return socket_items::try_add_item_via_any_extend_socket<ForeachBundleReduceItemsAccessor>(
      params.ntree, params.node, *output_node, params.link);
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "NodeForeachBundleInput", NODE_FOREACH_BUNDLE_INPUT);
  ntype.ui_name = "For Each Bundle Input";
  ntype.nclass = NODE_CLASS_INTERFACE;
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.labelfunc = node_label;
  ntype.insert_link = node_insert_link;
  ntype.gather_link_search_ops = nullptr;
  ntype.no_muting = true;
  bke::node_type_storage(
      ntype, "NodeForeachBundleInput", node_free_standard_storage, node_copy_standard_storage);
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace input_node

namespace output_node {

NODE_STORAGE_FUNCS(NodeForeachBundleOutput);

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_output<decl::Bundle>("Bundle");

  b.add_input<decl::Bundle>("Subbundle");
  b.add_input<decl::Bool>("Recurse").default_value(true);

  const bNode *node = b.node_or_null();
  const bNodeTree *tree = b.tree_or_null();

  {
    auto &p = b.add_panel("Reduction", 0).default_closed(true);
    if (node && tree) {
      const NodeForeachBundleOutput &storage = node_storage(*node);
      for (const int i : IndexRange(storage.reduce_items.items_num)) {
        const NodeForeachBundleReduceItem &item = storage.reduce_items.items[i];
        const eNodeSocketDatatype socket_type = eNodeSocketDatatype(item.socket_type);
        const StringRef name = item.name ? item.name : "";
        const std::string identifier =
            ForeachBundleReduceItemsAccessor::socket_identifier_for_item(item);
        auto &input_decl = p.add_input(socket_type, name, identifier)
                               .socket_name_ptr(&tree->id,
                                                *ForeachBundleReduceItemsAccessor::item_srna,
                                                &item,
                                                "name");
        auto &output_decl = p.add_output(socket_type, name, identifier).align_with_previous();
        if (socket_type_supports_fields(socket_type)) {
          input_decl.supports_field();
          output_decl.dependent_field({input_decl.index()});
        }
        input_decl.structure_type(StructureType::Dynamic);
        output_decl.structure_type(StructureType::Dynamic);
      }
      p.add_input<decl::Extend>("", "__extend__").structure_type(StructureType::Dynamic);
      p.add_output<decl::Extend>("", "__extend__")
          .structure_type(StructureType::Dynamic)
          .align_with_previous();
    }
  }
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  auto *storage = MEM_new_for_free<NodeForeachBundleOutput>(__func__);
  node->storage = storage;
}

static void node_free_storage(bNode *node)
{
  socket_items::destruct_array<ForeachBundleReduceItemsAccessor>(*node);
  MEM_freeN(static_cast<NodeForeachBundleOutput *>(node->storage));
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  const NodeForeachBundleOutput &src_storage = node_storage(*src_node);
  auto *dst_storage = MEM_new_for_free<NodeForeachBundleOutput>(__func__,
                                                                dna::shallow_copy(src_storage));
  dst_node->storage = dst_storage;

  socket_items::copy_array<ForeachBundleReduceItemsAccessor>(*src_node, *dst_node);
}

static bool node_insert_link(bke::NodeInsertLinkParams &params)
{
  return socket_items::try_add_item_via_any_extend_socket<ForeachBundleReduceItemsAccessor>(
      params.ntree, params.node, params.node, params.link);
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "NodeForeachBundleOutput", NODE_FOREACH_BUNDLE_OUTPUT);
  ntype.ui_name = "For Each Bundle Output";
  ntype.nclass = NODE_CLASS_INTERFACE;
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.labelfunc = input_node::node_label;
  ntype.insert_link = node_insert_link;
  ntype.gather_link_search_ops = nullptr;
  ntype.no_muting = true;
  bke::node_type_storage(ntype, "NodeForeachBundleOutput", node_free_storage, node_copy_storage);
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace output_node

}  // namespace node_geo_foreach_bundle_cc

StructRNA **ForeachBundleReduceItemsAccessor::item_srna = &RNA_ForeachBundleReduceItem;

void ForeachBundleReduceItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  BLO_write_string(writer, item.name);
}

void ForeachBundleReduceItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

}  // namespace blender::nodes
