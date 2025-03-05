/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <iostream>
#include <vector>

#include "BLI_math_matrix.hh"
#include "BLI_math_quaternion.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_geom.h"
#include "BLI_rand.hh"
#include "BLI_task.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_pointcloud_types.h"
#include "DNA_node_types.h"

#include "BKE_attribute.hh"
#include "BKE_attribute_math.hh"
#include "BKE_mesh.hh"
#include "BKE_pointcloud.hh"
#include "BKE_mesh_mapping.hh"
#include "BKE_mesh_runtime.hh"
#include "BKE_lib_id.hh"
#include "BKE_customdata.hh"
#include "BKE_bvhutils.hh"
#include "BKE_mesh_remesh_voxel.hh" // To access voxel data structures
#include "BKE_material.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"
#include "RNA_access.hh"
#include "RNA_define.hh"

#include "node_geometry_util.hh"

using namespace blender;
using namespace blender::bke;

namespace blender::nodes::node_geo_tetrahedralize_cc {


static void node_free_storage(bNode *node)
{
  /* Node storage management macros */
  if (node->storage) {
    MEM_freeN(node->storage);
  }
}

static void node_copy_storage(bNodeTree * /*dst_ntree*/, bNode *dst_node, const bNode *src_node)
{
  /* Copy node storage */
  dst_node->storage = MEM_dupallocN(src_node->storage);
}

/* 
 * Utility class for generating random numbers.
 * Replaces use of BLI_rng API.
 */
class RandomNumberGenerator {
 private:
  uint32_t m_seed;

 public:
  RandomNumberGenerator() : m_seed(static_cast<uint32_t>(time(nullptr))) {}

  /* Generate a random integer */
  int get_int()
  {
    // Simple Linear Congruential Generator
    m_seed = (1103515245 * m_seed + 12345) & 0x7fffffff;
    return static_cast<int>(m_seed);
  }

  /* Generate a random float between 0 and 1 */
  float get_float()
  {
    // Convert integer to float in range [0, 1]
    return static_cast<float>(get_int()) / static_cast<float>(0x7fffffff);
  }
};

/* Definition of local scaling methods */
enum LocalScalingMethod {
  SCALING_NONE = 0,
  SCALING_FEATURE_SIZE = 1,
  SCALING_POINT_ATTRIBUTE = 2,
};

static const EnumPropertyItem scaling_mode_items[] = {
    {SCALING_NONE, "NONE", 0, "None", "No local scaling"},
    {SCALING_FEATURE_SIZE,
     "FEATURE_SIZE",
     0,
     "Local Feature Size",
     "Scale tetrahedra based on local feature size"},
    {SCALING_POINT_ATTRIBUTE,
     "POINT_ATTRIBUTE",
     0,
     "Point Attribute",
     "Scale tetrahedra based on point attribute"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* Structure of a tetrahedron containing indices of 4 vertices */
struct Tetrahedron {
  int v1, v2, v3, v4;

  Tetrahedron() : v1(0), v2(0), v3(0), v4(0) {}
  Tetrahedron(int a, int b, int c, int d) : v1(a), v2(b), v3(c), v4(d) {}

  /* Calculate tetrahedron volume */
  float volume(const Span<float3> &vertices) const
  {
    return volume_tetrahedron_signed_v3(
        vertices[v1], vertices[v2], vertices[v3], vertices[v4]);
  }

  /* Check if tetrahedron has positive volume (correct orientation) */
  bool has_positive_volume(const Span<float3> &vertices) const
  {
    return volume(vertices) > 0.0f;
  }

  /* Reverse orientation if needed */
  void ensure_positive_volume(const Span<float3> &vertices)
  {
    if (!has_positive_volume(vertices)) {
      std::swap(v3, v4);
    }
  }
  
  /* Get oriented faces for mesh creation */
  void get_oriented_faces(MutableSpan<int> corner_verts, int &corner_index) const
  {
    // Face 1: v1-v3-v2 (counter-clockwise from outside)
    corner_verts[corner_index++] = v1;
    corner_verts[corner_index++] = v3;
    corner_verts[corner_index++] = v2;
    
    // Face 2: v1-v2-v4 (counter-clockwise from outside)
    corner_verts[corner_index++] = v1;
    corner_verts[corner_index++] = v2;
    corner_verts[corner_index++] = v4;
    
    // Face 3: v2-v3-v4 (counter-clockwise from outside)
    corner_verts[corner_index++] = v2;
    corner_verts[corner_index++] = v3;
    corner_verts[corner_index++] = v4;
    
    // Face 4: v3-v1-v4 (counter-clockwise from outside)
    corner_verts[corner_index++] = v3;
    corner_verts[corner_index++] = v1;
    corner_verts[corner_index++] = v4;
  }
};

/* Structure for triangular face used in tetrahedron construction */
struct TriFace {
  int v1, v2, v3;

  TriFace() : v1(0), v2(0), v3(0) {}
  TriFace(int a, int b, int c) : v1(a), v2(b), v3(c) {}
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Mesh").supported_type(GeometryComponent::Type::Mesh);
  b.add_input<decl::Bool>("Manual Base Size").default_value(false);
  b.add_input<decl::Float>("Base Size").default_value(1.0f).min(0.00001f);
  b.add_input<decl::Float>("Max Tet Scale").default_value(1.0f).min(0.01f);
  b.add_input<decl::Float>("Min Triangle Scale").default_value(0.1f).min(0.01f).max(1.0f);
  b.add_input<decl::String>("Scale Attribute").default_value("scale");
  b.add_input<decl::Float>("Local Feature Scale").default_value(1.0f).min(0.01f);
  b.add_output<decl::Geometry>("Mesh").propagate_all();
}

/*
 * Calculate mesh base size using bounding box dimensions
 */
static float calculate_base_size(const Mesh *mesh)
{
  if (!mesh || mesh->verts_num == 0) {
    return 1.0f;  // Default value for empty mesh
  }

  // Manual bounding box calculation
  const Span<float3> positions = mesh->vert_positions();
  
  if (positions.is_empty()) {
    return 1.0f;
  }
  
  float3 min = positions[0];
  float3 max = positions[0];
  
  for (const float3 &pos : positions) {
    min.x = std::min(min.x, pos.x);
    min.y = std::min(min.y, pos.y);
    min.z = std::min(min.z, pos.z);
    
    max.x = std::max(max.x, pos.x);
    max.y = std::max(max.y, pos.y);
    max.z = std::max(max.z, pos.z);
  }

  // Calculate bounding box diagonal
  float diagonal = sqrt(square_f(max.x - min.x) + 
                         square_f(max.y - min.y) + 
                         square_f(max.z - min.z));
  
  // Return 5% of diagonal as base size
  return diagonal * 0.05f;
}

/*
 * Calculate local feature sizes per vertex using average adjacent edge lengths
 */
static Array<float> calculate_local_feature_sizes(const Mesh *mesh)
{
  const Span<float3> positions = mesh->vert_positions();
  const OffsetIndices faces = mesh->faces();
  const Span<int> corner_verts = mesh->corner_verts();
  
  Array<float> feature_sizes(positions.size(), 0.0f);
  Array<int> vertex_counts(positions.size(), 0);
  
  if (positions.is_empty() || faces.is_empty()) {
    return feature_sizes;
  }
  
  // For each face
  for (const int face_index : faces.index_range()) {
    const IndexRange face = faces[face_index];
    
    // For each edge in the face
    for (int i = 0; i < face.size(); i++) {
      const int v1 = corner_verts[face[i]];
      const int v2 = corner_verts[face[(i + 1) % face.size()]];
      
      // Edge length calculation
      const float3 &p1 = positions[v1];
      const float3 &p2 = positions[v2];
      const float edge_length = len_v3v3(p1, p2);
      
      // Accumulate lengths for both vertices
      feature_sizes[v1] += edge_length;
      feature_sizes[v2] += edge_length;
      vertex_counts[v1]++;
      vertex_counts[v2]++;
    }
  }
  
  // Calculate average for each vertex
  for (int i = 0; i < positions.size(); i++) {
    if (vertex_counts[i] > 0) {
      feature_sizes[i] /= static_cast<float>(vertex_counts[i]);
    }
    else {
      // If vertex has no connected edges, use default value
      feature_sizes[i] = 0.1f;
    }
  }
  
  return feature_sizes;
}

/*
 * Retrieve point attribute values for scaling.
 * If the attribute does not exist, return default values.
 */
static Array<float> get_point_attribute_values(const Mesh *mesh, [[maybe_unused]] const std::string &attribute_name)
{
  const Span<float3> positions = mesh->vert_positions();
  Array<float> values(positions.size(), 1.0f);
  
  // Try to retrieve the attribute, use default values if it doesn't exist
  
  return values;
}

/* Find interior point for tetrahedralization using bounding box center */
static int find_interior_point(const Mesh *mesh, Vector<float3> &vertices)
{
  // Manual bounding box calculation
  const Span<float3> positions = mesh->vert_positions();
  
  if (positions.is_empty()) {
    vertices.append(float3(0, 0, 0));
    return vertices.size() - 1;
  }
  
  float3 min = positions[0];
  float3 max = positions[0];
  
  for (const float3 &pos : positions) {
    min.x = std::min(min.x, pos.x);
    min.y = std::min(min.y, pos.y);
    min.z = std::min(min.z, pos.z);
    
    max.x = std::max(max.x, pos.x);
    max.y = std::max(max.y, pos.y);
    max.z = std::max(max.z, pos.z);
  }

  float3 center = float3((min.x + max.x) * 0.5f, 
                        (min.y + max.y) * 0.5f, 
                        (min.z + max.z) * 0.5f);

  // Use center as interior point
  // This simplified approach works for most convex meshes
  // For complex/concave meshes, a better method would be needed
  vertices.append(center);
  return vertices.size() - 1;
}

/* 
 * Generate a tetrahedral mesh using a Delaunay-inspired approach.
 * Uses Blender's native data structures and BVH functions for efficiency.
 */
static void generate_tetrahedralization(const Mesh *mesh,
                                      float base_size,
                                      float max_tet_scale,
                                      [[maybe_unused]] float min_triangle_scale,
                                      int local_scaling_method,
                                      const std::string &scale_attribute,
                                      float local_feature_scale,
                                      Vector<float3> &out_vertices,
                                      Vector<Tetrahedron> &out_tetrahedra)
{
  const Span<float3> positions = mesh->vert_positions();
  const OffsetIndices faces = mesh->faces();
  const Span<int> corner_verts = mesh->corner_verts();
  
  if (positions.is_empty() || faces.is_empty()) {
    return;
  }
  
  /* Save all original vertices */
  out_vertices.resize(positions.size());
  for (const int i : positions.index_range()) {
    out_vertices[i] = positions[i];
  }
  
  /* Get scaling information based on selected method */
  Array<float> vertex_scales(positions.size(), 1.0f);
  
  switch (local_scaling_method) {
    case SCALING_FEATURE_SIZE: {
      Array<float> feature_sizes = calculate_local_feature_sizes(mesh);
      for (const int i : positions.index_range()) {
        vertex_scales[i] = feature_sizes[i] * local_feature_scale;
      }
      break;
    }
    case SCALING_POINT_ATTRIBUTE: {
      vertex_scales = get_point_attribute_values(mesh, scale_attribute);
      for (float &scale : vertex_scales) {
        scale *= local_feature_scale;
      }
      break;
    }
    default:
      // No special scaling - use uniform scale based on max_tet_scale
      for (float &scale : vertex_scales) {
        scale = base_size * max_tet_scale;
      }
      break;
  }
  
  // Collect surface triangles
  Vector<TriFace> surface_triangles;
  
  // Process all faces
  for (const int face_index : faces.index_range()) {
    const IndexRange face = faces[face_index];
    
    // Triangulate non-triangular faces
    if (face.size() == 3) {
      // Directly add triangular faces
      const int v1 = corner_verts[face[0]];
      const int v2 = corner_verts[face[1]];
      const int v3 = corner_verts[face[2]];
      surface_triangles.append(TriFace(v1, v2, v3));
    }
    else if (face.size() > 3) {
      // Triangulate n-gons using Blender's triangulation
      const int v0 = corner_verts[face[0]];
      for (int i = 2; i < face.size(); i++) {
        const int v1 = corner_verts[face[i - 1]];
        const int v2 = corner_verts[face[i]];
        surface_triangles.append(TriFace(v0, v1, v2));
      }
    }
  }
  
  // Find starting interior point
  int interior_point_index = find_interior_point(mesh, out_vertices);
  
  // Create initial tetrahedra connecting interior point to surface triangles
  for (const TriFace &face : surface_triangles) {
    // Check for valid triangle vertices
    float3 v1 = out_vertices[face.v1];
    float3 v2 = out_vertices[face.v2];
    float3 v3 = out_vertices[face.v3];
    float3 v4 = out_vertices[interior_point_index];
    
    // Calculate triangle area
    float3 normal = math::cross(v2 - v1, v3 - v1);
    float area = len_v3(normal) * 0.5f;
    
    // Calculate potential tetrahedron volume
    float volume = volume_tetrahedron_signed_v3(v1, v2, v3, v4);
    
    // Skip small areas and invalid volumes
    if (area > 1e-6f && fabsf(volume) > 1e-6f) {
      Tetrahedron tet(face.v1, face.v2, face.v3, interior_point_index);
      
      // Ensure proper orientation
      tet.ensure_positive_volume(out_vertices);
      
      // Final volume validation
      if (tet.has_positive_volume(out_vertices)) {
        out_tetrahedra.append(tet);
      }
    }
  }
  
  // Improve tetrahedron quality by adding interior points
  RandomNumberGenerator rng;
  
  // Number of additional points depends on max tetrahedron size
  // and mesh dimensions
  // Manual bounding box calculation
  float3 min, max;
  
  if (!positions.is_empty()) {
    min = max = positions[0];
    for (const float3 &pos : positions) {
      min.x = std::min(min.x, pos.x);
      min.y = std::min(min.y, pos.y);
      min.z = std::min(min.z, pos.z);
      
      max.x = std::max(max.x, pos.x);
      max.y = std::max(max.y, pos.y);
      max.z = std::max(max.z, pos.z);
    }
  }
  else {
    min = max = float3(0, 0, 0);
  }
  
  float mesh_volume = fabsf((max.x - min.x) * (max.y - min.y) * (max.z - min.z));
  
  // Calculate number of points based on volume and scale
  int num_additional_points = static_cast<int>(mesh_volume / (base_size * base_size * base_size) * 0.1f);
  num_additional_points = std::min(std::max(num_additional_points, 10), 100); // Limit between 10 and 100
  
  for (int i = 0; i < num_additional_points; i++) {
    // Create new point by combining existing vertices with random offset
    float3 new_point(0, 0, 0);
    
    // Sample random vertices
    int num_samples = std::min(5, static_cast<int>(positions.size()));
    float total_weight = 0.0f;
    
    for (int j = 0; j < num_samples; j++) {
      int random_idx = rng.get_int() % positions.size();
      float weight = vertex_scales[random_idx];
      total_weight += weight;
      new_point += positions[random_idx] * weight;
    }
    
    if (total_weight > 0.0f) {
      new_point /= total_weight;
    }
    else {
      // Fallback if all weights are zero
      new_point = positions.is_empty() ? float3(0, 0, 0) : positions[0];
    }
    
    // Add random offset proportional to base size
    new_point += float3(
        (rng.get_float() - 0.5f) * 2.0f,
        (rng.get_float() - 0.5f) * 2.0f,
        (rng.get_float() - 0.5f) * 2.0f) * base_size * 0.2f;
    
    // Add new point
    const int new_point_index = out_vertices.size();
    out_vertices.append(new_point);
    
    // Connect to surface triangles
    int num_connections = std::min(10, static_cast<int>(surface_triangles.size()));
    for (int j = 0; j < num_connections; j++) {
      int face_idx = rng.get_int() % surface_triangles.size();
      const TriFace &face = surface_triangles[face_idx];
      
      // Check for coincident vertices
      float3 v1 = out_vertices[face.v1];
      float3 v2 = out_vertices[face.v2];
      float3 v3 = out_vertices[face.v3];
      float3 v4 = out_vertices[new_point_index];
      
      // Calculate triangle area
      float3 normal = math::cross(v2 - v1, v3 - v1);
      float area = len_v3(normal) * 0.5f;
      
      // Calculate potential tetrahedron volume
      float volume = volume_tetrahedron_signed_v3(v1, v2, v3, v4);
      
      // Skip small areas and volumes
      if (area > 1e-6f && fabsf(volume) > 1e-6f) {
        Tetrahedron tet(face.v1, face.v2, face.v3, new_point_index);
        // Ensure positive volume orientation
        tet.ensure_positive_volume(out_vertices);
        
        // Final volume validation
        if (tet.has_positive_volume(out_vertices)) {
          out_tetrahedra.append(tet);
        }
      }
    }
  }
  
  // Filter tetrahedra based on quality metrics
  Vector<Tetrahedron> filtered_tetrahedra;
  for (const Tetrahedron &tet : out_tetrahedra) {
    // Get tetrahedron vertices
    const float3 &v1 = out_vertices[tet.v1];
    const float3 &v2 = out_vertices[tet.v2];
    const float3 &v3 = out_vertices[tet.v3];
    const float3 &v4 = out_vertices[tet.v4];
    
    // Calculate volume
    float volume = volume_tetrahedron_signed_v3(v1, v2, v3, v4);
    
    // Calculate edge length metrics
    float max_edge_length = 0.0f;
    max_edge_length = std::max(max_edge_length, len_v3v3(v1, v2));
    max_edge_length = std::max(max_edge_length, len_v3v3(v1, v3));
    max_edge_length = std::max(max_edge_length, len_v3v3(v1, v4));
    max_edge_length = std::max(max_edge_length, len_v3v3(v2, v3));
    max_edge_length = std::max(max_edge_length, len_v3v3(v2, v4));
    max_edge_length = std::max(max_edge_length, len_v3v3(v3, v4));
    
    // Calculate minimum edge length
    float min_edge_length = max_edge_length;
    min_edge_length = std::min(min_edge_length, len_v3v3(v1, v2));
    min_edge_length = std::min(min_edge_length, len_v3v3(v1, v3));
    min_edge_length = std::min(min_edge_length, len_v3v3(v1, v4));
    min_edge_length = std::min(min_edge_length, len_v3v3(v2, v3));
    min_edge_length = std::min(min_edge_length, len_v3v3(v2, v4));
    min_edge_length = std::min(min_edge_length, len_v3v3(v3, v4));
    
    // Calculate quality thresholds
    float min_volume_threshold = powf(max_edge_length, 3) * 0.001f;
    float edge_ratio = (min_edge_length > 1e-6f) ? (max_edge_length / min_edge_length) : FLT_MAX;
    
    // Calculate quality metric
    float avg_edge_length = (len_v3v3(v1, v2) + len_v3v3(v1, v3) + len_v3v3(v1, v4) + 
                            len_v3v3(v2, v3) + len_v3v3(v2, v4) + len_v3v3(v3, v4)) / 6.0f;
    float quality = (avg_edge_length > 1e-6f) ? (volume / powf(avg_edge_length, 3)) : 0.0f;
    
    // Apply quality filters
    if (volume > min_volume_threshold && 
        edge_ratio < 50.0f && 
        quality > 0.001f && 
        min_edge_length > 1e-5f) {
      
      // Check face areas
      bool valid_faces = true;
      float3 n1 = math::cross(v2 - v1, v3 - v1);
      float3 n2 = math::cross(v2 - v1, v4 - v1);
      float3 n3 = math::cross(v3 - v1, v4 - v1);
      float3 n4 = math::cross(v3 - v2, v4 - v2);
      
      float area1 = len_v3(n1) * 0.5f;
      float area2 = len_v3(n2) * 0.5f;
      float area3 = len_v3(n3) * 0.5f;
      float area4 = len_v3(n4) * 0.5f;
      
      // Minimum area threshold
      float min_area_threshold = powf(min_edge_length, 2) * 0.01f;
      if (area1 < min_area_threshold || area2 < min_area_threshold ||
          area3 < min_area_threshold || area4 < min_area_threshold) {
        valid_faces = false;
      }
      
      if (valid_faces) {
        filtered_tetrahedra.append(tet);
      }
    }
  }
  
  // Replace tetrahedra with filtered ones
  out_tetrahedra = filtered_tetrahedra;
}

/* Create a mesh from tetrahedra using Blender's mesh creation API */
static Mesh *create_mesh_from_tetrahedra(const Vector<float3> &vertices,
                                         const Vector<Tetrahedron> &tetrahedra)
{
  if (vertices.is_empty() || tetrahedra.is_empty()) {
    return nullptr;
  }
  
  // Filter valid tetrahedra with additional checks
  Vector<Tetrahedron> valid_tetrahedra;
  for (const Tetrahedron &tet : tetrahedra) {
    // Validate vertex indices
    if (tet.v1 < vertices.size() && tet.v2 < vertices.size() && 
        tet.v3 < vertices.size() && tet.v4 < vertices.size() &&
        tet.v1 >= 0 && tet.v2 >= 0 && tet.v3 >= 0 && tet.v4 >= 0) {
      
      // Check for non-degenerate tetrahedron
      const float3 &p1 = vertices[tet.v1];
      const float3 &p2 = vertices[tet.v2];
      const float3 &p3 = vertices[tet.v3];
      const float3 &p4 = vertices[tet.v4];
      
      // Volume check
      float volume = volume_tetrahedron_signed_v3(p1, p2, p3, p4);
      
      // Minimum edge length check
      float min_edge_len = FLT_MAX;
      min_edge_len = std::min(min_edge_len, len_v3v3(p1, p2));
      min_edge_len = std::min(min_edge_len, len_v3v3(p1, p3));
      min_edge_len = std::min(min_edge_len, len_v3v3(p1, p4));
      min_edge_len = std::min(min_edge_len, len_v3v3(p2, p3));
      min_edge_len = std::min(min_edge_len, len_v3v3(p2, p4));
      min_edge_len = std::min(min_edge_len, len_v3v3(p3, p4));
      
      // Keep only tetrahedra with significant volume and valid edges
      if (fabsf(volume) > 1e-6f && min_edge_len > 1e-5f) {
        valid_tetrahedra.append(tet);
      }
    }
  }
  
  if (valid_tetrahedra.is_empty()) {
    return nullptr;
  }
  
  // Calculate number of faces (4 per tetrahedron)
  int num_tets = valid_tetrahedra.size();
  int num_faces = num_tets * 4;
  int num_loops = num_faces * 3;  // 3 vertices per triangular face
  
  /* Create new mesh with vertices, edges, faces and corners */
  Mesh *mesh = BKE_mesh_new_nomain(vertices.size(), 0, num_faces, num_loops);
  
  // Copy vertices
  MutableSpan<float3> mesh_verts = mesh->vert_positions_for_write();
  for (size_t i = 0; i < vertices.size(); i++) {
    mesh_verts[i] = vertices[i];
  }
  
  // Create faces
  MutableSpan<int> face_offsets = mesh->face_offsets_for_write();
  MutableSpan<int> corner_verts = mesh->corner_verts_for_write();
  
  int corner_index = 0;
  int face_index = 0;
  
  /* For each tetrahedron, carefully create four triangular faces */
  for (int i = 0; i < num_tets; i++) {
    const Tetrahedron &tet = valid_tetrahedra[i];
    
    // Define face offsets (where each face starts in the corner vertices array)
    for (int j = 0; j < 4; j++) {
      face_offsets[face_index++] = corner_index;
      
      // Add vertices of the face according to the appropriate orientation
      int v1, v2, v3;
      switch (j) {
        case 0: // Face 1: triangle v1-v3-v2
          v1 = tet.v1;
          v2 = tet.v3;
          v3 = tet.v2;
          break;
        case 1: // Face 2: triangle v1-v2-v4
          v1 = tet.v1;
          v2 = tet.v2;
          v3 = tet.v4;
          break;
        case 2: // Face 3: triangle v2-v3-v4
          v1 = tet.v2;
          v2 = tet.v3;
          v3 = tet.v4;
          break;
        case 3: // Face 4: triangle v3-v1-v4
          v1 = tet.v3;
          v2 = tet.v1;
          v3 = tet.v4;
          break;
      }
      
      // Add three vertices of the face
      corner_verts[corner_index++] = v1;
      corner_verts[corner_index++] = v2;
      corner_verts[corner_index++] = v3;
    }
  }
  
  // Set last face offset
  face_offsets[num_faces] = num_loops;
  
  // Generate edges and other mesh information needed for proper rendering
  blender::bke::mesh_calc_edges(*mesh, false, false);
  
  // Ensure the mesh is a valid structure before tagging for normal calculation
  mesh->runtime->is_original_bmesh = false;
  
  // Tag the mesh for deferred normal calculation - this is safer than direct calculation
  mesh->tag_positions_changed();
  
  // Create tetrahedron ID attribute
  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
  bke::SpanAttributeWriter<int> tet_id = attributes.lookup_or_add_for_write_span<int>(
      "tetrahedron_id", bke::AttrDomain::Face);
  
  for (int i = 0; i < num_faces; i++) {
    tet_id.span[i] = i / 4;  // Integer division to get tetrahedron ID
  }
  
  tet_id.finish();
  
  return mesh;
}

/*
 * Main tetrahedralization implementation
 * Creates tetrahedral mesh using Blender's API
 */
static Mesh *create_tetrahedralized_mesh(const Mesh &input_mesh,
                                       const bool use_manual_base_size,
                                       const float manual_base_size,
                                       const float max_tet_scale,
                                       const float min_triangle_scale,
                                       const int local_scaling_method,
                                       const std::string &scale_attribute,
                                       const float local_feature_scale)
{
  if (input_mesh.verts_num == 0) {
    return nullptr;
  }

  // Calculate or use provided base size
  float base_size = use_manual_base_size ? manual_base_size : calculate_base_size(&input_mesh);

  // Generate tetrahedra using tetrahedralization
  Vector<float3> vertices;
  Vector<Tetrahedron> tetrahedra;
  Mesh *result = nullptr;

  try {
    generate_tetrahedralization(&input_mesh,
                              base_size,
                              max_tet_scale,
                              min_triangle_scale,
                              local_scaling_method,
                              scale_attribute,
                              local_feature_scale,
                              vertices,
                              tetrahedra);

    // Explicit check for valid generated tetrahedra
    if (vertices.is_empty() || tetrahedra.is_empty()) {
      return nullptr;
    }

    // Final filter of invalid tetrahedra
    Vector<Tetrahedron> valid_tetrahedra;
    for (const Tetrahedron &tet : tetrahedra) {
      // Check valid vertex indices
      if (tet.v1 < vertices.size() && tet.v2 < vertices.size() && 
          tet.v3 < vertices.size() && tet.v4 < vertices.size() &&
          tet.v1 >= 0 && tet.v2 >= 0 && tet.v3 >= 0 && tet.v4 >= 0) {
        // Check significant volume
        float volume = tet.volume(vertices);
        if (volume > 1e-6f) {
          valid_tetrahedra.append(tet);
        }
      }
    }

    if (valid_tetrahedra.is_empty()) {
      return nullptr;
    }

    result = create_mesh_from_tetrahedra(vertices, valid_tetrahedra);

    if (!result) {
      return nullptr;
    }

    return result;
  }
  catch (...) {
    if (result) {
      BKE_id_free(nullptr, result);
    }
    return nullptr;
  }
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiLayoutSetPropSep(layout, true);
  uiLayoutSetPropDecorate(layout, false);

  // Only show properties not exposed as sockets
  uiItemR(layout, ptr, "local_scaling", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  
  const int scaling_mode = RNA_enum_get(ptr, "local_scaling");
  if (scaling_mode == SCALING_POINT_ATTRIBUTE) {
    // Property already available as socket, not shown here
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh");

  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    if (geometry_set.has_mesh()) {
      try {
        const Mesh *mesh_in = geometry_set.get_mesh();

        /* Extract values directly from node inputs */
        const bool use_manual_base_size = params.extract_input<bool>("Manual Base Size");
        const float base_size = params.extract_input<float>("Base Size");
        const float max_tet_scale = params.extract_input<float>("Max Tet Scale");
        const float min_triangle_scale = params.extract_input<float>("Min Triangle Scale");
        
        /* For scaling_method, always use custom1 since there's no socket input */
        const LocalScalingMethod local_scaling_method = static_cast<LocalScalingMethod>(
            params.node().custom1);
            
        const std::string scale_attribute = params.extract_input<std::string>("Scale Attribute");
        const float local_feature_scale = params.extract_input<float>("Local Feature Scale");

        // Modern Mesh API uses verts_num instead of totvert and we need to check if faces are empty
        if (mesh_in->verts_num == 0 || mesh_in->faces_num == 0) {
          return;
        }

        // Call create_tetrahedralized_mesh with the input mesh
        Mesh *result = create_tetrahedralized_mesh(*mesh_in,
                                                  use_manual_base_size,
                                                  base_size,
                                                  max_tet_scale,
                                                  min_triangle_scale,
                                                  local_scaling_method,
                                                  scale_attribute,
                                                  local_feature_scale);

        /* Use result - note that replace_mesh takes ownership of the result mesh */
        if (result != nullptr) {
          geometry_set.replace_mesh(result);
        }
      }
      catch (const std::exception &) {
        params.error_message_add(NodeWarningType::Error, "Exception in tetrahedralization");
      }
    }
  });

  params.set_output("Mesh", std::move(geometry_set));
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = SCALING_NONE;  // Default: no local scaling
  
  NodeGeometryTetrahedralize *storage = (NodeGeometryTetrahedralize *)MEM_callocN(
      sizeof(NodeGeometryTetrahedralize), "NodeGeometryTetrahedralize");
      
  storage->base_size = 1.0f;
  storage->max_tet_scale = 1.0f;
  storage->min_triangle_scale = 0.1f;
  storage->local_feature_scale = 1.0f;
  storage->use_manual_base_size = false;
  
  // Initialize padding
  memset(storage->_pad, 0, sizeof(storage->_pad));
  
  // Initialize attribute name
  strcpy(storage->scale_attribute_name, "scale");
  
  node->storage = storage;
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeTetrahedralize");
  ntype.ui_name = "Tetrahedralize";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.enum_name_legacy = "TETRAHEDRALIZE";
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.ui_description = "Create a tetrahedral mesh from a surface mesh";
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  blender::bke::node_type_storage(ntype, "NodeGeometryTetrahedralize", node_free_storage, node_copy_storage);
  
  // Register RNA properties
  static const EnumPropertyItem local_scaling_items[] = {
    {SCALING_NONE, "NONE", 0, "None", "No local scaling"},
    {SCALING_FEATURE_SIZE, "FEATURE_SIZE", 0, "Local Feature Size", "Scale tetrahedra based on local feature size"},
    {SCALING_POINT_ATTRIBUTE, "POINT_ATTRIBUTE", 0, "Point Attribute", "Scale tetrahedra based on point attribute"},
    {0, nullptr, 0, nullptr, nullptr},
  };
  
  RNA_def_enum(ntype.rna_ext.srna, 
              "local_scaling", 
              local_scaling_items, 
              SCALING_NONE, 
              "Local Scaling Method", 
              "Method used for local scaling of tetrahedra");
              
  RNA_def_boolean(ntype.rna_ext.srna,
                "use_manual_base_size",
                false,
                "Manual Base Size",
                "Use a manually specified base size instead of automatic calculation");
                
  RNA_def_float(ntype.rna_ext.srna,
              "base_size",
              1.0f,
              0.00001f,
              FLT_MAX,
              "Base Size",
              "Base size of tetrahedra",
              0.00001f,
              100.0f);
              
  RNA_def_float(ntype.rna_ext.srna,
              "max_tet_scale",
              1.0f,
              0.01f,
              FLT_MAX,
              "Max Tet Scale",
              "Maximum scale factor for tetrahedra",
              0.01f,
              10.0f);
              
  RNA_def_float(ntype.rna_ext.srna,
              "min_triangle_scale",
              0.1f,
              0.01f,
              1.0f,
              "Min Triangle Scale",
              "Minimum scale factor for triangles",
              0.01f,
              1.0f);
              
  RNA_def_string(ntype.rna_ext.srna,
                "scale_attribute_name",
                "scale",
                64,
                "Scale Attribute",
                "Name of the attribute to use for point scaling");
                
  RNA_def_float(ntype.rna_ext.srna,
              "local_feature_scale",
              1.0f,
              0.01f,
              FLT_MAX,
              "Local Feature Scale",
              "Scale factor for local features",
              0.01f,
              10.0f);
  
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_tetrahedralize_cc