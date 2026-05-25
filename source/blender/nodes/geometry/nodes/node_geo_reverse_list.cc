/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute_math.hh"

#include "NOD_geometry_nodes_list.hh"
#include "NOD_rna_define.hh"
#include "NOD_socket.hh"
#include "NOD_socket_search_link.hh"

#include "RNA_enum_types.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_reverse_list_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_default_layout();

  if (!node) {
    return;
  }

  const auto type = eNodeSocketDatatype(node->custom1);
  b.add_input(type, "List"_ustr).structure_type(StructureType::List).hide_value();
  b.add_output(type, "List"_ustr)
      .propagate_all({0})
      .structure_type(StructureType::List)
      .align_with_previous();
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
    bNode &node = params.add_node("GeometryNodeReverseList"_ustr);
    node.custom1 = socket_type;
    params.update_and_connect_available_socket(node, socket_name);
  }
};

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  const eNodeSocketDatatype socket_type = eNodeSocketDatatype(params.other_socket().type);
  params.add_item(IFACE_("List"), SocketSearchOp{"List"_ustr, socket_type});
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

  const CPPType &type = list->cpp_type();
  const GList::DataVariant &list_data = list->data();

  if (std::get_if<GList::SingleData>(&list_data)) {
    params.set_output("List"_ustr, std::move(list));
    return;
  }

  if (const auto *array_data = std::get_if<GList::ArrayData>(&list_data)) {
    const GSpan src_span(type, array_data->data, list_size);
    GList::ArrayData reversed_data = GList::ArrayData::ForUninitialized(type, list_size);
    GMutableSpan dst_span = reversed_data.span_for_write(type, list_size);

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
                        nodes::BundlePtr,
                        nodes::ClosurePtr,
                        GeometrySet,
                        Material,
                        Object,
                        Image,
                        VFont,
                        Scene,
                        bSound>([&]<typename T>() {
      std::reverse_copy(
          src_span.typed<T>().begin(), src_span.typed<T>().end(), dst_span.typed<T>().begin());
      array_utils::gather(src_span.typed<T>(), indices.as_span(), dst_span.typed<T>());
    });
  }

  GListPtr reversed_list = GList::create(type, std::move(reversed_data), list_size);
  params.set_output("List"_ustr, std::move(reversed_list));
  return;
}

params.set_default_remaining_outputs();
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
  geo_node_type_base(&ntype, "GeometryNodeReverseList"_ustr);
  ntype.ui_name = "Reverse List";
  ntype.ui_description = "Reverse the order of elements in a list";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.gather_link_search_ops = node_gather_link_searches;
  bke::node_register_type(ntype);
  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_reverse_list_cc
