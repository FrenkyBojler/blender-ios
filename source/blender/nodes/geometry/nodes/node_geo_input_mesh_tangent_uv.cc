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
#include "BKE_mesh_mapping.hh"
#include "BKE_mesh_tangent.hh"
#include "BKE_type_conversions.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_mesh_tangent_uv_cc {

/* Socket names. */
const char INPUT_UV_COORDS[14] = "Source UV Map";
const char OUTPUT_TANGENT[8] = "Tangent";
const char OUTPUT_BITANGENT[10] = "Bitangent";

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Vector>(INPUT_UV_COORDS)
      .hide_value()
      .supports_field()
      .description("Source UV map coordinates defined on the corner domain");
  b.add_output<decl::Vector>(OUTPUT_TANGENT)
      .field_source()
      .description("Tangent basis vectors defined on the corner domain");
  b.add_output<decl::Vector>(OUTPUT_BITANGENT)
      .field_source()
      .description("Bitangent basis vectors defined on the corner domain");
}

/**
 * Compute the tangents on the corner domain for the given face selection.
 */
static void compute_mesh_corner_tangents_partial_face_domain(const Mesh &mesh,
                                                             const IndexMask &face_selection,
                                                             const Span<float2> uv_coords,
                                                             Array<float3> &r_tangents,
                                                             Array<float> &r_bitangents)
{
  const OffsetIndices faces = mesh.faces();
  const Span<int3> corner_tris = mesh.corner_tris();
  const Span<int> corner_verts = mesh.corner_verts();

  /* Offsets mapping faces to tesselated tris for the partial tesselation (ptess). */
  Array<int> ptess_face2tri_map_data(face_selection.size() + 1);
  OffsetIndices<int> ptess_face2tri_map(ptess_face2tri_map_data);
  ptess_face2tri_map_data[0] = 0;

  face_selection.foreach_index([&](const int64_t index_face, const int64_t index_rel) {
    ptess_face2tri_map_data[index_rel + 1] = ptess_face2tri_map_data[index_rel] +
                                             poly_to_tri_count(1, faces[index_face].size());
  });

  /* Partial buffers */
  Array<int> ptess_corner_verts(ptess_face2tri_map.total_size() * 3);
  Array<int> ptess_corner_corners(ptess_corner_verts.size());
  face_selection.foreach_index(
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

  const int64_t min_buffer_size = faces[face_selection.last()].one_after_last();
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

static void compute_mesh_corner_tangents(const Mesh &mesh,
                                         const IndexMask &corner_mask,
                                         const Span<float2> uv_coords,
                                         Array<float3> &r_tangents,
                                         Array<float> &r_bitangents)
{
  /* Convert corner -> face domain */
  const Span<int> corner_to_face_map = mesh.corner_to_face_map();

  Array<bool> affected_faces(mesh.faces_num, false);
  corner_mask.foreach_index(GrainSize(32768),
                            [&](const int64_t index_corner, const int64_t /* index_rel */) {
                              affected_faces[corner_to_face_map[index_corner]] = true;
                            });

  IndexMaskMemory mask_mem;
  IndexMask face_selection = IndexMask::from_bools(affected_faces, mask_mem);

  compute_mesh_corner_tangents_partial_face_domain(
      mesh, face_selection, uv_coords, r_tangents, r_bitangents);
}

static VArray<float3> construct_mesh_tangent_gvarray(const Mesh &mesh,
                                                     const IndexMask &corner_mask,
                                                     const Span<float2> uv_coords,
                                                     const bool output_bitangent)
{
  Array<float3> tangents;
  Array<float> bitangents;
  compute_mesh_corner_tangents(mesh, corner_mask, uv_coords, tangents, bitangents);

  if (output_bitangent) {
    /* Reuse tangent buffer. */
    const Span<float3> normals = mesh.corner_normals();

    corner_mask.foreach_index(
        GrainSize(32768), [&](const int64_t index_corner, const int64_t /* index_rel */) {
          float3 tan = tangents[index_corner];
          cross_v3_v3v3(tangents[index_corner], normals[index_corner], &tan.x);
          tangents[index_corner] *= bitangents[index_corner];
        });
  }
  return VArray<float3>::ForContainer(std::move(tangents));
}

class MeshUVTangentFieldInput final : public bke::MeshFieldInput {
 private:
  Field<float2> source_uv_coords_;
  bool output_bitangent_;

 public:
  MeshUVTangentFieldInput(Field<float2> source_uv_field, bool output_bitangent)
      : bke::MeshFieldInput(CPPType::get<float3>(), "UVMapTangent tangent field"),
        source_uv_coords_(source_uv_field),
        output_bitangent_(output_bitangent)
  {
    category_ = Category::Generated;
  }

  virtual GVArray get_varray_for_context(const Mesh &mesh,
                                         AttrDomain domain,
                                         const IndexMask &mask) const
  {
    const bke::MeshFieldContext corner_context{mesh, AttrDomain::Corner};
    fn::FieldEvaluator corner_evaluator{corner_context, mesh.corners_num};
    corner_evaluator.add(source_uv_coords_);
    corner_evaluator.evaluate();
    const VArraySpan<float2> uv_coords = corner_evaluator.get_evaluated<float2>(0);

    if (uv_coords.is_empty()) {
      return mesh.attributes().adapt_domain<float3>(
          VArray<float3>::ForSingle(float3(0.0f, 0.0f, 0.0f), mesh.corners_num),
          AttrDomain::Corner,
          domain);
    }

    /* Active UV layer fetch
  const CustomData *corner_data = &mesh.corner_data;
    int layer_index = CustomData_get_layer_index(corner_data, CD_PROP_FLOAT2);
    int active_layer = CustomData_get_active_layer(corner_data, CD_PROP_FLOAT2);
    const float2 *active_uv_data = static_cast<const float2 *>(
        corner_data->layers[active_layer + layer_index].data);
    Span<float2> uv_coords(active_uv_data, mesh.corners_num);
    */

    if (domain == AttrDomain::Corner) {
      return construct_mesh_tangent_gvarray(mesh, mask, uv_coords, output_bitangent_);
    }
    return nullptr;
  }

  uint64_t hash() const override
  {
    return get_default_hash(source_uv_coords_);
  }

  std::optional<AttrDomain> preferred_domain(const Mesh & /*mesh*/) const final
  {
    return AttrDomain::Corner;
  }

  bool is_equal_to(const fn::FieldNode &other) const override
  {
    if (const MeshUVTangentFieldInput *tother = dynamic_cast<const MeshUVTangentFieldInput *>(
            &other))
    {
      return tother->output_bitangent_ == this->output_bitangent_ && tother->source_uv_coords_ == this->source_uv_coords_;
    }
    return false;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  const bke::DataTypeConversions &conversions = bke::get_implicit_type_conversions();
  const CPPType &float2_type = CPPType::get<float2>();
  Field<float2> source_uv_map = conversions.try_convert(
      params.extract_input<Field<float3>>(INPUT_UV_COORDS), float2_type);

  Field<float3> tangent_field{std::make_shared<MeshUVTangentFieldInput>(source_uv_map, false)};
  params.set_output(OUTPUT_TANGENT, std::move(tangent_field));

  Field<float3> bitangent_field{std::make_shared<MeshUVTangentFieldInput>(source_uv_map, true)};
  params.set_output(OUTPUT_BITANGENT, std::move(bitangent_field));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeInputMeshTextureCoordinate", GEO_NODE_INPUT_MESH_TANGENT_UV);
  ntype.ui_name = "Texture Coordinate";
  ntype.ui_description =
      "UV coordinates for the current active UV map layer";
  ntype.enum_name_legacy = "INPUT_MESH_TEXTURE_COORDINATE";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_mesh_tangent_uv_cc
