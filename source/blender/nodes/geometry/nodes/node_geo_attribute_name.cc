/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "BKE_attribute.hh"
#include "NOD_rna_define.hh"
#include "RNA_enum_types.hh"

namespace blender::nodes::node_geo_attribute_name_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Geometry");
  b.add_input<decl::Int>("Index").min(0).default_value(0);

  b.add_output<decl::String>("Attribute Name");
  b.add_output<decl::Int>("Total Attributes");
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiLayoutSetPropSep(layout, true);
  uiLayoutSetPropDecorate(layout, false);
  uiItemR(layout, ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
  uiItemR(layout, ptr, "domain", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const bNode &node = params.node();

  /* Get inputs from sockets. */
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");
  const int search_index = params.extract_input<int>("Index");
  const eCustomDataType data_type = eCustomDataType(node.custom1);
  const AttrDomain domain = AttrDomain(node.custom2);

  if (geometry_set.is_empty()) {
    params.set_default_remaining_outputs();
    return;
  }

  const GeometryComponent *component = nullptr;
  for (const GeometryComponent::Type type : {GeometryComponent::Type::Mesh,
                                             GeometryComponent::Type::PointCloud,
                                             GeometryComponent::Type::Curve,
                                             GeometryComponent::Type::Instance,
                                             GeometryComponent::Type::GreasePencil})
  {
    if (geometry_set.has(type)) {
      component = geometry_set.get_component(type);
      break;
    }
  }

  const AttributeAccessor attributes = *component->attributes();
  int attribute_count = 0;
  std::string attribute_name;

  attributes.foreach_attribute([&](const AttributeIter &iter) {
    if (iter.domain == domain && iter.data_type == data_type && iter.name[0] != '.') {
      if (attribute_count == search_index) {
        attribute_name = iter.name;
      }
      attribute_count++;
    }
  });

  params.set_output("Attribute Name", attribute_name);
  params.set_output("Total Attributes", attribute_count);
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(
      srna,
      "data_type",
      "Data Type",
      "Type of attribute data to filter",
      rna_enum_attribute_type_items,
      NOD_inline_enum_accessors(custom1),
      CD_PROP_FLOAT,
      [](bContext * /*C*/, PointerRNA * /*ptr*/, PropertyRNA * /*prop*/, bool *r_free) {
        *r_free = true;
        return enum_items_filter(rna_enum_attribute_type_items,
                                 enums::generic_attribute_type_supported);
      });

  RNA_def_node_enum(srna,
                    "domain",
                    "Domain",
                    "Which attribute to filter",
                    rna_enum_attribute_domain_items,
                    NOD_inline_enum_accessors(custom2),
                    int(AttrDomain::Point));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeAttributeName");
  ntype.ui_name = "Attribute Name";
  ntype.ui_description =
      "Retrieves string name of attribute given an index, data type, and domain";
  ntype.nclass = NODE_CLASS_ATTRIBUTE;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  blender::bke::node_register_type(&ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_attribute_name_cc
