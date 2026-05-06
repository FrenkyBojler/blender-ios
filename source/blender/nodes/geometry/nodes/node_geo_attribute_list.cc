/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute.hh"
#include "BKE_attribute_legacy_convert.hh"

#include "BLI_sort.hh"

#include "NOD_rna_define.hh"

#include "RNA_enum_types.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_attribute_list_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Geometry"_ustr);

  b.add_input<decl::Bool>("Filter Data Type"_ustr).default_value(true);
  b.add_input<decl::Menu>("Data Type"_ustr)
      .static_items(rna_enum_attribute_type_items)
      .default_value(CD_PROP_FLOAT)
      .optional_label();

  b.add_input<decl::Bool>("Filter Domain"_ustr).default_value(true);
  b.add_input<decl::Menu>("Domain"_ustr)
      .static_items(rna_enum_attribute_domain_items)
      .default_value(AttrDomain::Point)
      .optional_label();

  b.add_output<decl::String>("Names"_ustr).structure_type(StructureType::List);
}

static bool component_is_available(const GeometrySet &geometry,
                                   const GeometryComponent::Type type,
                                   const AttrDomain domain)
{
  if (!geometry.has(type)) {
    return false;
  }
  const GeometryComponent &component = *geometry.get_component(type);
  return ((component.attribute_domain_size(domain) != 0));
}

static const GeometryComponent *find_source_component(const GeometrySet &geometry,
                                                      const AttrDomain domain)
{
  /* Choose the other component based on a consistent order, rather than some more complicated
   * heuristic. This is the same order visible in the spreadsheet and used in the ray-cast node. */
  static const Array<GeometryComponent::Type> supported_types = {
      GeometryComponent::Type::Mesh,
      GeometryComponent::Type::PointCloud,
      GeometryComponent::Type::Curve,
      GeometryComponent::Type::Instance,
      GeometryComponent::Type::GreasePencil};
  for (const GeometryComponent::Type src_type : supported_types) {
    if (component_is_available(geometry, src_type, domain)) {
      return geometry.get_component(src_type);
    }
  }

  return nullptr;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry"_ustr);
  const bool filter_data_type = params.extract_input<bool>("Filter Data Type"_ustr);
  const bool filter_domain = params.extract_input<bool>("Filter Domain"_ustr);

  const eCustomDataType data_type = params.extract_input<eCustomDataType>("Data Type"_ustr);
  const AttrDomain domain = params.extract_input<AttrDomain>("Domain"_ustr);

  const GeometryComponent *component = find_source_component(geometry_set, domain);
  if (!component) {
    params.set_default_remaining_outputs();
    return;
  }

  const AttributeAccessor attributes = *component->attributes();
  Vector<std::string> names;

  const bke::AttrType type_filter = *bke::custom_data_type_to_attr_type(data_type);
  attributes.foreach_attribute([&](const AttributeIter &iter) {
    if (filter_data_type) {
      if (iter.data_type != type_filter) {
        return;
      }
    }

    if (filter_domain) {
      if (iter.domain != domain) {
        return;
      }
    }

    if (iter.name[0] == '.') {
      return;
    }
    names.append(iter.name);
  });

  if (names.is_empty()) {
    params.set_default_remaining_outputs();
    return;
  }

  parallel_sort(
      names.begin(), names.end(), [](const StringRef &a, const StringRef &b) { return a < b; });

  params.set_output("Names"_ustr, GList::from_container(names));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeAttributeList"_ustr);
  ntype.ui_name = "Attribute List";
  ntype.ui_description = "Samples attribute names as a list";
  ntype.nclass = NODE_CLASS_ATTRIBUTE;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_attribute_list_cc
