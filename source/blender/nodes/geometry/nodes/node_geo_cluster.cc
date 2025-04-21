/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_mesh.hh"
#include "BKE_attribute_math.hh"
#include "BKE_attribute.hh"

#include "BLI_math_vector_types.hh"
#include "BLI_math_vector.h"
#include "BLI_rand.h"
#include "BLI_kdtree.h"
#include <queue>
#include <float.h>
#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_cluster_cc {

/* Pour stocker les informations sur les triangles et clusters */
struct NodeGeometryCluster {
  int faces_per_cluster;
  int total_faces;
  int cluster_count;
  bool random_colors;
  float compactness;
};

/* Structure pour représenter une boîte englobante (AABB) */
struct AABB {
  float3 min;
  float3 max;
  
  AABB() : min(FLT_MAX, FLT_MAX, FLT_MAX), max(-FLT_MAX, -FLT_MAX, -FLT_MAX) {}
  
  /* Étendre la boîte pour inclure un point */
  void expand(const float3 &point) {
    min.x = std::min(min.x, point.x);
    min.y = std::min(min.y, point.y);
    min.z = std::min(min.z, point.z);
    max.x = std::max(max.x, point.x);
    max.y = std::max(max.y, point.y);
    max.z = std::max(max.z, point.z);
  }
  
  /* Calculer le volume de la boîte */
  float volume() const {
    return (max.x - min.x) * (max.y - min.y) * (max.z - min.z);
  }
  
  /* Calculer la somme des dimensions (utilisé comme heuristique) */
  float surface_area() const {
    float3 dimensions = max - min;
    return 2.0f * (dimensions.x * dimensions.y + dimensions.x * dimensions.z + dimensions.y * dimensions.z);
  }
  
  /* Calculer le centre de la boîte */
  float3 center() const {
    return (min + max) * 0.5f;
  }
  
  /* Fusionner deux boîtes */
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

/* Structure pour représenter un triangle/face avec son cluster et AABB */
struct FaceInfo {
  int index;           // Index de la face
  int cluster_id;      // ID du cluster assigné
  AABB aabb;           // Boîte englobante de la face
  float3 centroid;     // Centre de la face
  Vector<int> adjacent_faces; // Faces adjacentes
  
  FaceInfo(int idx) : index(idx), cluster_id(-1) {}
};

/* Structure pour stocker les informations d'un cluster en cours de construction */
struct ClusterInfo {
  int id;               // ID du cluster
  float3 centroid;      // Centre de masse du cluster
  Vector<int> faces;    // Faces appartenant au cluster
  AABB aabb;            // Boîte englobante du cluster
  
  ClusterInfo(int cluster_id) : id(cluster_id), centroid(0, 0, 0) {}
  
  /* Mise à jour du centroïde basée sur les faces du cluster */
  void update_centroid(const Vector<FaceInfo> &face_infos) {
    if (faces.is_empty()) {
      return;
    }
    
    centroid = float3(0, 0, 0);
    for (int face_idx : faces) {
      centroid += face_infos[face_idx].centroid;
    }
    centroid /= float(faces.size());
    
    /* Mise à jour de l'AABB */
    aabb = AABB();
    for (int face_idx : faces) {
      aabb = aabb.merge(face_infos[face_idx].aabb);
    }
  }
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Mesh").supported_type(GeometryComponent::Type::Mesh);
  b.add_input<decl::Int>("Faces Per Cluster").default_value(64).min(1).max(10000);
  b.add_input<decl::Float>("Compactness").default_value(0.8f).min(0.0f).max(1.0f).subtype(PROP_FACTOR);
  b.add_input<decl::Bool>("Random Colors").default_value(true);
  b.add_output<decl::Geometry>("Mesh").propagate_all();
  b.add_output<decl::Color>("Cluster Colors").field_source_reference_all();
  b.add_output<decl::Int>("Cluster ID").field_source_reference_all();
}

/* Fonction qui construit la connectivité triangle-triangle */
static void build_face_adjacency(
    const Span<int> corner_verts,
    const OffsetIndices<int> faces,
    Vector<FaceInfo> &face_infos)
{
  /* Construire un mapping vertex->faces */
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
  
  /* Remplir le mapping vertex->faces */
  for (const int face_idx : faces.index_range()) {
    const Span<int> face_verts = corner_verts.slice(faces[face_idx]);
    for (const int vert : face_verts) {
      vert_to_faces[vert].append(face_idx);
    }
  }
  
  /* Trouver les faces adjacentes (partageant au moins un vertex) */
  for (const int face_idx : faces.index_range()) {
    const Span<int> face_verts = corner_verts.slice(faces[face_idx]);
    
    /* Collecter toutes les faces potentiellement adjacentes */
    Set<int> potential_adjacent;
    for (const int vert : face_verts) {
      for (const int adj_face : vert_to_faces[vert]) {
        if (adj_face != face_idx) {
          potential_adjacent.add(adj_face);
        }
      }
    }
    
    /* Vérifier quelles faces partagent une arête (2 vertices en commun) */
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
      
      /* Si les faces partagent au moins 2 vertices (une arête), elles sont adjacentes */
      if (shared_verts >= 2) {
        face_infos[face_idx].adjacent_faces.append(adj_face);
      }
    }
  }
}

/* Fonction qui calcule les AABB et centroïdes des faces */
static void calculate_face_geometries(
    const Span<float3> positions,
    const Span<int> corner_verts,
    const OffsetIndices<int> faces,
    Vector<FaceInfo> &face_infos)
{
  for (const int face_idx : faces.index_range()) {
    const Span<int> face_verts = corner_verts.slice(faces[face_idx]);
    
    /* Calculer l'AABB de la face */
    for (const int vert : face_verts) {
      face_infos[face_idx].aabb.expand(positions[vert]);
    }
    
    /* Calculer le centroïde de la face */
    float3 centroid(0, 0, 0);
    for (const int vert : face_verts) {
      centroid += positions[vert];
    }
    face_infos[face_idx].centroid = centroid / float(face_verts.size());
  }
}

/* Algorithme hybride K-means + croissance de région pour des clusters compacts et connexes */
static void create_kcompact_clusters(
    const Span<float3> positions,
    const Span<int> corner_verts,
    const OffsetIndices<int> faces,
    const int desired_faces_per_cluster,
    const float compactness,
    MutableSpan<int> face_cluster_ids)
{
  const int total_faces = faces.size();
  
  /* Initialiser les informations des faces */
  Vector<FaceInfo> face_infos;
  face_infos.reserve(total_faces);
  for (int i = 0; i < total_faces; i++) {
    face_infos.append(FaceInfo(i));
  }
  
  /* Construire la connectivité triangle-triangle */
  build_face_adjacency(corner_verts, faces, face_infos);
  
  /* Calculer les géométries des faces (AABB, centroïde) */
  calculate_face_geometries(positions, corner_verts, faces, face_infos);
  
  /* Initialiser un générateur de nombres aléatoires */
  RNG *rng = BLI_rng_new(42);
  
  /* Déterminer le nombre approximatif de clusters */
  int estimated_cluster_count = (total_faces + desired_faces_per_cluster - 1) / desired_faces_per_cluster;
  
  /* Phase 1: Placer les centres initiaux de clusters à des endroits éloignés les uns des autres */
  
  /* Utiliser la méthode K-means++ pour initialiser les centres de clusters */
  Vector<float3> cluster_centers;
  
  /* Sélectionner la première graine au hasard */
  int first_seed = BLI_rng_get_int(rng) % total_faces;
  cluster_centers.append(face_infos[first_seed].centroid);
  
  /* Tableau pour suivre la distance minimale de chaque face au centre le plus proche */
  Array<float> min_distances(total_faces);
  for (int i = 0; i < total_faces; i++) {
    min_distances[i] = math::distance_squared(face_infos[i].centroid, cluster_centers[0]);
  }
  
  /* Sélectionner les centres restants en utilisant la technique kmeans++ (pondération par distance²) */
  for (int i = 1; i < estimated_cluster_count; i++) {
    /* Calculer la somme des distances au carré */
    float sum_distances = 0.0f;
    for (int j = 0; j < total_faces; j++) {
      sum_distances += min_distances[j];
    }
    
    /* Choisir une face en fonction de sa distance pondérée */
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
    
    /* Ajouter le nouveau centre */
    cluster_centers.append(face_infos[selected_face].centroid);
    
    /* Mettre à jour les distances minimales */
    for (int j = 0; j < total_faces; j++) {
      float dist = math::distance_squared(face_infos[j].centroid, cluster_centers[i]);
      min_distances[j] = std::min(min_distances[j], dist);
    }
  }
  
  /* Phase 2: Croissance des clusters autour des centres */
  
  /* Créer des infos pour chaque cluster */
  Vector<ClusterInfo> clusters;
  for (int i = 0; i < cluster_centers.size(); i++) {
    clusters.append(ClusterInfo(i));
    clusters[i].centroid = cluster_centers[i];
  }
  
  /* Structure pour la file prioritaire de la croissance de région */
  struct FaceCandidate {
    int face_idx;
    int cluster_id;
    float score;
    
    FaceCandidate(int face, int cluster, float s) : face_idx(face), cluster_id(cluster), score(s) {}
    
    bool operator<(const FaceCandidate &other) const {
      return score < other.score;  /* Score plus élevé = meilleur candidat */
    }
  };
  
  /* Initialiser la file prioritaire avec les faces les plus proches de chaque centre */
  std::priority_queue<FaceCandidate> candidates;
  
  /* Trouver les meilleures graines pour chaque cluster */
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
      /* Assigner cette face comme graine du cluster */
      face_infos[best_face].cluster_id = cluster_id;
      face_cluster_ids[best_face] = cluster_id;
      
      clusters[cluster_id].faces.append(best_face);
      clusters[cluster_id].update_centroid(face_infos);
      
      /* Ajouter ses voisins à la file prioritaire */
      for (int adj_face : face_infos[best_face].adjacent_faces) {
        if (face_infos[adj_face].cluster_id == -1) {
          /* Calculer un score combinant distance au centroïde et compacité */
          float dist_to_center = math::distance(face_infos[adj_face].centroid, clusters[cluster_id].centroid);
          float dist_to_seed = math::distance(face_infos[adj_face].centroid, face_infos[best_face].centroid);
          
          /* Score plus élevé = meilleur candidat */
          /* Le facteur de compacité pondère l'importance de la proximité au centre vs. la proximité aux voisins */
          float score = (1.0f - compactness) * (1.0f / (dist_to_center + 0.001f)) + 
                        compactness * (1.0f / (dist_to_seed + 0.001f));
                        
          candidates.push(FaceCandidate(adj_face, cluster_id, score));
        }
      }
    }
  }
  
  /* Croissance simultanée de tous les clusters */
  int faces_assigned = 0;
  Array<int> faces_per_cluster(clusters.size(), 1);  /* Chaque cluster a déjà une face assignée */
  faces_assigned = clusters.size();
  
  while (!candidates.empty() && faces_assigned < total_faces) {
    /* Prendre le meilleur candidat */
    FaceCandidate best = candidates.top();
    candidates.pop();
    
    /* Vérifier si la face est toujours non assignée */
    if (face_infos[best.face_idx].cluster_id != -1) {
      continue;
    }
    
    /* Vérifier si le cluster a atteint sa taille maximale */
    if (faces_per_cluster[best.cluster_id] >= desired_faces_per_cluster) {
      continue;
    }
    
    /* Assigner la face au cluster */
    face_infos[best.face_idx].cluster_id = best.cluster_id;
    face_cluster_ids[best.face_idx] = best.cluster_id;
    faces_assigned++;
    faces_per_cluster[best.cluster_id]++;
    
    /* Mettre à jour les informations du cluster */
    clusters[best.cluster_id].faces.append(best.face_idx);
    clusters[best.cluster_id].update_centroid(face_infos);
    
    /* Ajouter les voisins à la file prioritaire */
    for (int adj_face : face_infos[best.face_idx].adjacent_faces) {
      if (face_infos[adj_face].cluster_id == -1) {
        /* Recalculer le score avec le centroïde mis à jour */
        float dist_to_center = math::distance(face_infos[adj_face].centroid, clusters[best.cluster_id].centroid);
        float dist_to_face = math::distance(face_infos[adj_face].centroid, face_infos[best.face_idx].centroid);
        
        float score = (1.0f - compactness) * (1.0f / (dist_to_center + 0.001f)) + 
                      compactness * (1.0f / (dist_to_face + 0.001f));
                      
        candidates.push(FaceCandidate(adj_face, best.cluster_id, score));
      }
    }
  }
  
  /* Phase 3: Assignation des faces restantes au cluster voisin le plus proche */
  bool has_unassigned = false;
  for (int i = 0; i < total_faces; i++) {
    if (face_infos[i].cluster_id == -1) {
      has_unassigned = true;
      break;
    }
  }
  
  /* S'il reste des faces non assignées, continuer la croissance sans limite de taille */
  if (has_unassigned) {
    /* Réinitialiser la file prioritaire */
    std::priority_queue<FaceCandidate> remaining_candidates;
    
    /* Ajouter tous les voisins des faces assignées */
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
    
    /* Assignation finale */
    while (!remaining_candidates.empty() && faces_assigned < total_faces) {
      FaceCandidate best = remaining_candidates.top();
      remaining_candidates.pop();
      
      if (face_infos[best.face_idx].cluster_id != -1) {
        continue;
      }
      
      /* Assigner la face */
      face_infos[best.face_idx].cluster_id = best.cluster_id;
      face_cluster_ids[best.face_idx] = best.cluster_id;
      faces_assigned++;
      
      /* Ajouter les voisins non assignés */
      for (int adj_face : face_infos[best.face_idx].adjacent_faces) {
        if (face_infos[adj_face].cluster_id == -1) {
          float dist = math::distance(face_infos[adj_face].centroid, clusters[best.cluster_id].centroid);
          remaining_candidates.push(FaceCandidate(adj_face, best.cluster_id, 1.0f / (dist + 0.001f)));
        }
      }
    }
  }
  
  /* Traiter les îles isolées qui ne sont toujours pas assignées */
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
  
  /* Libérer le générateur aléatoire */
  BLI_rng_free(rng);
}

/* Convertir les clusters de faces en clusters de vertices */
static void convert_face_clusters_to_vertex_clusters(
    const Span<int> corner_verts,
    const OffsetIndices<int> faces,
    const Span<int> face_cluster_ids,
    MutableSpan<int> vertex_cluster_ids)
{
  /* Initialiser les IDs de cluster des vertices à -1 */
  for (int i = 0; i < vertex_cluster_ids.size(); i++) {
    vertex_cluster_ids[i] = -1;
  }
  
  /* Pour chaque face, attribuer son cluster à ses vertices */
  for (const int face_idx : faces.index_range()) {
    const int cluster_id = face_cluster_ids[face_idx];
    
    /* Assigner ce cluster à tous les vertices de la face */
    for (const int vert : corner_verts.slice(faces[face_idx])) {
      if (vert < vertex_cluster_ids.size()) {
        /* Si le vertex n'est pas encore assigné ou appartient déjà à ce cluster */
        if (vertex_cluster_ids[vert] == -1 || vertex_cluster_ids[vert] == cluster_id) {
          vertex_cluster_ids[vert] = cluster_id;
        }
        else {
          /* Cas où un vertex appartient à plusieurs clusters (bord de cluster) */
          /* On garde l'ID existant pour maintenir la cohérence */
        }
      }
    }
  }
  
  /* S'assurer que tous les vertices sont assignés */
  for (int i = 0; i < vertex_cluster_ids.size(); i++) {
    if (vertex_cluster_ids[i] == -1) {
      /* Trouver un vertex voisin avec un cluster assigné */
      bool found = false;
      
      /* Parcourir toutes les faces pour trouver celles qui contiennent ce vertex */
      for (const int face_idx : faces.index_range()) {
        for (const int vert : corner_verts.slice(faces[face_idx])) {
          if (vert == i) {
            /* Trouver un autre vertex de cette face avec un cluster assigné */
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
      
      /* Si toujours pas assigné (vertex isolé), lui donner le cluster 0 */
      if (!found) {
        vertex_cluster_ids[i] = 0;
      }
    }
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  /* Get inputs from sockets. */
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh");
  const int faces_per_cluster = std::max(1, params.extract_input<int>("Faces Per Cluster"));
  const float compactness = params.extract_input<float>("Compactness");
  const bool random_colors = params.extract_input<bool>("Random Colors");

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
    
    /* Créer aussi un attribut de cluster_id pour les vertices */
    bke::SpanAttributeWriter<int> vertex_cluster_attribute =
        attributes.lookup_or_add_for_write_span<int>("cluster_id", bke::AttrDomain::Point);
    
    /* Créer une copie de l'attribut face_cluster_id dans un tableau */
    Array<int> face_cluster_ids(total_faces);
    bke::SpanAttributeWriter<int> face_reader = 
        attributes.lookup_or_add_for_write_span<int>("face_cluster_id", bke::AttrDomain::Face);
    
    for (int i = 0; i < total_faces; i++) {
      face_cluster_ids[i] = face_reader.span[i];
    }
    face_reader.finish();
    
    /* Convertir les clusters de faces en clusters de vertices */
    convert_face_clusters_to_vertex_clusters(
        corner_verts, faces, face_cluster_ids, vertex_cluster_attribute.span);
    
    /* Finish vertex cluster ID attribute writing */
    vertex_cluster_attribute.finish();
    
    /* Colorisation des faces en fonction des clusters */
    if (random_colors && cluster_count > 0 && mesh->faces_num > 0) {
      /* Create color attributes for faces - for flat colors with clear boundaries */
      bke::SpanAttributeWriter<ColorGeometry4f> face_color_attribute =
          attributes.lookup_or_add_for_write_span<ColorGeometry4f>("Color", bke::AttrDomain::Face);
      
      /* Initialize random number generator with a fixed seed for consistency */
      RNG *rng = BLI_rng_new(42);
      
      /* Pré-générer des couleurs aléatoires pour chaque cluster */
      Array<ColorGeometry4f> cluster_colors(cluster_count);
      for (int i = 0; i < cluster_count; i++) {
        /* Générer des couleurs aléatoires mais vives */
        float h = float(i) / float(cluster_count); // Distribuer les teintes sur le cercle chromatique
        float s = 0.8f + BLI_rng_get_float(rng) * 0.2f; // Saturation élevée (0.8-1.0)
        float v = 0.8f + BLI_rng_get_float(rng) * 0.2f; // Luminosité élevée (0.8-1.0)
        
        /* Convertir HSV en RGB */
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
        
        /* Stocker la couleur */
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
      }
      
      face_cluster_reader.finish();
      face_color_attribute.finish();
      
      /* Créer une copie de la couleur pour le domaine des vertices (utile pour le Viewer) */
      bke::SpanAttributeWriter<ColorGeometry4f> vert_color_attribute =
          attributes.lookup_or_add_for_write_span<ColorGeometry4f>("VertexColor", bke::AttrDomain::Point);
      
      /* Assign vertex colors based on their cluster */
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
  }

  /* Créer une sortie personnalisée avec des informations */
  params.set_output("Mesh", std::move(geometry_set));
  
  /* Exposer l'attribut de cluster_id comme un field pour la sortie */
  params.set_output("Cluster ID", bke::AttributeFieldInput::Create<int>("cluster_id"));
  
  /* Exposer l'attribut Color comme un field pour la sortie */
  params.set_output("Cluster Colors", bke::AttributeFieldInput::Create<ColorGeometry4f>("Color"));
  
  /* Ajouter un message dans l'interface utilisateur */
  params.error_message_add(
      NodeWarningType::Info,
      std::to_string(total_faces) + " faces in " + std::to_string(cluster_count) + 
      " clusters compacts et connexes");
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeCluster");
  ntype.ui_name = "Faces Clusters";
  ntype.ui_description = "Create compact, connected triangle clusters from faces";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_cluster_cc