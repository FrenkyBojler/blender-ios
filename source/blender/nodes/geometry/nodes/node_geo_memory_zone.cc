/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_string.h"
#include "BLI_string_utf8.h"

#include "NOD_geo_memory_zone.hh"
#include "NOD_socket.hh"
#include "NOD_socket_items_blend.hh"
#include "NOD_socket_items_ops.hh"
#include "NOD_socket_items_ui.hh"
#include "NOD_socket_search_link.hh"

#include "BLO_read_write.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "BKE_screen.hh"

#include "WM_api.hh"

#include "UI_interface_layout.hh"

#include "node_geometry_util.hh"

namespace blender {

namespace nodes::node_geo_memory_zone_cc {

namespace memory_zone_input_node {

NODE_STORAGE_FUNCS(NodeGeometryMemoryZoneInput);

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  const bNode *node = b.node_or_null();
  const bNodeTree *tree = b.tree_or_null();
  
  if (ELEM(nullptr, node, tree)) {
    return;
  }

  const NodeGeometryMemoryZoneInput &storage = node_storage(*node);
  if (const bNode *output_node = tree->node_by_id(storage.output_node_id)) {
    const auto &output_storage = *static_cast<const NodeGeometryMemoryZoneOutput *>(output_node->storage);
    const Span<NodeMemoryZoneInputItem> input_items = output_storage.input_items_span();
    for (const NodeMemoryZoneInputItem &item : input_items) {
      const eNodeSocketDatatype socket_type = item.socket_type;
      const UString name = item.name ? UString(item.name) : ""_ustr;
      const UString identifier(MemoryZoneInputItemsAccessor::socket_identifier_for_item(item));
      auto &input_decl = b.add_input(socket_type, name, identifier)
                             .socket_name_ptr(
                                 &tree->id, *MemoryZoneInputItemsAccessor::item_srna, &item, "name")
                             .structure_type(StructureType::Dynamic);
      b.add_output(socket_type, name, identifier)
          .align_with_previous()
          .propagate_all({input_decl.index()})
          .inferred_structure_type({input_decl.index()})
          .structure_type(StructureType::Dynamic);
    }
  }

  b.add_input<decl::Extend>(""_ustr, "__extend__"_ustr)
      .structure_type(StructureType::Dynamic)
      .custom_draw(socket_items::ui::draw_extend_socket_fn<MemoryZoneInputItemsAccessor>());
  b.add_output<decl::Extend>(""_ustr, "__extend__"_ustr)
      .structure_type(StructureType::Dynamic)
      .align_with_previous();
}

static void node_layout_ex(ui::Layout &layout, bContext *C, PointerRNA *current_node_ptr)
{
  bNodeTree &ntree = *reinterpret_cast<bNodeTree *>(current_node_ptr->owner_id);
  bNode *current_node = static_cast<bNode *>(current_node_ptr->data);

  const bke::bNodeTreeZones *zones = ntree.zones();
  if (!zones) {
    return;
  }
  const bke::bNodeTreeZone *zone = zones->get_zone_by_node(current_node->identifier);
  if (!zone) {
    return;
  }
  if (!zone->output_node_id) {
    return;
  }
  bNode &output_node = const_cast<bNode &>(*zone->output_node());
  PointerRNA output_node_ptr = RNA_pointer_create_discrete(
      current_node_ptr->owner_id, RNA_Node, &output_node);

  if (ui::Layout *panel = layout.panel(C, "input_items", false, IFACE_("Input Items"))) {
    socket_items::ui::draw_items_list_with_operators<MemoryZoneInputItemsAccessor>(
        C, panel, ntree, output_node);
    socket_items::ui::draw_active_item_props<MemoryZoneInputItemsAccessor>(
        ntree, output_node, [&](PointerRNA *item_ptr) {
          panel->use_property_split_set(true);
          panel->use_property_decorate_set(false);
          panel->prop(item_ptr, "socket_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        });
  }
}
static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  auto *data = MEM_new<NodeGeometryMemoryZoneInput>(__func__);
  data->output_node_id = 0;
  node->storage = data;
}

static void node_label(const bNodeTree * /*ntree*/,
                       const bNode * /*node*/,
                       char *label,
                       const int label_maxncpy)
{
  BLI_strncpy_utf8(label, CTX_IFACE_(BLT_I18NCONTEXT_ID_NODETREE, "Memory Zone"), label_maxncpy);
}

static bool node_insert_link(bke::NodeInsertLinkParams &params)
{
  bNode *output_node = params.ntree.node_by_id(node_storage(params.node).output_node_id);
  if (!output_node) {
    return true;
  }
  return socket_items::try_add_item_via_any_extend_socket<MemoryZoneInputItemsAccessor>(
      params.ntree, params.node, *output_node, params.link);
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeMemoryZoneInput"_ustr, NODE_MEMORY_ZONE_INPUT);
  ntype.ui_name = "Memory Zone Input";
  ntype.enum_name_legacy = "MEMORY_ZONE_INPUT";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.labelfunc = node_label;
  ntype.gather_link_search_ops = nullptr;
  ntype.insert_link = node_insert_link;
  ntype.no_muting = true;
  ntype.draw_buttons_ex = node_layout_ex;
  bke::node_type_storage(
      ntype, "NodeGeometryMemoryZoneInput", node_free_standard_storage, node_copy_standard_storage);
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace memory_zone_input_node

namespace memory_zonet_output_node {

NODE_STORAGE_FUNCS(NodeGeometryMemoryZoneOutput);

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  const bNodeTree *tree = b.tree_or_null();
  const bNode *node = b.node_or_null();
  
  if (ELEM(nullptr, node, tree)) {
    return;
  }

  const NodeGeometryMemoryZoneOutput &storage = node_storage(*node);
  const Span<NodeMemoryZoneOutputItem> output_items = storage.output_items_span();
  for (const NodeMemoryZoneOutputItem &item : output_items) {
    const eNodeSocketDatatype socket_type = item.socket_type;
    const UString name = item.name ? UString(item.name) : ""_ustr;
    const UString identifier(MemoryZoneOutputItemsAccessor::socket_identifier_for_item(item));
    auto &input_decl = b.add_input(socket_type, name, identifier)
                           .socket_name_ptr(
                               &tree->id, *MemoryZoneOutputItemsAccessor::item_srna, &item, "name")
                           .structure_type(StructureType::Dynamic);
    b.add_output(socket_type, name, identifier)
        .align_with_previous()
        .propagate_all({input_decl.index()})
        .inferred_structure_type({input_decl.index()})
        .structure_type(StructureType::Dynamic);
  }

  b.add_input<decl::Extend>(""_ustr, "__extend__"_ustr)
      .structure_type(StructureType::Dynamic)
      .custom_draw(socket_items::ui::draw_extend_socket_fn<MemoryZoneOutputItemsAccessor>());
  b.add_output<decl::Extend>(""_ustr, "__extend__"_ustr)
      .structure_type(StructureType::Dynamic)
      .align_with_previous();
}

static void node_layout_ex(ui::Layout &layout, bContext *C, PointerRNA *current_node_ptr)
{
  bNodeTree &ntree = *reinterpret_cast<bNodeTree *>(current_node_ptr->owner_id);
  bNode *current_node = static_cast<bNode *>(current_node_ptr->data);

  const bke::bNodeTreeZones *zones = ntree.zones();
  if (!zones) {
    return;
  }
  const bke::bNodeTreeZone *zone = zones->get_zone_by_node(current_node->identifier);
  if (!zone) {
    return;
  }
  if (!zone->output_node_id) {
    return;
  }
  bNode &output_node = const_cast<bNode &>(*zone->output_node());
  PointerRNA output_node_ptr = RNA_pointer_create_discrete(
      current_node_ptr->owner_id, RNA_Node, &output_node);

  if (ui::Layout *panel = layout.panel(C, "output_items", false, IFACE_("Output Items"))) {
    socket_items::ui::draw_items_list_with_operators<MemoryZoneOutputItemsAccessor>(
        C, panel, ntree, output_node);
    socket_items::ui::draw_active_item_props<MemoryZoneOutputItemsAccessor>(
        ntree, output_node, [&](PointerRNA *item_ptr) {
          panel->use_property_split_set(true);
          panel->use_property_decorate_set(false);
          panel->prop(item_ptr, "socket_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        });
  }
}

static void node_init(bNodeTree */*tree*/, bNode *node)
{
  auto *data = MEM_new<NodeGeometryMemoryZoneOutput>(__func__);
  data->input_items.next_identifier = 0;
  data->output_items.next_identifier = 0;
  node->storage = data;
}

static void node_free_storage(bNode *node)
{
  socket_items::destruct_array<MemoryZoneInputItemsAccessor>(*node);
  socket_items::destruct_array<MemoryZoneOutputItemsAccessor>(*node);
  MEM_delete(reinterpret_cast<NodeGeometryMemoryZoneOutput *>(node->storage));
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  const NodeGeometryMemoryZoneOutput &src_storage = node_storage(*src_node);
  auto *dst_storage = MEM_new<NodeGeometryMemoryZoneOutput>(__func__, dna::shallow_copy(src_storage));
  dst_node->storage = dst_storage;

  socket_items::copy_array<MemoryZoneInputItemsAccessor>(*src_node, *dst_node);
  socket_items::copy_array<MemoryZoneOutputItemsAccessor>(*src_node, *dst_node);
}

static bool node_insert_link(bke::NodeInsertLinkParams &params)
{
  return socket_items::try_add_item_via_any_extend_socket<MemoryZoneOutputItemsAccessor>(
      params.ntree, params.node, params.node, params.link);
}

static void node_operators()
{
  socket_items::ops::make_common_operators<MemoryZoneInputItemsAccessor>();
  socket_items::ops::make_common_operators<MemoryZoneOutputItemsAccessor>();
}

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  // const bNodeSocket &other_socket = params.other_socket();
  // if (!MemoryZoneOutputItemsAccessor::supports_socket_type(other_socket.type, params.node_tree().type)) {
  //   return;
  // }
  // params.add_item_full_name(IFACE_("Memory Zone"), [](LinkSearchOpParams &params) {
  //   bNode &input_node = params.add_node("GeometryNodeMemoryZoneInput"_ustr);
  //   bNode &output_node = params.add_node("GeometryNodeMemoryZoneOutput"_ustr);
  //   output_node.location[0] = 300;
  // 
  //   auto &input_storage = *static_cast<NodeGeometryMemoryZoneInput *>(input_node.storage);
  //   input_storage.output_node_id = output_node.identifier;
  // 
  //   socket_items::clear<MemoryZoneInputItemsAccessor>(output_node);
  //   const UString name(params.socket.name);
  //   socket_items::add_item_with_socket_type_and_name<MemoryZoneItemsAccessor>(
  //       params.node_tree, output_node, params.socket.type, name.c_str());
  //   update_node_declaration_and_sockets(params.node_tree, input_node);
  //   update_node_declaration_and_sockets(params.node_tree, output_node);
  //   if (params.socket.in_out == SOCK_IN) {
  //     params.connect_available_socket(output_node, name);
  //   }
  //   else {
  //     params.connect_available_socket(input_node, name);
  //   }
  //   params.node_tree.ensure_topology_cache();
  //   bke::node_add_link(params.node_tree,
  //                      input_node,
  //                      input_node.output_socket(1),
  //                      output_node,
  //                      output_node.input_socket(0));
  // });
}

static void node_blend_write(const bNodeTree & /*tree*/, const bNode &node, BlendWriter &writer)
{
  socket_items::blend_write<MemoryZoneInputItemsAccessor>(&writer, node);
  socket_items::blend_write<MemoryZoneOutputItemsAccessor>(&writer, node);
}

static void node_blend_read(bNodeTree & /*tree*/, bNode &node, BlendDataReader &reader)
{
  socket_items::blend_read_data<MemoryZoneInputItemsAccessor>(&reader, node);
  socket_items::blend_read_data<MemoryZoneOutputItemsAccessor>(&reader, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeMemoryZoneOutput"_ustr, NODE_MEMORY_ZONE_OUTPUT);
  ntype.ui_name = "Memory Zone Output";
  ntype.enum_name_legacy = "MEMORY_ZONE_OUTPUT";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.labelfunc = memory_zone_input_node::node_label;
  ntype.insert_link = node_insert_link;
  ntype.gather_link_search_ops = node_gather_link_searches;
  ntype.no_muting = true;
  ntype.draw_buttons_ex = node_layout_ex;
  ntype.register_operators = node_operators;
  ntype.blend_write_storage_content = node_blend_write;
  ntype.blend_data_read_storage_content = node_blend_read;
  bke::node_type_storage(ntype, "NodeGeometryMemoryZoneOutput", node_free_storage, node_copy_storage);
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace memory_zonet_output_node

}  // namespace nodes::node_geo_memory_zone_cc

namespace nodes {

StructRNA **MemoryZoneInputItemsAccessor::item_srna = &RNA_NodeMemoryZoneInputItem;

void MemoryZoneInputItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  writer->write_string(item.name);
}

void MemoryZoneInputItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

StructRNA **MemoryZoneOutputItemsAccessor::item_srna = &RNA_NodeMemoryZoneOutputItem;

void MemoryZoneOutputItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  writer->write_string(item.name);
}

void MemoryZoneOutputItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

}  // namespace nodes

Span<NodeMemoryZoneInputItem> NodeGeometryMemoryZoneOutput::input_items_span() const
{
  return Span<NodeMemoryZoneInputItem>(this->input_items.items, this->input_items.items_num);
}

MutableSpan<NodeMemoryZoneInputItem> NodeGeometryMemoryZoneOutput::input_items_span()
{
  return MutableSpan<NodeMemoryZoneInputItem>(this->input_items.items, this->input_items.items_num);
}

Span<NodeMemoryZoneOutputItem> NodeGeometryMemoryZoneOutput::output_items_span() const
{
  return Span<NodeMemoryZoneOutputItem>(this->output_items.items, this->output_items.items_num);
}

MutableSpan<NodeMemoryZoneOutputItem> NodeGeometryMemoryZoneOutput::output_items_span()
{
  return MutableSpan<NodeMemoryZoneOutputItem>(this->output_items.items, this->output_items.items_num);
}

}  // namespace blender
