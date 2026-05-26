/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute_math.hh"

#include "BLI_array_utils.hh"
#include "BLI_sort.hh"

#include "NOD_geometry_nodes_list.hh"
#include "NOD_geometry_nodes_values.hh"
#include "NOD_rna_define.hh"
#include "NOD_socket.hh"
#include "NOD_socket_search_link.hh"

#include "RNA_enum_types.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "list_function_eval.hh"
#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_sort_list_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();

  if (!node) {
    return;
  }

  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_default_layout();

  const auto type = eNodeSocketDatatype(node->custom1);
  b.add_input(type, "List"_ustr).structure_type(StructureType::List).hide_value();
  b.add_output(type, "List"_ustr)
      .propagate_all({0})
      .structure_type(StructureType::List)
      .align_with_previous();

  b.add_input<decl::Float>("Weights"_ustr)
      .default_value(0.0f)
      .hide_value()
      .structure_type(StructureType::Dynamic)
      .description("A field or list of values that will determine the sorted order");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "socket_type", UI_ITEM_NONE, "", ICON_NONE);
}

class SocketSearchOp {
 public:
  UString socket_name;
  eNodeSocketDatatype socket_type;
  void operator()(LinkSearchOpParams &params)
  {
    bNode &node = params.add_node("GeometryNodeSortList"_ustr);
    node.custom1 = socket_type;
    params.update_and_connect_available_socket(node, socket_name);
  }
};

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  const eNodeSocketDatatype socket_type = eNodeSocketDatatype(params.other_socket().type);
  if (params.in_out() == SOCK_IN) {
    if (params.node_tree().typeinfo->validate_link(socket_type, SOCK_FLOAT)) {
      params.add_item(IFACE_("Weights"), SocketSearchOp{"Weights"_ustr, SOCK_FLOAT});
    }
    params.add_item(IFACE_("List"), SocketSearchOp{"List"_ustr, socket_type});
  }
  else {
    params.add_item(IFACE_("List"), SocketSearchOp{"List"_ustr, socket_type});
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GListPtr list = params.extract_input<GListPtr>("List"_ustr);

  if (!list) {
    params.set_default_remaining_outputs();
    return;
  }

  if (!params.output_is_required("List"_ustr)) {
    return;
  }

  const int list_size = list->size();

  if (list_size <= 1) {
    params.set_output("List"_ustr, std::move(list));
    return;
  }

  bke::SocketValueVariant weights_variant = params.extract_input<bke::SocketValueVariant>(
      "Weights"_ustr);

  GListPtr weights_list;
  if (weights_variant.is_context_dependent_field()) {
    fn::GField field = weights_variant.extract<fn::GField>();
    weights_list = evaluate_field_to_list(std::move(field), list_size);
    if (!weights_list) {
      params.error_message_add(NodeWarningType::Error, "Failed to evaluate weights field");
      params.set_output("List"_ustr, std::move(list));
      return;
    }
  }
  else if (weights_variant.is_list()) {
    weights_list = weights_variant.get<GListPtr>();
    const int weights_list_size = weights_list->size();
    if (!weights_list) {
      params.set_output("List"_ustr, std::move(list));
      return;
    }
    if (weights_list_size <= 1) {
      params.set_output("List"_ustr, std::move(list));
      return;
    }
    if (weights_list_size != list_size) {
      params.error_message_add(
          NodeWarningType::Error,
          "List and Weights must have the same length (List: " + std::to_string(list_size) +
              ", Weights: " + std::to_string(weights_list->size()) + ")");
      params.set_default_remaining_outputs();
      return;
    }
  }
  else if (weights_variant.is_single()) {
    params.set_output("List"_ustr, std::move(list));
    return;
  }
  else {
    params.error_message_add(NodeWarningType::Warning,
                             "\"Weights\" input must be a field or a list");
    params.set_output("List"_ustr, std::move(list));
    return;
  }

  Array<float> weights(list_size);
  const VArray<float> weights_varray = weights_list->varray().typed<float>();
  weights_varray.materialize(weights.as_mutable_span());

  Array<int> indices(list_size);
  array_utils::fill_index_range<int>(indices.as_mutable_span());

  parallel_sort(indices.begin(), indices.end(), [&](const int index_a, const int index_b) {
    const float weight_a = weights[index_a];
    const float weight_b = weights[index_b];
    if (UNLIKELY(weight_a == weight_b)) {
      return index_a < index_b;
    }
    return weight_a < weight_b;
  });

  const CPPType &type = list->cpp_type();
  const GList::DataVariant &list_data = list->data();

  if (std::get_if<GList::SingleData>(&list_data)) {
    params.set_output("List"_ustr, std::move(list));
    return;
  }

  GList::ArrayData sorted_array_data = GList::ArrayData::ForUninitialized(type, list_size);

  if (const auto *array_data = std::get_if<GList::ArrayData>(&list_data)) {
    const GSpan src_span(type, array_data->data, list_size);
    GMutableSpan dst_span = sorted_array_data.span_for_write(type, list_size);

    type.to_static_type<float,
                        float2,
                        float3,
                        float4,
                        int,
                        int2,
                        bool,
                        int8_t,
                        short2,
                        ColorGeometry4f,
                        ColorGeometry4b,
                        math::Quaternion,
                        float4x4,
                        nodes::MenuValue,
                        std::string,
                        nodes::BundlePtr *,
                        nodes::ClosurePtr *,
                        GeometrySet,
                        Material *,
                        Object *,
                        Image *,
                        VFont *,
                        Scene *,
                        bSound *>([&]<typename T>() {
      array_utils::gather(src_span.typed<T>(), indices.as_span(), dst_span.typed<T>());
    });
  }

  GListPtr sorted_list = GList::create(type, std::move(sorted_array_data), list_size);
  params.set_output("List"_ustr, std::move(sorted_list));
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(
      srna,
      "socket_type",
      "Socket Type",
      "",
      rna_enum_node_socket_data_type_items,
      NOD_inline_enum_accessors(custom1),
      SOCK_FLOAT,
      [](bContext * /*C*/, PointerRNA *ptr, PropertyRNA * /*prop*/, bool *r_free) {
        *r_free = true;
        const bNodeTree &ntree = *reinterpret_cast<bNodeTree *>(ptr->owner_id);
        bke::bNodeTreeType *ntree_type = ntree.typeinfo;
        return enum_items_filter(
            rna_enum_node_socket_data_type_items, [&](const EnumPropertyItem &item) -> bool {
              bke::bNodeSocketType *socket_type = bke::node_socket_type_find_static(item.value);
              return ntree_type->valid_socket_type(ntree_type, socket_type);
            });
      });
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeSortList"_ustr);
  ntype.ui_name = "Sort List";
  ntype.ui_description = "Sort a list based on weights";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  ntype.declare = node_declare;
  ntype.gather_link_search_ops = node_gather_link_searches;
  bke::node_register_type(ntype);
  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_sort_list_cc
