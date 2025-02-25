/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"
#include "BLI_math_geom.h"
#include "BLI_task.hh"
#include "BLI_utildefines.h"

#include "BKE_curves.hh"
#include "BKE_customdata.hh"
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

static void compute_tangent_partial_corner_domain(const Mesh &mesh,
                                                  const IndexMask &faces_mask,
                                                  const Span<float2> uv_coords,
                                                  Array<float3>& r_tangents,
                                                  Array<float>& r_bitangents)
{
  const OffsetIndices faces = mesh.faces();
  const Span<int3> corner_tris = mesh.corner_tris();
  const Span<int> corner_verts = mesh.corner_verts();

  /* Offsets mapping faces to tesselated tris for the partial tesselation (ptess). */
  Array<int> ptess_face2tri_map_data(faces_mask.size() + 1);
  OffsetIndices<int> ptess_face2tri_map(ptess_face2tri_map_data);
  ptess_face2tri_map_data[0] = 0;

  faces_mask.foreach_index([&](const int64_t index_face, const int64_t index_rel) {
    ptess_face2tri_map_data[index_rel + 1] = ptess_face2tri_map_data[index_rel] +
                                             poly_to_tri_count(1, faces[index_face].size());
  });

  /* Partial buffers */
  Array<int> ptess_corner_verts(ptess_face2tri_map.total_size() * 3);
  Array<int> ptess_corner_corners(ptess_corner_verts.size());
  faces_mask.foreach_index(
      GrainSize(32768), [&](const int64_t index_face, const int64_t index_rel) {
        const IndexRange corner_tri_slice(poly_to_tri_count(index_face, faces[index_face].start()),
                                          poly_to_tri_count(1, faces[index_face].size()));

        const int64_t ptess_offset = ptess_face2tri_map[index_rel].start();
        int *ptess_corner_vert = &ptess_corner_verts[ptess_offset * 3];
        int *ptess_corner_corner = &ptess_corner_corners[ptess_offset * 3];

        for (const int64_t i : corner_tri_slice.index_range()) {
          const int3 corner_tri = corner_tris[corner_tri_slice[i]];
          *ptess_corner_vert++ = corner_verts[corner_tri.x];
          *ptess_corner_vert++ = corner_verts[corner_tri.y];
          *ptess_corner_vert++ = corner_verts[corner_tri.z];

          *ptess_corner_corner++ = corner_tri.x;
          *ptess_corner_corner++ = corner_tri.y;
          *ptess_corner_corner++ = corner_tri.z;
        }
      });

  const int64_t min_buffer_size = faces[faces_mask.last()].one_after_last();
  r_tangents = Array<float3>(min_buffer_size);
  r_bitangents = Array<float>(min_buffer_size);

  BKE_mesh_calc_virtual_loop_tangent_single_ex(ptess_face2tri_map.total_size(),
                                               ptess_corner_verts,
                                               ptess_corner_corners,
                                               mesh.vert_positions(),
                                               mesh.corner_normals(),
                                               uv_coords,
                                               r_tangents.as_mutable_span(),
                                               r_bitangents.as_mutable_span());
}

static void mesh_tangent_corner_domain(const Mesh &mesh,
                                       const IndexMask &mask,
                                       const Span<float2> uv_coords,
                                       Array<float3>& r_tangents,
                                       Array<float>& r_bitangents)
{
  const Span<int> corner_to_face_map = mesh.corner_to_face_map();

  Array<bool> affected_faces(mesh.faces_num, false);
  mask.foreach_index(GrainSize(32768),
                     [&](const int64_t index_corner, const int64_t index_rel) {
                       affected_faces[corner_to_face_map[index_corner]] = true;
                     });

  IndexMaskMemory mask_mem;
  IndexMask faces_mask = IndexMask::from_bools(affected_faces, mask_mem);

  compute_tangent_partial_corner_domain(mesh, faces_mask, uv_coords, r_tangents, r_bitangents);
}

static VArray<float3> construct_mesh_tangent_gvarray(const Mesh &mesh,
                                                     const AttrDomain domain,
                                                     const IndexMask &mask)
{
  const CustomData *corner_data = &mesh.corner_data;
  int layer_index = CustomData_get_layer_index(corner_data, CD_PROP_FLOAT2);
  int active_layer = CustomData_get_active_layer(corner_data, CD_PROP_FLOAT2);
  const float2 *active_uv_data = static_cast<const float2 *>(
      corner_data->layers[active_layer + layer_index].data);
  Span<float2> uv_coords(active_uv_data, mesh.corners_num);

  Array<float3> tangents;
  Array<float> bitangents;

  if (domain == AttrDomain::Corner) {
    mesh_tangent_corner_domain(mesh, mask, uv_coords, tangents, bitangents);
    return VArray<float3>::ForContainer(std::move(tangents));
  }

  if (domain == AttrDomain::Point) {
    mesh.attributes().adapt_domain(
        VArray<float3>::ForContainer(std::move(tangents)), bke::AttrDomain::Corner, domain);
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
      return construct_mesh_tangent_gvarray(*mesh, context.domain(), mask);
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

  geo_node_type_base(&ntype, "GeometryNodeInputMeshTangentUV", GEO_NODE_INPUT_TANGENT);
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
