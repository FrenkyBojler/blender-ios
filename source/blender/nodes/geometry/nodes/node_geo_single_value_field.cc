/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <queue>

#include "UI_interface.hh"

#include "NOD_socket_search_link.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_node_geo_input_single_value_field_cc {

static char INPUT_FIELD[6] = "Field";
static char OUTPUT_IS_SINGLE_VALUE[7] = "Result";

NODE_STORAGE_FUNCS(NodeGeometrySingleValueField);

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();

  if (node != nullptr) {
    const eCustomDataType data_type = eCustomDataType(node_storage(*node).data_type);
    b.add_input(data_type, INPUT_FIELD)
        .hide_value()
        .field_on_all()
        .description("Field value to determine the sparse representation for");
  }

  b.add_output<decl::Bool>(OUTPUT_IS_SINGLE_VALUE)
      .description("True if the field is a sparse representation with a single value");
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  /* `data_type` is never actually used in execution, it only toggles the socket color. */
  uiItemR(layout, ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometrySingleValueField *data = MEM_callocN<NodeGeometrySingleValueField>(__func__);
  data->data_type = CD_PROP_FLOAT;
  node->storage = data;
}

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  const NodeDeclaration &declaration = *params.node_type().static_declaration;
  search_link_ops_for_declarations(params, declaration.inputs);

  const std::optional<eCustomDataType> type = bke::socket_type_to_custom_data_type(
      eNodeSocketDatatype(params.other_socket().type));
  if (type && *type != CD_PROP_STRING) {
    /* The input and output sockets have the same name. */
    params.add_item(IFACE_(INPUT_FIELD), [type](LinkSearchOpParams &params) {
      bNode &node = params.add_node("GeometryNodeIsSingleValueField");
      node_storage(node).data_type = *type;
      params.update_and_connect_available_socket(node, INPUT_FIELD);
    });
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  /* See the `SampleIndex` node as an example for utilizing the property to simplify the node
   * execution to output a single value. */
  const SocketValueVariant index_value_variant = params.extract_input<SocketValueVariant>(
      INPUT_FIELD);
  params.set_output(OUTPUT_IS_SINGLE_VALUE, !index_value_variant.is_context_dependent_field());
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeIsSingleValueField", GEO_NODE_SINGLE_VALUE_FIELD);
  ntype.ui_name = "Is Single Value";
  ntype.ui_description = "Check if a field is a single value constant or not";
  ntype.enum_name_legacy = "SINGLE_VALUE_FIELD";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  blender::bke::node_type_storage(ntype,
                                  "NodeGeometrySingleValueField",
                                  node_free_standard_storage,
                                  node_copy_standard_storage);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  ntype.gather_link_search_ops = node_gather_link_searches;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_node_geo_input_single_value_field_cc
