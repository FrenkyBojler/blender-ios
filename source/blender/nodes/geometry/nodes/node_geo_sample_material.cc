/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_material.hh"

#include "DNA_curves_types.h"
#include "DNA_grease_pencil_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_pointcloud_types.h"

#include "RNA_enum_types.hh"

#include "UI_interface_icons.hh"
#include "UI_interface_layout.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_sample_material_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Geometry");
  b.add_input<decl::Int>("Material Index");
  b.add_output<decl::Material>("Material");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  using namespace blender::bke;

  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");

  Material **materials = nullptr;
  short materials_count = 0;

  const GeometryComponent::Type component = static_cast<GeometryComponent::Type>(
      params.node().custom1);

  if (!geometry_set.has(component)) {
    params.set_default_remaining_outputs();
    return;
  }

  switch (component) {
    case GeometryComponent::Type::Curve: {
      const Curves &curves = *geometry_set.get_curves();
      materials = curves.mat;
      materials_count = curves.totcol;
    } break;
    case GeometryComponent::Type::GreasePencil: {
      const GreasePencil &grease_pencil = *geometry_set.get_grease_pencil();
      materials = grease_pencil.material_array;
      materials_count = grease_pencil.material_array_num;
    } break;
    case GeometryComponent::Type::Mesh: {
      const Mesh &mesh = *geometry_set.get_mesh();
      materials = mesh.mat;
      materials_count = mesh.totcol;
    } break;
    case GeometryComponent::Type::PointCloud: {
      const PointCloud &point_cloud = *geometry_set.get_pointcloud();
      materials = point_cloud.mat;
      materials_count = point_cloud.totcol;
    } break;
    default:
      break;
  }

  const int material_index = params.extract_input<int>("Material Index");

  if (material_index < 0 || material_index >= materials_count) {
    params.set_default_remaining_outputs();
    return;
  }

  params.set_output("Material", materials[material_index]);
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->prop(ptr, "component", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(srna,
                    "component",
                    "Component",
                    "",
                    rna_enum_geometry_component_type_with_material_items,
                    NOD_inline_enum_accessors(custom1),
                    int(blender::bke::GeometryComponent::Type::Mesh));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSampleMaterial");
  ntype.ui_name = "Sample Material";
  ntype.ui_description = "Sample materials from a geometry component";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_sample_material_cc
