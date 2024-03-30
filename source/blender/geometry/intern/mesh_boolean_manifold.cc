/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <iostream>

#include "BLI_array.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_task.hh"
#include "BLI_timeit.hh"
#include "BLI_vector.hh"

#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.hh"

#include "mesh_boolean_manifold.hh"

#include "manifold.h"

using manifold::Manifold;
using manifold::MeshGL;

namespace blender::geometry::boolean {

static void transform_mesh_verts(std::vector<float> &vert_props,
                                 int num_prop,
                                 const Mesh *mesh,
                                 const float4x4 &transform)
{
  const int num_verts = mesh->verts_num;
  BLI_assert(num_prop >= 3 && vert_props.size() >= num_prop * num_verts);
  Span<float3> vpos = mesh->vert_positions();
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

/* Blender's Mesh triangulation may be cached, so maybe faster to use.
 * Downside is that it is not guaranteed to make a manifold triangulation,
 * though in usual cases we should be fine.
 */
static void triangulate_mesh_faces(std::vector<uint32_t> &tri_verts,
                                   std::vector<uint32_t> &face_ids,
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
      face_ids[i] = tri_faces[i];
    }
  });
}

static Manifold manifold_from_mesh(const Mesh *mesh,
                                   const float4x4 &transform)
{
  timeit::ScopedTimer timer("manifold from mesh");
  const int num_verts = mesh->verts_num;
  MeshGL mgl;
  /* TODO: add the props for all the Mesh attributes. For now, just x,y,z. */
  mgl.numProp = 3;
  mgl.vertProperties.resize(mgl.numProp * num_verts);
  transform_mesh_verts(mgl.vertProperties, mgl.numProp, mesh, transform);
  triangulate_mesh_faces(mgl.triVerts, mgl.faceID, mesh, math::is_negative(transform));
  /* DEBUG!! */
  std::cout << "\nMeshGL\n" << "num verts = " << mgl.NumVert()
  << "\nnum triangles = " << mgl.NumTri() << "\n"
  << "\nverts: ";
  for (const int j : IndexRange(mgl.vertProperties.size())) {
    std::cout << mgl.vertProperties[j] << ((j % 3) == 2 ? " / " : " ");
  }
  std::cout << "\ntris: ";
  for (const int k : IndexRange(mgl.triVerts.size())) {
    std::cout << mgl.triVerts[k] << ((k % 3) == 2 ? " / " : " ");
  }
  std::cout << "\nface ids: ";
  for (const int m : IndexRange(mgl.faceID.size())) {
    std::cout << mgl.faceID[m] << " ";
  }
  std::cout << "\n";
  /* end DEBUG!! */
  return Manifold(mgl);
}

static Mesh *meshgl_to_mesh(const MeshGL &mgl,
                            Span<const Mesh *> meshes,
                            Span<Array<short>> material_remaps)
{
  timeit::ScopedTimer timer("manifold to mesh");
  /* TODO: dissolve unnecessary triangle faces. */
  int tot_positions = mgl.NumVert();
  int tot_faces = mgl.NumTri();
  int tot_corners = tot_faces * 3;
  if (mgl.mergeFromVert.size() > 0) {
    /* TODO: handle vertex merging */
    std::cout << "IMPLEMNENT ME: handle vertex merging\n";
  }
  /* We will use Blender's parallelized fundiont to calculate edges later. */
  Mesh *mesh = BKE_mesh_new_nomain_from_template(meshes[0],
                                                 tot_positions,
                                                 0,
                                                 tot_faces,
                                                 tot_corners);
  int num_props = mgl.numProp;
  MutableSpan<float3> positions = mesh->vert_positions_for_write();
  const int grain_size = 100000;
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
  threading::parallel_for(IndexRange(tot_faces), grain_size, [&](const IndexRange range) {
    for (const int face_index : range) {
      int corner_index = 3 * face_index;
      face_offsets[face_index] = corner_index;
      corner_verts[corner_index] = mgl.vertProperties[num_props * face_index];
      corner_verts[corner_index + 1] = mgl.vertProperties[num_props * face_index + 1];
      corner_verts[corner_index + 2] = mgl.vertProperties[num_props * face_index + 2];
    }
  });
  face_offsets[tot_faces] = 3 * tot_faces;
  /* TODO: apply material_remaps. */
  UNUSED_VARS(material_remaps);
  bke::mesh_smooth_set(*mesh, false);
  bke::mesh_calc_edges(*mesh, false, false);
  return mesh;
}

Mesh *mesh_boolean_manifold(Span<const Mesh *> meshes,
                            Span<float4x4> transforms,
                            const float4x4 &target_transform,
                            Span<Array<short>> material_remaps,
                            BooleanOpParameters op_params)
{
  try {
    timeit::ScopedTimer timer("manifold boolean");
    const int num_meshes = meshes.size();
    std::vector<Manifold> manifolds(num_meshes);
    for (const int i : IndexRange(num_meshes)) {
      manifolds[i] = manifold_from_mesh(meshes[i], transforms[i]);
      if (manifolds[i].Status() != Manifold::Error::NoError) {
        std::cout << "Cannot convert Mesh to Manifold, so manifold solver fails\n";
        return nullptr;
      }
    }
    Operation op = op_params.boolean_mode;
    manifold::OpType mop = op == Operation::Intersect ? manifold::OpType::Intersect :
      (op == Operation::Union ? manifold::OpType::Add : manifold::OpType::Subtract);
    Manifold man_result = Manifold::BatchBoolean(manifolds, mop);
    MeshGL meshgl_result = man_result.GetMeshGL();
    Mesh *mesh_result = meshgl_to_mesh(meshgl_result, meshes, material_remaps);
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

}
