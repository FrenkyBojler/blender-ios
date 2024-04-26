/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <iostream>

#include "BLI_array.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"
#include "BLI_task.hh"
#include "BLI_timeit.hh"
#include "BLI_vector.hh"

#include "BKE_attribute.hh"
#include "BKE_customdata.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.hh"

#include "mesh_boolean_manifold.hh"

#include "manifold.h"

using manifold::Manifold;
using manifold::MeshGL;

/* Using this for now for debug printing of materials. Can remove later. */
#include "DNA_material_types.h"

namespace blender::geometry::boolean {

/* Some debug output functions. */

static std::ostream &operator<<(std::ostream &os, const glm::vec3 &v)
{
  os << "(" << v[0] << "," << v[1] << "," << v[2] << ")";
  return os;
}

static std::ostream &operator<<(std::ostream &os, const glm::ivec3 &v)
{
  os << "(" << v[0] << "," << v[1] << "," << v[2] << ")";
  return os;
}

template<typename T>
static void dump_vector(const std::vector<T> &vec, int stride, const std::string &name)
{
  std::cout << name << ":";
  for (size_t i = 0; i < vec.size(); i++) {
    if (i % 10 == 0) {
      std::cout << "\n[" << i << "] ";
    }
    std::cout << vec[i] << " ";
    if (stride > 1 && (i % stride) == stride - 1) {
      std::cout << "/ ";
    }
  }
  std::cout << "\n";
}

static void dump_meshgl(const MeshGL &mgl, const std::string &name)
{
  std::cout << "\nMeshGL " << name << ":\n"
            << "num verts = " << mgl.NumVert() << "\nnum triangles = " << mgl.NumTri() << "\n"
            << "\n";
  dump_vector(mgl.vertProperties, mgl.numProp, "vertProperties");
  dump_vector(mgl.triVerts, 3, "triVerts");
  dump_vector(mgl.faceID, 1, "faceID");
  if (mgl.mergeFromVert.size() > 0) {
    dump_vector(mgl.mergeFromVert, 1, "mergeFromVert");
    dump_vector(mgl.mergeToVert, 1, "mergeToVert");
  }
  dump_vector(mgl.runIndex, 1, "runIndex");
  dump_vector(mgl.runOriginalID, 1, "runOrigiinalID");
}

static void dump_manmesh(const manifold::Mesh &mmesh, const std::string &name)
{
  std::cout << "\nmanifold::Mesh " << name << ":\n";
  dump_vector(mmesh.vertPos, 1, "vertPos");
  dump_vector(mmesh.triVerts, 1, "triVerts");
}

template<typename T> static void dump_span(Span<T> span, const std::string &name)
{
  std::cout << name << ":";
  for (const int i : span.index_range()) {
    if (i % 10 == 0) {
      std::cout << "\n[" << i << "] ";
    }
    std::cout << span[i] << " ";
  }
  std::cout << "\n";
}

static void dump_mesh(const Mesh *mesh, const std::string &name)
{
  std::cout << "\nMesh " << name << ":\n"
            << "verts_num = " << mesh->verts_num << "\nfaces_num = " << mesh->faces_num
            << "\nedges_num = " << mesh->edges_num << "\ncorners_num = " << mesh->corners_num
            << "\n";
  dump_span(mesh->vert_positions(), "verts");
  dump_span(mesh->edges(), "edges");
  dump_span(mesh->corner_verts(), "corner_verts");
  dump_span(mesh->corner_edges(), "corner_edges");
  dump_span(mesh->face_offsets(), "face_offsets");
  std::cout << "triangulation:\n";
  dump_span(mesh->corner_tris(), "corner_tris");
  dump_span(mesh->corner_tri_faces(), "corner_tri_faces");
  std::cout << "attributes:\n";
  bke::AttributeAccessor attrs = mesh->attributes();
  attrs.for_all([&](const bke::AttributeIDRef &id, const bke::AttributeMetaData &meta_data) {
    if (ELEM(id.name(), "position", ".edge_verts", ".corner_vert", ".corner_edge")) {
      return true;
    }
    static const char *domain_names[] = {
        "point", "edge", "face", "corner", "curve", "instance", "layer"};
    const int di = static_cast<int8_t>(meta_data.domain);
    const char *domain = (di >= 0 && di < ATTR_DOMAIN_NUM) ? domain_names[di] : "?";
    std::string label = std::string(domain) + ": " + id.name();
    switch (meta_data.data_type) {
      case CD_PROP_FLOAT: {
        VArraySpan<float> floatspan(*attrs.lookup<float>(id));
        dump_span(floatspan, label);
      } break;
      case CD_PROP_INT32:
      case CD_PROP_BOOL: {
        VArraySpan<int> intspan(*attrs.lookup<int>(id));
        dump_span(intspan, label);
      } break;
      case CD_PROP_FLOAT3: {
        VArraySpan<float3> float3span(*attrs.lookup<float3>(id));
        dump_span(float3span, label);
      } break;
      case CD_PROP_FLOAT2: {
        VArraySpan<float2> float2span(*attrs.lookup<float2>(id));
        dump_span(float2span, label);
      } break;
      default:
        std::cout << label << " attribute not dumped\n";
        break;
    }
    return true;
  });
  std::cout << "materials:\n";
  for (int i = 0; i < mesh->totcol; i++) {
    std::cout << "[" << i << "]: " << (mesh->mat[i] ? mesh->mat[i]->id.name + 2 : "none")
    << "\n";
  }
}

static Manifold manifold_from_mesh_via_mesh(const Mesh *mesh)
{
  constexpr int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "\nMANIFOLD_FROM_MESH_VIA_MESH\n";
    dump_mesh(mesh, "mesh to convert");
  }
  timeit::ScopedTimer timer("manifold from mesh via mesh");
  const int num_verts = mesh->verts_num;
  manifold::Mesh manifold_mesh;
  manifold_mesh.vertPos.resize(num_verts);
  Span<float3> vpos = mesh->vert_positions();
  const int grain_size = 10000;
  threading::parallel_for(IndexRange(num_verts), grain_size, [&](const IndexRange range) {
    for (const int i : range) {
      const float3 &pos = vpos[i];
      manifold_mesh.vertPos[i] = glm::vec3(pos[0], pos[1], pos[2]);
    }
  });
  Span<int3> corner_tris = mesh->corner_tris();
  Span<int> corner_verts = mesh->corner_verts();
  manifold_mesh.triVerts.resize(corner_tris.size());
  threading::parallel_for(corner_tris.index_range(), grain_size, [&](const IndexRange range) {
      for (const int i : range) {
        const int3 &ctri = corner_tris[i];
        manifold_mesh.triVerts[i] = glm::ivec3(
                                               corner_verts[ctri[0]], corner_verts[ctri[1]], corner_verts[ctri[2]]);
      }
  });
  if (dbg_level > 0) {
    dump_manmesh(manifold_mesh, "converted result");
  }
  Manifold ans;
  {
    timeit::ScopedTimer mtimer("manifold constructor from mesh");
    ans = Manifold(manifold_mesh);
  }
  return ans;
}

/* Find the index in offset_indices that the first place
 * with a value >= x. Return -1 if there is no such index.
 * When the argument is a sorted array of range breakpoints, starting at 0,
 * this will return the index of the range that contains x, if there is one. */
template<typename T> static int which_offset_index(T x, Span<T> offset_indices)
{
  /* TODO: use binary search or std::lower if size of offset_indices is not small.
   * Maybe add this into the mathods for OffsetIndices. */
  int i = 0;
  for (; i != offset_indices.size() - 1; i++) {
    if (x < offset_indices[i + 1]) {
      return i;
    }
  }
  return -1;
}

/* Given an output triangle index \a output_tri in \a output_mesh_gl,
 * what is the corresponding input mesh index and face index? */
static std::pair<int, int> mesh_and_face(int output_tri,
                                         const MeshGL &output_meshgl,
                                         Span<const Mesh *> input_meshes,
                                         int original_id_offset)

{
  /* First find the index for the original input_mesh that contains the output_tri. */
  int output_run_index = which_offset_index(uint32_t(3 * output_tri),
                                            Span<uint32_t>(output_meshgl.runIndex));
  BLI_assert(output_run_index != -1);
  /* We assume that the auto-supplied runOriginalIDs start at some number and go up consecutively.
   */
  int input_mesh_index = output_meshgl.runOriginalID[output_run_index] - original_id_offset;
  BLI_assert(input_mesh_index >= 0 && input_mesh_index < input_meshes.size());

  /* Now find the face index in the input mesh, given the triangle index in the output meshgl. */
  int face_in_triangulated_input = output_meshgl.faceID[output_tri];
  int face_in_input_mesh =
      input_meshes[input_mesh_index]->corner_tri_faces()[face_in_triangulated_input];
  return {input_mesh_index, face_in_input_mesh};
}

#if 0
/* Copy face attributes (custom data) from face \a index_in_orig_me in \a orig_me
 * to face \a face_index in \a dest_mesh.
 * Material indices ineed special hanlding (remapping).
 * TODO: perhaps change all this to use new Attribute interface.
 * At least, avoid need to lookup src_material_indices each time.
 */
static void copy_face_attributes(Mesh *dest_mesh,
                                 const Mesh *orig_me,
                                 int face_index,
                                 int index_in_orig_me,
                                 Span<short> material_remap,
                                 MutableSpan<int> dst_material_indices)
{
  CustomData *target_cd = &dest_mesh->face_data;
  const CustomData *source_cd = &orig_me->face_data;
  for (int source_layer_i = 0; source_layer_i < source_cd->totlayer; ++source_layer_i) {
    const eCustomDataType ty = eCustomDataType(source_cd->layers[source_layer_i].type);
    const char *name = source_cd->layers[source_layer_i].name;
    int target_layer_i = CustomData_get_named_layer_index(target_cd, ty, name);
    if (target_layer_i != -1) {
      CustomData_copy_data_layer(
          source_cd, target_cd, source_layer_i, target_layer_i, index_in_orig_me, face_index, 1);
    }
  }

  /* Fix material indices after they have been transferred as a generic attribute. */
  const VArray<int> src_material_indices = *orig_me->attributes().lookup_or_default<int>(
      "material_index", bke::AttrDomain::Face, 0);
  const int src_index = src_material_indices[index_in_orig_me];
  if (material_remap.index_range().contains(src_index)) {
    const int remapped_index = material_remap[src_index];
    dst_material_indices[face_index] = remapped_index >= 0 ? remapped_index : src_index;
  }
  else {
    dst_material_indices[face_index] = src_index;
  }
  BLI_assert(dst_material_indices[face_index] >= 0);
}
#endif

class GAttributeReadWriteSpans {
public:
  /* A set of attributes we want copied. */
  Vector<bke::AttributeIDRef> attrs;
  /* Parallel array of data_type. */
  Vector<eCustomDataType> data_types;
  /* Destination attribute data, one span per attribute we want copied. */
  Vector<GMutableSpan> dest;
  /* Correpsonding AttributeWriters. */
  Vector<bke::GSpanAttributeWriter> dest_writers;
  /* For each input mesh, the source attribute data parallel to dest. */
  Array<Vector<std::optional<GVArraySpan>>> sources;

  GAttributeReadWriteSpans(Span<const Mesh *> input_meshes,
                                 Mesh *output_mesh,
                                 bke::AttrDomain domain);
  ~GAttributeReadWriteSpans();

  int find_attr_index(const char *name) const;
};

GAttributeReadWriteSpans::GAttributeReadWriteSpans(
        Span<const Mesh *> input_meshes,
        Mesh *output_mesh,
        bke::AttrDomain domain)
{
  const int num_mesh = input_meshes.size();
  Vector<bke::AttributeAccessor> input_accessors;
  this->sources.reinitialize(num_mesh);
  for (int i : IndexRange(num_mesh)) {
    input_accessors.append(input_meshes[i]->attributes());
  }
  bke::MutableAttributeAccessor output_accessor = output_mesh->attributes_for_write();
  output_accessor.for_all([&](const bke::AttributeIDRef &id,
                              const bke::AttributeMetaData &metadata) {
    if (metadata.domain != domain) {
      return true;
    }
    this->attrs.append(id);
    this->data_types.append(metadata.data_type);
    this->dest_writers.append(output_accessor.lookup_or_add_for_write_only_span(id, metadata.domain, metadata.data_type));
    this->dest.append(this->dest_writers.last().span);
    for (int i : IndexRange(num_mesh)) {
      this->sources[i].append(*input_accessors[i].lookup_or_default(id, domain, metadata.data_type));
    }
    return true;
  });
}

GAttributeReadWriteSpans::~GAttributeReadWriteSpans()
{
  for (bke::GSpanAttributeWriter& w : dest_writers) {
    w.finish();
  }
}

int GAttributeReadWriteSpans::find_attr_index(const char *name) const
{
  for (int i : this->attrs.index_range()) {
    if (this->attrs[i].name() == name) {
      return i;
    }
  }
  return -1;
}

static void copy_face_attrs(GAttributeReadWriteSpans &rw_spans,
                            int input_mesh_index,
                            int input_face,
                            int output_face,
                            int material_span_index,
                            Span<Array<short>> material_remaps)
{
  constexpr int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "copy_face_attrs, input mesh "
      << input_mesh_index << ", face " << input_face
    << " to  output face " << output_face << "\n";
  }
  for (const int i : rw_spans.attrs.index_range()) {
    std::optional<GVArraySpan> &src = rw_spans.sources[input_mesh_index][i];
    GMutableSpan &dst = rw_spans.dest[i];
    if (src.has_value()) {
      /* rw_spans.dest[output_face] = src[input_face] */
      dst.type().copy_assign(src.value()[input_face], dst[output_face]);
      /* Special additional handling for maetrial_index property. */
      if (i == material_span_index) {
        BLI_assert(dst.type().size() == sizeof(int32_t));
        int32_t src_mat;
        Span<short> remap = material_remaps[input_mesh_index];
        dst.type().copy_assign(src.value()[input_face], &src_mat);
        if (remap.index_range().contains(src_mat)) {
          int remapped_index = remap[src_mat];
          if (remapped_index >= 0) {
            dst.type().copy_assign(&remapped_index, dst[output_face]);
          }
        }
      }
    }
  }
}

/* Convert the meshgl that is the result of the boolean back into a
 * Blender Mesh.
 * The original_id_offset says what OriginalID the first argument
 * mesh has. We assume the others are sequential from there.
 * Note: the caller of mesh_boolean_manifold will fix the returned
 * mesh's mat[] array to hold materials approprite for the material_remaps.
 */
static Mesh *meshgl_to_mesh(const MeshGL &mgl,
                            Span<const Mesh *> meshes,
                            Span<Array<short>> material_remaps,
                            int original_id_offset)
{
  constexpr int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "\nMESHGL_TO_MESH\n";
    dump_meshgl(mgl, "meshgl_to_mesh argument");
    std::cout << "material_remaps:\n";
    for (int i : material_remaps.index_range()) {
      dump_span(material_remaps[i].as_span(), std::to_string(i));
    }
  }
  timeit::ScopedTimer timer("manifold to mesh");
  /* TODO: dissolve unnecessary triangle faces. */
  int tot_positions = mgl.NumVert();
  int tot_faces = mgl.NumTri();
  int tot_corners = tot_faces * 3;
  if (mgl.mergeFromVert.size() > 0) {
    /* TODO: handle vertex merging */
    std::cout << "IMPLEMENT ME: handle vertex merging\n";
  }
  /* We will use Blender's parallelized fundiont to calculate edges later. */
  Mesh *mesh = BKE_mesh_new_nomain_from_template(
      meshes[0], tot_positions, 0, tot_faces, tot_corners);
  int num_props = mgl.numProp;
  MutableSpan<float3> positions = mesh->vert_positions_for_write();
  int grain_size = 100000;
  threading::parallel_for(IndexRange(tot_positions), grain_size, [&](const IndexRange range) {
      for (const int i : range) {
        int offset = num_props * i;
        float3 pos(mgl.vertProperties[offset],
                   mgl.vertProperties[offset + 1],
                   mgl.vertProperties[offset + 2]);
        positions[i] = pos;
      }
  });
  GAttributeReadWriteSpans face_attrs(meshes, mesh, bke::AttrDomain::Face);
  int material_span_index = face_attrs.find_attr_index("material_index");
  /* TODO: following is very specific to all-triangle output, */
  MutableSpan<int> face_offsets = mesh->face_offsets_for_write();
  MutableSpan<int> corner_verts = mesh->corner_verts_for_write();
  grain_size = 50000;
  threading::parallel_for(IndexRange(tot_faces), grain_size, [&](const IndexRange range) {
      for (const int face_index : range) {
        int corner_index = 3 * face_index;
        face_offsets[face_index] = corner_index;
        corner_verts[corner_index] = mgl.triVerts[corner_index];
        corner_verts[corner_index + 1] = mgl.triVerts[corner_index + 1];
        corner_verts[corner_index + 2] = mgl.triVerts[corner_index + 2];
        std::pair<int, int> m_and_f = mesh_and_face(face_index, mgl, meshes,  original_id_offset);
        int input_mesh_index = m_and_f.first;
        int input_face_index = m_and_f.second;
        copy_face_attrs(face_attrs, input_mesh_index, input_face_index, face_index, material_span_index, material_remaps);
      }
  });
  face_offsets[tot_faces] = 3 * tot_faces;
  // mesh_material_indices.finish();
  // bke::mesh_smooth_set(*mesh, false);
  {
    timeit::ScopedTimer("calculating edges");
    bke::mesh_calc_edges(*mesh, false, false);
  }
  if (dbg_level > 0) {
    dump_mesh(mesh, "output mesh");
  }
  if (dbg_level > 1) {
    BKE_mesh_validate(mesh, true, true);
  }
  return mesh;
}

Mesh *mesh_boolean_manifold(Span<const Mesh *> meshes,
                            Span<float4x4> transforms,
                            const float4x4 &target_transform,
                            Span<Array<short>> material_remaps,
                            BooleanOpParameters op_params)
{
  constexpr int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "\nMESH_BOOLEAN_MANIFOLD with " << meshes.size() << " args\n";
  }
  try {
    timeit::ScopedTimer timer("manifold boolean");
    const int num_meshes = meshes.size();
    std::vector<Manifold> manifolds(num_meshes);
    std::vector<bool> manifold_ok(num_meshes);
    bool no_transforms = math::is_identity(target_transform);
    no_transforms &= std::all_of(transforms.begin(), transforms.end(), [](const float4x4 &t) {
      return math::is_identity(t);
    });
    if (!no_transforms) {
      std::cout << "IMPLEMENT ME: mesh_boolean_manifold with transforms\n";
      return nullptr;
    }
    threading::parallel_for_each(IndexRange(num_meshes), [&](int i) {
      manifolds[i] = manifold_from_mesh_via_mesh(meshes[i]);
      manifold_ok[i] = manifolds[i].Status() == Manifold::Error::NoError;
    });
    if (std::any_of(manifold_ok.begin(), manifold_ok.end(), [](bool v) { return !v; })) {
      std::cout << "Cannot convert Mesh to Manifold, so manifold solver fails\n";
      return nullptr;
    }
    int originalID0 = manifolds[0].OriginalID();
    Operation op = op_params.boolean_mode;
    manifold::OpType mop = op == Operation::Intersect ?
                               manifold::OpType::Intersect :
                               (op == Operation::Union ? manifold::OpType::Add :
                                                         manifold::OpType::Subtract);
    MeshGL meshgl_result;
    {
      timeit::ScopedTimer("doing boolean and getting meshgl result");
      Manifold man_result = Manifold::BatchBoolean(manifolds, mop);
      meshgl_result = man_result.GetMeshGL();
      if (dbg_level > 0) {
        std::cout << "boolean result has " << meshgl_result.NumTri() << " tris\n";
      }
    }
    Mesh *mesh_result = meshgl_to_mesh(meshgl_result, meshes, material_remaps, originalID0);
    /* TODO: if (unlikely) target_transform is not identity, trasform the mesh. */
    UNUSED_VARS(target_transform);
    return mesh_result;
  }
  catch (const std::exception &e) {
    std::cout << "mesh_boolean_manifold: exception: " << e.what() << "\n";
  }
  catch (...) {
    std::cout << "mesh_boolean_manifold: unknown exception\n";
  }
  return nullptr;
}

}  // namespace blender::geometry::boolean
