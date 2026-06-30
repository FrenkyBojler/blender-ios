/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup cmpnodes
 *
 * Compositor repeat zone. Executes inner body nodes N times, threading image and
 * additional user-defined values (items) from each iteration into the next.
 */

#include "BLI_string.hh"
#include "BLI_string_utf8.hh"

#include "BLT_translation.hh"

#include "BLO_read_write.hh"

#include "RNA_prototypes.hh"

#include "NOD_socket_items_blend.hh"
#include "NOD_socket_items_ops.hh"
#include "NOD_socket_items_ui.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"

#include "include/NOD_cmp_repeat.hh"

namespace blender {

namespace nodes::node_composite_repeat_cc {

/** Shared between repeat zone input and output node. */
static void node_layout(ui::Layout &layout, bContext *C, PointerRNA *current_node_ptr)
{
  bNodeTree &ntree = *reinterpret_cast<bNodeTree *>(current_node_ptr->owner_id);
  bNode *current_node = static_cast<bNode *>(current_node_ptr->data);

  const bke::bNodeTreeZones *zones = ntree.zones();
  if (!zones) {
    return;
  }
  const bke::bNodeTreeZone *zone = zones->get_zone_by_node(current_node->identifier);
  if (!zone || !zone->output_node_id) {
    return;
  }
  bNode &output_node = const_cast<bNode &>(*zone->output_node());

  if (ui::Layout *panel = layout.panel(C, "repeat_items", false, IFACE_("Repeat Items"))) {
    socket_items::ui::draw_items_list_with_operators<CompositorRepeatItemsAccessor>(
        C, panel, ntree, output_node);
    socket_items::ui::draw_active_item_props<CompositorRepeatItemsAccessor>(
        ntree, output_node, [&](PointerRNA *item_ptr) {
          panel->use_property_split_set(true);
          panel->use_property_decorate_set(false);
          panel->prop(item_ptr, "socket_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        });
  }
}

/* -------------------------------------------------------------------- */
/** \name Repeat Input
 * \{ */

namespace repeat_input_node {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_output<decl::Int>("Iteration"_ustr).description(
      "Index of the current iteration. Starts counting at zero");
  b.add_input<decl::Int>("Iterations"_ustr).min(0).default_value(1);

  const bNode *node = b.node_or_null();
  const bNodeTree *tree = b.tree_or_null();
  if (node && tree) {
    const NodeCompositorRepeatInput &input_storage =
        *static_cast<const NodeCompositorRepeatInput *>(node->storage);
    if (const bNode *output_node = tree->node_by_id(input_storage.output_node_id)) {
      const NodeCompositorRepeatOutput &out_storage =
          *static_cast<const NodeCompositorRepeatOutput *>(output_node->storage);
      for (const int i : IndexRange(out_storage.items_num)) {
        const NodeRepeatItem &item = out_storage.items[i];
        const UString name = item.name ? UString(item.name) : ""_ustr;
        const UString identifier(CompositorRepeatItemsAccessor::socket_identifier_for_item(item));
        b.add_input(item.socket_type, name, identifier)
            .socket_name_ptr(
                &tree->id, *CompositorRepeatItemsAccessor::item_srna, &item, "name")
            .compositor_realization_mode(CompositorInputRealizationMode::None)
            .structure_type(StructureType::Dynamic);
        b.add_output(item.socket_type, name, identifier)
            .align_with_previous()
            .structure_type(StructureType::Dynamic);
      }
    }
  }

  b.add_input<decl::Extend>(""_ustr, "__extend__"_ustr)
      .structure_type(StructureType::Dynamic)
      .custom_draw(socket_items::ui::draw_extend_socket_fn<CompositorRepeatItemsAccessor>());
  b.add_output<decl::Extend>(""_ustr, "__extend__"_ustr)
      .structure_type(StructureType::Dynamic)
      .align_with_previous();
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeCompositorRepeatInput *data = MEM_new<NodeCompositorRepeatInput>(__func__);
  data->output_node_id = 0;
  node->storage = data;
}

static void node_label(const bNodeTree * /*tree*/,
                       const bNode * /*node*/,
                       char *label,
                       const int label_maxncpy)
{
  BLI_strncpy_utf8(label, CTX_IFACE_(BLT_I18NCONTEXT_ID_NODETREE, "Repeat"), label_maxncpy);
}

static bool node_insert_link(bke::NodeInsertLinkParams &params)
{
  bNode *output_node = params.ntree.node_by_id(
      static_cast<NodeCompositorRepeatInput *>(params.node.storage)->output_node_id);
  if (!output_node) {
    return true;
  }
  return socket_items::try_add_item_via_any_extend_socket<CompositorRepeatItemsAccessor>(
      params.ntree, params.node, *output_node, params.link);
}

using namespace blender::compositor;

class RepeatInputOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    /* Forward each output socket from its matching input. "Iteration" is the exception. */
    for (const bNodeSocket *output : this->node().output_sockets()) {
      if (!is_socket_available(output)) {
        continue;
      }
      if (StringRef(output->identifier) == "Iteration") {
        Result &out = this->get_result("Iteration");
        if (out.should_compute()) {
          out.allocate_single_value();
          out.set_single_value(int(0));
        }
        continue;
      }
      if (StringRef(output->identifier) == "__extend__") {
        continue;
      }
      this->get_result(output->identifier).share_data(this->get_input(output->identifier));
    }
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new RepeatInputOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeRepeatInput"_ustr, CMP_NODE_REPEAT_INPUT);
  ntype.ui_name = "Repeat Input";
  ntype.ui_description = "Start of the repeat zone";
  ntype.enum_name_legacy = "REPEAT_INPUT";
  ntype.nclass = NODE_CLASS_INTERFACE;
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.labelfunc = node_label;
  ntype.insert_link = node_insert_link;
  ntype.no_muting = true;
  ntype.draw_buttons_ex = node_layout;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_type_storage(
      ntype, "NodeCompositorRepeatInput", node_free_standard_storage, node_copy_standard_storage);
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace repeat_input_node

/** \} */

/* -------------------------------------------------------------------- */
/** \name Repeat Output
 * \{ */

namespace repeat_output_node {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  const bNodeTree *tree = b.tree_or_null();
  const bNode *node = b.node_or_null();

  if (node) {
    const NodeCompositorRepeatOutput &storage =
        *static_cast<const NodeCompositorRepeatOutput *>(node->storage);
    for (const int i : IndexRange(storage.items_num)) {
      const NodeRepeatItem &item = storage.items[i];
      const UString name = item.name ? UString(item.name) : ""_ustr;
      const UString identifier(CompositorRepeatItemsAccessor::socket_identifier_for_item(item));
      b.add_input(item.socket_type, name, identifier)
          .socket_name_ptr(
              tree ? &tree->id : nullptr,
              *CompositorRepeatItemsAccessor::item_srna,
              &item,
              "name")
          .compositor_realization_mode(CompositorInputRealizationMode::None)
          .structure_type(StructureType::Dynamic);
      b.add_output(item.socket_type, name, identifier)
          .align_with_previous()
          .structure_type(StructureType::Dynamic);
    }
  }

  b.add_input<decl::Extend>(""_ustr, "__extend__"_ustr)
      .structure_type(StructureType::Dynamic)
      .custom_draw(socket_items::ui::draw_extend_socket_fn<CompositorRepeatItemsAccessor>());
  b.add_output<decl::Extend>(""_ustr, "__extend__"_ustr)
      .structure_type(StructureType::Dynamic)
      .align_with_previous();
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeCompositorRepeatOutput *data = MEM_new<NodeCompositorRepeatOutput>(__func__);
  data->next_identifier = 0;
  data->items = MEM_new_array<NodeRepeatItem>(1, __func__);
  data->items[0].name = BLI_strdup(DATA_("Image"));
  data->items[0].socket_type = SOCK_RGBA;
  data->items[0].identifier = data->next_identifier++;
  data->items_num = 1;
  node->storage = data;
}

static void node_free_storage(bNode *node)
{
  socket_items::destruct_array<CompositorRepeatItemsAccessor>(*node);
  MEM_delete(reinterpret_cast<NodeCompositorRepeatOutput *>(node->storage));
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  const NodeCompositorRepeatOutput &src_storage =
      *static_cast<const NodeCompositorRepeatOutput *>(src_node->storage);
  auto *dst_storage = MEM_new<NodeCompositorRepeatOutput>(__func__,
                                                          dna::shallow_copy(src_storage));
  dst_node->storage = dst_storage;
  socket_items::copy_array<CompositorRepeatItemsAccessor>(*src_node, *dst_node);
}

static void node_blend_write(const bNodeTree & /*tree*/, const bNode &node, BlendWriter &writer)
{
  socket_items::blend_write<CompositorRepeatItemsAccessor>(&writer, node);
}

static void node_blend_read(bNodeTree & /*tree*/, bNode &node, BlendDataReader &reader)
{
  socket_items::blend_read_data<CompositorRepeatItemsAccessor>(&reader, node);
}

static bool node_insert_link(bke::NodeInsertLinkParams &params)
{
  return socket_items::try_add_item_via_any_extend_socket<CompositorRepeatItemsAccessor>(
      params.ntree, params.node, params.node, params.link);
}

static void node_operators()
{
  socket_items::ops::make_common_operators<CompositorRepeatItemsAccessor>();
}

using namespace blender::compositor;

class RepeatOutputOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    /* Forward each input to its matching output socket. */
    for (const bNodeSocket *output : this->node().output_sockets()) {
      if (!is_socket_available(output)) {
        continue;
      }
      if (StringRef(output->identifier) == "__extend__") {
        continue;
      }
      this->get_result(output->identifier).share_data(this->get_input(output->identifier));
    }
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new RepeatOutputOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeRepeatOutput"_ustr, CMP_NODE_REPEAT_OUTPUT);
  ntype.ui_name = "Repeat Output";
  ntype.ui_description = "End of the repeat zone";
  ntype.enum_name_legacy = "REPEAT_OUTPUT";
  ntype.nclass = NODE_CLASS_INTERFACE;
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.labelfunc = repeat_input_node::node_label;
  ntype.insert_link = node_insert_link;
  ntype.register_operators = node_operators;
  ntype.draw_buttons_ex = node_layout;
  ntype.no_muting = true;
  ntype.blend_write_storage_content = node_blend_write;
  ntype.blend_data_read_storage_content = node_blend_read;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_type_storage(
      ntype, "NodeCompositorRepeatOutput", node_free_storage, node_copy_storage);
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace repeat_output_node

/** \} */

}  // namespace nodes::node_composite_repeat_cc

namespace nodes {

StructRNA **CompositorRepeatItemsAccessor::item_srna = &RNA_CompositorRepeatItem;

void CompositorRepeatItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  writer->write_string(item.name);
}

void CompositorRepeatItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

}  // namespace nodes

Span<NodeRepeatItem> NodeCompositorRepeatOutput::items_span() const
{
  return Span<NodeRepeatItem>(items, items_num);
}

MutableSpan<NodeRepeatItem> NodeCompositorRepeatOutput::items_span()
{
  return MutableSpan<NodeRepeatItem>(items, items_num);
}

}  // namespace blender
