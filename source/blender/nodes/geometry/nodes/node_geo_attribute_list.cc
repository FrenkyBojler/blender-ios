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

#include "NOD_geo_bundle.hh"
#include "NOD_geometry_nodes_bundle.hh"

namespace blender::nodes::node_geo_attribute_list_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Geometry");
  b.add_output<decl::Bundle>("Attributes").structure_type(StructureType::List);
  b.add_input<decl::Bool>("Filter").default_value(true);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);
  layout.prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(ptr, "domain", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = CD_PROP_FLOAT;
  node->custom2 = int8_t(AttrDomain::Point);
}

static bool component_is_available(const GeometrySet &geometry,
                                   const GeometryComponent::Type type,
                                   const AttrDomain domain)
{
  if (!geometry.has(type)) {
    return false;
  }
  const GeometryComponent &component = *geometry.get_component(type);
  return component.attribute_domain_size(domain) != 0;
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
  const bNode &node = params.node();

  const GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");
  const bool filter = params.extract_input<bool>("Filter");
  const eCustomDataType data_type = eCustomDataType(node.custom1);
  const AttrDomain domain = AttrDomain(node.custom2);

  const GeometryComponent *component = find_source_component(geometry_set, domain);
  if (!component) {
    params.set_default_remaining_outputs();
    return;
  }

  const AttributeAccessor attributes = *component->attributes();
  Vector<BundlePtr> bundles;

  attributes.foreach_attribute([&](const AttributeIter &iter) {
    bool valid_name;
    if (filter) {
      valid_name = iter.domain == domain &&
                   iter.data_type == bke::custom_data_type_to_attr_type(data_type) &&
                   iter.name[0] != '.';
    }
    else {
      valid_name = iter.name[0] != '.';
    }

    if (valid_name) {
      BundlePtr bundle_ptr;
      bundle_ptr = Bundle::create();
      Bundle &bundle = bundle_ptr.ensure_mutable_inplace();
      bundle.add("Name", std::string(iter.name));
      bundle.add("Data Type", static_cast<int>(iter.data_type));
      bundle.add("Domain", static_cast<int>(iter.domain));
      bundles.append(bundle_ptr);
    }
  });

  if (bundles.is_empty()) {
    params.set_default_remaining_outputs();
    return;
  }

  parallel_sort(bundles.begin(), bundles.end(), [](const BundlePtr &a, const BundlePtr &b) {
    return (a->lookup<std::string>("Name")) < (b->lookup<std::string>("Name"));
  });

  params.set_output("Attributes", List::from_container(bundles));
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

  geo_node_type_base(&ntype, "GeometryNodeAttributeList");
  ntype.ui_name = "Attribute List";
  ntype.ui_description = "Samples attribute names as a list";
  ntype.nclass = NODE_CLASS_ATTRIBUTE;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_attribute_list_cc
