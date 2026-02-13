/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_intersect_lists_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  // TODO: generalize socket type
  b.add_input<decl::String>("Lists").multi_input().hide_value();
  b.add_output<decl::Bool>("Intersect");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  auto input_variants = params.extract_input<GeoNodesMultiInput<SocketValueVariant>>("Lists");
  Vector<ListPtr> lists;
  for (SocketValueVariant &input_variant : input_variants.values) {
    if (!input_variant.is_list()) {
      params.set_output("Intersect", false);
      return;
    }
    ListPtr list = input_variant.extract<ListPtr>();
    if (!list) {
      params.set_output("Intersect", false);
      return;
    }
    lists.append(std::move(list));
  }
  if (lists.is_empty()) {
    params.set_output("Intersect", false);
    return;
  }
  const List &first_list = *lists[0];
  Map<std::string, int> values;
  first_list.foreach<std::string>([&](const std::string &value) { values.add(value, 1); });
  for (const int i : lists.index_range().drop_front(1)) {
    const List &list = *lists[i];
    Set<std::string> list_values;
    list.foreach<std::string>([&](const std::string &value) {
      int *count = values.lookup_ptr(value);
      if (count && list_values.add(value)) {
        *count += 1;
      }
    });
  }

  const bool any_intersection = std::any_of(
      values.values().begin(), values.values().end(), [&](const int count) {
        return count == lists.size();
      });
  params.set_output("Intersect", any_intersection);
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeIntersectLists");
  ntype.ui_name = "Intersect Lists";
  ntype.ui_description = "Check if all given lists have values in common";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_intersect_lists_cc
