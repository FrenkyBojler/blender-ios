/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_mesh_gpu.hh"

#include <fmt/format.h>

#include "BKE_mesh.hh"

#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_task.hh"

#include "DNA_mesh_types.h"

#include "GPU_context.hh"

bool BKE_mesh_gpu_topology_create(const Mesh *mesh, blender::bke::MeshGPUTopology &topology)
{
  if (!mesh) {
    return false;
  }

  /* Clear any existing data */
  BKE_mesh_gpu_topology_free(topology);

  /* Get mesh topology data */
  const auto face_offsets = mesh->face_offsets();
  const auto corner_to_face = mesh->corner_to_face_map();
  const auto corner_verts_span = mesh->corner_verts();
  const auto corner_tris = mesh->corner_tris();
  const auto corner_tri_faces = mesh->corner_tri_faces();
  const auto edges = mesh->edges();
  const auto corner_edges_span = mesh->corner_edges();

  /* Convert spans to vectors for easier handling */
  blender::Vector<int> corner_verts_vec(corner_verts_span.begin(), corner_verts_span.end());
  blender::Vector<int> corner_tris_flat;
  corner_tris_flat.reserve(corner_tris.size() * 3);
  for (const blender::int3 &tri : corner_tris) {
    corner_tris_flat.append(tri.x);
    corner_tris_flat.append(tri.y);
    corner_tris_flat.append(tri.z);
  }
  blender::Vector<int> corner_tri_faces_vec(corner_tri_faces.begin(), corner_tri_faces.end());

  blender::Vector<int> edges_flat;
  edges_flat.reserve(edges.size() * 2);
  for (const blender::int2 &edge : edges) {
    edges_flat.append(edge.x);
    edges_flat.append(edge.y);
  }

  blender::Vector<int> corner_edges_vec(corner_edges_span.begin(), corner_edges_span.end());

  /* Get vertex-to-face mapping */
  const blender::OffsetIndices<int> v2f_off = mesh->vert_to_face_map_offsets();
  const blender::GroupedSpan<int> v2f = mesh->vert_to_face_map();

  const int v2f_offsets_size = v2f_off.size();
  blender::Vector<int> v2f_offsets(v2f_offsets_size);
  for (int v = 0; v < v2f_offsets_size; ++v) {
    v2f_offsets[v] = v2f_off.data()[v];
  }
  const int total_v2f = v2f_offsets.is_empty() ? 0 : v2f_offsets.last();

  blender::Vector<int> v2f_indices;
  v2f_indices.resize(std::max(total_v2f, 0));
  if (v2f_offsets_size > 0) {
    blender::threading::parallel_for(
        blender::IndexRange(v2f_offsets_size - 1), 4096, [&](const blender::IndexRange range) {
          for (int v : range) {
            const blender::Span<int> faces_v = v2f[v];
            const int dst = v2f_off.data()[v];
            if (!faces_v.is_empty()) {
              std::copy(faces_v.begin(), faces_v.end(), v2f_indices.begin() + dst);
            }
          }
        });
  }

  /* Compute offsets for packed buffer */
  topology.face_offsets_offset = 0;
  topology.corner_to_face_offset = topology.face_offsets_offset + int(face_offsets.size());
  topology.corner_verts_offset = topology.corner_to_face_offset + int(corner_to_face.size());
  topology.corner_tris_offset = topology.corner_verts_offset + int(corner_verts_vec.size());
  topology.corner_tri_faces_offset = topology.corner_tris_offset + int(corner_tris_flat.size());
  topology.edges_offset = topology.corner_tri_faces_offset +
                          int(corner_tri_faces_vec.size());
  topology.corner_edges_offset = topology.edges_offset + int(edges_flat.size());
  topology.vert_to_face_offsets_offset = topology.corner_edges_offset +
                                         int(corner_edges_vec.size());
  topology.vert_to_face_offset = topology.vert_to_face_offsets_offset + int(v2f_offsets.size());
  topology.total_size = topology.vert_to_face_offset + int(v2f_indices.size());

  /* Pack into single int vector */
  topology.data.clear();
  topology.data.reserve(topology.total_size);
  topology.data.extend(face_offsets);
  topology.data.extend(corner_to_face);
  topology.data.extend(corner_verts_vec);
  topology.data.extend(corner_tris_flat);
  topology.data.extend(corner_tri_faces_vec);
  topology.data.extend(edges_flat);
  topology.data.extend(corner_edges_vec);
  topology.data.extend(v2f_offsets);
  topology.data.extend(v2f_indices);

  return true;
}

bool BKE_mesh_gpu_topology_upload(blender::bke::MeshGPUTopology &topology)
{
  if (topology.data.is_empty()) {
    return false;
  }

  if (!GPU_context_active_get()) {
    return false;
  }

  /* Free existing SSBO if present */
  if (topology.ssbo) {
    GPU_storagebuf_free(topology.ssbo);
    topology.ssbo = nullptr;
  }

  /* Create and upload new SSBO */
  topology.ssbo = GPU_storagebuf_create(sizeof(int) * topology.total_size);
  if (!topology.ssbo) {
    return false;
  }

  GPU_storagebuf_update(topology.ssbo, topology.data.data());
  return true;
}

void BKE_mesh_gpu_topology_free(blender::bke::MeshGPUTopology &topology)
{
  if (topology.ssbo) {
    if (GPU_context_active_get()) {
      GPU_storagebuf_free(topology.ssbo);
    }
    /* If no GPU context, the SSBO will be cleaned up by GPU module cleanup */
    topology.ssbo = nullptr;
  }
  topology.data.clear();
  topology.total_size = 0;
}

std::string BKE_mesh_gpu_topology_glsl_accessors_string(
    const blender::bke::MeshGPUTopology &topology)
{
  return fmt::format(R"GLSL(
// Mesh topology accessors (generated)
int face_offsets(int i) {{ return topo[{} + i]; }}
int corner_to_face(int i) {{ return topo[{} + i]; }}
int corner_verts(int i) {{ return topo[{} + i]; }}
int corner_tri(int tri_idx, int vert_idx) {{ return topo[{} + tri_idx * 3 + vert_idx]; }}
int corner_tri_face(int i) {{ return topo[{} + i]; }}
int2 edges(int i) {{ return int2(topo[{} + i * 2], topo[{} + i * 2 + 1]); }}
int corner_edges(int i) {{ return topo[{} + i]; }}
int vert_to_face_offsets(int i) {{ return topo[{} + i]; }}
int vert_to_face(int i) {{ return topo[{} + i]; }}
)GLSL",
                     topology.face_offsets_offset,
                     topology.corner_to_face_offset,
                     topology.corner_verts_offset,
                     topology.corner_tris_offset,
                     topology.corner_tri_faces_offset,
                     topology.edges_offset,
                     topology.edges_offset,
                     topology.corner_edges_offset,
                     topology.vert_to_face_offsets_offset,
                     topology.vert_to_face_offset);
}

void BKE_mesh_gpu_topology_add_specialization_constants(
    blender::gpu::shader::ShaderCreateInfo &info, const blender::bke::MeshGPUTopology &topology)
{
  using namespace blender::gpu::shader;
  info.specialization_constant(Type::int_t, "face_offsets_offset", topology.face_offsets_offset);
  info.specialization_constant(
      Type::int_t, "corner_to_face_offset", topology.corner_to_face_offset);
  info.specialization_constant(Type::int_t, "corner_verts_offset", topology.corner_verts_offset);
  info.specialization_constant(Type::int_t, "corner_tris_offset", topology.corner_tris_offset);
  info.specialization_constant(
      Type::int_t, "corner_tri_faces_offset", topology.corner_tri_faces_offset);
  info.specialization_constant(Type::int_t, "edges_offset", topology.edges_offset);
  info.specialization_constant(Type::int_t, "corner_edges_offset", topology.corner_edges_offset);
  info.specialization_constant(
      Type::int_t, "vert_to_face_offsets_offset", topology.vert_to_face_offsets_offset);
  info.specialization_constant(Type::int_t, "vert_to_face_offset", topology.vert_to_face_offset);
}

blender::gpu::StorageBuf *BKE_mesh_gpu_positions_create_ssbo(const Mesh *mesh)
{
  if (!mesh || mesh->verts_num == 0) {
    return nullptr;
  }

  if (!GPU_context_active_get()) {
    return nullptr;
  }

  const blender::Span<blender::float3> positions = mesh->vert_positions();
  blender::Vector<blender::float4> positions_float4;
  positions_float4.resize(positions.size());

  for (const int i : positions.index_range()) {
    positions_float4[i] = blender::float4(positions[i], 1.0f);
  }

  blender::gpu::StorageBuf *ssbo = GPU_storagebuf_create_ex(positions_float4.size() *
                                                                sizeof(blender::float4),
                                                            positions_float4.data(),
                                                            GPU_USAGE_STATIC,
                                                            __func__);

  return ssbo;
}
