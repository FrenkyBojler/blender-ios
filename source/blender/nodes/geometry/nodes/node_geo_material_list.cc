/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_material.hh"

#include "DNA_curves_types.h"
#include "DNA_grease_pencil_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_pointcloud_types.h"

#include "NOD_geometry_nodes_list.hh"

#include "UI_interface_icons.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_material_list {

const EnumPropertyItem geometry_component_type_with_material_items[] = {
  {int(blender::bke::GeometryComponent::Type::Mesh),
    "MESH",
    ICON_MESH_DATA,
    "Mesh",
    "Mesh component containing point, corner, edge and face data"},
  {int(blender::bke::GeometryComponent::Type::PointCloud),
    "POINTCLOUD",
    ICON_POINTCLOUD_DATA,
    "Point Cloud",
    "Point cloud component containing only point data"},
  {int(blender::bke::GeometryComponent::Type::Curve),
    "CURVE",
    ICON_CURVE_DATA,
    "Curve",
    "Curve component containing spline and control point data"},
  {int(blender::bke::GeometryComponent::Type::GreasePencil),
    "GREASEPENCIL",
    ICON_GREASEPENCIL,
    "Grease Pencil",
    "Grease Pencil component containing layers and curves data"},
  {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Geometry"_ustr);
  b.add_input<decl::Menu>("Component Type"_ustr)
    .static_items(geometry_component_type_with_material_items)
    .optional_label();
  b.add_output<decl::Material>("Materials"_ustr).structure_type(StructureType::List);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry"_ustr);
  const GeometryComponent::Type component = params.extract_input<GeometryComponent::Type>("Component Type"_ustr);

  if (!geometry_set.has(component)) {
    params.set_default_remaining_outputs();
    return;
  }

  Material **materials = nullptr;
  short count = 0;

  switch (component) {
  case GeometryComponent::Type::Curve: {
    const Curves &curves = *geometry_set.get_curves();
    materials = curves.mat;
    count = curves.totcol;
  } break;
  case GeometryComponent::Type::GreasePencil: {
    const GreasePencil &grease_pencil = *geometry_set.get_grease_pencil();
    materials = grease_pencil.material_array;
    count = grease_pencil.material_array_num;
  } break;
  case GeometryComponent::Type::Mesh: {
    const Mesh &mesh = *geometry_set.get_mesh();
    materials = mesh.mat;
    count = mesh.totcol;
  } break;
  case GeometryComponent::Type::PointCloud: {
    const PointCloud &point_cloud = *geometry_set.get_pointcloud();
    materials = point_cloud.mat;
    count = point_cloud.totcol;
  } break;
  default:
    BLI_assert_unreachable();
    break;
  }

  if (count == 0 || materials == nullptr) {
    params.set_default_remaining_outputs();
    return;
  }

  const CPPType &type = CPPType::get<Material *>();
  GArray<> array(type, count, NoInitialization());
  type.move_assign_n(materials, array.data(), count);
  params.set_output("Materials"_ustr, GList::from_garray(std::move(array)));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeMaterialList"_ustr);
  ntype.ui_name = "Material List";
  ntype.ui_description = "Get a list of the materials used by a geometry component";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_material_list
