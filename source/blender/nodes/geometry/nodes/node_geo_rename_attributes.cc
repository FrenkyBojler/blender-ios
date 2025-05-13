/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include <fmt/format.h>

namespace blender::nodes::node_geo_rename_attributes_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Geometry>("Geometry");
  b.add_output<decl::Geometry>("Geometry").propagate_all().align_with_previous();
  b.add_input<decl::String>("From").is_attribute_name();
  b.add_input<decl::String>("To").is_attribute_name();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");
  const std::string from_pattern = params.extract_input<std::string>("From");
  const std::string to_pattern = params.extract_input<std::string>("To");

  const int from_wildcard_count = Span(from_pattern.c_str(), from_pattern.size()).count('*');
  const int to_wildcard_count = Span(to_pattern.c_str(), to_pattern.size()).count('*');

  if (from_wildcard_count != 1 || to_wildcard_count != 1) {
    const std::string message = TIP_("Both patterns must have a single * each");
    params.error_message_add(NodeWarningType::Warning, message);
    params.set_output("Geometry", std::move(geometry_set));
    return;
  }

  const int from_wildcard_index = from_pattern.find('*');
  const int to_wildcard_index = to_pattern.find('*');

  const StringRef from_prefix = StringRef(from_pattern).substr(0, from_wildcard_index);
  const StringRef from_suffix = StringRef(from_pattern).substr(from_wildcard_index + 1);
  const StringRef to_prefix = StringRef(to_pattern).substr(0, to_wildcard_index);
  const StringRef to_suffix = StringRef(to_pattern).substr(to_wildcard_index + 1);

  Mutex attribute_log_mutex;
  Set<std::string> removed_attributes;
  Set<std::string> added_attributes;
  Set<std::string> from_failed_attributes;
  Set<std::string> to_failed_attributes;

  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    for (const GeometryComponent::Type type : {GeometryComponent::Type::Mesh,
                                               GeometryComponent::Type::PointCloud,
                                               GeometryComponent::Type::Curve,
                                               GeometryComponent::Type::Instance,
                                               GeometryComponent::Type::GreasePencil})
    {
      if (!geometry_set.has(type)) {
        continue;
      }
      /* First check if the attribute exists before getting write access,
       * to avoid potentially expensive unnecessary copies. */
      const GeometryComponent &read_only_component = *geometry_set.get_component(type);
      Vector<std::string> attribute_stems;
      read_only_component.attributes()->foreach_attribute([&](const bke::AttributeIter &iter) {
        const StringRef attribute_name = iter.name;
        if (bke::attribute_name_is_anonymous(attribute_name)) {
          return;
        }
        if (attribute_name.startswith(from_prefix)) {
          StringRef attribute_name_without_from_prefix = attribute_name.drop_known_prefix(
              from_prefix);
          if (attribute_name_without_from_prefix.endswith(from_suffix)) {
            attribute_stems.append(
                attribute_name_without_from_prefix.drop_suffix(from_suffix.size()));
          }
        }
      });
      if (attribute_stems.is_empty()) {
        break;
      }

      GeometryComponent &component = geometry_set.get_component_for_write(type);
      for (const StringRef attribute_stem : attribute_stems) {
        std::string attribute_name_from = from_prefix + attribute_stem + from_suffix;
        std::string attribute_name_to = to_prefix + attribute_stem + to_suffix;
        if (!bke::allow_procedural_attribute_access(attribute_name_from)) {
          continue;
        }
        if (!bke::allow_procedural_attribute_access(attribute_name_to)) {
          continue;
        }
        std::optional<MutableAttributeAccessor> attributes_writer =
            component.attributes_for_write();
        if (attributes_writer->rename(attribute_name_from, attribute_name_to)) {
          std::lock_guard lock{attribute_log_mutex};
          /* The rename function will silently ignore failure to remove the old attribute.
           * This may be intentional. Catch this to warn the user. */
          if (attributes_writer->contains(attribute_name_from)) {
            from_failed_attributes.add(attribute_name_from);
          }
          else {
            removed_attributes.add(attribute_name_from);
          }
          added_attributes.add(attribute_name_to);
        }
        else {
          std::lock_guard lock{attribute_log_mutex};
          to_failed_attributes.add(attribute_name_to);
        }
      }
    }
  });

  for (const StringRef attribute_name : removed_attributes) {
    params.used_named_attribute(attribute_name, NamedAttributeUsage::Remove);
  }

  for (const StringRef attribute_name : added_attributes) {
    params.used_named_attribute(attribute_name, NamedAttributeUsage::Write);
  }

  if (!from_failed_attributes.is_empty()) {
    Vector<std::string> quoted_attribute_names;
    for (const StringRef attribute_name : from_failed_attributes) {
      quoted_attribute_names.append(fmt::format("\"{}\"", attribute_name));
    }
    const std::string message = fmt::format(
        fmt::runtime(TIP_("Cannot remove built-in attributes: {}")),
        fmt::join(quoted_attribute_names, ", "));
    params.error_message_add(NodeWarningType::Warning, message);
  }
  if (!to_failed_attributes.is_empty()) {
    Vector<std::string> quoted_attribute_names;
    for (const StringRef attribute_name : to_failed_attributes) {
      quoted_attribute_names.append(fmt::format("\"{}\"", attribute_name));
    }
    const std::string message = fmt::format(
        fmt::runtime(TIP_("Conflicts with existing or built-in attributes: {}")),
        fmt::join(quoted_attribute_names, ", "));
    params.error_message_add(NodeWarningType::Warning, message);
  }

  params.set_output("Geometry", std::move(geometry_set));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeRenameAttributes");
  ntype.ui_name = "Rename Attributes";
  ntype.ui_description =
      "Rewrite attribute names by stripping and then adding prefixes and suffixes";
  ntype.nclass = NODE_CLASS_ATTRIBUTE;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_rename_attributes_cc
