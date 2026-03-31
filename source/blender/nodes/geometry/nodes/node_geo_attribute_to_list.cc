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

#include "BKE_curves.hh"
#include "BKE_grease_pencil.hh"
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

  b.add_input<decl::Geometry>("Geometry"_ustr)
      .description("Geometry to evaluate the given fields on");
  b.add_input<decl::Bool>("Selection"_ustr).default_value(true).hide_value().field_on_all();
  if (node != nullptr) {
    const NodeGeometryAttributeToList &storage = node_storage(*node);
    for (const NodeGeometryAttributeToListItem &item : Span(storage.items, storage.items_num)) {
      const eNodeSocketDatatype data_type = eNodeSocketDatatype(item.socket_type);
      const UString name(item.name);
      const UString identifier(AttributeToListItemsAccessor::socket_identifier_for_item(item));
      b.add_input(data_type, name, identifier)
          .field_on_all()
          .socket_name_ptr(&tree->id, *AttributeToListItemsAccessor::item_srna, &item, "name");
      b.add_output(data_type, name, identifier)
          .align_with_previous()
          .structure_type(StructureType::List);
    }
  }
  b.add_input<decl::Extend>(""_ustr, "__extend__"_ustr).structure_type(StructureType::Field);
  b.add_output<decl::Extend>(""_ustr, "__extend__"_ustr)
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

static void output_empty_lists(GeoNodeExecParams &params)
{
  const NodeGeometryAttributeToList &storage = node_storage(params.node());
  for (const int item_i : IndexRange(storage.items_num)) {
    const NodeGeometryAttributeToListItem &item = storage.items[item_i];
    const eNodeSocketDatatype socket_type = eNodeSocketDatatype(item.socket_type);
    const CPPType &cpp_type = *bke::socket_type_to_geo_nodes_base_cpp_type(socket_type);
    const std::string identifier = AttributeToListItemsAccessor::socket_identifier_for_item(item);
    params.set_output(UString(identifier), List::from_garray(GArray(cpp_type)));
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry"_ustr);
  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection"_ustr);

  const NodeGeometryAttributeToList &storage = node_storage(params.node());
  const AttrDomain domain = AttrDomain(storage.domain);

  if (storage.items_num == 0) {
    output_empty_lists(params);
    return;
  }

  ResourceScope scope;
  Vector<fn::FieldEvaluator *> field_evaluators;
  if (geometry_set.has_mesh()) {
    const bke::MeshComponent &component = *geometry_set.get_component<bke::MeshComponent>();
    const int domain_size = component.attribute_domain_size(domain);
    if (domain_size > 0) {
      const bke::GeometryFieldContext &field_context = scope.construct<bke::GeometryFieldContext>(
          component, domain);
      field_evaluators.append(&scope.construct<fn::FieldEvaluator>(field_context, domain_size));
    }
  }
  if (geometry_set.has_pointcloud()) {
    const bke::PointCloudComponent &component =
        *geometry_set.get_component<bke::PointCloudComponent>();
    const int domain_size = component.attribute_domain_size(domain);
    if (domain_size > 0) {
      const bke::GeometryFieldContext &field_context = scope.construct<bke::GeometryFieldContext>(
          component, domain);
      field_evaluators.append(&scope.construct<fn::FieldEvaluator>(field_context, domain_size));
    }
  }
  if (geometry_set.has_curves()) {
    const bke::CurveComponent &component = *geometry_set.get_component<bke::CurveComponent>();
    const int domain_size = component.attribute_domain_size(domain);
    if (domain_size > 0) {
      const bke::GeometryFieldContext &field_context = scope.construct<bke::GeometryFieldContext>(
          component, domain);
      field_evaluators.append(&scope.construct<fn::FieldEvaluator>(field_context, domain_size));
    }
  }
  if (geometry_set.has_grease_pencil()) {
    using namespace bke::greasepencil;
    const bke::GreasePencilComponent &component =
        *geometry_set.get_component<bke::GreasePencilComponent>();
    if (domain == AttrDomain::Layer) {
      const int domain_size = component.attribute_domain_size(domain);
      if (domain_size > 0) {
        const bke::GeometryFieldContext &field_context =
            scope.construct<bke::GeometryFieldContext>(component, domain);
        field_evaluators.append(&scope.construct<fn::FieldEvaluator>(field_context, domain_size));
      }
    }
    else {
      const GreasePencil &grease_pencil = *component.get();
      for (const int layer_i : grease_pencil.layers().index_range()) {
        const Layer &layer = grease_pencil.layer(layer_i);
        const Drawing *drawing = grease_pencil.get_eval_drawing(layer);
        if (!drawing) {
          continue;
        }
        const int domain_size = drawing->strokes().attributes().domain_size(domain);
        if (domain_size > 0) {
          const bke::GeometryFieldContext &field_context =
              scope.construct<bke::GeometryFieldContext>(grease_pencil, domain, layer_i);
          field_evaluators.append(
              &scope.construct<fn::FieldEvaluator>(field_context, domain_size));
        }
      }
    }
  }
  if (geometry_set.has_instances()) {
    const bke::InstancesComponent &component =
        *geometry_set.get_component<bke::InstancesComponent>();
    const int domain_size = component.attribute_domain_size(domain);
    if (domain_size > 0) {
      const bke::GeometryFieldContext &field_context = scope.construct<bke::GeometryFieldContext>(
          component, domain);
      field_evaluators.append(&scope.construct<fn::FieldEvaluator>(field_context, domain_size));
    }
  }

  if (field_evaluators.is_empty()) {
    output_empty_lists(params);
    return;
  }

  Vector<GField> input_fields(storage.items_num);
  for (const int item_i : IndexRange(storage.items_num)) {
    const NodeGeometryAttributeToListItem &item = storage.items[item_i];
    const std::string identifier = AttributeToListItemsAccessor::socket_identifier_for_item(item);
    input_fields[item_i] = params.extract_input<GField>(UString(identifier));
  }

  for (fn::FieldEvaluator *field_evaluator : field_evaluators) {
    field_evaluator->set_selection(selection_field);
    for (const int item_i : IndexRange(storage.items_num)) {
      field_evaluator->add(input_fields[item_i]);
    }
  }

  for (fn::FieldEvaluator *field_evaluator : field_evaluators) {
    field_evaluator->evaluate();
  }

  Vector<int> output_offsets;
  output_offsets.append(0);
  for (fn::FieldEvaluator *field_evaluator : field_evaluators) {
    const IndexMask &mask = field_evaluator->get_evaluated_selection_as_mask();
    output_offsets.append(output_offsets.last() + mask.size());
  }
  const int output_size = output_offsets.last();
  if (output_size == 0) {
    output_empty_lists(params);
    return;
  }

  for (const int item_i : IndexRange(storage.items_num)) {
    const NodeGeometryAttributeToListItem &item = storage.items[item_i];
    const CPPType &cpp_type = *bke::socket_type_to_geo_nodes_base_cpp_type(
        eNodeSocketDatatype(item.socket_type));
    GArray<> values(cpp_type, output_size, NoInitialization{});
    for (const int evaluator_i : field_evaluators.index_range()) {
      const fn::FieldEvaluator &field_evaluator = *field_evaluators[evaluator_i];
      const IndexMask &mask = field_evaluator.get_evaluated_selection_as_mask();
      if (!mask.is_empty()) {
        const GVArray &values_varray = field_evaluator.get_evaluated(item_i);
        BLI_assert(values_varray.type() == cpp_type);
        values_varray.materialize_compressed_to_uninitialized(
            mask, POINTER_OFFSET(values.data(), cpp_type.size * output_offsets[evaluator_i]));
      }
    }
    const std::string identifier = AttributeToListItemsAccessor::socket_identifier_for_item(item);
    params.set_output(UString(identifier), List::from_garray(std::move(values)));
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
      params.connect_available_socket(node, "Geometry"_ustr);
    });
  }
  if (!AttributeToListItemsAccessor::supports_socket_type(type, params.node_tree().type)) {
    return;
  }

  params.add_item(IFACE_("Value"), [type](LinkSearchOpParams &params) {
    bNode &node = params.add_node("GeometryNodeAttributeToList");
    socket_items::add_item_with_socket_type_and_name<AttributeToListItemsAccessor>(
        params.node_tree, node, type, params.socket.name);
    params.update_and_connect_available_socket(node, UString(params.socket.name));
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
