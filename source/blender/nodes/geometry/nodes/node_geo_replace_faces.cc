/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_geometry_set.hh"
#include "BKE_instances.hh"

#include "BKE_node_socket_value.hh"
#include "DNA_node_tree_interface_types.h"
#include "GEO_mesh_replace_faces.hh"

#include "NOD_geometry_nodes_list.hh"
#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_replace_faces_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Geometry>("Mesh"_ustr)
      .only_realized_data()
      .supported_type(GeometryComponent::Type::Mesh)
      .description("Base mesh to instantiate face meshes on");
  b.add_output<decl::Geometry>("Mesh"_ustr).propagate_all_geometry().align_with_previous();
  b.add_output<decl::Bool>("Selection"_ustr)
      .structure_type(StructureType::Field)
      .propagate_references()
      .description("The faces to replace with the provided meshes");
  b.add_input<decl::Geometry>("Meshes"_ustr)
      .supported_type(GeometryComponent::Type::Mesh)
      .structure_type(StructureType::List);
  b.add_input<decl::Int>("Indices"_ustr).min(0).hide_value().evaluated_geometry_field({0});
}

static void node_geo_exec(GeoNodeExecParams params)
{
  bke::GeometrySet geometry = params.extract_input<bke::GeometrySet>("Mesh"_ustr);
  const Mesh *base = geometry.get_mesh();
  if (!base) {
    params.set_output("Mesh"_ustr, std::move(geometry));
    return;
  }
  auto geometries = params.extract_input<bke::SocketValueVariant>("Meshes"_ustr);

  Vector<bke::GeometrySet> geometry_storage;
  Array<const Mesh *> meshes;
  if (geometries.is_single()) {
    geometry_storage.append(geometries.extract<bke::GeometrySet>());
    meshes.reinitialize(1);
    meshes.first() = geometry_storage.last().get_mesh();
  }
  else if (geometries.is_list()) {
    auto list = geometries.extract<GListPtr>();
    VArray<bke::GeometrySet> varray = list->varray().typed<bke::GeometrySet>();
    geometry_storage.resize(varray.size());
    varray.materialize(geometry_storage);
    meshes.reinitialize(varray.size());
    for (const int i : geometry_storage.index_range()) {
      meshes[i] = geometry_storage[i].get_mesh();
    }
  }
  else {
    params.set_output("Mesh"_ustr, std::move(geometry));
    return;
  }

  if (meshes.as_span().contains(nullptr)) {
    params.set_output("Mesh"_ustr, std::move(geometry));
    return;
  }

  const fn::Field<bool> selection = params.extract_input<fn::Field<bool>>("Selection"_ustr);
  const fn::Field<int> indices = params.extract_input<fn::Field<int>>("Indices"_ustr);
  Mesh *result = geometry::replace_faces(*base, selection, indices, meshes);
  geometry.replace_mesh(result);
  params.set_output("Mesh"_ustr, std::move(geometry));
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeReplaceFaces"_ustr);
  ntype.ui_name = "Replace Faces";
  ntype.ui_description = "Instantiate meshes on faces of a base mesh";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_replace_faces_cc
