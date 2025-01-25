/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_geometry_set.hh"

#include "bmesh.hh"
#include "bmesh_tools.hh"

#include "BKE_type_conversions.hh"

#include "GEO_dyntopo.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_dyntopo_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Mesh");
  b.add_input<decl::Vector>("UV").field_on_all().hide_value();
  b.add_input<decl::Vector>("Position");
  b.add_input<decl::Float>("Radius");
  b.add_input<decl::Float>("Max Length");

  b.add_input<decl::Bool>("BMesh");

  b.add_output<decl::Geometry>("Mesh").propagate_all();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh");
  GField uv_field = params.extract_input<GField>("UV");

  const float2 position = params.extract_input<float3>("Position").xy();
  const float radius = params.extract_input<float>("Radius");
  const float max_length = params.extract_input<float>("Max Length");

  const Field<float2> uv_field_typed = bke::get_implicit_type_conversions().try_convert(
      std::move(uv_field), CPPType::get<float2>());

  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    const Mesh *mesh = geometry_set.get_mesh();
    if (mesh == nullptr) {
      return;
    }
    bke::MeshFieldContext context(*mesh, bke::AttrDomain::Point);
    FieldEvaluator evaluator(context, mesh->verts_num);
    evaluator.add(uv_field_typed);
    evaluator.evaluate();
    const VArraySpan<float2> mesh_uv = evaluator.get_evaluated<float2>(0);

    geometry_set.replace_mesh(
        geometry::dyntopo::subdivide(*mesh, mesh_uv, position, radius, max_length));
  });

  params.set_output("Mesh", std::move(geometry_set));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeDyntopo");
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.ui_name = "Dyntopo";
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_dyntopo_cc
