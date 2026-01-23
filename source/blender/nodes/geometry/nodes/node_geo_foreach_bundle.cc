/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_string_utf8.h"
#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_foreach_bundle_cc {

namespace input_node {

NODE_STORAGE_FUNCS(NodeForeachBundleInput);

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_output<decl::Bundle>("Nested Bundle");
  b.add_output<decl::Bundle>("Accumulator");
  b.add_output<decl::String>("Path");

  b.add_input<decl::Bundle>("Bundle");
  b.add_input<decl::Bundle>("Initial Accumulator");
  auto &p = b.add_panel("Filter");
  p.add_input<decl::String>("Type");
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

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "NodeForeachBundleInput", NODE_FOREACH_BUNDLE_INPUT);
  ntype.ui_name = "For Each Bundle Input";
  ntype.nclass = NODE_CLASS_INTERFACE;
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.labelfunc = node_label;
  ntype.gather_link_search_ops = nullptr;
  ntype.no_muting = true;
  bke::node_type_storage(
      ntype, "NodeForeachBundleInput", node_free_standard_storage, node_copy_standard_storage);
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace input_node

namespace output_node {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_output<decl::Bundle>("Bundle");
  b.add_output<decl::Bundle>("Accumulator");

  b.add_input<decl::Bundle>("Nested Bundle");
  b.add_input<decl::Bundle>("Accumulator");
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  auto *storage = MEM_new_for_free<NodeForeachBundleOutput>(__func__);
  node->storage = storage;
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
  ntype.gather_link_search_ops = nullptr;
  ntype.no_muting = true;
  bke::node_type_storage(
      ntype, "NodeForeachBundleOutput", node_free_standard_storage, node_copy_standard_storage);
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace output_node

}  // namespace blender::nodes::node_geo_foreach_bundle_cc
