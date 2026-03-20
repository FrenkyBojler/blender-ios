/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "GEO_foreach_geometry.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_rename_attribute_cc {

enum class RenameMode {
  Single,
  Prefix,
};

const EnumPropertyItem rename_mode_items[] = {
    {int(RenameMode::Single),
     "SINGLE",
     0,
     "Single",
     "Rename a single attribute with the provided attribute name"},
    {int(RenameMode::Prefix),
     "PREFIX",
     0,
     "Prefix",
     "Rename all attributes with the provided prefix"},
    {},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_input<decl::Geometry>("Geometry");
  b.add_output<decl::Geometry>("Geometry").align_with_previous().propagate_all();

  b.add_input<decl::Menu>("Mode").static_items(rename_mode_items).optional_label();

  b.add_input<decl::String>("Old").optional_label();
  b.add_input<decl::String>("New").optional_label();
  b.add_input<decl::Bool>("Overwrite").default_value(false);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");
  const RenameMode mode = params.extract_input<RenameMode>("Mode");
  const std::string old_name = params.extract_input<std::string>("Old");
  const std::string new_name = params.extract_input<std::string>("New");
  const bool overwrite = params.extract_input<bool>("Overwrite");

  if (old_name.empty() || new_name.empty()) {
    params.set_output("Geometry", std::move(geometry_set));
    return;
  }

  std::atomic<bool> not_found = false;
  std::atomic<bool> rename_failed = false;
  /* TODO: Support grease pencil and instances. */
  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry) {
    for (const GeometryComponent::Type type : {
             GeometryComponent::Type::Mesh,
             GeometryComponent::Type::PointCloud,
             GeometryComponent::Type::Curve,
         })
    {
      if (!geometry.has(type)) {
        continue;
      }
      Vector<std::pair<std::string, std::string>> renames;
      {
        const GeometryComponent &component = *geometry.get_component(type);
        const AttributeAccessor attributes = *component.attributes();
        switch (mode) {
          case RenameMode::Single: {
            if (!attributes.contains(old_name)) {
              not_found = true;
              continue;
            }
            renames.append({old_name, new_name});
            break;
          }
          case RenameMode::Prefix: {
            attributes.foreach_attribute([&](const bke::AttributeIter &iter) {
              if (iter.name.startswith(old_name)) {
                renames.append({iter.name, new_name + iter.name.substr(old_name.size())});
              }
            });
            break;
          }
        }
      }
      if (renames.is_empty()) {
        continue;
      }
      GeometryComponent &component = geometry.get_component_for_write(type);
      MutableAttributeAccessor attributes = *component.attributes_for_write();
      for (const std::pair<std::string, std::string> &rename : renames) {
        if (!attributes.rename(rename.first, rename.second, overwrite)) {
          rename_failed = true;
        }
      }
    }
  });

  if (not_found) {
    params.error_message_add(NodeWarningType::Warning,
                             fmt::format("{}: '{}'", TIP_("Attribute not found"), old_name));
  }
  if (rename_failed) {
    params.error_message_add(
        NodeWarningType::Warning,
        fmt::format("{}: '{}' to '{}'", TIP_("Failed to rename attribute"), old_name, new_name));
  }

  params.set_output("Geometry", std::move(geometry_set));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeRenameAttribute");
  ntype.ui_name = "Rename Attribute";
  ntype.ui_description = "Change the name of an attribute";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_rename_attribute_cc
