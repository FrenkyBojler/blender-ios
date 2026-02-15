/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_mesh_types.h"

#include "GEO_foreach_geometry.hh"
#include "GEO_mesh_triangulate.hh"
#include "GEO_randomize.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_triangulate_cc {

static const EnumPropertyItem rna_node_geometry_triangulate_quad_method_items[] = {
    {.value = int(geometry::TriangulateQuadMode::Beauty),
     .identifier = "BEAUTY",
     .icon = 0,
     .name = N_("Beauty"),
     .description = N_("Split the quads in nice triangles, slower method")},
    {.value = int(geometry::TriangulateQuadMode::Fixed),
     .identifier = "FIXED",
     .icon = 0,
     .name = N_("Fixed"),
     .description = N_("Split the quads on the first and third vertices")},
    {.value = int(geometry::TriangulateQuadMode::Alternate),
     .identifier = "FIXED_ALTERNATE",
     .icon = 0,
     .name = N_("Fixed Alternate"),
     .description = N_("Split the quads on the 2nd and 4th vertices")},
    {.value = int(geometry::TriangulateQuadMode::ShortEdge),
     .identifier = "SHORTEST_DIAGONAL",
     .icon = 0,
     .name = N_("Shortest Diagonal"),
     .description = N_("Split the quads along their shortest diagonal")},
    {.value = int(geometry::TriangulateQuadMode::LongEdge),
     .identifier = "LONGEST_DIAGONAL",
     .icon = 0,
     .name = N_("Longest Diagonal"),
     .description = N_("Split the quads along their longest diagonal")},
    {.value = 0, .identifier = nullptr, .icon = 0, .name = nullptr, .description = nullptr},
};

static const EnumPropertyItem rna_node_geometry_triangulate_ngon_method_items[] = {
    {.value = int(geometry::TriangulateNGonMode::Beauty),
     .identifier = "BEAUTY",
     .icon = 0,
     .name = N_("Beauty"),
     .description = N_("Arrange the new triangles evenly (slow)")},
    {.value = int(geometry::TriangulateNGonMode::EarClip),
     .identifier = "CLIP",
     .icon = 0,
     .name = N_("Clip"),
     .description = N_("Split the polygons with an ear clipping algorithm")},
    {.value = 0, .identifier = nullptr, .icon = 0, .name = nullptr, .description = nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_input<decl::Geometry>("Mesh")
      .supported_type(GeometryComponent::Type::Mesh)
      .is_default_link_socket()
      .description("Mesh to triangulate");
  b.add_output<decl::Geometry>("Mesh").propagate_all().align_with_previous();
  b.add_input<decl::Bool>("Selection").default_value(true).field_on_all().hide_value();
  b.add_input<decl::Menu>("Quad Method")
      .static_items(rna_node_geometry_triangulate_quad_method_items)
      .default_value(geometry::TriangulateQuadMode::ShortEdge)
      .optional_label()
      .description("Method for splitting the quads into triangles");
  b.add_input<decl::Menu>("N-gon Method")
      .default_value(geometry::TriangulateNGonMode::Beauty)
      .static_items(rna_node_geometry_triangulate_ngon_method_items)
      .optional_label()
      .description("Method for splitting the n-gons into triangles");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh");
  Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");
  const AttributeFilter &attribute_filter = params.get_attribute_filter("Mesh");

  const auto ngon_method = params.extract_input<geometry::TriangulateNGonMode>("N-gon Method");
  const auto quad_method = params.extract_input<geometry::TriangulateQuadMode>("Quad Method");

  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry_set) {
    const Mesh *src_mesh = geometry_set.get_mesh();
    if (!src_mesh) {
      return;
    }
    if (src_mesh->corners_num == src_mesh->faces_num * 3) {
      /* The mesh is already completely triangulated. */
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

    std::optional<Mesh *> mesh = geometry::mesh_triangulate(
        *src_mesh,
        selection,
        geometry::TriangulateNGonMode(ngon_method),
        geometry::TriangulateQuadMode(quad_method),
        attribute_filter);
    if (!mesh) {
      return;
    }

    /* Vertex order is not affected. */
    geometry::debug_randomize_edge_order(*mesh);
    geometry::debug_randomize_face_order(*mesh);

    geometry_set.replace_mesh(*mesh);
  });

  params.set_output("Mesh", std::move(geometry_set));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeTriangulate", GEO_NODE_TRIANGULATE);
  ntype.ui_name = "Triangulate";
  ntype.ui_description = "Convert all faces in a mesh to triangular faces";
  ntype.enum_name_legacy = "TRIANGULATE";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_triangulate_cc
