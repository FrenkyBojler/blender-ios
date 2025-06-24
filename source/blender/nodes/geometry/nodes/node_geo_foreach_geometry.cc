/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "BLI_string_utf8.h"

#include "BLO_read_write.hh"

namespace blender::nodes::node_geo_foreach_geometry_cc {

namespace input_node {

NODE_STORAGE_FUNCS(NodeGeometryForeachGeometryInput);

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Geometry");
  b.add_output<decl::Geometry>("Geometry").align_with_previous();
}

static void node_label(const bNodeTree * /*ntree*/,
                       const bNode * /*node*/,
                       char *label,
                       const int label_maxncpy)
{
  BLI_strncpy_utf8(label, IFACE_("For Each Geometry"), label_maxncpy);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryForeachGeometryInput *data = MEM_callocN<NodeGeometryForeachGeometryInput>(__func__);
  node->storage = data;
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeForeachGeometryInput", GEO_NODE_FOREACH_GEOMETRY_INPUT);
  ntype.ui_name = "For Each Geometry Input";
  ntype.nclass = NODE_CLASS_INTERFACE;
  ntype.declare = node_declare;
  ntype.gather_link_search_ops = nullptr;
  ntype.initfunc = node_init;
  ntype.labelfunc = node_label;
  ntype.no_muting = true;
  bke::node_type_storage(ntype,
                         "NodeGeometryForeachGeometryInput",
                         node_free_standard_storage,
                         node_copy_standard_storage);
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace input_node

namespace output_node {

NODE_STORAGE_FUNCS(NodeGeometryForeachGeometryOutput);

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Geometry");
  b.add_output<decl::Geometry>("Geometry").align_with_previous();
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryForeachGeometryOutput *data = MEM_callocN<NodeGeometryForeachGeometryOutput>(
      __func__);
  node->storage = data;
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  const NodeGeometryForeachGeometryOutput &src_storage = node_storage(*src_node);
  auto *dst_storage = MEM_dupallocN<NodeGeometryForeachGeometryOutput>(__func__, src_storage);
  dst_node->storage = dst_storage;
}

static void node_free_storage(bNode *node)
{
  MEM_freeN(node->storage);
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(
      &ntype, "GeometryNodeForeachGeometryOutput", GEO_NODE_FOREACH_GEOMETRY_OUTPUT);
  ntype.ui_name = "For Each Geometry Output";
  ntype.nclass = NODE_CLASS_INTERFACE;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.labelfunc = input_node::node_label;
  ntype.no_muting = true;
  ntype.gather_link_search_ops = nullptr;
  ntype.blend_write_storage_content = nullptr;
  ntype.blend_data_read_storage_content = nullptr;
  bke::node_type_storage(
      ntype, "NodeGeometryForeachGeometryOutput", node_free_storage, node_copy_storage);
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace output_node

}  // namespace blender::nodes::node_geo_foreach_geometry_cc
