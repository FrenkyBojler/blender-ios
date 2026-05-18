/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_curves_types.h"
#include "DNA_mesh_types.h"

#include "BKE_curves.hh"
#include "BKE_geometry_set.hh"

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
    {0, nullptr, 0, nullptr, nullptr}};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Mesh"_ustr).supported_type(GeometryComponent::Type::Mesh);
  b.add_input<decl::Menu>("Affect Kind"_ustr)
      .default_value(geometry::BevelAffect::Edges)
      .static_items(affect_items)
      .optional_label();
  b.add_input<decl::Bool>("Selection"_ustr)
      .default_value(true)
      .field_on_all()
      .description("Selects elements of 'Affect Kind' for beveling");
  /* TODO: when there is good support for 4d vectors, use those here. */
  b.add_input<decl::Float>("Offset 0"_ustr)
      .default_value(0.1f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .field_on_all()
      .description("Offset for left side of source end of edge");
  b.add_input<decl::Float>("Offset 1"_ustr)
      .default_value(0.1f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .field_on_all()
      .description("Offset for right side of source end of edge");
  b.add_input<decl::Float>("Offset 2"_ustr)
      .default_value(0.1f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .field_on_all()
      .description("Offset for left side of dest end of edge");
  b.add_input<decl::Float>("Offset 3"_ustr)
      .default_value(0.1f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .field_on_all()
      .description("Offset for right side of dest end of edge");
  b.add_input<decl::Bool>("Miter"_ustr)
      .default_value(false)
      .field_on_all()
      .description("Use a miter for corner");
  b.add_input<decl::Float>("Spread"_ustr)
      .default_value(0.0f)
      .subtype(PROP_DISTANCE)
      .field_on_all()
      .description("Per corner specification of 'spread' for arc miters")
      .usage_by_bool("Miter"_ustr, true);
  b.add_input<decl::Int>("Segments"_ustr)
      .default_value(1)
      .description(
          "How many pieces is an edge beveled into, "
          "or, for vertex bevels, the how many segments on the arcs between the edges.");
  b.add_input<decl::Float>("Shape"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
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

/* -------------------------------------------------------------------- */
/** \name Profile Curve Sampling
 *
 * Samples the first spline of a #bke::CurvesGeometry into a flat array of (x, y) pairs
 * for use as `BevelParameters::custom_profile_samples`.
 *
 * The input curve is expected to start near (0, 1, z) and end near (1, 0, z) in its local
 * XY plane (Z is ignored).  The evaluated positions are used directly, so Bezier, poly,
 * Catmull-Rom, and NURBS curves all work without any special-casing.
 * \{ */

/**
 * Returns the evaluated (x, y) positions of the first spline of `curves`.
 * Returns an empty array when the geometry has no splines or fewer than 2 evaluated points.
 */
static Array<float2> sample_profile_curve(const bke::CurvesGeometry &curves)
{
  if (curves.curves_num() == 0) {
    return {};
  }

  /* Use the evaluator to get dense positions that correctly represent the curve shape
   * regardless of type (Bezier, Catmull-Rom, poly, NURBS). */
  const OffsetIndices<int> eval_by_curve = curves.evaluated_points_by_curve();
  const Span<float3> eval_positions = curves.evaluated_positions();
  const IndexRange eval_pts = eval_by_curve[0];

  if (eval_pts.size() < 2) {
    return {};
  }

  Array<float2> samples(eval_pts.size());
  for (const int i : IndexRange(eval_pts.size())) {
    const float3 &p = eval_positions[eval_pts[i]];
    samples[i] = float2(p.x, p.y);
  }
  return samples;
}

/** \} */

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh"_ustr);
  Field<bool> selection_field = params.extract_input<Field<bool>>("Selection"_ustr);
  const AttributeFilter &attribute_filter = params.get_attribute_filter("Mesh"_ustr);
  int segments = params.extract_input<int>("Segments"_ustr);
  geometry::BevelAffect affect = params.extract_input<blender::geometry::BevelAffect>(
      "Affect Kind"_ustr);

  Field<float> offset0_field = params.extract_input<Field<float>>("Offset 0"_ustr);
  Field<float> offset1_field = params.extract_input<Field<float>>("Offset 1"_ustr);
  Field<float> offset2_field = params.extract_input<Field<float>>("Offset 2"_ustr);
  Field<float> offset3_field = params.extract_input<Field<float>>("Offset 3"_ustr);

  Field<bool> miter_field = params.extract_input<Field<bool>>("Miter"_ustr);
  Field<float> spread_field = params.extract_input<Field<float>>("Spread"_ustr);

  /* Sample the Profile input curve into a flat float2 array.
   * The array is built once here and shared across all instances in the geometry loop;
   * BevelParameters holds it by const reference (non-owning Span). */
  GeometrySet profile_set = params.extract_input<GeometrySet>("Profile"_ustr);
  Array<float2> profile_samples;
  if (const Curves *profile_curves_id = profile_set.get_curves()) {
    const bke::CurvesGeometry &profile_geom = profile_curves_id->geometry.wrap();
    profile_samples = sample_profile_curve(profile_geom);
  }

  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry_set) {
    const Mesh *src_mesh = geometry_set.get_mesh();
    if (!src_mesh) {
      return;
    }
    geometry::BevelParameters bevel_params;
    bevel_params.affect_type = affect;
    bevel_params.segments = segments;
    bevel_params.shape = params.extract_input<float>("Shape"_ustr);
    /* Move the samples into bevel_params (zero-copy; profile_samples stays valid
     * through the geometry loop because it is declared in this outer scope). */
    bevel_params.custom_profile_samples = profile_samples;
    const int ne = src_mesh->edges_num;

    /* Shared logic executed after the selection/offset evaluator (and its lifetime) is set up.
     * The `selection` reference must not outlive `evaluator`. */
    auto run_bevel = [&](const IndexMask &selection) {
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
          params.get_output_anonymous_attribute_id_if_needed("Mid Edge"_ustr);

      std::optional<Mesh *> mesh = geometry::mesh_bevel(
          *src_mesh, selection, bevel_params, attribute_filter);
      if (!mesh) {
        return;
      }
      geometry_set.replace_mesh(*mesh);
    };

    if (affect == geometry::BevelAffect::Vertices) {
      /* Vertex bevel: selection and offset0 are per-vertex.
       * offsets[0][v] is the slide distance at vertex v; offsets[1..3] are unused. */
      const int nv = src_mesh->verts_num;
      bevel_params.offsets = {Array<float>(nv), Array<float>(0), Array<float>(0), Array<float>(0)};

      const bke::MeshFieldContext vert_context(*src_mesh, AttrDomain::Point);
      FieldEvaluator vert_evaluator{vert_context, nv};
      vert_evaluator.add(selection_field);
      vert_evaluator.add_with_destination(offset0_field,
                                          bevel_params.offsets[0].as_mutable_span());
      vert_evaluator.evaluate();

      /* Pass to run_bevel while vert_evaluator is still alive. */
      run_bevel(vert_evaluator.get_evaluated_as_mask(0));
    }
    else {
      /* Edge bevel: selection and all four offsets are per-edge. */
      bevel_params.offsets = {
          Array<float>(ne), Array<float>(ne), Array<float>(ne), Array<float>(ne)};

      const bke::MeshFieldContext edge_context(*src_mesh, AttrDomain::Edge);
      FieldEvaluator edge_evaluator{edge_context, ne};
      edge_evaluator.add(selection_field);
      edge_evaluator.add_with_destination(offset0_field,
                                          bevel_params.offsets[0].as_mutable_span());
      edge_evaluator.add_with_destination(offset1_field,
                                          bevel_params.offsets[1].as_mutable_span());
      edge_evaluator.add_with_destination(offset2_field,
                                          bevel_params.offsets[2].as_mutable_span());
      edge_evaluator.add_with_destination(offset3_field,
                                          bevel_params.offsets[3].as_mutable_span());
      edge_evaluator.evaluate();

      /* Pass to run_bevel while edge_evaluator is still alive. */
      run_bevel(edge_evaluator.get_evaluated_as_mask(0));
    }
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
