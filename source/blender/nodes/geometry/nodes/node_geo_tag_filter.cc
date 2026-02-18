/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geo_tag_filter.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_tag_filter_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("Tag Filter").optional_label();
  b.add_input<decl::String>("Tags").structure_type(StructureType::List);
  b.add_output<decl::Bool>("Match");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  SocketValueVariant tags_variant = params.extract_input<SocketValueVariant>("Tags");
  const std::string tag_filter = params.extract_input<std::string>("Tag Filter");

  Set<std::string> tags;
  if (tags_variant.is_list()) {
    const ListPtr tags_list_ptr = tags_variant.extract<ListPtr>();
    if (tags_list_ptr) {
      const List &list = *tags_list_ptr;
      if (list.cpp_type().is<std::string>()) {
        list.foreach<std::string>([&](const std::string &tag) { tags.add(tag); });
      }
    }
  }
  const bool match = tag_filter_matches(tag_filter, tags);
  params.set_output("Match", match);
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeTagFilter");
  ntype.ui_name = "Tag Filter";
  ntype.ui_description = "Check if a filter string matches a list of tags";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_tag_filter_cc

namespace blender::nodes {

bool tag_filter_matches(const StringRef tag_filter, const Set<std::string> &tags)
{
  if (tag_filter.is_empty()) {
    /* An empty filter matches everything. */
    return true;
  }
  StringRef remaining = tag_filter;
  while (!remaining.is_empty()) {
    const int sep = remaining.find(',');
    if (sep == -1) {
      const StringRef tag = remaining.trim();
      if (tags.contains_as(tag)) {
        return true;
      }
      return false;
    }
    const StringRef tag = remaining.substr(0, sep).trim();
    if (tags.contains_as(tag)) {
      return true;
    }
    remaining = remaining.substr(sep + 1);
  }
  return false;
}

}  // namespace blender::nodes
