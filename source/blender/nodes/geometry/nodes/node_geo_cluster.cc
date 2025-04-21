/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_mesh.hh"
#include "BKE_attribute_math.hh"
#include "BKE_attribute.hh"
#include "BKE_mesh_runtime.hh"  /* For BKE_mesh_normals_tag_dirty */
#include "BKE_mesh.h"  /* For BKE_mesh_calc_edges */

#include "BLI_math_vector_types.hh"
#include "BLI_math_vector.h"
#include "BLI_rand.h"
#include "BLI_kdtree.h"
#include <queue>
#include <float.h>
#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_cluster_cc {

/* Structure to store triangle and cluster information */
struct NodeGeometryCluster {
  int faces_per_cluster;
  int total_faces;
  int cluster_count;
  bool random_colors;
  float compactness;
};

/* Structure to represent an axis-aligned bounding box (AABB) */
struct AABB {
  float3 min;
  float3 max;
  
  AABB() : min(FLT_MAX, FLT_MAX, FLT_MAX), max(-FLT_MAX, -FLT_MAX, -FLT_MAX) {}
  
  /* Expand the box to include a point */
  void expand(const float3 &point) {
    min.x = std::min(min.x, point.x);
    min.y = std::min(min.y, point.y);
    min.z = std::min(min.z, point.z);
    max.x = std::max(max.x, point.x);
    max.y = std::max(max.y, point.y);
    max.z = std::max(max.z, point.z);
  }
  
  /* Calculate the volume of the box */
  float volume() const {
    return (max.x - min.x) * (max.y - min.y) * (max.z - min.z);
  }
  
  /* Calculate the sum of dimensions (used as heuristic) */
  float surface_area() const {
    float3 dimensions = max - min;
    return 2.0f * (dimensions.x * dimensions.y + dimensions.x * dimensions.z + dimensions.y * dimensions.z);
  }
  
  /* Calculate the center of the box */
  float3 center() const {
    return (min + max) * 0.5f;
  }
  
  /* Merge two boxes */
  AABB merge(const AABB &other) const {
    AABB result;
    result.min.x = std::min(min.x, other.min.x);
    result.min.y = std::min(min.y, other.min.y);
    result.min.z = std::min(min.z, other.min.z);
    result.max.x = std::max(max.x, other.max.x);
    result.max.y = std::max(max.y, other.max.y);
    result.max.z = std::max(max.z, other.max.z);
    return result;
  }
};

/* Structure to represent a triangle/face with its cluster and AABB */
struct FaceInfo {
  int index;           // Face index
  int cluster_id;      // Assigned cluster ID
  AABB aabb;           // Bounding box of the face
  float3 centroid;     // Center of the face
  Vector<int> adjacent_faces; // Adjacent faces
  
  FaceInfo(int idx) : index(idx), cluster_id(-1) {}
};

/* Structure to store information about a cluster during construction */
struct ClusterInfo {
  int id;               // Cluster ID
  float3 centroid;      // Center of mass of the cluster
  Vector<int> faces;    // Faces belonging to the cluster
  AABB aabb;            // Bounding box of the cluster
  
  ClusterInfo(int cluster_id) : id(cluster_id), centroid(0, 0, 0) {}
  
  /* Update the centroid based on the faces in the cluster */
  void update_centroid(const Vector<FaceInfo> &face_infos) {
    if (faces.is_empty()) {
      return;
    }
    
    centroid = float3(0, 0, 0);
    for (int face_idx : faces) {
      centroid += face_infos[face_idx].centroid;
    }
    centroid /= float(faces.size());
    
    /* Update the AABB */
    aabb = AABB();
    for (int face_idx : faces) {
      aabb = aabb.merge(face_infos[face_idx].aabb);
    }
  }
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Mesh").supported_type(GeometryComponent::Type::Mesh);
  b.add_input<decl::Int>("Max Faces Per Cluster").default_value(64).min(1).max(50000);
  b.add_input<decl::Float>("Compactness").default_value(0.8f).min(0.0f).max(1.0f).subtype(PROP_FACTOR);
  b.add_input<decl::Bool>("Random Colors").default_value(true);
  b.add_input<decl::Bool>("Build Bounding Data").default_value(false);
  b.add_output<decl::Geometry>("Mesh").propagate_all();
  b.add_output<decl::Geometry>("Bounds of cluster");
  b.add_output<decl::Color>("Cluster Colors").field_source_reference_all();
  b.add_output<decl::Int>("Cluster ID").field_source_reference_all();
}

/* Function that builds the triangle-triangle connectivity */
static void build_face_adjacency(
    const Span<int> corner_verts,
    const OffsetIndices<int> faces,
    Vector<FaceInfo> &face_infos)
{
  /* Build a vertex->faces mapping */
  int total_verts = 0;
  for (const int face_idx : faces.index_range()) {
    const Span<int> face_verts = corner_verts.slice(faces[face_idx]);
    for (const int vert : face_verts) {
      if (vert > total_verts) {
        total_verts = vert;
      }
    }
  }
  
  Vector<Vector<int>> vert_to_faces(total_verts + 1);
  
  /* Fill the vertex->faces mapping */
  for (const int face_idx : faces.index_range()) {
    const Span<int> face_verts = corner_verts.slice(faces[face_idx]);
    for (const int vert : face_verts) {
      vert_to_faces[vert].append(face_idx);
    }
  }
  
  /* Find adjacent faces (sharing at least one vertex) */
  for (const int face_idx : faces.index_range()) {
    const Span<int> face_verts = corner_verts.slice(faces[face_idx]);
    
    /* Collect all potentially adjacent faces */
    Set<int> potential_adjacent;
    for (const int vert : face_verts) {
      for (const int adj_face : vert_to_faces[vert]) {
        if (adj_face != face_idx) {
          potential_adjacent.add(adj_face);
        }
      }
    }
    
    /* Check which faces share an edge (2 vertices in common) */
    for (const int adj_face : potential_adjacent) {
      const Span<int> adj_face_verts = corner_verts.slice(faces[adj_face]);
      
      int shared_verts = 0;
      for (const int vert1 : face_verts) {
        for (const int vert2 : adj_face_verts) {
          if (vert1 == vert2) {
            shared_verts++;
            break;
          }
        }
      }
      
      /* If the faces share at least 2 vertices (an edge), they are adjacent */
      if (shared_verts >= 2) {
        face_infos[face_idx].adjacent_faces.append(adj_face);
      }
    }
  }
}

/* Function that calculates AABBs and centroids of faces */
static void calculate_face_geometries(
    const Span<float3> positions,
    const Span<int> corner_verts,
    const OffsetIndices<int> faces,
    Vector<FaceInfo> &face_infos)
{
  for (const int face_idx : faces.index_range()) {
    const Span<int> face_verts = corner_verts.slice(faces[face_idx]);
    
    /* Calculate the face's AABB */
    for (const int vert : face_verts) {
      face_infos[face_idx].aabb.expand(positions[vert]);
    }
    
    /* Calculate the face centroid */
    float3 centroid(0, 0, 0);
    for (const int vert : face_verts) {
      centroid += positions[vert];
    }
    face_infos[face_idx].centroid = centroid / float(face_verts.size());
  }
}

/* Hybrid K-means + region growing algorithm for compact, connected clusters */
static void create_kcompact_clusters(
    const Span<float3> positions,
    const Span<int> corner_verts,
    const OffsetIndices<int> faces,
    const int desired_faces_per_cluster,
    const float compactness,
    MutableSpan<int> face_cluster_ids)
{
  const int total_faces = faces.size();
  
  /* Initialize face information */
  Vector<FaceInfo> face_infos;
  face_infos.reserve(total_faces);
  for (int i = 0; i < total_faces; i++) {
    face_infos.append(FaceInfo(i));
  }
  
  /* Build triangle-triangle connectivity */
  build_face_adjacency(corner_verts, faces, face_infos);
  
  /* Calculate face geometries (AABB, centroid) */
  calculate_face_geometries(positions, corner_verts, faces, face_infos);
  
  /* Initialize random number generator */
  RNG *rng = BLI_rng_new(42);
  
  /* Determine approximate number of clusters */
  int estimated_cluster_count = (total_faces + desired_faces_per_cluster - 1) / desired_faces_per_cluster;
  
  /* Phase 1: Place initial cluster centers at locations far from each other */
  
  /* Use K-means++ method to initialize cluster centers */
  Vector<float3> cluster_centers;
  
  /* Select the first seed randomly */
  int first_seed = BLI_rng_get_int(rng) % total_faces;
  cluster_centers.append(face_infos[first_seed].centroid);
  
  /* Array to track minimum distance of each face to the nearest center */
  Array<float> min_distances(total_faces);
  for (int i = 0; i < total_faces; i++) {
    min_distances[i] = math::distance_squared(face_infos[i].centroid, cluster_centers[0]);
  }
  
  /* Select remaining centers using kmeans++ technique (weighted by distance²) */
  for (int i = 1; i < estimated_cluster_count; i++) {
    /* Calculate sum of squared distances */
    float sum_distances = 0.0f;
    for (int j = 0; j < total_faces; j++) {
      sum_distances += min_distances[j];
    }
    
    /* Choose a face based on its weighted distance */
    float threshold = BLI_rng_get_float(rng) * sum_distances;
    float cumul = 0.0f;
    int selected_face = 0;
    
    for (int j = 0; j < total_faces; j++) {
      cumul += min_distances[j];
      if (cumul >= threshold) {
        selected_face = j;
        break;
      }
    }
    
    /* Add the new center */
    cluster_centers.append(face_infos[selected_face].centroid);
    
    /* Update minimum distances */
    for (int j = 0; j < total_faces; j++) {
      float dist = math::distance_squared(face_infos[j].centroid, cluster_centers[i]);
      min_distances[j] = std::min(min_distances[j], dist);
    }
  }
  
  /* Phase 2: Grow clusters around centers */
  
  /* Create info for each cluster */
  Vector<ClusterInfo> clusters;
  for (int i = 0; i < cluster_centers.size(); i++) {
    clusters.append(ClusterInfo(i));
    clusters[i].centroid = cluster_centers[i];
  }
  
  /* Structure for the priority queue for region growing */
  struct FaceCandidate {
    int face_idx;
    int cluster_id;
    float score;
    
    FaceCandidate(int face, int cluster, float s) : face_idx(face), cluster_id(cluster), score(s) {}
    
    bool operator<(const FaceCandidate &other) const {
      return score < other.score;  /* Higher score = better candidate */
    }
  };
  
  /* Initialize priority queue with the faces closest to each center */
  std::priority_queue<FaceCandidate> candidates;
  
  /* Find the best seeds for each cluster */
  for (int cluster_id = 0; cluster_id < clusters.size(); cluster_id++) {
    int best_face = -1;
    float best_dist = FLT_MAX;
    
    for (int face_idx = 0; face_idx < total_faces; face_idx++) {
      if (face_infos[face_idx].cluster_id == -1) {
        float dist = math::distance_squared(face_infos[face_idx].centroid, clusters[cluster_id].centroid);
        if (dist < best_dist) {
          best_dist = dist;
          best_face = face_idx;
        }
      }
    }
    
    if (best_face != -1) {
      /* Assign this face as the cluster seed */
      face_infos[best_face].cluster_id = cluster_id;
      face_cluster_ids[best_face] = cluster_id;
      
      clusters[cluster_id].faces.append(best_face);
      clusters[cluster_id].update_centroid(face_infos);
      
      /* Add its neighbors to the priority queue */
      for (int adj_face : face_infos[best_face].adjacent_faces) {
        if (face_infos[adj_face].cluster_id == -1) {
          /* Calculate a score combining distance to centroid and compactness */
          float dist_to_center = math::distance(face_infos[adj_face].centroid, clusters[cluster_id].centroid);
          float dist_to_seed = math::distance(face_infos[adj_face].centroid, face_infos[best_face].centroid);
          
          /* Higher score = better candidate */
          /* The compactness factor weights the importance of proximity to center vs. proximity to neighbors */
          float score = (1.0f - compactness) * (1.0f / (dist_to_center + 0.001f)) + 
                        compactness * (1.0f / (dist_to_seed + 0.001f));
                        
          candidates.push(FaceCandidate(adj_face, cluster_id, score));
        }
      }
    }
  }
  
  /* Simultaneous growth of all clusters */
  int faces_assigned = 0;
  Array<int> faces_per_cluster(clusters.size(), 1);  /* Each cluster already has one face assigned */
  faces_assigned = clusters.size();
  
  while (!candidates.empty() && faces_assigned < total_faces) {
    /* Take the best candidate */
    FaceCandidate best = candidates.top();
    candidates.pop();
    
    /* Check if the face is still unassigned */
    if (face_infos[best.face_idx].cluster_id != -1) {
      continue;
    }
    
    /* Check if the cluster has reached its maximum size */
    if (faces_per_cluster[best.cluster_id] >= desired_faces_per_cluster) {
      continue;
    }
    
    /* Assign the face to the cluster */
    face_infos[best.face_idx].cluster_id = best.cluster_id;
    face_cluster_ids[best.face_idx] = best.cluster_id;
    faces_assigned++;
    faces_per_cluster[best.cluster_id]++;
    
    /* Update cluster information */
    clusters[best.cluster_id].faces.append(best.face_idx);
    clusters[best.cluster_id].update_centroid(face_infos);
    
    /* Add neighbors to the priority queue */
    for (int adj_face : face_infos[best.face_idx].adjacent_faces) {
      if (face_infos[adj_face].cluster_id == -1) {
        /* Recalculate score with updated centroid */
        float dist_to_center = math::distance(face_infos[adj_face].centroid, clusters[best.cluster_id].centroid);
        float dist_to_face = math::distance(face_infos[adj_face].centroid, face_infos[best.face_idx].centroid);
        
        float score = (1.0f - compactness) * (1.0f / (dist_to_center + 0.001f)) + 
                      compactness * (1.0f / (dist_to_face + 0.001f));
                      
        candidates.push(FaceCandidate(adj_face, best.cluster_id, score));
      }
    }
  }
  
  /* Phase 3: Assign remaining faces to the closest neighboring cluster */
  bool has_unassigned = false;
  for (int i = 0; i < total_faces; i++) {
    if (face_infos[i].cluster_id == -1) {
      has_unassigned = true;
      break;
    }
  }
  
  /* If there are unassigned faces, continue growing without size limit */
  if (has_unassigned) {
    /* Reset the priority queue */
    std::priority_queue<FaceCandidate> remaining_candidates;
    
    /* Add all neighbors of assigned faces */
    for (int face_idx = 0; face_idx < total_faces; face_idx++) {
      if (face_infos[face_idx].cluster_id != -1) {
        for (int adj_face : face_infos[face_idx].adjacent_faces) {
          if (face_infos[adj_face].cluster_id == -1) {
            int cluster_id = face_infos[face_idx].cluster_id;
            float dist = math::distance(face_infos[adj_face].centroid, clusters[cluster_id].centroid);
            
            remaining_candidates.push(FaceCandidate(adj_face, cluster_id, 1.0f / (dist + 0.001f)));
          }
        }
      }
    }
    
    /* Final assignment */
    while (!remaining_candidates.empty() && faces_assigned < total_faces) {
      FaceCandidate best = remaining_candidates.top();
      remaining_candidates.pop();
      
      if (face_infos[best.face_idx].cluster_id != -1) {
        continue;
      }
      
      /* Assign the face */
      face_infos[best.face_idx].cluster_id = best.cluster_id;
      face_cluster_ids[best.face_idx] = best.cluster_id;
      faces_assigned++;
      
      /* Add unassigned neighbors */
      for (int adj_face : face_infos[best.face_idx].adjacent_faces) {
        if (face_infos[adj_face].cluster_id == -1) {
          float dist = math::distance(face_infos[adj_face].centroid, clusters[best.cluster_id].centroid);
          remaining_candidates.push(FaceCandidate(adj_face, best.cluster_id, 1.0f / (dist + 0.001f)));
        }
      }
    }
  }
  
  /* Handle isolated islands that are still unassigned */
  for (int i = 0; i < total_faces; i++) {
    if (face_infos[i].cluster_id == -1) {
      float min_dist = FLT_MAX;
      int closest_cluster = 0;
      
      for (int cluster_id = 0; cluster_id < clusters.size(); cluster_id++) {
        float dist = math::distance_squared(face_infos[i].centroid, clusters[cluster_id].centroid);
        if (dist < min_dist) {
          min_dist = dist;
          closest_cluster = cluster_id;
        }
      }
      
      face_infos[i].cluster_id = closest_cluster;
      face_cluster_ids[i] = closest_cluster;
    }
  }
  
  /* Free the random number generator */
  BLI_rng_free(rng);
}

/* Convert face clusters to vertex clusters */
static void convert_face_clusters_to_vertex_clusters(
    const Span<int> corner_verts,
    const OffsetIndices<int> faces,
    const Span<int> face_cluster_ids,
    MutableSpan<int> vertex_cluster_ids)
{
  /* Initialize vertex cluster IDs to -1 */
  for (int i = 0; i < vertex_cluster_ids.size(); i++) {
    vertex_cluster_ids[i] = -1;
  }
  
  /* For each face, assign its cluster to its vertices */
  for (const int face_idx : faces.index_range()) {
    const int cluster_id = face_cluster_ids[face_idx];
    
    /* Assign this cluster to all vertices of the face */
    for (const int vert : corner_verts.slice(faces[face_idx])) {
      if (vert < vertex_cluster_ids.size()) {
        /* If vertex is not yet assigned or already belongs to this cluster */
        if (vertex_cluster_ids[vert] == -1 || vertex_cluster_ids[vert] == cluster_id) {
          vertex_cluster_ids[vert] = cluster_id;
        }
        else {
          /* Case where a vertex belongs to multiple clusters (cluster boundary) */
          /* Keep the existing ID for consistency */
        }
      }
    }
  }
  
  /* Ensure all vertices are assigned */
  for (int i = 0; i < vertex_cluster_ids.size(); i++) {
    if (vertex_cluster_ids[i] == -1) {
      /* Find a neighboring vertex with an assigned cluster */
      bool found = false;
      
      /* Go through all faces to find those containing this vertex */
      for (const int face_idx : faces.index_range()) {
        for (const int vert : corner_verts.slice(faces[face_idx])) {
          if (vert == i) {
            /* Find another vertex from this face with an assigned cluster */
            for (const int other_vert : corner_verts.slice(faces[face_idx])) {
              if (other_vert != i && other_vert < vertex_cluster_ids.size() && 
                  vertex_cluster_ids[other_vert] != -1) {
                vertex_cluster_ids[i] = vertex_cluster_ids[other_vert];
                found = true;
                break;
              }
            }
            if (found) {
              break;
            }
          }
        }
        if (found) {
          break;
        }
      }
      
      /* If still not assigned (isolated vertex), give it cluster 0 */
      if (!found) {
        vertex_cluster_ids[i] = 0;
      }
    }
  }
}

/* Create a mesh to visualize the bounding boxes of clusters */
static Mesh *create_bounds_mesh(const Array<AABB> *cluster_aabbs,
                                const Array<ColorGeometry4f> &cluster_colors)
{
  if (!cluster_aabbs || cluster_aabbs->size() == 0) {
    return nullptr;
  }
  
  const int total_boxes = cluster_aabbs->size();
  if (total_boxes < 1) {
    return nullptr;
  }
  
  /* Filter and count valid boxes */
  Vector<int> valid_box_indices;
  for (int i = 0; i < total_boxes; i++) {
    /* Boxes are stored as AABB objects */
    const AABB &bbox = (*cluster_aabbs)[i];
    const float3 &bbox_min = bbox.min;
    const float3 &bbox_max = bbox.max;
    
    /* Check that the box is valid (min < max) */
    if (bbox_min.x < bbox_max.x && bbox_min.y < bbox_max.y && bbox_min.z < bbox_max.z) {
      valid_box_indices.append(i);
    }
  }
  
  const int valid_box_count = valid_box_indices.size();
  if (valid_box_count == 0) {
    return nullptr;
  }
  
  /* 8 vertices per box */
  const int verts_num = valid_box_count * 8;
  /* 6 faces per box, 4 vertices per face */
  const int loops_num = valid_box_count * 6 * 4;
  /* 6 faces per box */
  const int faces_num = valid_box_count * 6;
  
  /* Create an empty mesh */
  Mesh *mesh = BKE_mesh_new_nomain(verts_num, 0, faces_num, loops_num);
  
  /* Initialize vertex positions */
  MutableSpan<float3> positions = mesh->vert_positions_for_write();
  
  /* Add vertices */
  int vert_index = 0;
  for (int box_idx = 0; box_idx < valid_box_count; box_idx++) {
    const int cluster_idx = valid_box_indices[box_idx];
    const AABB &bbox = (*cluster_aabbs)[cluster_idx];
    const float3 &bbox_min = bbox.min;
    const float3 &bbox_max = bbox.max;
    
    /* Ensure box dimensions are not too small (avoid degenerate faces) */
    float3 size = bbox_max - bbox_min;
    const float min_size = 0.0001f;
    if (size.x < min_size) {
      size.x = min_size;
    }
    if (size.y < min_size) {
      size.y = min_size;
    }
    if (size.z < min_size) {
      size.z = min_size;
    }
    
    float3 adjusted_min = bbox_min;
    float3 adjusted_max = adjusted_min + size;
    
    /* 8 corners of a cube */
    float3 corners[8] = {
      float3(adjusted_min.x, adjusted_min.y, adjusted_min.z),
      float3(adjusted_max.x, adjusted_min.y, adjusted_min.z),
      float3(adjusted_max.x, adjusted_max.y, adjusted_min.z),
      float3(adjusted_min.x, adjusted_max.y, adjusted_min.z),
      float3(adjusted_min.x, adjusted_min.y, adjusted_max.z),
      float3(adjusted_max.x, adjusted_min.y, adjusted_max.z),
      float3(adjusted_max.x, adjusted_max.y, adjusted_max.z),
      float3(adjusted_min.x, adjusted_max.y, adjusted_max.z),
    };
    
    for (int j = 0; j < 8; j++) {
      positions[vert_index] = corners[j];
      vert_index++;
    }
  }
  
  /* Create polygons (faces) */
  MutableSpan<int> corner_verts = mesh->corner_verts_for_write();
  MutableSpan<int> corner_edges = mesh->corner_edges_for_write();
  MutableSpan<int> face_offsets = mesh->face_offsets_for_write();
  
  int loop_index = 0;
  int face_offset = 0;
  
  for (int box_idx = 0; box_idx < valid_box_count; box_idx++) {
    const int base_vert = box_idx * 8;
    
    /* Indices for 6 faces of a cube, defined clockwise for correct orientation */
    int face_indices[6][4] = {
      {0, 3, 2, 1},   /* Front face (Z-) */
      {4, 5, 6, 7},   /* Back face (Z+) */
      {0, 1, 5, 4},   /* Bottom face (Y-) */
      {1, 2, 6, 5},   /* Right face (X+) */
      {3, 7, 6, 2},   /* Top face (Y+) */
      {0, 4, 7, 3},   /* Left face (X-) */
    };
    
    for (int face = 0; face < 6; face++) {
      face_offsets[box_idx * 6 + face] = face_offset;
      
      for (int j = 0; j < 4; j++) {
        corner_verts[loop_index] = base_vert + face_indices[face][j];
        corner_edges[loop_index] = 0;  /* Will be calculated by BKE_mesh_calc_edges */
        loop_index++;
      }
      
      face_offset += 4;
    }
  }
  
  /* Set the last face offset */
  face_offsets[faces_num] = face_offset;
  
  /* Add color attribute */
  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
  
  bke::SpanAttributeWriter<ColorGeometry4f> colors =
      attributes.lookup_or_add_for_write_span<ColorGeometry4f>("color", bke::AttrDomain::Face);
      
  /* Add cluster ID attribute for filtering */
  bke::SpanAttributeWriter<int> cluster_ids =
      attributes.lookup_or_add_for_write_span<int>("face_cluster_id", bke::AttrDomain::Face);
      
  /* Assign colors and cluster IDs to faces */
  for (int box_idx = 0; box_idx < valid_box_count; box_idx++) {
    const int cluster_idx = valid_box_indices[box_idx];
    const ColorGeometry4f &color = cluster_colors[cluster_idx];
    
    for (int face = 0; face < 6; face++) {
      const int face_idx = box_idx * 6 + face;
      colors.span[face_idx] = color;
      cluster_ids.span[face_idx] = cluster_idx;
    }
  }
  
  colors.finish();
  cluster_ids.finish();
  
  blender::bke::mesh_calc_edges(*mesh, false, false);
  
  mesh->runtime->vert_normals_cache.tag_dirty();
  mesh->runtime->face_normals_cache.tag_dirty();
  mesh->runtime->corner_normals_cache.tag_dirty();
  
  return mesh;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  /* Get inputs from sockets. */
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh");
  const int faces_per_cluster = std::max(1, params.extract_input<int>("Max Faces Per Cluster"));
  const float compactness = params.extract_input<float>("Compactness");
  const bool random_colors = params.extract_input<bool>("Random Colors");
  const bool build_bounds = params.extract_input<bool>("Build Bounding Data");

  int total_faces = 0;
  int cluster_count = 0;

  /* Process mesh if available. */
  if (Mesh *mesh = geometry_set.get_mesh_for_write()) {
    /* Get vertex positions and mesh topology */
    const Span<float3> positions = mesh->vert_positions();
    const Span<int> corner_verts = mesh->corner_verts();
    const OffsetIndices faces = mesh->faces();
    total_faces = faces.size();
    
    /* Créer un attribut de cluster_id pour les faces */
    bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
    bke::SpanAttributeWriter<int> face_cluster_attribute =
        attributes.lookup_or_add_for_write_span<int>("face_cluster_id", bke::AttrDomain::Face);
    
    /* Create optimized spatial clusters with compact shapes */
    create_kcompact_clusters(
        positions, corner_verts, faces, faces_per_cluster, compactness, face_cluster_attribute.span);
    
    /* Count the number of clusters created */
    cluster_count = 0;
    for (int i = 0; i < total_faces; i++) {
      cluster_count = std::max(cluster_count, face_cluster_attribute.span[i] + 1);
    }
    
    /* Finish face cluster ID attribute writing */
    face_cluster_attribute.finish();
    
    bke::SpanAttributeWriter<int> vertex_cluster_attribute =
        attributes.lookup_or_add_for_write_span<int>("cluster_id", bke::AttrDomain::Point);
    
    Array<int> face_cluster_ids(total_faces);
    bke::SpanAttributeWriter<int> face_reader = 
        attributes.lookup_or_add_for_write_span<int>("face_cluster_id", bke::AttrDomain::Face);
    
    for (int i = 0; i < total_faces; i++) {
      face_cluster_ids[i] = face_reader.span[i];
    }
    face_reader.finish();
    
    convert_face_clusters_to_vertex_clusters(
        corner_verts, faces, face_cluster_ids, vertex_cluster_attribute.span);
    
    vertex_cluster_attribute.finish();
    
    if (random_colors && cluster_count > 0 && mesh->faces_num > 0) {
      bke::SpanAttributeWriter<ColorGeometry4f> face_color_attribute =
          attributes.lookup_or_add_for_write_span<ColorGeometry4f>("Color", bke::AttrDomain::Face);
      
      RNG *rng = BLI_rng_new(42);
      
      Array<ColorGeometry4f> cluster_colors(cluster_count);
      for (int i = 0; i < cluster_count; i++) {
        float h = float(i) / float(cluster_count); 
        float s = 0.8f + BLI_rng_get_float(rng) * 0.2f; 
        float v = 0.8f + BLI_rng_get_float(rng) * 0.2f;
        
        float r, g, b;
        float h6 = h * 6.0f;
        float i_part = floorf(h6);
        float f_part = h6 - i_part;
        
        float p = v * (1.0f - s);
        float q = v * (1.0f - s * f_part);
        float t = v * (1.0f - s * (1.0f - f_part));
        
        switch (int(i_part) % 6) {
          case 0: r = v; g = t; b = p; break;
          case 1: r = q; g = v; b = p; break;
          case 2: r = p; g = v; b = t; break;
          case 3: r = p; g = q; b = v; break;
          case 4: r = t; g = p; b = v; break;
          default: r = v; g = p; b = q; break;
        }
        
        cluster_colors[i] = ColorGeometry4f(r, g, b, 1.0f);
      }
      
      /* Free RNG */
      BLI_rng_free(rng);
      
      /* Assign face colors based on their cluster */
      bke::SpanAttributeWriter<int> face_cluster_reader = 
          attributes.lookup_or_add_for_write_span<int>("face_cluster_id", bke::AttrDomain::Face);
      
      for (const int face_idx : faces.index_range()) {
        const int cluster_id = face_cluster_reader.span[face_idx];
        if (cluster_id >= 0 && cluster_id < cluster_count) {
          face_color_attribute.span[face_idx] = cluster_colors[cluster_id];
        }
        else {
          face_color_attribute.span[face_idx] = ColorGeometry4f(0.5f, 0.5f, 0.5f, 1.0f);
        }
      }
      
      face_cluster_reader.finish();
      face_color_attribute.finish();
      
      bke::SpanAttributeWriter<ColorGeometry4f> vert_color_attribute =
          attributes.lookup_or_add_for_write_span<ColorGeometry4f>("VertexColor", bke::AttrDomain::Point);
      
      bke::SpanAttributeWriter<int> vertex_cluster_reader = 
          attributes.lookup_or_add_for_write_span<int>("cluster_id", bke::AttrDomain::Point);
      
      for (int i = 0; i < vertex_cluster_reader.span.size(); i++) {
        int cluster_id = vertex_cluster_reader.span[i];
        if (cluster_id >= 0 && cluster_id < cluster_count) {
          vert_color_attribute.span[i] = cluster_colors[cluster_id];
        }
      }
      
      vertex_cluster_reader.finish();
      vert_color_attribute.finish();
    }

    if (build_bounds && cluster_count > 0) {
      Array<AABB> cluster_aabbs(cluster_count);
      for (int i = 0; i < cluster_count; i++) {
        cluster_aabbs[i] = AABB();
      }
      
      bke::SpanAttributeWriter<int> face_reader = 
          attributes.lookup_or_add_for_write_span<int>("face_cluster_id", bke::AttrDomain::Face);
      
      for (const int face_idx : faces.index_range()) {
        const int cluster_id = face_reader.span[face_idx];
        if (cluster_id >= 0 && cluster_id < cluster_count) {
          /* Calculer l'AABB de cette face */
          AABB face_aabb;
          for (const int vert : corner_verts.slice(faces[face_idx])) {
            face_aabb.expand(positions[vert]);
          }
          cluster_aabbs[cluster_id] = cluster_aabbs[cluster_id].merge(face_aabb);
        }
      }
      face_reader.finish();
      
      if (params.output_is_required("Bounds of cluster")) {
        GeometrySet bounds_geometry;
        
        if (mesh && mesh->verts_num > 0 && cluster_count > 0) {
          /* Récupérer ou générer les couleurs des clusters */
          Array<ColorGeometry4f> cluster_colors(cluster_count);
          
          if (random_colors) {
            RNG *rng = BLI_rng_new(42);
            
            for (int i = 0; i < cluster_count; i++) {
              float h = float(i) / float(cluster_count);
              float s = 0.8f + BLI_rng_get_float(rng) * 0.2f;
              float v = 0.8f + BLI_rng_get_float(rng) * 0.2f;
              
              float r, g, b;
              float h6 = h * 6.0f;
              float i_part = floorf(h6);
              float f_part = h6 - i_part;
              
              float p = v * (1.0f - s);
              float q = v * (1.0f - s * f_part);
              float t = v * (1.0f - s * (1.0f - f_part));
              
              switch (int(i_part) % 6) {
                case 0: r = v; g = t; b = p; break;
                case 1: r = q; g = v; b = p; break;
                case 2: r = p; g = v; b = t; break;
                case 3: r = p; g = q; b = v; break;
                case 4: r = t; g = p; b = v; break;
                default: r = v; g = p; b = q; break;
              }
              
              cluster_colors[i] = ColorGeometry4f(r, g, b, 1.0f);
            }
            
            BLI_rng_free(rng);
          }
          else {
            for (int i = 0; i < cluster_count; i++) {
              float h = float(i) / float(cluster_count);
              
              float r, g, b;
              float h6 = h * 6.0f;
              float i_part = floorf(h6);
              float f_part = h6 - i_part;
              
              float p = 0.2f;
              float q = 0.2f + 0.8f * f_part;
              float t = 0.2f + 0.8f * (1.0f - f_part);
              
              switch (int(i_part) % 6) {
                case 0: r = 1.0f; g = t; b = p; break;
                case 1: r = q; g = 1.0f; b = p; break;
                case 2: r = p; g = 1.0f; b = t; break;
                case 3: r = p; g = q; b = 1.0f; break;
                case 4: r = t; g = p; b = 1.0f; break;
                default: r = 1.0f; g = p; b = q; break;
              }
              
              cluster_colors[i] = ColorGeometry4f(r, g, b, 1.0f);
            }
          }
          
          Mesh *bounds_mesh = create_bounds_mesh(&cluster_aabbs, cluster_colors);
          if (bounds_mesh) {
            bounds_geometry.replace_mesh(bounds_mesh);
          }
        }
        params.set_output("Bounds of cluster", std::move(bounds_geometry));
      }
    }
    else {
      if (params.output_is_required("Bounds of cluster")) {
        params.set_output("Bounds of cluster", GeometrySet());
      }
    }
  }

  params.set_output("Mesh", std::move(geometry_set));
  params.set_output("Cluster ID", bke::AttributeFieldInput::Create<int>("face_cluster_id"));  
  params.set_output("Cluster Colors", bke::AttributeFieldInput::Create<ColorGeometry4f>("Color"));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeCluster");
  ntype.ui_name = "Faces Clusters";
  ntype.ui_description = "Clustering somes faces of a mesh with an option to build a bounds of each cluster";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_cluster_cc