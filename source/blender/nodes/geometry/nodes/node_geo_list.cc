/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_pointcloud.hh"
#include "DNA_pointcloud_types.h"

#include "NOD_geometry_nodes_list.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_points_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_default_layout();
  const bNode *node = b.node_or_null();

  if (node != nullptr) {
    const eNodeSocketDatatype type = eNodeSocketDatatype(node->custom1);
    b.add_output(type, "List").structure_type(StructureType::List);
  }

  b.add_input<decl::Int>("Count").default_value(1).min(0).description(
      "The number of elements in the list");

  if (node != nullptr) {
    const eNodeSocketDatatype type = eNodeSocketDatatype(node->custom1);
    b.add_input(type, "Value").field_on_all();
  }
}

class ListFieldContext : public FieldContext {
 public:
  ListFieldContext() = default;

  GVArray get_varray_for_input(const FieldInput &field_input,
                               const IndexMask &mask,
                               ResourceScope & /*scope*/) const override
  {
    const bke::IDAttributeFieldInput *id_field_input =
        dynamic_cast<const bke::IDAttributeFieldInput *>(&field_input);

    const fn::IndexFieldInput *index_field_input = dynamic_cast<const fn::IndexFieldInput *>(
        &field_input);

    if (id_field_input == nullptr && index_field_input == nullptr) {
      return {};
    }

    return fn::IndexFieldInput::get_index_varray(mask);
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  const int count = params.extract_input<int>("Count");
  if (count <= 0) {
    params.set_default_remaining_outputs();
    return;
  }

  ListPtr list = List::create();

  GField field = params.extract_input<GField>("Value");
  Field<float> radius_field = params.extract_input<Field<float>>("Radius");

  PointCloud *points = BKE_pointcloud_new_nomain(count);
  MutableAttributeAccessor attributes = points->attributes_for_write();
  AttributeWriter<float> output_radii = attributes.lookup_or_add_for_write<float>(
      "radius", AttrDomain::Point);

  ListFieldContext context{};
  fn::FieldEvaluator evaluator{context, count};
  evaluator.add_with_destination(field, list.values_for_write());
  evaluator.evaluate();

  params.set_output("List", std::move(list));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodePoints", GEO_NODE_POINTS);
  ntype.ui_name = "Points";
  ntype.ui_description = "Generate a point cloud with positions and radii defined by fields";
  ntype.enum_name_legacy = "POINTS";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_points_cc
