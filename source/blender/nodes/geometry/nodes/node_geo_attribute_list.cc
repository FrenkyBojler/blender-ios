/* SPDX-FileCopyrightText: 2025 Blender Authors
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
  b.add_input<decl::Geometry>("Geometry");
  b.add_output<decl::String>("Names").structure_type(StructureType::List);
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->use_property_split_set(true);
  layout->use_property_decorate_set(false);
  layout->prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
  layout->prop(ptr, "domain", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = CD_PROP_FLOAT;
  node->custom2 = int8_t(AttrDomain::Point);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const bNode &node = params.node();

  const GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");
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
  std::vector<AttributeIter> sort_attributes;

  attributes.foreach_attribute([&](const AttributeIter &iter) {
    if (iter.domain == domain && iter.data_type == bke::custom_data_type_to_attr_type(data_type) &&
        iter.name[0] != '.')
    {
      sort_attributes.push_back(iter);
    }
  });

  if (!sort_attributes.empty()) {
    attribute_count = sort_attributes.size();

    parallel_sort(sort_attributes.begin(),
                  sort_attributes.end(),
                  [](const AttributeIter &a, const AttributeIter &b) { return a.name < b.name; });
  }

  auto *names = new ImplicitSharedValue<Vector<std::string>>();

  for (int i = 0; i < sort_attributes.size(); i++) {
    names->data.append(sort_attributes[i].name);
  }

  List::ArrayData names_array_data = {names->data.data(), ImplicitSharingPtr<>(names)};

  params.set_output("Names",
                    List::create(CPPType::get<std::string>(),
                                 std::move(names_array_data),
                                 names->data.size()));
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
