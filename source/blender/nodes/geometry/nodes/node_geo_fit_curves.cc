/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute.hh"
#include "BKE_curves.hh"
#include "BKE_grease_pencil.hh"

#include "GEO_fit_curves.hh"
#include "GEO_randomize.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"

#include "NOD_geo_fit_curves.hh"
#include "NOD_rna_define.hh"

#include "BKE_geometry_set.hh"

#include "node_geometry_util.hh"

namespace blender {
namespace nodes::node_geo_fit_curves_cc {

NODE_STORAGE_FUNCS(GeometryNodeFitCurves)
using ItemsAccessor = FitCurvesItemsAccessor;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_default_layout();

  b.add_input<decl::Geometry>("Poly Curves", "Curves")
      .supported_type({GeometryComponent::Type::Curve, GeometryComponent::Type::GreasePencil});
  b.add_output<decl::Geometry>("Curves").propagate_all().align_with_previous();
  b.add_input<decl::Bool>("Selection").default_value(true).field_on_all().hide_value();
  b.add_input<decl::Bool>("Corners").default_value(false).field_on_all().hide_value();
  b.add_input<decl::Float>("Error")
      .default_value(0.01f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .supports_field()
      .description("The error distance that the resulting points are allowed to be within");

  const bNodeTree *tree = b.tree_or_null();
  const bNode *node = b.node_or_null();
  if (!node || !tree) {
    return;
  }
  const GeometryNodeFitCurves &storage = node_storage(*node);

  PanelDeclarationBuilder &panel =
      b.add_panel("Extra Dimensions")
          .description(
              "Evaluate these fields on the point domain and uses them together with the curve "
              "positions to fit n-dimensional Bézier curves")
          .default_closed(true);
  const Span<GeometryNodeFitCurvesItem> items(storage.items, storage.items_num);
  for (const int i : items.index_range()) {
    const GeometryNodeFitCurvesItem &item = items[i];
    const eNodeSocketDatatype data_type = eNodeSocketDatatype(item.data_type);
    const std::string identifier = ItemsAccessor::socket_identifier_for_item(item);
    panel.add_input(data_type, item.name, identifier)
        .supports_field()
        .socket_name_ptr(&tree->id, *FitCurvesItemsAccessor::item_srna, &item, "name");
  }
  panel.add_input<decl::Extend>("", "__extend__").structure_type(StructureType::Field);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "mode", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_layout_ex(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  bNodeTree &tree = *reinterpret_cast<bNodeTree *>(ptr->owner_id);
  bNode &node = *static_cast<bNode *>(ptr->data);
  if (ui::Layout *panel = layout.panel(C, "field_items", false, IFACE_("Fields"))) {
    socket_items::ui::draw_items_list_with_operators<ItemsAccessor>(C, panel, tree, node);
    socket_items::ui::draw_active_item_props<ItemsAccessor>(tree, node, [&](PointerRNA *item_ptr) {
      panel->use_property_split_set(true);
      panel->use_property_decorate_set(false);
      panel->prop(item_ptr, "data_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    });
  }
}

static bke::CurvesGeometry fit_curves_geometry(const bke::CurvesGeometry &src_curves,
                                               const IndexMask &poly_curves,
                                               const Field<bool> &selection_field,
                                               const Field<bool> &corners_field,
                                               const Field<float> &threshold_field,
                                               const Span<fn::GField> attribute_fields,
                                               const GeometryNodeFitCurvesMode mode,
                                               const AttributeFilter &attribute_filter)
{
  const bke::CurvesFieldContext curve_field_context{src_curves, AttrDomain::Curve};
  fn::FieldEvaluator curve_evaluator{curve_field_context, &poly_curves};
  curve_evaluator.set_selection(selection_field);
  curve_evaluator.add(threshold_field);
  curve_evaluator.evaluate();

  AlignedBuffer<8192, 8> allocation_buffer;
  ResourceScope scope;
  scope.allocator().provide_buffer(allocation_buffer);

  const bke::CurvesFieldContext point_field_context{src_curves, AttrDomain::Point};
  fn::FieldEvaluator point_evaluator{point_field_context, src_curves.points_num()};
  point_evaluator.add(corners_field);

  Array<GMutableSpan> attribute_spans(attribute_fields.size());
  for (const int i : attribute_fields.index_range()) {
    const CPPType &type = attribute_fields[i].cpp_type();
    attribute_spans[i] = {type,
                          scope.allocator().allocate_array(type, src_curves.points_num()),
                          src_curves.points_num()};
    point_evaluator.add_with_destination(attribute_fields[i], attribute_spans[i]);
  }
  point_evaluator.evaluate();

  geometry::FitMethod method;
  switch (mode) {
    case GEO_NODE_CURVE_FIT_SPLIT:
      method = geometry::FitMethod::Split;
      break;
    case GEO_NODE_CURVE_FIT_REFIT:
      method = geometry::FitMethod::Refit;
      break;
    default:
      BLI_assert_unreachable();
  }

  bke::CurvesGeometry curves = geometry::fit_poly_curve_attributes_to_bezier_curves(
      src_curves,
      curve_evaluator.get_evaluated_selection_as_mask(),
      attribute_spans.as_span(),
      curve_evaluator.get_evaluated<float>(0),
      point_evaluator.get_evaluated<bool>(0),
      method,
      attribute_filter);

  geometry::debug_randomize_curve_order(&curves);
  return curves;
}

static Curves *fit_curves(const Curves &src_curves_id,
                          const Field<bool> &selection_field,
                          const Field<bool> &corners_field,
                          const Field<float> &threshold_field,
                          const Span<fn::GField> attribute_fields,
                          const GeometryNodeFitCurvesMode mode,
                          const NodeAttributeFilter attribute_filter)
{
  const bke::CurvesGeometry &src_curves = src_curves_id.geometry.wrap();

  IndexMaskMemory memory;
  const IndexMask poly_curves = src_curves.indices_for_curve_type(CURVE_TYPE_POLY, memory);
  if (poly_curves.is_empty()) {
    return nullptr;
  }
  bke::CurvesGeometry dst_curves = fit_curves_geometry(src_curves,
                                                       poly_curves,
                                                       selection_field,
                                                       corners_field,
                                                       threshold_field,
                                                       attribute_fields,
                                                       mode,
                                                       attribute_filter);
  Curves *dst_curves_id = bke::curves_new_nomain(std::move(dst_curves));
  bke::curves_copy_parameters(src_curves_id, *dst_curves_id);
  return dst_curves_id;
}

static bool fit_grease_pencil_curves(GreasePencil &grease_pencil,
                                     const Field<bool> &selection_field,
                                     const Field<bool> &corners_field,
                                     const Field<float> &threshold_field,
                                     const Span<fn::GField> attribute_fields,
                                     const GeometryNodeFitCurvesMode mode,
                                     const NodeAttributeFilter attribute_filter)
{
  using namespace bke::greasepencil;
  bool has_poly_curves = false;
  for (const int layer_index : grease_pencil.layers().index_range()) {
    Drawing *drawing = grease_pencil.get_eval_drawing(grease_pencil.layer(layer_index));
    if (drawing == nullptr) {
      continue;
    }

    const bke::CurvesGeometry &src_curves = drawing->strokes();
    IndexMaskMemory memory;
    const IndexMask poly_curves = src_curves.indices_for_curve_type(CURVE_TYPE_POLY, memory);
    if (poly_curves.is_empty()) {
      continue;
    }
    bke::CurvesGeometry dst_curves = fit_curves_geometry(src_curves,
                                                         poly_curves,
                                                         selection_field,
                                                         corners_field,
                                                         threshold_field,
                                                         attribute_fields,
                                                         mode,
                                                         attribute_filter);
    drawing->strokes_for_write() = std::move(dst_curves);
    drawing->tag_topology_changed();
    has_poly_curves = true;
  }
  return has_poly_curves;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Curves");
  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");
  const Field<bool> corners_field = params.extract_input<Field<bool>>("Corners");
  const Field<float> threshold_field = params.extract_input<Field<float>>("Error");
  const NodeAttributeFilter attribute_filter = params.get_attribute_filter("Curves");

  const GeometryNodeFitCurves &storage = node_storage(params.node());
  const GeometryNodeFitCurvesMode mode = static_cast<GeometryNodeFitCurvesMode>(storage.mode);
  const Span<GeometryNodeFitCurvesItem> items(storage.items, storage.items_num);

  Vector<fn::GField> attribute_fields(items.size());
  for (const int i : items.index_range()) {
    const std::string identifier = ItemsAccessor::socket_identifier_for_item(items[i]);
    attribute_fields[i] = params.extract_input<fn::GField>(identifier);
  }

  bool has_any_poly_curves = false;
  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry_set) {
    if (const Curves *src_curves_id = geometry_set.get_curves()) {
      const bke::CurvesGeometry &src_curves = src_curves_id->geometry.wrap();
      if (!src_curves.has_curve_with_type(CURVE_TYPE_POLY)) {
        return;
      }
      Curves *dst_curves_id = fit_curves(*src_curves_id,
                                         selection_field,
                                         corners_field,
                                         threshold_field,
                                         attribute_fields.as_span(),
                                         mode,
                                         attribute_filter);
      if (dst_curves_id) {
        geometry_set.replace_curves(dst_curves_id);
      }
      has_any_poly_curves = true;
    }
    if (geometry_set.has_grease_pencil()) {
      GreasePencil &grease_pencil = *geometry_set.get_grease_pencil_for_write();
      if (fit_grease_pencil_curves(grease_pencil,
                                   selection_field,
                                   corners_field,
                                   threshold_field,
                                   attribute_fields.as_span(),
                                   mode,
                                   attribute_filter))
      {
        has_any_poly_curves = true;
      }
    }
  });

  if (!has_any_poly_curves) {
    params.error_message_add(NodeWarningType::Warning, "Input curves have no poly curves");
  }

  params.set_output("Curves", std::move(geometry_set));
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  GeometryNodeFitCurves *data = MEM_new<GeometryNodeFitCurves>(__func__);
  node->storage = data;
}

static void node_free_storage(bNode *node)
{
  socket_items::destruct_array<ItemsAccessor>(*node);
  MEM_delete(static_cast<GeometryNodeFitCurves *>(node->storage));
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  const GeometryNodeFitCurves &src_storage = node_storage(*src_node);
  auto *dst_storage = MEM_new<GeometryNodeFitCurves>(__func__, dna::shallow_copy(src_storage));
  dst_node->storage = dst_storage;

  socket_items::copy_array<ItemsAccessor>(*src_node, *dst_node);
}

static void node_operators()
{
  socket_items::ops::make_common_operators<ItemsAccessor>();
}

static bool node_insert_link(bke::NodeInsertLinkParams &params)
{
  return socket_items::try_add_item_via_any_extend_socket<ItemsAccessor>(
      params.ntree, params.node, params.node, params.link);
}

static void node_blend_write(const bNodeTree & /*tree*/, const bNode &node, BlendWriter &writer)
{
  socket_items::blend_write<ItemsAccessor>(&writer, node);
}

static void node_blend_read(bNodeTree & /*tree*/, bNode &node, BlendDataReader &reader)
{
  socket_items::blend_read_data<ItemsAccessor>(&reader, node);
}

static const bNodeSocket *node_internally_linked_input(const bNodeTree & /*tree*/,
                                                       const bNode &node,
                                                       const bNodeSocket &output_socket)
{
  return node.input_by_identifier(output_socket.identifier);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeFitCurves");
  ntype.ui_name = "Fit Curves";
  ntype.ui_description = "Fit the points of the input curves to Bézier curves";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  bke::node_type_storage(ntype, "GeometryNodeFitCurves", node_free_storage, node_copy_storage);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  ntype.draw_buttons_ex = node_layout_ex;
  ntype.register_operators = node_operators;
  ntype.insert_link = node_insert_link;
  ntype.ignore_inferred_input_socket_visibility = true;
  // ntype.gather_link_search_ops = node_gather_link_search_ops;
  ntype.internally_linked_input = node_internally_linked_input;
  ntype.blend_write_storage_content = node_blend_write;
  ntype.blend_data_read_storage_content = node_blend_read;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace nodes::node_geo_fit_curves_cc

namespace nodes {

StructRNA **FitCurvesItemsAccessor::item_srna = &RNA_GeometryNodeFitCurvesItem;

void FitCurvesItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  BLO_write_string(writer, item.name);
}

void FitCurvesItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

}  // namespace nodes
}  // namespace blender
