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
const char OUTPUT_TANGENT[14] = "Tangents";

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Vector>(INPUT_UV_COORDS)
      .hide_value()
      .supports_field()
      .description("Source UV map coordinates defined on the corner domain");
  b.add_output<decl::Vector>(OUTPUT_TANGENT).field_source();
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

static void mesh_tangent_corner_domain(const Mesh &mesh,
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

static void mesh_tangent_point_domain(const Mesh &mesh,
                                      const IndexMask &vert_mask,
                                      const Span<float2> uv_coords,
                                      Array<float3> &r_tangents,
                                      Array<float> &r_bitangents)
{
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();

  Array<bool> affected_faces(mesh.faces_num, false);
  vert_mask.foreach_index(GrainSize(32768),
                          [&](const int64_t index_corner, const int64_t /* index_rel */) {
                            for (const int64_t i : vert_to_face_map.offsets[index_corner]) {
                              affected_faces[vert_to_face_map.data[i]] = true;
                            }
                          });

  IndexMaskMemory mask_mem;
  IndexMask faces_mask = IndexMask::from_bools(affected_faces, mask_mem);

  compute_mesh_corner_tangents_partial_face_domain(
      mesh, faces_mask, uv_coords, r_tangents, r_bitangents);
}

static void mesh_tangent_edge_domain(const Mesh &mesh,
                                     const IndexMask &edge_mask,
                                     const Span<float2> uv_coords,
                                     Array<float3> &r_tangents,
                                     Array<float> &r_bitangents)
{
  Array<int> edge_to_face_offsets;
  Array<int> edge_to_face_indices;
  const GroupedSpan<int> edge_to_corner_map = bke::mesh::build_edge_to_face_map(
      mesh.faces(),
      mesh.corner_edges(),
      mesh.edges_num,
      edge_to_face_offsets,
      edge_to_face_indices);

  Array<bool> affected_faces(mesh.faces_num, false);
  edge_mask.foreach_index(GrainSize(32768),
                          [&](const int64_t index_corner, const int64_t /* index_rel */) {
                            for (const int64_t i : edge_to_corner_map.offsets[index_corner]) {
                              affected_faces[edge_to_corner_map.data[i]] = true;
                            }
                          });

  IndexMaskMemory mask_mem;
  IndexMask faces_mask = IndexMask::from_bools(affected_faces, mask_mem);

  compute_mesh_corner_tangents_partial_face_domain(
      mesh, faces_mask, uv_coords, r_tangents, r_bitangents);
}

static VArray<float3> construct_mesh_tangent_gvarray(const Mesh &mesh,
                                                     const AttrDomain domain,
                                                     const IndexMask &mask,
                                                     const Span<float2> uv_coords)
{
  Array<float3> tangents;
  Array<float> bitangents;

  if (domain == AttrDomain::Corner) {
    mesh_tangent_corner_domain(mesh, mask, uv_coords, tangents, bitangents);
    return VArray<float3>::ForContainer(std::move(tangents));
  }

  if (domain == AttrDomain::Point) {
    mesh_tangent_point_domain(mesh, mask, uv_coords, tangents, bitangents);
    return mesh.attributes().adapt_domain(
        VArray<float3>::ForContainer(std::move(tangents)), bke::AttrDomain::Corner, domain);
  }

  if (domain == AttrDomain::Face) {
    compute_mesh_corner_tangents_partial_face_domain(mesh, mask, uv_coords, tangents, bitangents);
    return mesh.attributes().adapt_domain(
        VArray<float3>::ForContainer(std::move(tangents)), bke::AttrDomain::Corner, domain);
  }

  if (domain == AttrDomain::Edge) {
    mesh_tangent_edge_domain(mesh, mask, uv_coords, tangents, bitangents);
    return mesh.attributes().adapt_domain(
        VArray<float3>::ForContainer(std::move(tangents)), bke::AttrDomain::Corner, domain);
  }

  return nullptr;
}

class MeshTangentFieldInput final : public bke::MeshFieldInput {
 private:
  Field<float2> source_uv_coords_;

 public:
  MeshTangentFieldInput(Field<float2> source_uv_field)
      : bke::MeshFieldInput(CPPType::get<float3>(), "Tangent node"),
        source_uv_coords_(source_uv_field)
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

    return construct_mesh_tangent_gvarray(mesh, domain, mask, uv_coords);
  }

  uint64_t hash() const override
  {
    /* Some random constant hash. */
    return 78180125203;
  }

  std::optional<AttrDomain> preferred_domain(const Mesh & /*mesh*/) const final
  {
    return AttrDomain::Corner;
  }

  bool is_equal_to(const fn::FieldNode &other) const override
  {
    return dynamic_cast<const MeshTangentFieldInput *>(&other) != nullptr;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  const bke::DataTypeConversions &conversions = bke::get_implicit_type_conversions();
  const CPPType &float2_type = CPPType::get<float2>();
  Field<float2> source_uv_map = conversions.try_convert(
      params.extract_input<Field<float3>>(INPUT_UV_COORDS), float2_type);

  Field<float3> tangent_field{std::make_shared<MeshTangentFieldInput>(source_uv_map)};
  params.set_output(OUTPUT_TANGENT, std::move(tangent_field));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeInputMeshTangentUV", GEO_NODE_INPUT_MESH_TANGENT_UV);
  ntype.ui_name = "Mesh Tangent";
  ntype.ui_description =
      "Retrieve the tangent space basis axis associated with the given UV coordinates";
  ntype.enum_name_legacy = "INPUT_MESH_UV_TANGENT";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_mesh_tangent_uv_cc
