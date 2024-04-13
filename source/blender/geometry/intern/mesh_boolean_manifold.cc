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

#include "GEO_join_geometries.hh"
#include "GEO_realize_instances.hh"

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

/* Class to keep track of offsets into the fundamental arrays
 * when a sequence of Meshes is joined. */
class JoinedMeshOffsets {
  Array<int> vert_offsets_data_;
  Array<int> edge_offsets_data_;
  Array<int> face_offsets_data_;
  Array<int> corner_offsets_data_;

 public:
  JoinedMeshOffsets() {}
  JoinedMeshOffsets(Span<const Mesh *> meshes);

  OffsetIndices<int> vert_offsets;
  OffsetIndices<int> edge_offsets;
  OffsetIndices<int> face_offsets;
  OffsetIndices<int> corner_offsets;
};

/* Build a structure to hold the OffsetIndices representing the index
 * ranges for each of the meshes when the are concatenated in the given
 * order.  Get ranges for each of verts, edges, faces, and corners. */
JoinedMeshOffsets::JoinedMeshOffsets(Span<const Mesh *> meshes)
{
  const int num_meshes = meshes.size();
  vert_offsets_data_.reinitialize(num_meshes + 1);
  edge_offsets_data_.reinitialize(num_meshes + 1);
  face_offsets_data_.reinitialize(num_meshes + 1);
  corner_offsets_data_.reinitialize(num_meshes + 1);
  vert_offsets_data_[0] = 0;
  edge_offsets_data_[0] = 0;
  face_offsets_data_[0] = 0;
  corner_offsets_data_[0] = 0;
  for (const int i : IndexRange(num_meshes)) {
    vert_offsets_data_[i + 1] = vert_offsets_data_[i] + meshes[i]->verts_num;
    edge_offsets_data_[i + 1] = edge_offsets_data_[i] + meshes[i]->edges_num;
    face_offsets_data_[i + 1] = face_offsets_data_[i] + meshes[i]->faces_num;
    corner_offsets_data_[i + 1] = corner_offsets_data_[i] + meshes[i]->corners_num;
  }
  vert_offsets = OffsetIndices<int>(vert_offsets_data_);
  edge_offsets = OffsetIndices<int>(edge_offsets_data_);
  face_offsets = OffsetIndices<int>(face_offsets_data_);
  corner_offsets = OffsetIndices<int>(corner_offsets_data_);
}

/* It is not yet clear whether or not we are better off using
 * manifold::MeshGL or manifold::Mesh to construct our Manifolds.
 * Leave both code paths here for nowl
 */

// #define USE_MESHGL_INPUT

#ifdef USE_MESHGL_INPUT
static void transform_mesh_verts(std::vector<float> &vert_props,
                                 int num_prop,
                                 const Mesh *mesh,
                                 const float4x4 &transform)
{
  const int num_verts = mesh->verts_num;
  BLI_assert(num_prop >= 3 && vert_props.size() >= num_prop * num_verts);
  Span<float3> vpos = mesh->vert_positions();
  if (math::is_identity(transform)) {
    const int grain_size = 100000;
    threading::parallel_for(IndexRange(num_verts), grain_size, [&](const IndexRange range) {
      for (const int i : range) {
        const float3 &pos = vpos[i];
        int offset = i * num_prop;
        vert_props[offset] = pos[0];
        vert_props[offset + 1] = pos[1];
        vert_props[offset + 2] = pos[2];
      }
    });
  }
  else {
    const int grain_size = 50000;

    threading::parallel_for(IndexRange(num_verts), grain_size, [&](const IndexRange range) {
      for (const int i : range) {
        float3 transformed_pos = math::transform_point(transform, vpos[i]);
        int offset = i * num_prop;
        vert_props[offset] = transformed_pos[0];
        vert_props[offset + 1] = transformed_pos[1];
        vert_props[offset + 2] = transformed_pos[2];
      }
    });
  }
}

/* Triangulate the faces in mesh and store the vertex indices of the
 * triangles in tri_verts.
 * Record the original mesh face index, added to face_id_offset, in
 * the face_ids argument
 * Blender's Mesh triangulation may be cached, so maybe faster to use.
 * Downside is that it is not guaranteed to make a manifold triangulation,
 * though in usual cases we should be fine.
 * TODO: experiment with using manifold's triangulator.
 */
static void triangulate_mesh_faces(std::vector<uint32_t> &tri_verts,
                                   std::vector<uint32_t> &face_ids,
                                   int face_id_offset,
                                   const Mesh *mesh,
                                   bool reverse_order)
{
  Span<int3> corner_tris = mesh->corner_tris();
  Span<int> tri_faces = mesh->corner_tri_faces();
  Span<int> corner_verts = mesh->corner_verts();
  tri_verts.resize(3 * corner_tris.size());
  face_ids.resize(corner_tris.size());
  /* Order to take triangle vertices depends on whether the transform
   * matrix applied after the triangulation was negative or not.
   */
  int tri_1_index = reverse_order ? 2 : 1;
  int tri_2_index = reverse_order ? 1 : 2;
  const int grain_size = 100000;
  threading::parallel_for(corner_tris.index_range(), grain_size, [&](const IndexRange range) {
    for (const int i : range) {
      const int3 &ctri = corner_tris[i];
      int offset = i * 3;
      tri_verts[offset] = corner_verts[ctri[0]];
      tri_verts[offset + 1] = corner_verts[ctri[tri_1_index]];
      tri_verts[offset + 2] = corner_verts[ctri[tri_2_index]];
      face_ids[i] = tri_faces[i] + face_id_offset;
    }
  });
}

/* Convert mesh into a manifold. Apply the transform to all vertices
 * (usually it will be the identity matrix).
 * Store the original vertex id in the mesh, plus vert_id_offset, as the fourth
 * vertex property in meshGL.
 * When setting faceID, add the face_id_offset to the original face ids in menns.
 */
static Manifold manifold_from_mesh(const Mesh *mesh,
                                   const float4x4 &transform,
                                   int mesh_id,
                                   int face_id_offset)
{
  constexpr int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "\nMANIFOLD_FROM_MESH\n";
    std::cout << "face_id_offset = " << face_id_offset << "\n";
    dump_mesh(mesh, "mesh to convert");
  }
  timeit::ScopedTimer timer("manifold from mesh");
  const int num_verts = mesh->verts_num;
  MeshGL mgl;
  /* Vertex props will be x,y,z. */
  mgl.numProp = 3;
  mgl.vertProperties.resize(mgl.numProp * num_verts);
  transform_mesh_verts(mgl.vertProperties, mgl.numProp, mesh, transform);
  triangulate_mesh_faces(
      mgl.triVerts, mgl.faceID, face_id_offset, mesh, math::is_negative(transform));
  mgl.runOriginalID.push_back(mesh_id);
  if (dbg_level > 0) {
    dump_meshgl(mgl, "manifold_from_mesh result");
  }
  return Manifold(mgl);
}

#else

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
  // Span<int> tri_faces = mesh->corner_tri_faces();
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
  return Manifold(manifold_mesh);
}
#endif

/* Get a  Mesh that is the join of each of the argument meshes. The main reason
 * to do this is that the joined result will have the merger of all needed attributes
 * and materials, and proper assignment of attributes values to each element.
 * Return the GeometrySet that contains the Mesh; its destructor will free
 * the Mesh.
 */
static bke::GeometrySet joined_meshes(Span<const Mesh *> meshes)
{
  constexpr int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "\JOINED_MESHES\n";
    if (dbg_level > 1) {
      int k = 0;
      for (const Mesh *m : meshes) {
        dump_mesh(m, "join argument " + std::to_string(k++));
      }
    }
  }
  Array<bke::GeometrySet> geometries(meshes.size());
  for (const int i : meshes.index_range()) {
    Mesh *mesh = const_cast<Mesh *>(meshes[i]);
    geometries[i] = bke::GeometrySet::from_mesh(mesh, bke::GeometryOwnershipType::ReadOnly);
  }
  /* For now, propagate all anonymous attributes. TODO: what should we really do? */
  const bke::AnonymousAttributePropagationInfo propagation_info;
  bke::GeometrySet join_result = geometry::join_geometries(geometries, propagation_info);
  geometry::RealizeInstancesOptions options;
  options.keep_original_ids = false;
  options.realize_instance_attributes = true;
  options.propagation_info = propagation_info;
  return geometry::realize_instances(join_result, options);
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
 * what is the corresponding face index in the joined mesh that is the
 * result of joining all the \a input_meshees ?
 * The \a join_offsets argument has precalculated the IndexRanges where
 * each element type of an input mesh lies in the joined mesh. */
static int join_mesh_face(int output_tri,
                          const MeshGL &output_meshgl,
                          Span<const Mesh *> input_meshes,
                          const JoinedMeshOffsets &join_offsets,
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

  /* Now find the face index in the joined mesh, given the triangle index in the output meshgl. */
  int face_in_triangulated_input = output_meshgl.faceID[output_tri];
  int face_in_input_mesh =
      input_meshes[input_mesh_index]->corner_tri_faces()[face_in_triangulated_input];
  int face_in_joined_mesh = join_offsets.face_offsets[input_mesh_index][face_in_input_mesh];
  return face_in_joined_mesh;
}

/* Convert the meshgl that is the result of the boolean back into a
 * Blender Mesh.
 * The joined_mesh argument contains the input Meshes joined together,
 * and the mesh_offsets argument tells us where each argument mesh's
 * elements are in joined_mesh.
 * The original_id_offset says what OriginalID the first argument
 * mesh has. We assume the others are sequential from there.
 */
static Mesh *meshgl_to_mesh(const MeshGL &mgl,
                            const Mesh *join_mesh,
                            Span<const Mesh *> meshes,
                            const JoinedMeshOffsets &mesh_offsets,
                            int original_id_offset)
{
  constexpr int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "\nMESHGL_TO_MESH\n";
    dump_meshgl(mgl, "meshgl_to_mesh argument");
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
      join_mesh, tot_positions, 0, tot_faces, tot_corners);
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
  /* TODO: following is very specific to all-triangle output, */
  MutableSpan<int> face_offsets = mesh->face_offsets_for_write();
  MutableSpan<int> corner_verts = mesh->corner_verts_for_write();
  bke::MutableAttributeAccessor mesh_attrs = mesh->attributes_for_write();
  bke::AttributeAccessor join_mesh_attrs = join_mesh->attributes();
  grain_size = 50000;
  threading::parallel_for(IndexRange(tot_faces), grain_size, [&](const IndexRange range) {
    for (const int face_index : range) {
      int corner_index = 3 * face_index;
      face_offsets[face_index] = corner_index;
      corner_verts[corner_index] = mgl.triVerts[corner_index];
      corner_verts[corner_index + 1] = mgl.triVerts[corner_index + 1];
      corner_verts[corner_index + 2] = mgl.triVerts[corner_index + 2];
      int jface = join_mesh_face(face_index, mgl, meshes, mesh_offsets, original_id_offset);
      if (dbg_level > 1) {
        std::cout << "output tri " << face_index << " -> joined face index " << jface << "\n";
      }
      mesh_attrs.for_all(
          [&](const bke::AttributeIDRef &id, const bke::AttributeMetaData &meta_data) {
            if (meta_data.domain == bke::AttrDomain::Face) {
              /* TODO: speed up by moving these lookups out of the loop.
               * and do the finish outside the loop at the end.
               * Perhaps use realize_instances for inspiration. */
              if (dbg_level > 1) {
                std::cout << "do face attr " << id.name() << "\n";
              }
              bke::GAttributeReader reader = join_mesh_attrs.lookup(id);
              bke::GAttributeWriter writer = mesh_attrs.lookup_for_write(id);
              if (reader && writer) {
                const CPPType &type = reader.varray.type();
                BUFFER_FOR_CPP_TYPE_VALUE(type, buffer);
                reader.varray.get_to_uninitialized(jface, buffer);
                /* HACK: I don't understand what is going on here, but this makes the materials work. */
                if (id.name() == "material_index") {
                  int32_t *p = static_cast<int32_t *>(buffer);
                  *p -= 1;
                }
                /* end HACK */
                writer.varray.set_by_copy(face_index, buffer);
                writer.finish();
              }
            }
            return true;
          });
    }
  });
  face_offsets[tot_faces] = 3 * tot_faces;
  bke::mesh_smooth_set(*mesh, false);
  bke::mesh_calc_edges(*mesh, false, false);
  if (dbg_level > 0) {
    dump_mesh(mesh, "output mesh");
  }
  BKE_mesh_validate(mesh, true, true);
  return mesh;
}

Mesh *mesh_boolean_manifold(Span<const Mesh *> meshes,
                            Span<float4x4> transforms,
                            const float4x4 &target_transform,
                            Span<Array<short>> /* material_remaps */,
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
    bool no_transforms = math::is_identity(target_transform);
    no_transforms &= std::all_of(transforms.begin(), transforms.end(), [](const float4x4 &t) {
      return math::is_identity(t);
    });
    if (!no_transforms) {
      std::cout << "IMPLEMENT ME: mesh_boolean_manifold with transforms\n";
      return nullptr;
    }
    bke::GeometrySet join_set = joined_meshes(meshes);
    JoinedMeshOffsets jmo(meshes);
    const Mesh *join_mesh = join_set.get_mesh();
    if (dbg_level > 1) {
      dump_mesh(join_mesh, "join_mesh");
    }
    for (const int i : IndexRange(num_meshes)) {
#ifdef USE_MESHGL_INPUT
      manifolds[i] = manifold_from_mesh(meshes[i], transforms[i], i, jmo.face_offsets[i].start());
#else
      manifolds[i] = manifold_from_mesh_via_mesh(meshes[i]);
#endif
      if (manifolds[i].Status() != Manifold::Error::NoError) {
        std::cout << "Cannot convert Mesh to Manifold, so manifold solver fails\n";
        return nullptr;
      }
    }
    int originalID0 = manifolds[0].OriginalID();
    Operation op = op_params.boolean_mode;
    manifold::OpType mop = op == Operation::Intersect ?
                               manifold::OpType::Intersect :
                               (op == Operation::Union ? manifold::OpType::Add :
                                                         manifold::OpType::Subtract);
    Manifold man_result = Manifold::BatchBoolean(manifolds, mop);
    MeshGL meshgl_result = man_result.GetMeshGL();
    if (dbg_level > 0) {
      std::cout << "boolean result has " << meshgl_result.NumTri() << " tris\n";
    }
    Mesh *mesh_result = meshgl_to_mesh(meshgl_result, join_mesh, meshes, jmo, originalID0);
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
