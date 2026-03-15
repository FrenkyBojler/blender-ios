/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_mesh_types.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "NOD_geo_attribute_to_list.hh"
#include "NOD_socket_items_blend.hh"
#include "NOD_socket_items_ops.hh"
#include "NOD_socket_items_ui.hh"
#include "NOD_socket_search_link.hh"

#include "RNA_enum_types.hh"

#include "BLO_read_write.hh"

#include "BKE_screen.hh"

#include "node_geometry_util.hh"

namespace blender {

namespace nodes::node_geo_attribute_to_list_cc {

NODE_STORAGE_FUNCS(NodeGeometryAttributeToList)

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNodeTree *tree = b.tree_or_null();
  const bNode *node = b.node_or_null();
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_default_layout();

  b.add_input<decl::Geometry>("Geometry").description("Geometry to evaluate the given fields on");
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();
  if (node != nullptr) {
    const NodeGeometryAttributeToList &storage = node_storage(*node);
    for (const NodeGeometryAttributeToListItem &item : Span(storage.items, storage.items_num)) {
      const eNodeSocketDatatype data_type = eNodeSocketDatatype(item.socket_type);
      const std::string identifier = AttributeToListItemsAccessor::socket_identifier_for_item(
          item);
      b.add_input(data_type, item.name, identifier)
          .field_on_all()
          .socket_name_ptr(&tree->id, *AttributeToListItemsAccessor::item_srna, &item, "name");
      b.add_output(data_type, item.name, identifier)
          .align_with_previous()
          .structure_type(StructureType::List);
    }
  }
  b.add_input<decl::Extend>("", "__extend__").structure_type(StructureType::Field);
  b.add_output<decl::Extend>("", "__extend__")
      .structure_type(StructureType::Field)
      .align_with_previous();
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);
  layout.prop(ptr, "domain", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryAttributeToList *data = MEM_new<NodeGeometryAttributeToList>(__func__);
  data->domain = int8_t(AttrDomain::Point);
  node->storage = data;
}

static void node_layout_ex(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  bNodeTree &tree = *reinterpret_cast<bNodeTree *>(ptr->owner_id);
  bNode &node = *static_cast<bNode *>(ptr->data);

  layout.prop(ptr, "domain", UI_ITEM_NONE, "", ICON_NONE);

  if (ui::Layout *panel = layout.panel(C, "attribute_to_list_items", false, IFACE_("Items"))) {
    socket_items::ui::draw_items_list_with_operators<AttributeToListItemsAccessor>(
        C, panel, tree, node);
    socket_items::ui::draw_active_item_props<AttributeToListItemsAccessor>(
        tree, node, [&](PointerRNA *item_ptr) {
          panel->use_property_split_set(true);
          panel->use_property_decorate_set(false);
          panel->prop(item_ptr, "socket_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        });
  }
}

static void node_operators()
{
  socket_items::ops::make_common_operators<AttributeToListItemsAccessor>();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");
  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");

  const NodeGeometryAttributeToList &storage = node_storage(params.node());
  const AttrDomain domain = AttrDomain(storage.domain);

  const Mesh *mesh = geometry_set.get_mesh();
  if (!mesh) {
    params.set_default_remaining_outputs();
    return;
  }
  const int domain_size = mesh->attributes().domain_size(domain);

  const bke::GeometryFieldContext field_context{*mesh, domain};
  fn::FieldEvaluator field_evaluator{field_context, domain_size};
  field_evaluator.set_selection(selection_field);

  for (const int item_i : IndexRange(storage.items_num)) {
    const NodeGeometryAttributeToListItem &item = storage.items[item_i];
    const std::string identifier = AttributeToListItemsAccessor::socket_identifier_for_item(item);
    const GField field = params.extract_input<GField>(identifier);
    field_evaluator.add(field);
  }

  field_evaluator.evaluate();

  const IndexMask &mask = field_evaluator.get_evaluated_selection_as_mask();
  const int mask_size = mask.size();

  for (const int item_i : IndexRange(storage.items_num)) {
    const NodeGeometryAttributeToListItem &item = storage.items[item_i];
    const GVArray values = field_evaluator.get_evaluated(item_i);
    const CPPType &type = values.type();
    GArray values_array(type, mask_size, NoInitialization{});
    values.materialize_compressed_to_uninitialized(mask, values_array.data());
    const std::string identifier = AttributeToListItemsAccessor::socket_identifier_for_item(item);
    params.set_output(identifier, List::from_garray(std::move(values_array)));
  }
}

static bool node_insert_link(bke::NodeInsertLinkParams &params)
{
  return socket_items::try_add_item_via_any_extend_socket<AttributeToListItemsAccessor>(
      params.ntree, params.node, params.node, params.link);
}

static void node_free_storage(bNode *node)
{
  socket_items::destruct_array<AttributeToListItemsAccessor>(*node);
  MEM_delete(reinterpret_cast<NodeGeometryAttributeToList *>(node->storage));
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  const NodeGeometryAttributeToList &src_storage = node_storage(*src_node);
  NodeGeometryAttributeToList *dst_storage = MEM_new<NodeGeometryAttributeToList>(
      __func__, dna::shallow_copy(src_storage));
  dst_node->storage = dst_storage;

  socket_items::copy_array<AttributeToListItemsAccessor>(*src_node, *dst_node);
}

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  const eNodeSocketDatatype type = eNodeSocketDatatype(params.other_socket().type);
  if (type == SOCK_GEOMETRY) {
    params.add_item(IFACE_("Geometry"), [](LinkSearchOpParams &params) {
      bNode &node = params.add_node("GeometryNodeAttributeToList");
      params.connect_available_socket(node, "Geometry");
    });
  }
  if (!AttributeToListItemsAccessor::supports_socket_type(type, params.node_tree().type)) {
    return;
  }

  params.add_item(IFACE_("Value"), [type](LinkSearchOpParams &params) {
    bNode &node = params.add_node("GeometryNodeAttributeToList");
    socket_items::add_item_with_socket_type_and_name<AttributeToListItemsAccessor>(
        params.node_tree, node, type, params.socket.name);
    params.update_and_connect_available_socket(node, params.socket.name);
  });
}

static const bNodeSocket *node_internally_linked_input(const bNodeTree & /*tree*/,
                                                       const bNode &node,
                                                       const bNodeSocket &output_socket)
{
  return &node.input_socket(output_socket.index());
}

static void node_blend_write(const bNodeTree & /*tree*/, const bNode &node, BlendWriter &writer)
{
  socket_items::blend_write<AttributeToListItemsAccessor>(&writer, node);
}

static void node_blend_read(bNodeTree & /*tree*/, bNode &node, BlendDataReader &reader)
{
  socket_items::blend_read_data<AttributeToListItemsAccessor>(&reader, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeAttributeToList");
  ntype.ui_name = "Attribute to List";
  ntype.ui_description = "Evaluate the input fields on the geometry and output the result as list";
  ntype.nclass = NODE_CLASS_ATTRIBUTE;
  bke::node_type_storage(
      ntype, "NodeGeometryAttributeToList", node_free_storage, node_copy_storage);
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.insert_link = node_insert_link;
  ntype.draw_buttons = node_layout;
  ntype.draw_buttons_ex = node_layout_ex;
  ntype.register_operators = node_operators;
  ntype.gather_link_search_ops = node_gather_link_searches;
  ntype.internally_linked_input = node_internally_linked_input;
  ntype.blend_write_storage_content = node_blend_write;
  ntype.blend_data_read_storage_content = node_blend_read;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace nodes::node_geo_attribute_to_list_cc

namespace nodes {

StructRNA **AttributeToListItemsAccessor::item_srna = &RNA_NodeGeometryCaptureAttributeItem;

void AttributeToListItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  writer->write_string(item.name);
}

void AttributeToListItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

}  // namespace nodes
}  // namespace blender
