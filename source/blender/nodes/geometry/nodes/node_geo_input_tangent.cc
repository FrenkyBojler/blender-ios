/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_task.hh"
#include "BLI_math_geom.h"

#include "BKE_curves.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_tangent.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_tangent_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Vector>("Tangent").field_source();
}

static Array<float3> curve_tangent_point_domain(const bke::CurvesGeometry &curves)
{
  const OffsetIndices points_by_curve = curves.points_by_curve();
  const OffsetIndices evaluated_points_by_curve = curves.evaluated_points_by_curve();
  const VArray<int8_t> types = curves.curve_types();
  const VArray<int> resolutions = curves.resolution();
  const VArray<bool> cyclic = curves.cyclic();
  const Span<float3> positions = curves.positions();

  const Span<float3> evaluated_tangents = curves.evaluated_tangents();

  Array<float3> results(curves.points_num());

  threading::parallel_for(curves.curves_range(), 128, [&](IndexRange range) {
    for (const int i_curve : range) {
      const IndexRange points = points_by_curve[i_curve];
      const IndexRange evaluated_points = evaluated_points_by_curve[i_curve];

      MutableSpan<float3> curve_tangents = results.as_mutable_span().slice(points);

      switch (types[i_curve]) {
        case CURVE_TYPE_CATMULL_ROM: {
          Span<float3> tangents = evaluated_tangents.slice(evaluated_points);
          const int resolution = resolutions[i_curve];
          for (const int i : IndexRange(points.size())) {
            curve_tangents[i] = tangents[resolution * i];
          }
          break;
        }
        case CURVE_TYPE_POLY:
          curve_tangents.copy_from(evaluated_tangents.slice(evaluated_points));
          break;
        case CURVE_TYPE_BEZIER: {
          Span<float3> tangents = evaluated_tangents.slice(evaluated_points);
          curve_tangents.first() = tangents.first();
          const Span<int> offsets = curves.bezier_evaluated_offsets_for_curve(i_curve);
          for (const int i : IndexRange(points.size()).drop_front(1)) {
            curve_tangents[i] = tangents[offsets[i]];
          }
          break;
        }
        case CURVE_TYPE_NURBS: {
          const Span<float3> curve_positions = positions.slice(points);
          bke::curves::poly::calculate_tangents(curve_positions, cyclic[i_curve], curve_tangents);
          break;
        }
      }
    }
  });
  return results;
}

static VArray<float3> construct_curve_tangent_gvarray(const bke::CurvesGeometry &curves,
                                                      const AttrDomain domain)
{
  const VArray<int8_t> types = curves.curve_types();
  if (curves.is_single_type(CURVE_TYPE_POLY)) {
    return curves.adapt_domain<float3>(
        VArray<float3>::ForSpan(curves.evaluated_tangents()), AttrDomain::Point, domain);
  }

  Array<float3> tangents = curve_tangent_point_domain(curves);

  if (domain == AttrDomain::Point) {
    return VArray<float3>::ForContainer(std::move(tangents));
  }

  if (domain == AttrDomain::Curve) {
    return curves.adapt_domain<float3>(
        VArray<float3>::ForContainer(std::move(tangents)), AttrDomain::Point, AttrDomain::Curve);
  }

  return nullptr;
}


static Array<float4> mesh_tangent_corner_domain(const Mesh &mesh) {

  const int tottri = poly_to_tri_count(mesh.faces_num, mesh.corners_num);

  /* calculate normal for each face only once */
  uint mpoly_prev = UINT_MAX;
  blender::float3 no;

  const blender::Span<blender::float3> positions = mesh.vert_positions();
  const blender::OffsetIndices faces = mesh.faces();
  const blender::Span<int> corner_verts = mesh.corner_verts();
  const bke::AttributeAccessor attributes = mesh.attributes();

  Array<int3> corner_tris(tottri);

  blender::bke::mesh::corner_tris_calc(positions, faces, corner_verts, corner_tris.as_mutable_span());
}

static VArray<float3> construct_mesh_tangent_gvarray(const Mesh &mesh,
                                                      const AttrDomain domain)
{
  Array<float4> tangents = mesh_tangent_corner_domain(mesh);

  if (domain == AttrDomain::Point) {
    return VArray<float3>::ForContainer(std::move(tangents));
  }

  if (domain == AttrDomain::Curve) {
    return curves.adapt_domain<float3>(
        VArray<float3>::ForContainer(std::move(tangents)), AttrDomain::Point, AttrDomain::Curve);
  }

  return nullptr;
}

class TangentFieldInput final : public bke::GeometryFieldInput {
 public:
  TangentFieldInput() : bke::GeometryFieldInput(CPPType::get<float3>(), "Tangent node")
  {
    category_ = Category::Generated;
  }

  GVArray get_varray_for_context(const bke::GeometryFieldContext &context,
                                 const IndexMask &mask) const final
  {
    if (const Mesh *mesh = context.mesh()) {
      return construct_mesh_tangent_gvarray(*mesh, context.domain());
    }
    if (const bke::CurvesGeometry *curves = context.curves_or_strokes()) {
      return construct_curve_tangent_gvarray(*curves, context.domain());
    }
    return {};
  }

  uint64_t hash() const override
  {
    /* Some random constant hash. */
    return 91827364589;
  }

  bool is_equal_to(const fn::FieldNode &other) const override
  {
    return dynamic_cast<const TangentFieldInput *>(&other) != nullptr;
  }

  std::optional<AttrDomain> preferred_domain(const GeometryComponent & /*component*/) const final
  {
    return AttrDomain::Point;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  Field<float3> tangent_field{std::make_shared<TangentFieldInput>()};
  params.set_output("Tangent", std::move(tangent_field));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeInputTangent", GEO_NODE_INPUT_TANGENT);
  ntype.ui_name = "Curve Tangent";
  ntype.ui_description = "Retrieve the direction of curves at each control point";
  ntype.enum_name_legacy = "INPUT_TANGENT";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_tangent_cc
