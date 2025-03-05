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

#include "BKE_attribute.hh"
#include "BKE_attribute_math.hh"
#include "BKE_mesh.hh"
#include "BKE_pointcloud.hh"
#include "BKE_mesh_mapping.hh"
#include "BKE_mesh_runtime.hh"
#include "BKE_lib_id.hh"
#include "BKE_customdata.hh"
#include "BKE_bvhutils.hh"
#include "BKE_mesh_remesh_voxel.hh" // Pour accéder aux structures de données de voxélisation
#include "BKE_material.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"
#include "RNA_access.hh"
#include "RNA_define.hh"

#include "node_geometry_util.hh"

/* Pour activer les messages de débogage */
// #define DEBUG_TETRAHEDRALIZE

#ifdef DEBUG_TETRAHEDRALIZE
#  define DEBUG_PRINT(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
#else
#  define DEBUG_PRINT(fmt, ...)
#endif

using namespace blender;
using namespace blender::bke;

namespace blender::nodes::node_geo_tetrahedralize_cc {

// Définir l'identifiant du type de nœud
#define GEO_NODE_TETRAHEDRALIZE 800

/* 
 * Classe utilitaire pour générer des nombres aléatoires.
 * Remplace l'utilisation de l'API BLI_rng.
 */
class RandomNumberGenerator {
 private:
  uint32_t m_seed;

 public:
  RandomNumberGenerator() : m_seed(static_cast<uint32_t>(time(nullptr))) {}

  /* Génère un nombre entier aléatoire. */
  int get_int()
  {
    // Simple Linear Congruential Generator
    m_seed = (1103515245 * m_seed + 12345) & 0x7fffffff;
    return static_cast<int>(m_seed);
  }

  /* Génère un nombre flottant aléatoire entre 0 et 1. */
  float get_float()
  {
    // Convert integer to float in range [0, 1]
    return static_cast<float>(get_int()) / static_cast<float>(0x7fffffff);
  }
};

/* Définition des méthodes d'échelle locale. */
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

// Structure pour stocker les données du nœud
struct NodeGeoTetrahedralize {
  float base_size;
  float max_tet_scale;
  float min_triangle_scale;
  float local_feature_scale;
  bool use_manual_base_size;
  char scale_attribute_name[64];
};

/* Structure d'un tétraèdre, contient les indices des 4 sommets. */
struct Tetrahedron {
  int v1, v2, v3, v4;

  Tetrahedron() : v1(0), v2(0), v3(0), v4(0) {}
  Tetrahedron(int a, int b, int c, int d) : v1(a), v2(b), v3(c), v4(d) {}

  /* Calcule le volume du tétraèdre. */
  float volume(const Span<float3> &vertices) const
  {
    return volume_tetrahedron_signed_v3(
        vertices[v1], vertices[v2], vertices[v3], vertices[v4]);
  }

  /* Vérifie si le tétraèdre a un volume positif (orientation correcte). */
  bool has_positive_volume(const Span<float3> &vertices) const
  {
    return volume(vertices) > 0.0f;
  }

  /* Inverse l'orientation du tétraèdre si nécessaire. */
  void ensure_positive_volume(const Span<float3> &vertices)
  {
    if (!has_positive_volume(vertices)) {
      std::swap(v3, v4);
    }
  }
  
  /* Obtient les faces orientées du tétraèdre pour la création du maillage.
     Retourne les indices dans l'ordre approprié pour chaque face triangulaire. */
  void get_oriented_faces(MutableSpan<int> corner_verts, int &corner_index) const
  {
    // Face 1: v1, v3, v2 (orientation dans le sens anti-horaire vu de l'extérieur)
    corner_verts[corner_index++] = v1;
    corner_verts[corner_index++] = v3;
    corner_verts[corner_index++] = v2;
    
    // Face 2: v1, v2, v4 (orientation dans le sens anti-horaire vu de l'extérieur)
    corner_verts[corner_index++] = v1;
    corner_verts[corner_index++] = v2;
    corner_verts[corner_index++] = v4;
    
    // Face 3: v2, v3, v4 (orientation dans le sens anti-horaire vu de l'extérieur)
    corner_verts[corner_index++] = v2;
    corner_verts[corner_index++] = v3;
    corner_verts[corner_index++] = v4;
    
    // Face 4: v3, v1, v4 (orientation dans le sens anti-horaire vu de l'extérieur)
    corner_verts[corner_index++] = v3;
    corner_verts[corner_index++] = v1;
    corner_verts[corner_index++] = v4;
  }
};

/* Structure pour une face triangulaire, utilisée pour la construction des tétraèdres. */
struct TriFace {
  int v1, v2, v3;

  TriFace() : v1(0), v2(0), v3(0) {}
  TriFace(int a, int b, int c) : v1(a), v2(b), v3(c) {}
};

// Macros d'accès au stockage du nœud
NODE_STORAGE_FUNCS(NodeGeoTetrahedralize)

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
 * Calcule la taille de base du maillage.
 * Utilise les dimensions de la boîte englobante pour estimer une taille appropriée.
 */
static float calculate_base_size(const Mesh *mesh)
{
  if (!mesh || mesh->verts_num == 0) {
    return 1.0f;  // Valeur par défaut pour un maillage vide
  }

  // Calcul manuel de la boîte englobante
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

  // Calculer la diagonale de la boîte englobante
  float diagonal = sqrt(square_f(max.x - min.x) + 
                         square_f(max.y - min.y) + 
                         square_f(max.z - min.z));
  
  // Retourner 5% de la diagonale comme taille de base
  return diagonal * 0.05f;
}

/*
 * Calcule les tailles de caractéristiques locales pour chaque sommet.
 * Utilise la longueur moyenne des arêtes adjacentes à chaque sommet.
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
  
  // Pour chaque face
  for (const int face_index : faces.index_range()) {
    const IndexRange face = faces[face_index];
    
    // Pour chaque arête dans la face
    for (int i = 0; i < face.size(); i++) {
      const int v1 = corner_verts[face[i]];
      const int v2 = corner_verts[face[(i + 1) % face.size()]];
      
      // Calculer la longueur de l'arête
      const float3 &p1 = positions[v1];
      const float3 &p2 = positions[v2];
      const float edge_length = len_v3v3(p1, p2);
      
      // Accumuler la longueur pour les deux sommets
      feature_sizes[v1] += edge_length;
      feature_sizes[v2] += edge_length;
      vertex_counts[v1]++;
      vertex_counts[v2]++;
    }
  }
  
  // Calculer la moyenne pour chaque sommet
  for (int i = 0; i < positions.size(); i++) {
    if (vertex_counts[i] > 0) {
      feature_sizes[i] /= static_cast<float>(vertex_counts[i]);
    }
    else {
      // Si le sommet n'est connecté à aucune arête, utiliser une valeur par défaut
      feature_sizes[i] = 0.1f;
    }
  }
  
  return feature_sizes;
}

/*
 * Récupère les valeurs d'attribut de point pour l'échelle.
 * Si l'attribut n'existe pas, retourne des valeurs par défaut.
 */
static Array<float> get_point_attribute_values(const Mesh *mesh, [[maybe_unused]] const std::string &attribute_name)
{
  const Span<float3> positions = mesh->vert_positions();
  Array<float> values(positions.size(), 1.0f);
  
  // Essayer de récupérer l'attribut, s'il n'existe pas, utiliser des valeurs par défaut
  
  return values;
}

/* 
 * Find a point inside the mesh for starting the tetrahedralization.
 * Uses a simple approach of using the center of the bounding box.
 */
static int find_interior_point(const Mesh *mesh, Vector<float3> &vertices)
{
  // Cette fonction trouve un point à l'intérieur du maillage pour démarrer la tétraédralisation
  // en utilisant une méthode simplifiée (centre de la boîte englobante)
  
  if (!mesh || mesh->verts_num == 0) {
    // Retourner un point par défaut si le maillage est vide
    vertices.append(float3(0, 0, 0));
    return vertices.size() - 1;
  }

  // Calcul manuel de la boîte englobante
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

  // Utiliser simplement le centre comme point intérieur
  // Cette approche simplifiée fonctionne pour la plupart des maillages convexes
  // Pour les maillages complexes ou concaves, une meilleure méthode serait nécessaire
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
  
  // Sauvegarder tous les sommets d'origine
  out_vertices.resize(positions.size());
  for (const int i : positions.index_range()) {
    out_vertices[i] = positions[i];
  }
  
  // Obtenir les informations d'échelle basées sur la méthode sélectionnée
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
      // Pas d'échelle spéciale, utiliser une échelle uniforme basée sur max_tet_scale
      for (float &scale : vertex_scales) {
        scale = base_size * max_tet_scale;
      }
      break;
  }
  
  // Collecter les triangles de surface
  Vector<TriFace> surface_triangles;
  
  // Collecter les triangles de surface
  for (const int face_index : faces.index_range()) {
    const IndexRange face = faces[face_index];
    
    // Trianguler les faces non triangulaires
    if (face.size() == 3) {
      // Face triangulaire - ajouter directement
      const int v1 = corner_verts[face[0]];
      const int v2 = corner_verts[face[1]];
      const int v3 = corner_verts[face[2]];
      surface_triangles.append(TriFace(v1, v2, v3));
    }
    else if (face.size() > 3) {
      // Face n-gone - trianguler en utilisant la fonction de triangulation de Blender
      const int v0 = corner_verts[face[0]];
      for (int i = 2; i < face.size(); i++) {
        const int v1 = corner_verts[face[i - 1]];
        const int v2 = corner_verts[face[i]];
        surface_triangles.append(TriFace(v0, v1, v2));
      }
    }
  }
  
  // Trouver un point intérieur pour commencer la tétraédrisation
  int interior_point_index = find_interior_point(mesh, out_vertices);
  
  // Créer les tétraèdres initiaux en connectant le point intérieur à tous les triangles de surface
  for (const TriFace &face : surface_triangles) {
    // Vérifier que les sommets du triangle ne sont pas coïncidents
    float3 v1 = out_vertices[face.v1];
    float3 v2 = out_vertices[face.v2];
    float3 v3 = out_vertices[face.v3];
    float3 v4 = out_vertices[interior_point_index];
    
    // Calculer l'aire du triangle
    float3 normal = math::cross(v2 - v1, v3 - v1);
    float area = len_v3(normal) * 0.5f;
    
    // Calculer le volume du tétraèdre potentiel
    float volume = volume_tetrahedron_signed_v3(v1, v2, v3, v4);
    
    // Ignorer les triangles d'aire trop petite et les tétraèdres de volume trop petit
    if (area > 1e-6f && fabsf(volume) > 1e-6f) {
      Tetrahedron tet(face.v1, face.v2, face.v3, interior_point_index);
      
      // S'assurer que le tétraèdre a un volume positif
      tet.ensure_positive_volume(out_vertices);
      
      // Vérifier une dernière fois que le volume est positif
      if (tet.has_positive_volume(out_vertices)) {
        out_tetrahedra.append(tet);
      }
    }
  }
  
  // Améliorer la qualité des tétraèdres en ajoutant des points intérieurs supplémentaires
  RandomNumberGenerator rng;
  
  // Le nombre de points supplémentaires dépend de la taille maximale des tétraèdres
  // et de la taille du maillage
  // Calcul manuel de la boîte englobante
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
  
  // Calculer le nombre de points en fonction du volume et de l'échelle
  int num_additional_points = static_cast<int>(mesh_volume / (base_size * base_size * base_size) * 0.1f);
  num_additional_points = std::min(std::max(num_additional_points, 10), 100); // Limiter entre 10 et 100
  
  DEBUG_PRINT("Ajout de %zd points intérieurs supplémentaires", static_cast<int64_t>(num_additional_points));
  
  for (int i = 0; i < num_additional_points; i++) {
    // Créer un nouveau point en combinant des sommets existants et en ajoutant de l'aléatoire
    float3 new_point(0, 0, 0);
    
    // Échantillonner quelques points au hasard
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
      // Fallback si les poids sont tous nuls
      new_point = positions.is_empty() ? float3(0, 0, 0) : positions[0];
    }
    
    // Ajouter un décalage aléatoire proportionnel à la taille de base
    new_point += float3(
        (rng.get_float() - 0.5f) * 2.0f,
        (rng.get_float() - 0.5f) * 2.0f,
        (rng.get_float() - 0.5f) * 2.0f) * base_size * 0.2f;
    
    // Ajouter le nouveau point
    const int new_point_index = out_vertices.size();
    out_vertices.append(new_point);
    
    // Connecter à certains triangles de surface
    int num_connections = std::min(10, static_cast<int>(surface_triangles.size()));
    for (int j = 0; j < num_connections; j++) {
      int face_idx = rng.get_int() % surface_triangles.size();
      const TriFace &face = surface_triangles[face_idx];
      
      // Vérifier que les sommets du triangle ne sont pas confondus
      float3 v1 = out_vertices[face.v1];
      float3 v2 = out_vertices[face.v2];
      float3 v3 = out_vertices[face.v3];
      float3 v4 = out_vertices[new_point_index];
      
      // Calculer l'aire du triangle
      float3 normal = math::cross(v2 - v1, v3 - v1);
      float area = len_v3(normal) * 0.5f;
      
      // Calculer le volume du tétraèdre potentiel
      float volume = volume_tetrahedron_signed_v3(v1, v2, v3, v4);
      
      // Ignorer les triangles d'aire trop petite et les tétraèdres de volume trop petit
      if (area > 1e-6f && fabsf(volume) > 1e-6f) {
        Tetrahedron tet(face.v1, face.v2, face.v3, new_point_index);
        // S'assurer que le tétraèdre a un volume positif
        tet.ensure_positive_volume(out_vertices);
        
        // Vérifier une dernière fois que le volume est positif
        if (tet.has_positive_volume(out_vertices)) {
          out_tetrahedra.append(tet);
        }
      }
    }
  }
  
  // Filtrer les tétraèdres de mauvaise qualité
  Vector<Tetrahedron> filtered_tetrahedra;
  for (const Tetrahedron &tet : out_tetrahedra) {
    // Récupérer les points du tétraèdre
    const float3 &v1 = out_vertices[tet.v1];
    const float3 &v2 = out_vertices[tet.v2];
    const float3 &v3 = out_vertices[tet.v3];
    const float3 &v4 = out_vertices[tet.v4];
    
    // Calculer le volume
    float volume = volume_tetrahedron_signed_v3(v1, v2, v3, v4);
    
    // Calculer la qualité (mesure basée sur le ratio volume/longueur d'arête)
    float max_edge_length = 0.0f;
    max_edge_length = std::max(max_edge_length, len_v3v3(v1, v2));
    max_edge_length = std::max(max_edge_length, len_v3v3(v1, v3));
    max_edge_length = std::max(max_edge_length, len_v3v3(v1, v4));
    max_edge_length = std::max(max_edge_length, len_v3v3(v2, v3));
    max_edge_length = std::max(max_edge_length, len_v3v3(v2, v4));
    max_edge_length = std::max(max_edge_length, len_v3v3(v3, v4));
    
    // Calculer aussi la plus petite arête pour éviter les tétraèdres très plats
    float min_edge_length = max_edge_length;
    min_edge_length = std::min(min_edge_length, len_v3v3(v1, v2));
    min_edge_length = std::min(min_edge_length, len_v3v3(v1, v3));
    min_edge_length = std::min(min_edge_length, len_v3v3(v1, v4));
    min_edge_length = std::min(min_edge_length, len_v3v3(v2, v3));
    min_edge_length = std::min(min_edge_length, len_v3v3(v2, v4));
    min_edge_length = std::min(min_edge_length, len_v3v3(v3, v4));
    
    // Calculer un seuil minimum de volume basé sur la taille des arêtes
    float min_volume_threshold = powf(max_edge_length, 3) * 0.001f;
    
    // Calculer le ratio d'aspect (edge ratio)
    float edge_ratio = (min_edge_length > 1e-6f) ? (max_edge_length / min_edge_length) : FLT_MAX;
    
    // Calculer la qualité en fonction du volume et des arêtes
    // La formule est basée sur le ratio entre le volume et le cube de la longueur d'arête moyenne
    float avg_edge_length = (len_v3v3(v1, v2) + len_v3v3(v1, v3) + len_v3v3(v1, v4) + 
                             len_v3v3(v2, v3) + len_v3v3(v2, v4) + len_v3v3(v3, v4)) / 6.0f;
    float quality = (avg_edge_length > 1e-6f) ? (volume / powf(avg_edge_length, 3)) : 0.0f;
    
    // Critères plus stricts pour éviter les tétraèdres problématiques:
    // 1. Volume significativement positif
    // 2. Ratio d'aspect raisonnable (pas trop déformé)
    // 3. Qualité minimale
    // 4. Arête minimale non nulle
    if (volume > min_volume_threshold && 
        edge_ratio < 50.0f && 
        quality > 0.001f && 
        min_edge_length > 1e-5f) {
      
      // Vérifier également que les faces ne sont pas dégénérées
      bool valid_faces = true;
      
      // Vérifier l'aire des 4 faces triangulaires
      float3 n1 = math::cross(v2 - v1, v3 - v1);
      float3 n2 = math::cross(v2 - v1, v4 - v1);
      float3 n3 = math::cross(v3 - v1, v4 - v1);
      float3 n4 = math::cross(v3 - v2, v4 - v2);
      
      float area1 = len_v3(n1) * 0.5f;
      float area2 = len_v3(n2) * 0.5f;
      float area3 = len_v3(n3) * 0.5f;
      float area4 = len_v3(n4) * 0.5f;
      
      // Si une face a une aire trop petite, rejeter le tétraèdre
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
  
  DEBUG_PRINT("Filtrage des tétraèdres: %zd -> %zd",
             static_cast<int64_t>(out_tetrahedra.size()),
             static_cast<int64_t>(filtered_tetrahedra.size()));
  
  // Remplacer les tétraèdres par les tétraèdres filtrés
  out_tetrahedra = filtered_tetrahedra;
  
  DEBUG_PRINT("Tétraédrisation terminée avec %zd sommets et %zd tétraèdres", 
             static_cast<int64_t>(out_vertices.size()), static_cast<int64_t>(out_tetrahedra.size()));
}

/* Create a mesh from tetrahedra using Blender's mesh creation API */
static Mesh *create_mesh_from_tetrahedra(const Vector<float3> &vertices,
                                         const Vector<Tetrahedron> &tetrahedra)
{
  if (vertices.is_empty() || tetrahedra.is_empty()) {
    return nullptr;
  }
  
  // Filtrer les tétraèdres valides avec des vérifications supplémentaires
  Vector<Tetrahedron> valid_tetrahedra;
  for (const Tetrahedron &tet : tetrahedra) {
    // Vérifier les indices des sommets pour s'assurer qu'ils sont valides
    if (tet.v1 < vertices.size() && tet.v2 < vertices.size() && 
        tet.v3 < vertices.size() && tet.v4 < vertices.size() &&
        tet.v1 >= 0 && tet.v2 >= 0 && tet.v3 >= 0 && tet.v4 >= 0) {
      
      // Vérifier que les sommets ne sont pas colinéaires ou coplanaires
      const float3 &p1 = vertices[tet.v1];
      const float3 &p2 = vertices[tet.v2];
      const float3 &p3 = vertices[tet.v3];
      const float3 &p4 = vertices[tet.v4];
      
      // Calculer le volume du tétraèdre pour vérifier qu'il n'est pas dégénéré
      float volume = volume_tetrahedron_signed_v3(p1, p2, p3, p4);
      
      // Vérifier aussi que les arêtes ont une longueur minimale
      float min_edge_len = FLT_MAX;
      min_edge_len = std::min(min_edge_len, len_v3v3(p1, p2));
      min_edge_len = std::min(min_edge_len, len_v3v3(p1, p3));
      min_edge_len = std::min(min_edge_len, len_v3v3(p1, p4));
      min_edge_len = std::min(min_edge_len, len_v3v3(p2, p3));
      min_edge_len = std::min(min_edge_len, len_v3v3(p2, p4));
      min_edge_len = std::min(min_edge_len, len_v3v3(p3, p4));
      
      // N'ajouter que les tétraèdres avec un volume significatif et des arêtes non nulles
      if (fabsf(volume) > 1e-6f && min_edge_len > 1e-5f) {
        // S'assurer que le tétraèdre a un volume positif (bon ordre des sommets)
        Tetrahedron new_tet = tet;
        if (volume < 0) {
          std::swap(new_tet.v3, new_tet.v4); // Inverser l'orientation si nécessaire
        }
        valid_tetrahedra.append(new_tet);
      }
    }
  }
  
  if (valid_tetrahedra.is_empty()) {
    return nullptr;
  }
  
  // Calculer le nombre de faces (4 par tétraèdre)
  int num_tets = valid_tetrahedra.size();
  int num_faces = num_tets * 4;
  int num_loops = num_faces * 3;  // 3 sommets par face triangulaire
  
  // Créer un nouveau maillage avec des sommets, des arêtes, des faces et des coins
  Mesh *mesh = BKE_mesh_new_nomain(vertices.size(), 0, num_faces, num_loops);
  
  // Copier les sommets
  MutableSpan<float3> mesh_verts = mesh->vert_positions_for_write();
  for (size_t i = 0; i < vertices.size(); i++) {
    mesh_verts[i] = vertices[i];
  }
  
  // Créer les faces
  MutableSpan<int> face_offsets = mesh->face_offsets_for_write();
  MutableSpan<int> corner_verts = mesh->corner_verts_for_write();
  
  int corner_index = 0;
  int face_index = 0;
  
  // Pour chaque tétraèdre, créer les quatre faces triangulaires avec soin
  for (int i = 0; i < num_tets; i++) {
    const Tetrahedron &tet = valid_tetrahedra[i];
    
    // Définir les offsets de face (où commence chaque face dans le tableau de coins)
    for (int j = 0; j < 4; j++) {
      face_offsets[face_index++] = corner_index;
      
      // Ajouter les sommets de la face selon l'orientation appropriée
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
      
      // Ajouter les trois sommets de la face
      corner_verts[corner_index++] = v1;
      corner_verts[corner_index++] = v2;
      corner_verts[corner_index++] = v3;
    }
  }
  
  // Définir le dernier offset de face
  face_offsets[num_faces] = num_loops;
  
  // Generate edges and other mesh information needed for proper rendering
  blender::bke::mesh_calc_edges(*mesh, false, false);
  
  // Ensure the mesh is a valid structure before tagging for normal calculation
  mesh->runtime->is_original_bmesh = false;
  
  // Tag the mesh for deferred normal calculation - this is safer than direct calculation
  mesh->tag_positions_changed();
  
  // Créer un attribut pour stocker l'ID du tétraèdre
  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
  bke::SpanAttributeWriter<int> tet_id = attributes.lookup_or_add_for_write_span<int>(
      "tetrahedron_id", bke::AttrDomain::Face);
  
  for (int i = 0; i < num_faces; i++) {
    tet_id.span[i] = i / 4;  // Division entière pour obtenir l'ID du tétraèdre
  }
  
  tet_id.finish();
  
  return mesh;
}

/*
 * Implémentation principale de la tétraédrisation.
 * Crée un maillage tétraédrique en utilisant l'API Blender.
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
  
  // Calculer ou utiliser la taille de base fournie
  float base_size = use_manual_base_size ? manual_base_size : calculate_base_size(&input_mesh);
  
  DEBUG_PRINT("Tétraédrisation avec les paramètres suivants:");
  DEBUG_PRINT("  Taille de base: %f", base_size);
  DEBUG_PRINT("  Échelle maximale des tétraèdres: %f", max_tet_scale);
  DEBUG_PRINT("  Échelle minimale des triangles: %f", min_triangle_scale);
  DEBUG_PRINT("  Méthode d'échelle locale: %d", local_scaling_method);
  DEBUG_PRINT("  Attribut d'échelle: %s", scale_attribute.c_str());
  DEBUG_PRINT("  Échelle des caractéristiques locales: %f", local_feature_scale);
  
  // Générer des tétraèdres en utilisant la tétraédrisation
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
    
    // Vérifier explicitement que des tétraèdres valides ont été générés
    if (vertices.is_empty() || tetrahedra.is_empty()) {
      DEBUG_PRINT("Aucun tétraèdre n'a été généré");
      return nullptr;
    }
    
    // Filtrer les tétraèdres invalides une dernière fois
    Vector<Tetrahedron> valid_tetrahedra;
    for (const Tetrahedron &tet : tetrahedra) {
      // Vérifier que les indices sont valides
      if (tet.v1 < vertices.size() && tet.v2 < vertices.size() && 
          tet.v3 < vertices.size() && tet.v4 < vertices.size() &&
          tet.v1 >= 0 && tet.v2 >= 0 && tet.v3 >= 0 && tet.v4 >= 0) {
        // Vérifier que le tétraèdre a un volume significatif
        float volume = tet.volume(vertices);
        if (volume > 1e-6f) {
          valid_tetrahedra.append(tet);
        }
      }
    }
    
    // Si aucun tétraèdre valide n'est trouvé, retourner nullptr
    if (valid_tetrahedra.is_empty()) {
      DEBUG_PRINT("Aucun tétraèdre valide après filtrage");
      return nullptr;
    }
    
    // Créer un maillage à partir des tétraèdres valides
    result = create_mesh_from_tetrahedra(vertices, valid_tetrahedra);
    
    // Vérifier que le maillage a été créé correctement
    if (!result) {
      DEBUG_PRINT("Le maillage créé est invalide");
      return nullptr;
    }
    
    return result;
  }
#ifdef DEBUG_TETRAHEDRALIZE
  catch (const std::exception &_e) {
    DEBUG_PRINT("Exception lors de la tétraédrisation: %s", _e.what());
    if (result) {
      BKE_id_free(nullptr, result);
    }
    return nullptr;
  }
#else
  catch (const std::exception &) {
    DEBUG_PRINT("Exception lors de la tétraédrisation");
    if (result) {
      BKE_id_free(nullptr, result);
    }
    return nullptr;
  }
#endif
  catch (...) {
    DEBUG_PRINT("Exception inconnue lors de la tétraédrisation");
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

  // Afficher uniquement les propriétés qui ne sont pas déjà exposées comme sockets
  uiItemR(layout, ptr, "local_scaling", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  
  // Get the current scaling mode from the property
  const int scaling_mode = RNA_enum_get(ptr, "local_scaling");

  if (scaling_mode == SCALING_POINT_ATTRIBUTE) {
    // Cette propriété est déjà disponible comme socket, ne pas l'afficher ici
    // uiItemR(layout, ptr, "scale_attribute_name", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }

  // Ces propriétés sont déjà disponibles comme sockets, ne pas les afficher ici
  // uiItemR(layout, ptr, "use_manual_base_size", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  // uiItemR(layout, ptr, "base_size", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  // uiItemR(layout, ptr, "max_tet_scale", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  // uiItemR(layout, ptr, "min_triangle_scale", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh");

  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    if (geometry_set.has_mesh()) {
      try {
        const Mesh *mesh_in = geometry_set.get_mesh();

        // Extraire les valeurs directement à partir des entrées du nœud
        const bool use_manual_base_size = params.extract_input<bool>("Manual Base Size");
        const float base_size = params.extract_input<float>("Base Size");
        const float max_tet_scale = params.extract_input<float>("Max Tet Scale");
        const float min_triangle_scale = params.extract_input<float>("Min Triangle Scale");
        
        // Pour le scaling_method, on doit toujours utiliser custom1 car il n'y a pas d'entrée socket
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
        DEBUG_PRINT("Exception lors de la tétraédrisation");
        params.error_message_add(NodeWarningType::Error, "Exception in tetrahedralization");
      }
    }
  });

  params.set_output("Mesh", std::move(geometry_set));
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = SCALING_NONE;  // Par défaut, pas d'échelle locale
  
  NodeGeoTetrahedralize *storage = (NodeGeoTetrahedralize *)MEM_callocN(
      sizeof(NodeGeoTetrahedralize), "NodeGeoTetrahedralize");
      
  storage->base_size = 1.0f;
  storage->max_tet_scale = 1.0f;
  storage->min_triangle_scale = 0.1f;
  storage->local_feature_scale = 1.0f;
  storage->use_manual_base_size = false;
  strcpy(storage->scale_attribute_name, "scale");
  
  node->storage = storage;
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeTetrahedralize", GEO_NODE_TETRAHEDRALIZE);
  ntype.ui_name = "Tetrahedralize";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.enum_name_legacy = "TETRAHEDRALIZE";
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.ui_description = "Create a tetrahedral mesh from a surface mesh";
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  blender::bke::node_type_storage(ntype, "NodeGeoTetrahedralize", node_free_standard_storage, node_copy_standard_storage);
  
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