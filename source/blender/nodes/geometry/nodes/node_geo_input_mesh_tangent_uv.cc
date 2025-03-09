/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_node_types.h"

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

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_mesh_tangent_uv_cc {

/* Socket names. */
const char INPUT_UV_COORDS[14] = "Source UV Map";
const char INPUT_CORNER_NORMALS[8] = "Normals";
const char OUTPUT_TANGENT[8] = "Tangent";
const char OUTPUT_BITANGENT[10] = "Bitangent";

NODE_STORAGE_FUNCS(NodeGeometryMeshTangentUV)

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Vector>(INPUT_UV_COORDS)
      .hide_value()
      .supports_field()
      .description("Source UV map coordinates defined on the corner domain");
  b.add_input<decl::Vector>(INPUT_CORNER_NORMALS)
      .hide_value()
      .supports_field()
      .description(
          "Corner domain normals for computing the tangent space. Optional argument defaulting to "
          "the mesh defined normals.");
  b.add_output<decl::Vector>(OUTPUT_TANGENT)
      .field_source()
      .description("Tangent basis vectors defined on the corner domain");
  b.add_output<decl::Vector>(OUTPUT_BITANGENT)
      .field_source()
      .description("Bitangent basis vectors defined on the corner domain");
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "tang_method", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryMeshTangentUV *data = MEM_callocN<NodeGeometryMeshTangentUV>(__func__);
  data->tang_method = GEO_NODE_MESH_TANGENT_METHOD_SIMPLE;
  node->storage = data;
}

static void compute_partial_triangulation(const Mesh &mesh,
                                          const IndexMask &face_selection,
                                          Array<int> &r_corner_corners)
{
  const OffsetIndices faces = mesh.faces();
  const Span<int3> corner_tris = mesh.corner_tris();
  const Span<int> corner_verts = mesh.corner_verts();

  /* Faces offsets for the partial triangulation. */
  Array<int> faces_offsets(face_selection.size() + 1);
  faces_offsets[0] = 0;
  face_selection.foreach_index([&](const int64_t index_f, const int64_t index_rel) {
    faces_offsets[index_rel + 1] = faces_offsets[index_rel] +
                                     poly_to_tri_count(1, faces[index_f].size());
  });
  OffsetIndices<int> ptri_faces(faces_offsets);

  /* Partial triangulation */
  r_corner_corners = Array<int>(ptri_faces.total_size() * 3);
  face_selection.foreach_index(
      GrainSize(32768), [&](const int64_t index_f, const int64_t index_rel) {
        const IndexRange corner_tri_slice(poly_to_tri_count(index_f, faces[index_f].start()),
                                          poly_to_tri_count(1, faces[index_f].size()));

        const int64_t ptri_offset = ptri_faces[index_rel].start();
        int *ptri_corner_corner = &r_corner_corners[ptri_offset * 3];

        for (const int64_t i : corner_tri_slice.index_range()) {
          const int3 corner_tri = corner_tris[corner_tri_slice[i]];
          *ptri_corner_corner++ = corner_tri.x;
          *ptri_corner_corner++ = corner_tri.y;
          *ptri_corner_corner++ = corner_tri.z;
        }
      });
}

/**
 * Compute the tangents on the corner domain for the given face selection.
 */
static void compute_mikkt_corner_tangents_partial(const Mesh &mesh,
                                                  const IndexMask &face_selection,
                                                  const Span<float2> uv_coords,
                                                  const Span<float3> corner_normals,
                                                  Array<float3> &r_tangents,
                                                  Array<float> &r_bitangents)
{
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  Span<int> corner_tris = mesh.corner_tris().cast<int>();

  Array<int> ptri_corner_corners;
  if (face_selection.size() < mesh.faces_num) {
    compute_partial_triangulation(
        mesh, face_selection, ptri_corner_corners);
    corner_tris = ptri_corner_corners.as_span();
  }

  const int64_t min_buffer_size = faces[face_selection.last()].one_after_last();
  r_tangents = Array<float3>(min_buffer_size);
  r_bitangents = Array<float>(min_buffer_size);
  BKE_mesh_calc_virtual_loop_tangent_single_ex(corner_tris,
                                               corner_verts,
                                               mesh.vert_positions(),
                                               corner_normals,
                                               uv_coords,
                                               r_tangents.as_mutable_span(),
                                               r_bitangents.as_mutable_span());
}

static IndexMask adapt_corner_to_face_mask(const Mesh &mesh,
                                           const IndexMask &corner_mask,
                                           IndexMaskMemory &r_mask_mem)
{
  /* Convert corner -> face domain */
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face = mesh.vert_to_face_map();

  Array<bool> affected_faces(mesh.faces_num, false);
  corner_mask.foreach_index(
      GrainSize(32768), [&](const int64_t index_corner, const int64_t /* index_rel */) {
        for (const int64_t f_index : vert_to_face[corner_verts[index_corner]]) {
          affected_faces[f_index] = true;
        }
      });

  return IndexMask::from_bools(affected_faces, r_mask_mem);
}

static VArray<float3> construct_mesh_tangent_gvarray(const Mesh &mesh,
                                                     const IndexMask &corner_mask,
                                                     const Span<float2> uv_coords,
                                                     const Span<float3> corner_normals,
                                                     GeometryNodeMeshTangentMode mode,
                                                     const bool output_bitangent)
{
  IndexMaskMemory mask_mem;
  IndexMask face_selection = adapt_corner_to_face_mask(mesh, corner_mask, mask_mem);

  Array<float3> tangents;
  Array<float> bitangents;
  if (mode == GeometryNodeMeshTangentMode::GEO_NODE_MESH_TANGENT_METHOD_MIKKT) {
    compute_mikkt_corner_tangents_partial(
        mesh, face_selection, uv_coords, corner_normals, tangents, bitangents);
  }
  else {
    compute_mikkt_corner_tangents_partial(
        mesh, face_selection, uv_coords, corner_normals, tangents, bitangents);
  }

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
  Field<float3> corner_normals_;
  GeometryNodeMeshTangentMode tang_method_;
  bool output_bitangent_;

 public:
  MeshUVTangentFieldInput(Field<float2> source_uv_field,
                          Field<float3> corner_normals,
                          GeometryNodeMeshTangentMode tang_method,
                          bool output_bitangent)
      : bke::MeshFieldInput(CPPType::get<float3>(), "UVMapTangent tangent field"),
        source_uv_coords_(source_uv_field),
        corner_normals_(corner_normals),
        tang_method_(tang_method),
        output_bitangent_(output_bitangent)
  {
    category_ = Category::Generated;
  }

  virtual GVArray get_varray_for_context(const Mesh &mesh,
                                         AttrDomain domain,
                                         const IndexMask &mask) const
  {
    if (domain != AttrDomain::Corner) {
      return nullptr;
    }
    const bke::MeshFieldContext corner_context{mesh, AttrDomain::Corner};
    fn::FieldEvaluator corner_evaluator{corner_context, mesh.corners_num};
    corner_evaluator.add(source_uv_coords_);
    corner_evaluator.add(corner_normals_);
    corner_evaluator.evaluate();
    const VArraySpan<float2> uv_coords = corner_evaluator.get_evaluated<float2>(0);
    VArraySpan<float3> corner_normals = corner_evaluator.get_evaluated<float3>(1);

    if (uv_coords.is_empty()) {
      return mesh.attributes().adapt_domain<float3>(
          VArray<float3>::ForSingle(float3(0.0f, 0.0f, 0.0f), mesh.corners_num),
          AttrDomain::Corner,
          domain);
    }
    if (corner_normals.is_empty()) {
      return construct_mesh_tangent_gvarray(
          mesh, mask, uv_coords, mesh.corner_normals(), tang_method_, output_bitangent_);
    }
    return construct_mesh_tangent_gvarray(
        mesh, mask, uv_coords, corner_normals, tang_method_, output_bitangent_);
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
      return tother->output_bitangent_ == this->output_bitangent_ &&
             tother->source_uv_coords_ == this->source_uv_coords_;
    }
    return false;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  const NodeGeometryMeshTangentUV *storage = &node_storage(params.node());
  const GeometryNodeMeshTangentMode tang_method = storage ? GeometryNodeMeshTangentMode(
                                                                storage->tang_method) :
                                                            GeometryNodeMeshTangentMode::GEO_NODE_MESH_TANGENT_METHOD_SIMPLE;

  const bke::DataTypeConversions &conversions = bke::get_implicit_type_conversions();
  const CPPType &float2_type = CPPType::get<float2>();
  Field<float2> source_uv_map = conversions.try_convert(
      params.extract_input<Field<float3>>(INPUT_UV_COORDS), float2_type);
  Field<float3> corner_normals = params.extract_input<Field<float3>>(INPUT_CORNER_NORMALS);

  Field<float3> tangent_field{std::make_shared<MeshUVTangentFieldInput>(
      source_uv_map, corner_normals, tang_method, false)};
  params.set_output(OUTPUT_TANGENT, std::move(tangent_field));

  Field<float3> bitangent_field{
      std::make_shared<MeshUVTangentFieldInput>(source_uv_map, corner_normals, tang_method, true)};
  params.set_output(OUTPUT_BITANGENT, std::move(bitangent_field));
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
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_mesh_tangent_uv_cc
