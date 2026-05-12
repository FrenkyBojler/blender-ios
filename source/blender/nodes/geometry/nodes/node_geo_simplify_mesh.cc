/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute.hh"
#include "BKE_attribute_filter.hh"
#include "BKE_mesh.hh"
#include "BLI_offset_indices.hh"
#include "DNA_mesh_types.h"

#include "GEO_foreach_geometry.hh"
#include "GEO_randomize.hh"

#include "node_geometry_util.hh"

#include "/home/hans/blender-git/blender/lib/linux_x64/meshoptimizer/include/meshoptimizer.h"

namespace blender::nodes::node_geo_simplify_mesh_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_input<decl::Geometry>("Mesh"_ustr)
      .supported_type(GeometryComponent::Type::Mesh)
      .is_default_link_socket()
      .description("Mesh to simplify");
  b.add_output<decl::Geometry>("Mesh"_ustr).propagate_all().align_with_previous();
  b.add_input<decl::Float>("Error"_ustr).default_value(0.001f).subtype(PROP_FACTOR);
  b.add_input<decl::Bool>("Selection"_ustr).default_value(true).field_on_all().hide_value();
}

static Mesh *simplify_mesh(const Mesh &src_mesh,
                           const bke::AttributeFilter &attribute_filter,
                           const float error)
{
  const Span<int3> corner_tris = src_mesh.corner_tris();
  Array<int3> vert_tris(corner_tris.size());
  bke::mesh::vert_tris_from_corner_tris(src_mesh.corner_verts(), corner_tris, vert_tris);
  const Span<int> src_indices = vert_tris.as_span().cast<int>();

  Array<int> dst_vertices(src_indices.size());
  const int dst_indices_num = meshopt_simplify(dst_vertices.data(),
                                               src_indices.data(),
                                               src_indices.size(),
                                               src_mesh.vert_positions().cast<float>().data(),
                                               src_mesh.verts_num,
                                               sizeof(float3),
                                               3,
                                               error,
                                               0,
                                               nullptr);
  std::cout << dst_indices_num << std::endl;

  Mesh *dst_mesh = bke::mesh_new_no_attributes(
      src_mesh.verts_num, 0, dst_indices_num / 3, dst_indices_num);
  offset_indices::fill_constant_group_size(3, 0, dst_mesh->face_offsets_for_write());
  bke::MutableAttributeAccessor dst_attributes = dst_mesh->attributes_for_write();
  dst_attributes.add<int>(".corner_vert",
                          bke::AttrDomain::Corner,
                          bke::AttributeInitVArray(VArray<int>::from_span(
                              dst_vertices.as_span().take_front(dst_indices_num))));
  bke::copy_attributes(src_mesh.attributes(),
                       bke::AttrDomain::Point,
                       bke::AttrDomain::Point,
                       attribute_filter,
                       dst_attributes);
  bke::mesh_calc_edges(*dst_mesh, false, false);

  return dst_mesh;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh"_ustr);
  Field<bool> selection_field = params.extract_input<Field<bool>>("Selection"_ustr);
  const AttributeFilter &attribute_filter = params.get_attribute_filter("Mesh"_ustr);
  const float error = params.extract_input<float>("Error"_ustr);

  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry_set) {
    const Mesh *src_mesh = geometry_set.get_mesh();
    if (!src_mesh) {
      return;
    }

    const bke::MeshFieldContext context(*src_mesh, AttrDomain::Face);
    FieldEvaluator evaluator{context, src_mesh->faces_num};
    evaluator.add(selection_field);
    evaluator.evaluate();
    const IndexMask selection = evaluator.get_evaluated_as_mask(0);
    if (selection.is_empty()) {
      return;
    }

    Mesh *mesh = simplify_mesh(*src_mesh, attribute_filter, error);
    if (!mesh) {
      return;
    }

    geometry::debug_randomize_mesh_order(mesh);

    geometry_set.replace_mesh(mesh);
  });

  params.set_output("Mesh"_ustr, std::move(geometry_set));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSimplifyMesh"_ustr);
  ntype.ui_name = "Simplify Mesh";
  ntype.ui_description = "Reduce the number of triangles in a mesh";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_simplify_mesh_cc
