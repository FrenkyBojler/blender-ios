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
  b.add_input<decl::Int>("Target"_ustr).default_value(100).min(1);
  b.add_input<decl::Float>("Error"_ustr).default_value(0.1f);
  b.add_input<decl::Bool>("Selection"_ustr)
      .default_value(true)
      .evaluated_geometry_field()
      .hide_value();
  b.add_input<decl::Bool>("Lock Vertex"_ustr)
      .default_value(false)
      .evaluated_geometry_field()
      .hide_value();
  b.add_input<decl::Bool>("Regularize"_ustr);
  b.add_input<decl::Bool>("Permissive"_ustr);
  b.add_output<decl::Bool>("Absolute Error"_ustr);
}

static Mesh *simplify_mesh(const Mesh &src_mesh,
                           const bke::AttributeFilter &attribute_filter,
                           const int target_index_count,
                           const float error,
                           const Field<bool> &lock_vertex_field,
                           const bool regularize,
                           const bool permissive,
                           const bool absolute_error)
{
  const Span<int3> corner_tris = src_mesh.corner_tris();
  Array<int3> vert_tris(corner_tris.size());
  bke::mesh::vert_tris_from_corner_tris(src_mesh.corner_verts(), corner_tris, vert_tris);
  const Span<int> src_indices = vert_tris.as_span().cast<int>();

  bke::MeshFieldContext context(src_mesh, bke::AttrDomain::Point);
  fn::FieldEvaluator evaluator{context, src_mesh.verts_num};
  evaluator.add(lock_vertex_field);
  evaluator.evaluate();
  const IndexMask locked_vertices = evaluator.get_evaluated_as_mask(0);
  if (locked_vertices.size() == src_mesh.verts_num) {
    return BKE_mesh_copy_for_eval(src_mesh);
  }

  Array<uchar> vertex_lock;
  if (!locked_vertices.is_empty()) {
    vertex_lock = Array<uchar>(src_mesh.verts_num, 0);
    index_mask::masked_fill<uchar>(vertex_lock, meshopt_SimplifyVertex_Lock, locked_vertices);
  }

  int options = 0;
  if (regularize) {
    options |= meshopt_SimplifyRegularize;
  }
  if (permissive) {
    options |= meshopt_SimplifyPermissive;
  }
  if (absolute_error) {
    options |= meshopt_SimplifyErrorAbsolute;
  }

  Array<int> dst_vertices(src_indices.size());
  const int dst_indices_num = meshopt_simplifyWithAttributes(
      dst_vertices.data(),
      src_indices.data(),
      src_indices.size(),
      src_mesh.vert_positions().cast<float>().data(),
      src_mesh.verts_num,
      sizeof(float3),
      nullptr,
      0,
      nullptr,
      0,
      vertex_lock.is_empty() ? nullptr : vertex_lock.data(),
      target_index_count,
      error,
      options,
      nullptr);

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
  const int target_index_count = params.extract_input<int>("Target"_ustr);
  const float error = params.extract_input<float>("Error"_ustr);
  const Field<bool> lock_vertex_field = params.extract_input<Field<bool>>("Lock Vertex"_ustr);
  const bool regularize = params.extract_input<bool>("Regularize"_ustr);
  const bool permissive = params.extract_input<bool>("Permissive"_ustr);
  const bool absolute_error = params.extract_input<bool>("Absolute Error"_ustr);

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

    Mesh *mesh = simplify_mesh(*src_mesh,
                               attribute_filter,
                               target_index_count,
                               error,
                               lock_vertex_field,
                               regularize,
                               permissive,
                               absolute_error);
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
