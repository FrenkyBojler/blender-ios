/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "GPU_storage_buffer.hh"

#include "../gpu/intern/gpu_shader_create_info.hh"

struct Mesh;

/**
 * Mesh GPU topology data for compute shaders.
 * Contains packed mesh topology data with computed offsets for efficient GPU access.
 */
namespace blender::bke {

struct MeshGPUTopology {
  /* Packed topology data arrays with their offsets */
  int face_offsets_offset = 0;
  int corner_to_face_offset = 0;
  int corner_verts_offset = 0;
  int corner_tris_offset = 0;
  int corner_tri_faces_offset = 0;
  int edges_offset = 0;
  int corner_edges_offset = 0;
  int vert_to_face_offsets_offset = 0;
  int vert_to_face_offset = 0;

  /* Total size of packed data */
  int total_size = 0;

  /* Packed data vector */
  blender::Vector<int> data;

  /* GPU storage buffer (null if not uploaded) */
  blender::gpu::StorageBuf *ssbo = nullptr;

  /* Constructor */
  MeshGPUTopology() = default;
};

}  // namespace blender::bke

// Type alias for the old MeshGPUTopology type for compatibility
using MeshGPUTopology = blender::bke::MeshGPUTopology;

/**
 * Build mesh topology data for GPU compute shaders.
 * Packs face offsets, corner-to-face mapping, corner vertices, corner triangles,
 * triangle-to-face mapping, edges, corner edges, vertex-to-face offsets and indices into a single
 * buffer.
 *
 * \param mesh: Source mesh data
 * \param topology: Output topology structure with computed offsets and data
 * \return true on success, false on failure
 */
bool BKE_mesh_gpu_topology_create(const Mesh *mesh, blender::bke::MeshGPUTopology &topology);

/**
 * Upload mesh topology data to GPU storage buffer.
 * Creates or updates the SSBO with the packed topology data.
 *
 * \param topology: Topology data to upload
 * \return true on success, false on failure (e.g., no GPU context)
 */
bool BKE_mesh_gpu_topology_upload(blender::bke::MeshGPUTopology &topology);

/**
 * Free GPU resources associated with topology data.
 * Safe to call multiple times or without GPU context.
 *
 * \param topology: Topology data to free
 */
void BKE_mesh_gpu_topology_free(blender::bke::MeshGPUTopology &topology);

/**
 * Create a GPU storage buffer (SSBO) from the vertex positions of a mesh.
 * The positions are packed as float4 for alignment.
 *
 * \param mesh: The mesh to get vertex positions from.
 * \return A new GPUStorageBuf* on success, or nullptr on failure. The caller is responsible for
 * freeing the buffer with GPU_storagebuf_free().
 */
blender::gpu::StorageBuf *BKE_mesh_gpu_positions_create_ssbo(const Mesh *mesh);

/**
 * Get accessor functions for GLSL shader integration.
 * Returns strings containing GLSL functions to access topology data by offset.
 *
 * \param topology: Topology data with computed offsets
 * \return GLSL accessor functions as string
 */
std::string BKE_mesh_gpu_topology_glsl_accessors_string(
    const blender::bke::MeshGPUTopology &topology);

/**
 * Add all topology offsets from a MeshGPUTopology struct as specialization
 * constants to a shader create info object.
 *
 * This automates the process of keeping the shader constants in sync with the
 * struct definition.
 *
 * \param info: The shader create info to add constants to.
 * \param topology: The topology data containing the offsets.
 */
void BKE_mesh_gpu_topology_add_specialization_constants(
    blender::gpu::shader::ShaderCreateInfo &info, const blender::bke::MeshGPUTopology &topology);
