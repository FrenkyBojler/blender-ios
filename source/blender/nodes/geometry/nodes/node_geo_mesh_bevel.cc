/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_mesh_types.h"

#include "NOD_rna_define.hh"

#include "GEO_foreach_geometry.hh"
#include "GEO_mesh_bevel.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_bevel_cc {

static const EnumPropertyItem affect_items[] = {
    {int(geometry::BevelAffect::Vertices), "VERTICES", 0, "Vertices", "Bevel affects vertices"},
    {int(geometry::BevelAffect::Edges), "EDGES", 0, "Edges", "Bevel affects edges"},
    {int(geometry::BevelAffect::Faces), "FACES", 0, "Faces", "Bevel affects faces"},
    {0, nullptr, 0, nullptr, nullptr}};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Mesh"_ustr).supported_type(GeometryComponent::Type::Mesh);
  b.add_input<decl::Menu>("Affect Kind"_ustr)
      .default_value(geometry::BevelAffect::Edges)
      .static_items(affect_items);
  b.add_input<decl::Bool>("Selection"_ustr)
      .default_value(true)
      .field_on_all()
      .description("Selects elements of 'Affect Kind' for beveling");
  /* TODO: when there is good support for 4d vectors, use those here. */
  b.add_input<decl::Float>("Offset0"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .field_on_all()
      .description("Offset for left side of source end of edge");
  b.add_input<decl::Float>("Offset1"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .field_on_all()
      .description("Offset for right side of source end of edge");
  b.add_input<decl::Float>("Offset2"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .field_on_all()
      .description("Offset for left side of dest end of edge");
  b.add_input<decl::Float>("Offset3"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .field_on_all()
      .description("Offset for right side of dest end of edge");
  b.add_input<decl::Bool>("Miter"_ustr)
      .default_value(false)
      .field_on_all()
      .description("Use a miter for corner");
  b.add_input<decl::Float>("Spread"_ustr)
      .default_value(0.0f)
      .field_on_all()
      .description("Per corner specification of 'spread' for arc miters");
  b.add_input<decl::Int>("Segments"_ustr)
      .default_value(1)
      .description(
          "How many pieces is an edge beveled into, "
          "or, for vertex bevels, the how many segments on the arcs between the edges.");
  b.add_input<decl::Float>("Shape"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .description(
          "Superellipse shape parameter, used when there is no Profile, "
          " and also used for Arc and Patch miters");
  b.add_input<decl::Geometry>("Profile"_ustr)
      .supported_type(GeometryComponent::Type::Curve)
      .description("If present, will be sampled to give custom profile on edges");
  b.add_output<decl::Geometry>("Mesh"_ustr).propagate_all();
  b.add_output<decl::Bool>("Vertex Face"_ustr)
      .field_on_all()
      .description("Identifies output faces that are in the new mesh parts for vertices");
  b.add_output<decl::Bool>("Edge Face"_ustr)
      .field_on_all()
      .description("Identifies output faces that are in the new mesh parts for edges");
  b.add_output<decl::Bool>("Outer Edge"_ustr)
      .field_on_all()
      .description("Identifies output edges that are on the outsides of new mesh parts for edges");
  b.add_output<decl::Bool>("Mid Edge"_ustr)
      .field_on_all()
      .description(
          "Identifies output edges that are in the middle of new mesh parts of edges "
          " and continued through vertices (round down if odd number of segments)");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh"_ustr);
  Field<bool> selection_field = params.extract_input<Field<bool>>("Selection"_ustr);
  const AttributeFilter &attribute_filter = params.get_attribute_filter("Mesh"_ustr);
  int segments = params.extract_input<int>("Segments"_ustr);
  geometry::BevelAffect affect = params.extract_input<blender::geometry::BevelAffect>(
      "Affect Kind"_ustr);

  Field<float> offset0_field = params.extract_input<Field<float>>("Offset0"_ustr);
  Field<float> offset1_field = params.extract_input<Field<float>>("Offset1"_ustr);
  Field<float> offset2_field = params.extract_input<Field<float>>("Offset2"_ustr);
  Field<float> offset3_field = params.extract_input<Field<float>>("Offset3"_ustr);

  Field<bool> miter_field = params.extract_input<Field<bool>>("Miter"_ustr);
  Field<float> spread_field = params.extract_input<Field<float>>("Spread"_ustr);

  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry_set) {
    const Mesh *src_mesh = geometry_set.get_mesh();
    if (!src_mesh) {
      return;
    }
    geometry::BevelParameters bevel_params;
    bevel_params.affect_type = affect;
    bevel_params.segments = segments;
    const int ne = src_mesh->edges_num;
    bevel_params.offsets = {
        Array<float>(ne), Array<float>(ne), Array<float>(ne), Array<float>(ne)};

    const bke::MeshFieldContext edge_context(*src_mesh, AttrDomain::Edge);
    FieldEvaluator edge_evaluator{edge_context, src_mesh->edges_num};
    edge_evaluator.add(selection_field);
    edge_evaluator.add_with_destination(offset0_field, bevel_params.offsets[0].as_mutable_span());
    edge_evaluator.add_with_destination(offset1_field, bevel_params.offsets[1].as_mutable_span());
    edge_evaluator.add_with_destination(offset2_field, bevel_params.offsets[2].as_mutable_span());
    edge_evaluator.add_with_destination(offset3_field, bevel_params.offsets[3].as_mutable_span());
    edge_evaluator.evaluate();
    const IndexMask selection = edge_evaluator.get_evaluated_as_mask(0);
    if (selection.is_empty()) {
      return;
    }

    const bke::MeshFieldContext corner_context(*src_mesh, AttrDomain::Corner);
    FieldEvaluator corner_evaluator{corner_context, src_mesh->corners_num};
    /* TODO: make this more efficient in usual case of no miters. */
    bevel_params.miter = Array<bool>(src_mesh->corners_num);
    bevel_params.spread = Array<float>(src_mesh->corners_num);
    corner_evaluator.add_with_destination(miter_field, bevel_params.miter.as_mutable_span());
    corner_evaluator.add_with_destination(spread_field, bevel_params.spread.as_mutable_span());
    corner_evaluator.evaluate();

    bevel_params.attribute_outputs.vertex_face_id =
        params.get_output_anonymous_attribute_id_if_needed("Vertex Face"_ustr);
    bevel_params.attribute_outputs.edge_face_id =
        params.get_output_anonymous_attribute_id_if_needed("Edge Face"_ustr);
    bevel_params.attribute_outputs.outer_edge_id =
        params.get_output_anonymous_attribute_id_if_needed("Outer Edge"_ustr);
    bevel_params.attribute_outputs.mid_edge_id =
        params.get_output_anonymous_attribute_id_if_needed("Vertex Face"_ustr);

    std::optional<Mesh *> mesh = geometry::mesh_bevel(
        *src_mesh, selection, bevel_params, attribute_filter);
    if (!mesh) {
      return;
    }

    geometry_set.replace_mesh(*mesh);
  });

  params.set_output("Mesh"_ustr, std::move(geometry_set));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeMeshBevel"_ustr, std::nullopt);
  ntype.ui_name = "Mesh Bevel";
  ntype.ui_description = "Bevel selected edges or vertices";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_bevel_cc
