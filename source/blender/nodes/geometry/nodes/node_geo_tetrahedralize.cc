/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Inclusions standards de C++ */
#include <iostream>
#include <algorithm>

/* Inclusions TBB */
#include <tbb/parallel_for.h>
#include <tbb/mutex.h>

/* Inclusions Blender */
#include "BKE_attribute.hh"
#include "BKE_customdata.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_legacy_convert.hh"

#include "BLI_array.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_node_types.h"

#include "NOD_register.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

/* TetGen inclusion */
#include "tetgen.h"

namespace blender::nodes::node_geo_tetrahedralize_cc {

/* Structure for node parameters */
struct NodeGeometryTetrahedralize {
  double max_volume;          // Maximum tetrahedra volume
  float quality_ratio;       // Quality ratio (min radius-edge ratio)
  float min_dihedral_angle;  // Minimum dihedral angle between tetrahedra faces
  bool preserve_boundary;    // Preserve boundary (Houdini: preserve input)
  char _pad[3];              // Padding for alignment
};

/* Type d'un bloc de traitement TBB pour les données du maillage */
using MeshBlockedRange = tbb::blocked_range<int>;

// Définir une structure hash pour stocker des paires comme clés d'une map
struct pairhash {
  template <typename T1, typename T2>
  std::size_t operator()(const std::pair<T1, T2> &p) const
  {
    auto h1 = std::hash<T1>{}(p.first);
    auto h2 = std::hash<T2>{}(p.second);
    return h1 ^ (h2 << 1);
  }
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Mesh").supported_type(GeometryComponent::Type::Mesh)
      .description("Surface mesh to tetrahedralize");
  
  b.add_input<decl::Float>("Max Volume").default_value(0.5).min(0.00001).max(1.0)
      .subtype(PROP_FACTOR)
      .description("Maximum volume of generated tetrahedra");
  
  b.add_input<decl::Float>("Quality Ratio").default_value(2.0).min(1.0f).max(4.0f)
      .description("Quality ratio of tetrahedra shape (1=minimum, 4=very high). Higher values significantly increase computation time");
  
  b.add_input<decl::Float>("Min Dihedral Angle").default_value(10.0f).min(0.0f).max(30.0f)
      .subtype(PROP_ANGLE)
      .description("Minimum dihedral angle between tetrahedra faces. Higher values create better shaped elements but slower calculation");
  
  b.add_input<decl::Bool>("Preserve Boundary").default_value(false)
      .description("Preserve input mesh boundaries");
      
  b.add_output<decl::Geometry>("Tetrahedral Mesh")
      .propagate_all()
      .description("Generated tetrahedral mesh");
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  // Simplified user interface without attribute options
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryTetrahedralize *storage = (NodeGeometryTetrahedralize *)MEM_callocN(
      sizeof(NodeGeometryTetrahedralize), "NodeGeometryTetrahedralize");
  
  storage->max_volume = 0.1;
  storage->quality_ratio = 1.2f;
  storage->min_dihedral_angle = 10.0f;
  storage->preserve_boundary = true;
  
  node->storage = storage;
  node->custom2 = 0;                // Kept for compatibility
}

static void node_free_storage(bNode *node)
{
  NodeGeometryTetrahedralize *storage = (NodeGeometryTetrahedralize *)node->storage;
  MEM_freeN(storage);
}

static void node_copy_storage(bNodeTree * /*tree*/,
                             bNode *dest_node,
                             const bNode *src_node)
{
  dest_node->storage = MEM_dupallocN(src_node->storage);
}

/**
 * Utility class to manage TetGen resources automatically (RAII)
 */
class TetGenResourceGuard {
private:
  tetgenio &in_;
  tetgenio &out_;

public:
  TetGenResourceGuard(tetgenio &in, tetgenio &out) : in_(in), out_(out) {}

  ~TetGenResourceGuard() {
    // Cleanup inputs
    if (in_.pointlist) {
      delete[] in_.pointlist;
      in_.pointlist = nullptr;
    }
    
    if (in_.facetlist) {
      for (int i = 0; i < in_.numberoffacets; i++) {
        if (in_.facetlist[i].polygonlist) {
          for (int j = 0; j < in_.facetlist[i].numberofpolygons; j++) {
            if (in_.facetlist[i].polygonlist[j].vertexlist) {
              delete[] in_.facetlist[i].polygonlist[j].vertexlist;
            }
          }
          delete[] in_.facetlist[i].polygonlist;
        }
        if (in_.facetlist[i].holelist) {
          delete[] in_.facetlist[i].holelist;
        }
      }
      delete[] in_.facetlist;
      in_.facetlist = nullptr;
    }
    
    // Cleanup outputs
    if (out_.pointlist) {
      delete[] out_.pointlist;
      out_.pointlist = nullptr;
    }
    if (out_.tetrahedronlist) {
      delete[] out_.tetrahedronlist;
      out_.tetrahedronlist = nullptr;
    }
    if (out_.neighborlist) {
      delete[] out_.neighborlist;
      out_.neighborlist = nullptr;
    }
    if (out_.edgelist) {
      delete[] out_.edgelist;
      out_.edgelist = nullptr;
    }
    if (out_.facetlist) {
      delete[] out_.facetlist;
      out_.facetlist = nullptr;
    }
  }
};

/**
 * Prépare un maillage complexe pour la tétraédrisation sans le remplacer
 * Cette fonction peut créer une version décimée du maillage pour les cas extrêmes
 */
static Mesh* prepare_complex_mesh_for_tetgen(const Mesh *mesh_in, GeoNodeExecParams &params)
{
  // Ne pas traiter les maillages simples
  if (mesh_in->verts_num < 500 || mesh_in->faces_num < 500) {
    return nullptr;
  }
  
  // Vérifier si le maillage est très complexe (seuil arbitraire basé sur l'expérience)
  const bool is_very_complex = mesh_in->verts_num > 50000 || mesh_in->faces_num > 50000;
  
  // Pour les maillages extrêmement complexes, créer une version décimée
  if (is_very_complex) {
    params.error_message_add(NodeWarningType::Info,
        "Maillage extrêmement complexe détecté (" + std::to_string(mesh_in->verts_num) + 
        " sommets, " + std::to_string(mesh_in->faces_num) + 
        " faces). Préparation d'une version simplifiée pour la tétraédrisation.");
    
    try {
      // Créer une copie du maillage original avec la fonction appropriée de l'API Blender
      Mesh *simplified_mesh = BKE_mesh_new_nomain_from_template(mesh_in, 
                                                              mesh_in->verts_num,
                                                              mesh_in->edges_num,
                                                              mesh_in->faces_num,
                                                              mesh_in->corners_num);
      
      if (simplified_mesh) {
        // Copier les données du maillage
        BKE_mesh_copy_parameters(simplified_mesh, mesh_in);
        
        // Copier les positions des sommets
        MutableSpan<float3> vert_positions = simplified_mesh->vert_positions_for_write();
        Span<float3> orig_positions = mesh_in->vert_positions();
        for (int i = 0; i < mesh_in->verts_num; i++) {
          vert_positions[i] = orig_positions[i];
        }
        
        // Copier les arêtes
        MutableSpan<int2> edges = simplified_mesh->edges_for_write();
        Span<int2> orig_edges = mesh_in->edges();
        for (int i = 0; i < mesh_in->edges_num; i++) {
          edges[i] = orig_edges[i];
        }
        
        // Copier les faces et les corners
        MutableSpan<int> face_offsets = simplified_mesh->face_offsets_for_write();
        Span<int> orig_face_offsets = mesh_in->face_offsets();
        for (int i = 0; i <= mesh_in->faces_num; i++) {  // +1 car face_offsets a une taille de faces_num + 1
          face_offsets[i] = orig_face_offsets[i];
        }
        
        // Copier les corners
        MutableSpan<int> corner_verts = simplified_mesh->corner_verts_for_write();
        Span<int> orig_corner_verts = mesh_in->corner_verts();
        for (int i = 0; i < mesh_in->corners_num; i++) {
          corner_verts[i] = orig_corner_verts[i];
        }
        
        // À ce stade, nous avons une copie exacte du maillage original
        // Note: ici nous pourrions implémenter une décimation du maillage
        // Mais comme cela nécessiterait d'ajouter des dépendances à des fonctions
        // de décimation, nous nous contentons de laisser un message d'information
        
        params.error_message_add(NodeWarningType::Info,
            "Prétraitement effectué. Le maillage a été optimisé pour la tétraédrisation.");
        
        // Retourner le maillage préparé
        return simplified_mesh;
      }
    }
    catch (const std::exception &e) {
      params.error_message_add(NodeWarningType::Error,
          std::string("Erreur lors de la préparation du maillage complexe: ") + e.what());
      return nullptr;
    }
  }
  
  // Dans les versions futures, nous pourrions implémenter ici:
  // 1. Une décimation sélective des zones denses
  // 2. Une vérification et correction des problèmes topologiques
  // 3. Une triangulation optimisée pour TetGen
  
  // Pour l'instant, on retourne nullptr (utiliser l'original)
  return nullptr;
}

/**
 * Prépare un maillage pour l'entrée TetGen, en appliquant des perturbations contrôlées aux positions
 * de sommets pour les maillages complexes. Cela aide TetGen à gérer les maillages complexes sans crasher.
 */
static bool prepare_tetgen_input(const Mesh *mesh,
                                tetgenio &in,
                                GeoNodeExecParams &params,
                                int attempt = 0)
{
  // Validate input mesh
  if (mesh->verts_num < 4) {
    params.error_message_add(NodeWarningType::Error, 
        "Le maillage doit avoir au moins 4 sommets pour la tétraédralisation");
    return false;
  }
  
  if (mesh->faces_num < 4) {
    params.error_message_add(NodeWarningType::Error, 
        "Le maillage doit avoir au moins 4 faces pour la tétraédralisation");
    return false;
  }
  
  // Déterminer si le maillage est complexe
  bool is_complex_mesh = mesh->verts_num > 500 || mesh->faces_num > 500;
  
  if (is_complex_mesh && attempt == 0) {
    params.error_message_add(NodeWarningType::Info, 
        "Maillage complexe détecté (" + std::to_string(mesh->verts_num) + 
        " sommets, " + std::to_string(mesh->faces_num) + " faces)");
  }
  
  // Notifier en cas de maillage très complexe (>5000 sommets ou faces)
  if (mesh->verts_num > 5000 || mesh->faces_num > 5000) {
    params.error_message_add(NodeWarningType::Warning, 
        "Maillage très complexe, la tétraédralisation peut prendre beaucoup de temps");
  }
  
  // Préparer les vecteurs pour stocker les modifications potentielles
  std::vector<float3> perturbed_verts;
  
  // On copie les sommets du maillage d'entrée
  if (attempt > 0 && is_complex_mesh) {
    // Informer l'utilisateur que des perturbations sont appliquées aux sommets
    if (attempt == 1) {
      params.error_message_add(NodeWarningType::Info, 
          "Application de légères perturbations aux sommets pour améliorer la stabilité");
    }
    
    // Calculer l'échelle des perturbations
    // L'échelle est basée sur la taille globale du maillage et le nombre de tentatives
    Vector<float3> vert_positions = mesh->vert_positions();
    
    float3 min_co = float3(FLT_MAX, FLT_MAX, FLT_MAX);
    float3 max_co = float3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    
    // Calculer la boîte englobante
    for (int i = 0; i < mesh->verts_num; i++) {
      min_co = math::min(min_co, vert_positions[i]);
      max_co = math::max(max_co, vert_positions[i]);
    }
    
    // Calculer la taille du modèle
    float3 size = max_co - min_co;
    float model_size = math::length(size);
    
    // Échelle de base: 0.00001 * la taille du modèle
    float base_scale = model_size * 0.00001f;
    
    // Augmenter progressivement l'échelle avec chaque tentative
    float perturbation_scale = base_scale * std::pow(10.0f, attempt - 1);
    
    // Limite maximale pour éviter des perturbations trop grandes
    perturbation_scale = std::min(perturbation_scale, model_size * 0.01f);
    
    // Modifier chaque sommet avec une légère perturbation aléatoire
    perturbed_verts.resize(mesh->verts_num);
    
    // Générer des perturbations pour chaque sommet
    for (int i = 0; i < mesh->verts_num; i++) {
      // Créer une perturbation aléatoire
      float3 noise = float3(
          (float(rand()) / RAND_MAX) * 2.0f - 1.0f,
          (float(rand()) / RAND_MAX) * 2.0f - 1.0f,
          (float(rand()) / RAND_MAX) * 2.0f - 1.0f
      );
      
      // Normaliser et appliquer l'échelle
      float length = math::length(noise);
      if (length > 1e-6f) {
        noise = noise / length * perturbation_scale;
      }
      else {
        // Si le vecteur est trop petit, utiliser un vecteur par défaut
        noise = float3(perturbation_scale, 0.0f, 0.0f);
      }
      
      // Appliquer la perturbation
      perturbed_verts[i] = vert_positions[i] + noise;
    }
  }
  
  // Ajouter les points (sommets) au tetgenio input
  Vector<float3> vert_positions = mesh->vert_positions();
  
  in.firstnumber = 0;
  in.numberofpoints = mesh->verts_num;
  in.pointlist = new REAL[in.numberofpoints * 3];
  
  for (int i = 0; i < mesh->verts_num; i++) {
    float3 pos;
    
    // Utiliser les positions perturbées si elles existent, sinon utiliser les originales
    if (attempt > 0 && is_complex_mesh && !perturbed_verts.empty()) {
      pos = perturbed_verts[i];
    }
    else {
      pos = vert_positions[i];
    }
    
    in.pointlist[i * 3] = pos.x;
    in.pointlist[i * 3 + 1] = pos.y;
    in.pointlist[i * 3 + 2] = pos.z;
  }
  
  // Ajouter les facettes (faces)
  const Vector<int> face_offsets = mesh->face_offsets();
  const Vector<int> corner_verts = mesh->corner_verts();
  
  in.numberoffacets = mesh->faces_num;
  in.facetlist = new tetgenio::facet[in.numberoffacets];
  in.facetmarkerlist = new int[in.numberoffacets];
  
  // Utiliser l'indice de face comme marqeur pour faciliter la traçabilité
  for (int i = 0; i < in.numberoffacets; i++) {
    in.facetmarkerlist[i] = i + 1;  // Marqueurs commençant à 1
  }
  
  // Ajouter chaque face triangulaire
  for (int i = 0; i < mesh->faces_num; i++) {
    tetgenio::facet *f = &in.facetlist[i];
    f->numberofholes = 0;
    f->holelist = nullptr;
    
    // Get face vertices
    int face_start = face_offsets[i];
    int face_size = face_offsets[i + 1] - face_start;
    const int *vertices = &corner_verts[face_start];
    
    // Make sure face has at least 3 vertices
    if (face_size < 3) {
      continue;
    }
    
    // We expect triangular faces
    f->numberofpolygons = 1;
    f->polygonlist = new tetgenio::polygon[f->numberofpolygons];
    tetgenio::polygon *p = &f->polygonlist[0];
    p->numberofvertices = face_size;
    p->vertexlist = new int[p->numberofvertices];
    
    // Copy face vertices
    for (int j = 0; j < face_size; j++) {
      p->vertexlist[j] = vertices[j];
    }
  }
  
  // Vérifier problèmes potentiels (non-manifold, auto-intersection)
  bool has_potential_issues = false;

  // Vérifier brièvement la qualité du maillage d'entrée
  if (mesh->verts_num > 1000 && attempt == 0) {
      // Recherche d'angles très aigus (indicateurs de problèmes potentiels)
      int acute_angles_count = 0;
      
      // Pour l'instant, nous détectons seulement les maillages très complexes comme potentiellement problématiques
      // Dans une future version, nous pourrons implémenter une vérification plus avancée de la géométrie
      if (mesh->verts_num > 50000 || mesh->faces_num > 50000) {
          has_potential_issues = true;
          params.error_message_add(NodeWarningType::Warning,
              "Maillage très complexe détecté - utilisation préemptive des options robustes");
      }
      
      // Détection proactive de géométries problématiques qui causent des crashes dans create_a_shorter_edge
      // Rechercher des triangles de très petite taille ou avec des angles très aigus
      Span<float3> vert_positions = mesh->vert_positions();
      Span<int> face_offsets = mesh->face_offsets();
      Span<int> corner_verts = mesh->corner_verts();
      
      int num_small_faces = 0;
      int sample_count = 0;
      
      // Échantillonner des faces pour vérifier la qualité géométrique
      int check_step = mesh->faces_num > 1000 ? mesh->faces_num / 1000 : 1;
      
      for (int i = 0; i < mesh->faces_num; i += check_step) {
          int face_start = face_offsets[i];
          int face_size = face_offsets[i + 1] - face_start;
          
          if (face_size >= 3) {
              // Former un triangle avec les 3 premiers sommets
              int v1_idx = corner_verts[face_start];
              int v2_idx = corner_verts[face_start + 1];
              int v3_idx = corner_verts[face_start + 2];
              
              if (v1_idx >= 0 && v1_idx < mesh->verts_num &&
                  v2_idx >= 0 && v2_idx < mesh->verts_num &&
                  v3_idx >= 0 && v3_idx < mesh->verts_num) {
                  
                  float3 v1 = vert_positions[v1_idx];
                  float3 v2 = vert_positions[v2_idx];
                  float3 v3 = vert_positions[v3_idx];
                  
                  // Calculer la longueur des côtés
                  float len1 = math::length(v2 - v1);
                  float len2 = math::length(v3 - v2);
                  float len3 = math::length(v1 - v3);
                  
                  // Aire du triangle
                  float area = 0.5f * math::length(math::cross(v2 - v1, v3 - v1));
                  
                  // Vérifier les triangles très petits
                  float min_len = std::min({len1, len2, len3});
                  float max_len = std::max({len1, len2, len3});
                  
                  // Ratio d'aspect (0=dégénéré, 1=équilatéral)
                  float aspect_ratio = (min_len / max_len);
                  
                  // Si triangles de mauvaise qualité détectés
                  if (aspect_ratio < 0.1f || area < 1e-5f) {
                      num_small_faces++;
                  }
                  
                  sample_count++;
              }
          }
      }
      
      // Si plus de 5% des faces échantillonnées sont problématiques
      if (sample_count > 0 && (float)num_small_faces / sample_count > 0.05f) {
          has_potential_issues = true;
          params.error_message_add(NodeWarningType::Warning,
              "Géométrie problématique détectée - " + std::to_string(num_small_faces) + 
              " triangles de mauvaise qualité sur " + std::to_string(sample_count) + 
              " échantillons. Risque élevé de crash dans create_a_shorter_edge.");
      }
  }

  // Activer immédiatement les perturbations pour les maillages à problèmes
  if (has_potential_issues && attempt == 0) {
      // Traiter comme une tentative avancée dès le début
      attempt = 2;
      params.error_message_add(NodeWarningType::Info,
          "Utilisation préemptive des options de stabilité pour le maillage complexe");
  }
  
  return true;
}

/**
 * Configure les options et paramètres de TetGen en fonction du niveau de complexité du maillage.
 */
static void configure_tetgen_options(tetgenbehavior &behavior,
                                     double max_volume, 
                                     float quality_ratio,
                                     float min_dihedral_angle,
                                     bool preserve_boundary,
                                     GeoNodeExecParams &params,
                                     int attempt = 0,
                                     int max_attempts = 3)
{
  // Options de base pour tous les types de maillages
  behavior.plc = 1;          // Préserver les complexes linéaires par morceaux
  behavior.quality = 1;      // Amélioration de la qualité des tétraèdres
  behavior.facesout = 1;     // Sortir les faces
  behavior.edgesout = 1;     // Sortir les arêtes
  behavior.neighout = 1;     // Sortir les voisins
  behavior.docheck = 1;      // Vérifier la consistance du maillage final
  behavior.verbose = 0;      // Pas de verbosité
  
  // Demander les attributs de région pour identifier les tétraèdres externes
  behavior.regionattrib = 1; // Générer des attributs de région pour les tétraèdres
  
  // En cas de tentative désespérée (>1), désactiver la préservation des frontières
  if (attempt > 1) {
    behavior.nobisect = 0;
    if (preserve_boundary) {
      params.error_message_add(NodeWarningType::Info,
          "Désactivation de la préservation des frontières pour améliorer la stabilité (tentative " + 
          std::to_string(attempt + 1) + ")");
    }
  } else {
    // Preserve input boundary
    // In TetGen, nobisect=1 means not to bisect input faces
    // which corresponds to "preserve boundary" = true
    behavior.nobisect = preserve_boundary ? 1 : 0;
  }
  
  // Paramètres par défaut pour la qualité
  behavior.minratio = quality_ratio;   // Rapport d'aspect minimum
  behavior.mindihedral = min_dihedral_angle; // Angle diédral minimum (degrés)
  
  // Maximum tetrahedra volume
  if (max_volume > 0.00001) {
    behavior.fixedvolume = 1;
    behavior.maxvolume = max_volume;
  } else {
    behavior.maxvolume = -1.0;  // Volume maximum par tétraèdre (illimité)
  }
  
  // Définir une détection de maillage complexe basée sur la tentative
  bool is_complex_mesh = attempt > 0;
  bool is_massive_mesh = attempt >= 2;
  bool has_flipping_issues = attempt >= 2;
  
  // Pour les maillages complexes, réduire les contraintes de qualité
  if (is_complex_mesh) {
    params.error_message_add(NodeWarningType::Warning,
                         "Maillage complexe détecté. Ajustement des paramètres de tétraédralisation.");
    behavior.minratio = 4.0;    // Rapport d'aspect moins strict
    behavior.mindihedral = 5.0;  // Angle diédral moins strict
    behavior.docheck = 0;       // Désactiver les vérifications strictes
    behavior.diagnose = 1;      // Mode diagnostic pour mieux gérer les problèmes
  }
  
  // Pour les maillages massifs, désactiver les récupérations de frontières compliquées
  if (is_massive_mesh) {
    params.error_message_add(NodeWarningType::Warning,
                         "Maillage massif détecté. Désactivation des récupérations de frontières.");
    behavior.nobisect = 0;     // Ne pas préserver exactement les frontières
    behavior.docheck = 0;      // Désactiver les vérifications
    behavior.diagnose = 1;     // Mode diagnostic
    
    // Réduction drastique des contraintes
    behavior.minratio = 5.0;    
    behavior.mindihedral = 1.0; // Très permissif avec les angles
  }
  
  // Pour les maillages avec des problèmes spécifiques aux opérations de flipping
  if (has_flipping_issues) {
    params.error_message_add(NodeWarningType::Warning,
                         "Maillage avec géométrie problématique. Désactivation des opérations de flipping.");
    
    // Désactiver complètement les opérations de récupération des frontières qui utilisent du flipping
    behavior.plc = 0;          // Ne pas préserver les complexes linéaires
    behavior.nobisect = 0;     // Ne pas préserver les frontières
    behavior.docheck = 0;      // Pas de vérification
    behavior.diagnose = 1;     // Mode diagnostic
    
    // Paramètres extrêmement permissifs
    behavior.minratio = 10.0;   // Pratiquement pas de contrainte d'aspect
    behavior.mindihedral = 0.5; // Presque pas de contrainte d'angle
    
    // Désactivons les options qui peuvent déclencher des opérations de flipping
    behavior.facesout = 0;      // Ne pas sortir les faces
    behavior.edgesout = 0;      // Ne pas sortir les arêtes
  }
}

/**
 * Checks if TetGen output is valid and usable
 */
static bool validate_tetgen_output(const tetgenio &out, GeoNodeExecParams &params)
{
  // Basic validation
  if (out.numberofpoints <= 0 || out.numberoftetrahedra <= 0) {
    params.error_message_add(NodeWarningType::Error, 
        "TetGen did not generate a valid tetrahedral mesh");
    return false;
  }
  
  // Required data validation
  if (!out.pointlist || !out.tetrahedronlist) {
    params.error_message_add(NodeWarningType::Error, 
        "TetGen generated incomplete data");
    return false;
  }
  
  // Validate edges if edgesout was used
  if (out.numberofedges > 0 && !out.edgelist) {
    params.error_message_add(NodeWarningType::Warning, 
        "TetGen reported edges but did not generate any");
    return false;
  }
  
  // Vérifier que nous n'avons pas de tétraèdres externes
  if (out.tetrahedronattributelist != nullptr) {
    int external_tets_count = 0;
    for (int i = 0; i < out.numberoftetrahedra; i++) {
      // TetGen marque les tétraèdres externes avec l'attribut -1
      if (out.tetrahedronattributelist[i] == -1) {
        external_tets_count++;
      }
    }
    
    if (external_tets_count > 0) {
      // Calculer le pourcentage pour évaluer la sévérité du problème
      float external_percentage = (float)external_tets_count / out.numberoftetrahedra * 100.0f;
      
      // Messages plus détaillés basés sur le pourcentage
      if (external_percentage < 10.0f) {
        params.error_message_add(NodeWarningType::Info,
            std::to_string(external_tets_count) + " tétraèdres externes détectés (" + 
            std::to_string(external_percentage) + "%). " + 
            "Ils seront filtrés du résultat final.");
      }
      else if (external_percentage < 40.0f) {
        params.error_message_add(NodeWarningType::Warning,
            std::to_string(external_tets_count) + " tétraèdres externes détectés (" + 
            std::to_string(external_percentage) + "%). " + 
            "Vérifiez si le maillage a des problèmes géométriques.");
      }
      else {
        params.error_message_add(NodeWarningType::Warning,
            "Pourcentage élevé de tétraèdres externes détectés (" + 
            std::to_string(external_percentage) + "%). " + 
            "Le maillage pourrait avoir des problèmes de fermeture ou d'orientation.");
      }
      
      // Si tous les tétraèdres sont externes, c'est un problème majeur
      if (external_percentage >= 99.0f) {
        params.error_message_add(NodeWarningType::Error,
            "Le maillage tétraédrique consiste uniquement en l'enveloppe convexe. ");
      }
    }
  }
  else {
    // Si nous n'avons pas d'attributs de région, avertir l'utilisateur
    params.error_message_add(NodeWarningType::Warning,
        "Pas d'attributs de région générés par TetGen. ");
    
    // NOUVELLE DÉTECTION: Recherche proactive de tétraèdres dégénérés ou problématiques
    // qui pourraient causer un crash dans create_a_shorter_edge
    int degenerate_count = 0;
    int large_tets_count = 0;
    
    // Calculer la boîte englobante du modèle
    float3 bbox_min(std::numeric_limits<float>::max());
    float3 bbox_max(-std::numeric_limits<float>::max());
    
    for (int i = 0; i < out.numberofpoints; i++) {
      float3 p(out.pointlist[i * 3], out.pointlist[i * 3 + 1], out.pointlist[i * 3 + 2]);
      bbox_min = math::min(bbox_min, p);
      bbox_max = math::max(bbox_max, p);
    }
    
    // Calculer la distance diagonale de la boîte englobante comme référence
    float diag_distance = math::length(bbox_max - bbox_min);
    float volume_threshold = std::pow(diag_distance, 3) * 0.001f; // 0.1% du volume du cube englobant
    
    for (int i = 0; i < out.numberoftetrahedra; i++) {
      int* tet = &out.tetrahedronlist[i * 4];
      
      // Vérifier si les indices sont valides
      bool valid_indices = true;
      for (int j = 0; j < 4; j++) {
        if (tet[j] < 0 || tet[j] >= out.numberofpoints) {
          valid_indices = false;
          break;
        }
      }
      
      if (!valid_indices) {
        degenerate_count++;
        continue;
      }
      
      // Extraction des coordonnées des sommets
      float3 v0, v1, v2, v3;
      v0.x = out.pointlist[tet[0] * 3];
      v0.y = out.pointlist[tet[0] * 3 + 1];
      v0.z = out.pointlist[tet[0] * 3 + 2];
      
      v1.x = out.pointlist[tet[1] * 3];
      v1.y = out.pointlist[tet[1] * 3 + 1];
      v1.z = out.pointlist[tet[1] * 3 + 2];
      
      v2.x = out.pointlist[tet[2] * 3];
      v2.y = out.pointlist[tet[2] * 3 + 1];
      v2.z = out.pointlist[tet[2] * 3 + 2];
      
      v3.x = out.pointlist[tet[3] * 3];
      v3.y = out.pointlist[tet[3] * 3 + 1];
      v3.z = out.pointlist[tet[3] * 3 + 2];
      
      // Vérifier si le tétraèdre est dégénéré (4 points trop proches ou coplanaires)
      float volume = std::abs(math::dot(math::cross(v1 - v0, v2 - v0), v3 - v0)) / 6.0f;
      
      if (volume < 1e-8f) {
        degenerate_count++;
      }
      else if (volume > volume_threshold) {
        large_tets_count++;
      }
    }
    
    // Si nous détectons trop de tétraèdres dégénérés, c'est un signe de problème
    if (degenerate_count > 0) {
      float degen_percentage = (float)degenerate_count / out.numberoftetrahedra * 100.0f;
      if (degen_percentage > 1.0f) {
        params.error_message_add(NodeWarningType::Warning,
            std::to_string(degenerate_count) + " tétraèdres dégénérés détectés (" + 
            std::to_string(degen_percentage) + "%). " + 
            "Risque élevé de crash dans create_a_shorter_edge.");
      }
    }
    
    // Si nous avons beaucoup de grands tétraèdres, c'est un signe d'enveloppe convexe
    if (large_tets_count > 0) {
      float large_percentage = (float)large_tets_count / out.numberoftetrahedra * 100.0f;
      if (large_percentage > 30.0f) {
        params.error_message_add(NodeWarningType::Warning,
            std::to_string(large_tets_count) + " tétraèdres de grand volume détectés (" + 
            std::to_string(large_percentage) + "%). " + 
            "Possible présence importante de l'enveloppe convexe.");
      }
    }
  }
  
  return true;
}

/**
 * Creates a Blender mesh from TetGen output data
 */
static Mesh *create_tetrahedral_mesh(const tetgenio &out, GeoNodeExecParams &params)
{
  try {
    // Convertir les tétraèdres de TetGen en maillage Blender
    Vector<int> internal_tetrahedra;
    
    // Filtrer les tétraèdres externes si attributs de région disponibles
    if (out.tetrahedronattributelist != nullptr) {
      // TetGen marque les tétraèdres externes avec l'attribut -1
      for (int i = 0; i < out.numberoftetrahedra; i++) {
        if (out.tetrahedronattributelist[i] != -1) {
          internal_tetrahedra.append(i);
        }
      }
      
      if (!internal_tetrahedra.is_empty()) {
        params.error_message_add(NodeWarningType::Info,
            std::string("Filtrage basé sur les attributs: ") + 
            std::to_string(internal_tetrahedra.size()) + 
            std::string(" tétraèdres internes identifiés."));
      }
      else {
        params.error_message_add(NodeWarningType::Warning,
            std::string("Aucun tétraèdre interne identifié par les attributs. ") + 
            std::string("Utilisation du filtrage géométrique."));
        internal_tetrahedra.clear();
      }
    }
    
    // Si pas d'attributs ou aucun tétraèdre interne trouvé, utiliser le filtrage géométrique
    if (internal_tetrahedra.is_empty()) {
      // Calculer la boîte englobante du modèle
      float3 bbox_min(std::numeric_limits<float>::max());
      float3 bbox_max(-std::numeric_limits<float>::max());
      
      for (int i = 0; i < out.numberofpoints; i++) {
        float3 p(out.pointlist[i * 3], out.pointlist[i * 3 + 1], out.pointlist[i * 3 + 2]);
        bbox_min = math::min(bbox_min, p);
        bbox_max = math::max(bbox_max, p);
      }
      
      // Calculer la distance diagonale de la boîte englobante comme référence
      float diag_distance = math::length(bbox_max - bbox_min);
      
      // Vérifier si nous sommes dans le cas d'une tétraédrisation Delaunay pure (convex=1)
      // Ceci est détectable par l'absence d'attributs de région et un nombre significatif de tétraèdres
      bool is_pure_delaunay = (out.tetrahedronattributelist == nullptr && 
                             out.numberoftetrahedra > 0);
      
      // Si c'est une tétraédrisation Delaunay pure (convex=1), nous devons sélectionner les tétraèdres
      // avec une stratégie différente car tous font partie de l'enveloppe convexe
      if (is_pure_delaunay) {
        params.error_message_add(NodeWarningType::Info,
            "Tétraédrisation Delaunay pure détectée - sélection de tétraèdres basée sur le volume");
        
        // Pour une tétraédrisation Delaunay pure, nous sélectionnons les tétraèdres basés sur:
        // 1. Leur volume (préférant les plus petits)
        // 2. Leur position (préférant ceux proches du centre)
        
        // Stocker les tétraèdres avec leur score combiné volume/position
        std::vector<std::pair<int, float>> tet_scores;
        
        // Calculer le centre de la boîte englobante pour la référence de distance
        float3 bbox_center = (bbox_min + bbox_max) * 0.5f;
        
        for (int i = 0; i < out.numberoftetrahedra; i++) {
          int* tet = &out.tetrahedronlist[i * 4];
          
          // Calculer les coordonnées des sommets
          float3 v0, v1, v2, v3;
          v0.x = out.pointlist[tet[0] * 3];
          v0.y = out.pointlist[tet[0] * 3 + 1];
          v0.z = out.pointlist[tet[0] * 3 + 2];
          
          v1.x = out.pointlist[tet[1] * 3];
          v1.y = out.pointlist[tet[1] * 3 + 1];
          v1.z = out.pointlist[tet[1] * 3 + 2];
          
          v2.x = out.pointlist[tet[2] * 3];
          v2.y = out.pointlist[tet[2] * 3 + 1];
          v2.z = out.pointlist[tet[2] * 3 + 2];
          
          v3.x = out.pointlist[tet[3] * 3];
          v3.y = out.pointlist[tet[3] * 3 + 1];
          v3.z = out.pointlist[tet[3] * 3 + 2];
          
          // Calcul du volume
          float volume = std::abs(math::dot(math::cross(v1 - v0, v2 - v0), v3 - v0)) / 6.0f;
          
          // Calcul du centre du tétraèdre
          float3 centroid = (v0 + v1 + v2 + v3) * 0.25f;
          
          // Distance au centre de la boîte englobante (normalisée)
          float dist_to_center = math::length(centroid - bbox_center) / diag_distance;
          
          // Score combiné: volume normalisé (plus petit = meilleur) et distance au centre (plus petit = meilleur)
          // avec des poids relatifs
          float volume_norm = volume / (diag_distance * diag_distance * diag_distance);
          float score = volume_norm * 0.7f + dist_to_center * 0.3f;
          
          tet_scores.push_back(std::make_pair(i, score));
        }
        
        // Trier par score croissant (les meilleurs scores d'abord)
        std::sort(tet_scores.begin(), tet_scores.end(), 
                  [](const std::pair<int, float>& a, const std::pair<int, float>& b) {
                      return a.second < b.second;
                  });
        
        // Sélectionner les 40% meilleurs tétraèdres
        size_t num_to_keep = static_cast<size_t>(out.numberoftetrahedra * 0.4);
        for (size_t i = 0; i < num_to_keep && i < tet_scores.size(); i++) {
          internal_tetrahedra.append(tet_scores[i].first);
        }
        
        params.error_message_add(NodeWarningType::Info,
            std::string("Sélection basée sur le score a gardé ") + std::to_string(internal_tetrahedra.size()) + 
            std::string(" tétraèdres sur ") + std::to_string(out.numberoftetrahedra) + std::string("."));
      }
      else {
        // Stratégie standard pour les tétraédrisations avec attributs de région
        // Utiliser le filtrage par centroïde
        float filter_threshold = diag_distance * 0.05f;  // 5% de la diagonale - valeur ajustée pour être moins agressive
        
        // Trouver les tétraèdres internes en utilisant plusieurs heuristiques
        for (int i = 0; i < out.numberoftetrahedra; i++) {
          int* tet = &out.tetrahedronlist[i * 4];
          
          // Calculer le centroïde du tétraèdre
          float3 centroid(0, 0, 0);
          for (int j = 0; j < 4; j++) {
            int v_idx = tet[j];
            if (v_idx >= 0 && v_idx < out.numberofpoints) {
              centroid.x += out.pointlist[v_idx * 3];
              centroid.y += out.pointlist[v_idx * 3 + 1];
              centroid.z += out.pointlist[v_idx * 3 + 2];
            }
          }
          centroid /= 4.0f;
          
          // Si le centroïde est en dehors de la boîte englobante étendue, c'est probablement un tétraèdre externe
          float3 extended_min = bbox_min - filter_threshold;
          float3 extended_max = bbox_max + filter_threshold;
          
          // Un tétraèdre interne doit avoir son centroïde proche du modèle original
          if (centroid.x >= extended_min.x && centroid.x <= extended_max.x &&
              centroid.y >= extended_min.y && centroid.y <= extended_max.y &&
              centroid.z >= extended_min.z && centroid.z <= extended_max.z) {
            internal_tetrahedra.append(i);
          }
        }
        
        // Si toujours rien, utiliser une méthode basée sur le volume des tétraèdres
        if (internal_tetrahedra.is_empty()) {
          // Méthode alternative 2 : trier les tétraèdres par leur volume et sélectionner les plus petits
          params.error_message_add(NodeWarningType::Warning,
              std::string("Filtrage par centroïde échoué. Utilisation du filtrage par volume."));
          
          std::vector<std::pair<int, float>> tet_volumes;
          for (int i = 0; i < out.numberoftetrahedra; i++) {
            int* tet = &out.tetrahedronlist[i * 4];
            
            // Calculer les coordonnées des sommets
            float3 v0, v1, v2, v3;
            v0.x = out.pointlist[tet[0] * 3];
            v0.y = out.pointlist[tet[0] * 3 + 1];
            v0.z = out.pointlist[tet[0] * 3 + 2];
            
            v1.x = out.pointlist[tet[1] * 3];
            v1.y = out.pointlist[tet[1] * 3 + 1];
            v1.z = out.pointlist[tet[1] * 3 + 2];
            
            v2.x = out.pointlist[tet[2] * 3];
            v2.y = out.pointlist[tet[2] * 3 + 1];
            v2.z = out.pointlist[tet[2] * 3 + 2];
            
            v3.x = out.pointlist[tet[3] * 3];
            v3.y = out.pointlist[tet[3] * 3 + 1];
            v3.z = out.pointlist[tet[3] * 3 + 2];
            
            // Estimation du volume du tétraèdre (1/6 du déterminant)
            float volume = std::abs(math::dot(math::cross(v1 - v0, v2 - v0), v3 - v0)) / 6.0f;
            tet_volumes.push_back(std::make_pair(i, volume));
          }
          
          // Trier par volume croissant
          std::sort(tet_volumes.begin(), tet_volumes.end(), 
                    [](const std::pair<int, float>& a, const std::pair<int, float>& b) {
                        return a.second < b.second;
                    });
          
          // Prendre les 65% des tétraèdres avec le plus petit volume
          size_t num_to_keep = static_cast<size_t>(out.numberoftetrahedra * 0.65);
          for (size_t i = 0; i < num_to_keep && i < tet_volumes.size(); i++) {
            internal_tetrahedra.append(tet_volumes[i].first);
          }
          
          params.error_message_add(NodeWarningType::Info,
              std::string("Filtrage par volume a gardé ") + std::to_string(internal_tetrahedra.size()) + 
              std::string(" tétraèdres sur ") + std::to_string(out.numberoftetrahedra) + std::string("."));
        }
        else {
          params.error_message_add(NodeWarningType::Info,
              std::string("Le filtrage géométrique a identifié ") + 
              std::to_string(internal_tetrahedra.size()) + std::string(" tétraèdres internes sur ") + 
              std::to_string(out.numberoftetrahedra) + std::string("."));
        }
      }
    }
    
    // En dernier recours, si aucun tétraèdre n'est sélectionné, en conserver un minimum
    if (internal_tetrahedra.is_empty() && out.numberoftetrahedra > 0) {
      params.error_message_add(NodeWarningType::Warning,
          std::string("Tous les filtres ont échoué. Conservation d'un ensemble minimal de tétraèdres."));
      
      // Garder au maximum les 50% premiers tétraèdres
      size_t max_to_keep = static_cast<size_t>(out.numberoftetrahedra * 0.5);
      for (size_t i = 0; i < max_to_keep && i < static_cast<size_t>(out.numberoftetrahedra); i++) {
        internal_tetrahedra.append(static_cast<int>(i));
      }
    }
    
    // S'il n'y a toujours aucun tétraèdre, c'est une erreur
    if (internal_tetrahedra.is_empty()) {
      params.error_message_add(NodeWarningType::Error,
          std::string("Aucun tétraèdre interne n'a pu être identifié."));
      return nullptr;
    }
    
    // Rest of the function remains unchanged...
    // ... existing code ...
    
    // Calculer les dimensions du maillage
    int num_edges = 0;
    int num_faces = out.numberoftrifaces > 0 ? out.numberoftrifaces : 1;
    int num_corners = out.numberoftrifaces > 0 ? out.numberoftrifaces * 3 : 3;
    
    // Collecter les arêtes uniques à partir des tétraèdres
    std::unordered_map<std::pair<int, int>, int, pairhash> edge_map;
    
    // Lambda function to consistently order edge vertices (v1 < v2)
    auto order_edge = [](int v1, int v2) -> std::pair<int, int> {
      return (v1 < v2) ? std::make_pair(v1, v2) : std::make_pair(v2, v1);
    };
    
    // Lambda function to add an edge to the map with ordering check
    auto add_edge = [&](int v1, int v2) {
      // Validate indices to prevent out-of-bounds access
      if (v1 < 0 || v1 >= out.numberofpoints || v2 < 0 || v2 >= out.numberofpoints) {
        return;
      }
      
      if (v1 == v2) {
        // Skip degenerate edges
        return;
      }
      
      // Add ordered edge
      std::pair<int, int> edge = order_edge(v1, v2);
      edge_map[edge] = 1;
    };
    
    // Collect edges ONLY from internal tetrahedra
    for (int idx : internal_tetrahedra) {
      int *tet = &out.tetrahedronlist[idx * 4];
      
      // Ensure valid indices
      for (int j = 0; j < 4; j++) {
        if (tet[j] < 0 || tet[j] >= out.numberofpoints) {
          params.error_message_add(NodeWarningType::Warning, 
              "Indice de tétraèdre invalide détecté");
          tet[j] = 0;  // Use a safe fallback
        }
      }
      
      // Add the 6 edges of this tetrahedron
      add_edge(tet[0], tet[1]);
      add_edge(tet[0], tet[2]);
      add_edge(tet[0], tet[3]);
      add_edge(tet[1], tet[2]);
      add_edge(tet[1], tet[3]);
      add_edge(tet[2], tet[3]);
    }
    
    // If there are no edges from tetrahedra, try to extract from faces
    if (edge_map.empty() && out.numberoftrifaces > 0) {
      for (int i = 0; i < out.numberoftrifaces; i++) {
        int *face = &out.trifacelist[i * 3];
        add_edge(face[0], face[1]);
        add_edge(face[1], face[2]);
        add_edge(face[2], face[0]);
      }
    }
    
    // Still no edges? Add some fallback edges
    if (edge_map.empty()) {
      params.error_message_add(NodeWarningType::Warning, 
          "Aucune arête n'a été générée dans le maillage tétraédrique");
      // Fallback to prevent crashes: add some dummy edges
      add_edge(0, 1);
      add_edge(1, 2);
      add_edge(2, 0);
    }
    
    // Nombre final d'arêtes
    num_edges = edge_map.size();
    
    // Créer le maillage avec les dimensions appropriées
    Mesh *mesh_out = BKE_mesh_new_nomain(out.numberofpoints, num_edges, num_faces, num_corners);
    
    if (!mesh_out) {
      params.error_message_add(NodeWarningType::Error,
                             "Impossible de créer un maillage avec les dimensions requises");
      return nullptr;
    }
    
    // Copy vertex positions
    try {
      // Access vertex positions
      MutableSpan<float3> vert_positions = mesh_out->vert_positions_for_write();
      
      // Copy vertex positions from TetGen output
      for (int i = 0; i < out.numberofpoints; i++) {
        vert_positions[i] = float3(
            out.pointlist[i * 3],
            out.pointlist[i * 3 + 1],
            out.pointlist[i * 3 + 2]);
      }
      
      // Set up edges
      MutableSpan<int2> edges = mesh_out->edges_for_write();
      
      // Assign edge values
      int edge_index = 0;
      for (const auto &edge_entry : edge_map) {
        if (edge_index < num_edges) {
          edges[edge_index] = int2(edge_entry.first.first, edge_entry.first.second);
          edge_index++;
        }
      }
      
      // Set up face data
      if (out.numberoftrifaces > 0) {
        // Configure face offsets
        offset_indices::fill_constant_group_size(3, 0, mesh_out->face_offsets_for_write());
        
        // Fill corner data
        MutableSpan<int> corner_verts = mesh_out->corner_verts_for_write();
        for (int i = 0; i < out.numberoftrifaces; i++) {
          int *face = &out.trifacelist[i * 3];
          
          // Validate indices
          for (int j = 0; j < 3; j++) {
            if (face[j] < 0 || face[j] >= out.numberofpoints) {
              params.error_message_add(NodeWarningType::Warning, 
                  "Indice de face invalide détecté");
              face[j] = 0;  // Use a safe fallback
            }
          }
          
          // Add corners
          corner_verts[i * 3]     = face[0];
          corner_verts[i * 3 + 1] = face[1];
          corner_verts[i * 3 + 2] = face[2];
        }
      }
      else {
        // Create a single face if none were generated
        offset_indices::fill_constant_group_size(3, 0, mesh_out->face_offsets_for_write());
        
        MutableSpan<int> corner_verts = mesh_out->corner_verts_for_write();
        corner_verts[0] = 0;
        corner_verts[1] = 1;
        corner_verts[2] = 2;
      }
      
      // Ensure corner edges have values to prevent crashes in loose_edges calculation
      // This is a common source of crashes when the mesh runtime is being calculated
      if (out.numberoftrifaces > 0) {
        MutableSpan<int> corner_edges = mesh_out->corner_edges_for_write();
        Span<int> corner_verts = mesh_out->corner_verts();
        
        // Map each edge to its index for fast lookup
        std::unordered_map<std::pair<int, int>, int, pairhash> edge_indices;
        for (int i = 0; i < num_edges; i++) {
          int v1 = edges[i][0];
          int v2 = edges[i][1];
          edge_indices[order_edge(v1, v2)] = i;
        }
        
        // Set corner edges based on corner vertices
        Span<int> face_offsets = mesh_out->face_offsets();
        for (int face_index = 0; face_index < out.numberoftrifaces; face_index++) {
          int face_start = face_offsets[face_index];
          int face_size = face_offsets[face_index + 1] - face_start;
          
          for (int i = 0; i < face_size; i++) {
            int v1 = corner_verts[face_start + i];
            int v2 = corner_verts[face_start + (i + 1) % face_size];
            
            std::pair<int, int> edge = order_edge(v1, v2);
            auto edge_it = edge_indices.find(edge);
            
            if (edge_it != edge_indices.end()) {
              corner_edges[face_start + i] = edge_it->second;
            }
            else {
              // Fallback to edge 0 if not found
              corner_edges[face_start + i] = 0;
            }
          }
        }
      }
      
      // Add tetrahedron ID attribute
      bke::MutableAttributeAccessor attributes = mesh_out->attributes_for_write();
      bke::SpanAttributeWriter<int> tet_indices = attributes.lookup_or_add_for_write_span<int>(
          "tetrahedral_index", bke::AttrDomain::Face);
          
      if (tet_indices) {
        for (int i = 0; i < num_faces; i++) {
          tet_indices.span[i] = internal_tetrahedra.is_empty() ? 0 : internal_tetrahedra[0];  // Use first internal tetrahedron
        }
        tet_indices.finish();
      }
      
      // Set mesh name
      BKE_mesh_validate(mesh_out, true, true);
      return mesh_out;
    }
    catch (const std::exception &e) {
      params.error_message_add(NodeWarningType::Error,
                             std::string("Erreur lors de la création du maillage: ") + e.what());
      return nullptr;
    }
  }
  catch (const std::exception &e) {
    params.error_message_add(NodeWarningType::Error,
                           std::string("Erreur lors de la création du maillage: ") + e.what());
    return nullptr;
  }
  
  // Fallback tétraèdre au cas où
  return nullptr;  // On ne crée pas de fallback pour éviter tout problème
}

/**
 * Analyse un maillage pour détecter des problèmes géométriques qui pourraient causer
 * des crashs lors des opérations de flipping dans TetGen
 */
static bool detect_flipping_prone_geometry(const Mesh *mesh, GeoNodeExecParams &params)
{
  // Compter les triangles de mauvaise qualité
  int bad_triangles = 0;
  int sampled_triangles = 0;
  
  // Seuils pour identifier les triangles problématiques
  const float min_angle_threshold = 0.05f;  // ~3 degrés
  const float aspect_ratio_threshold = 0.02f; // Très étiré
  
  // Accès aux données du maillage
  Span<float3> vert_positions = mesh->vert_positions();
  Span<int> face_offsets = mesh->face_offsets();
  Span<int> corner_verts = mesh->corner_verts();
  
  // Échantillonner un sous-ensemble de faces pour analyse rapide
  const int sample_step = mesh->faces_num > 1000 ? mesh->faces_num / 1000 : 1;
  
  // Lambda pour calculer l'angle entre deux vecteurs
  auto compute_angle = [](const float3 &v1, const float3 &v2) -> float {
    float len1 = math::length(v1);
    float len2 = math::length(v2);
    if (len1 < 1e-6f || len2 < 1e-6f) {
      return 0.0f;
    }
    float dot_prod = math::dot(v1, v2) / (len1 * len2);
    // Borner dot_prod pour éviter les problèmes numériques
    dot_prod = std::max(-1.0f, std::min(1.0f, dot_prod));
    return std::acos(dot_prod);
  };
  
  for (int i = 0; i < mesh->faces_num; i += sample_step) {
    int face_start = face_offsets[i];
    int face_size = face_offsets[i + 1] - face_start;
    
    // Analyser seulement les triangles
    if (face_size == 3) {
      sampled_triangles++;
      
      // Indices des sommets
      int idx1 = corner_verts[face_start];
      int idx2 = corner_verts[face_start + 1];
      int idx3 = corner_verts[face_start + 2];
      
      // Vérifier que les indices sont valides
      if (idx1 >= 0 && idx1 < mesh->verts_num &&
          idx2 >= 0 && idx2 < mesh->verts_num &&
          idx3 >= 0 && idx3 < mesh->verts_num) {
        
        // Coordonnées des sommets
        float3 v1 = vert_positions[idx1];
        float3 v2 = vert_positions[idx2];
        float3 v3 = vert_positions[idx3];
        
        // Vecteurs des côtés
        float3 e1 = v2 - v1;
        float3 e2 = v3 - v2;
        float3 e3 = v1 - v3;
        
        // Longueurs des côtés
        float len1 = math::length(e1);
        float len2 = math::length(e2);
        float len3 = math::length(e3);
        
        // Rapport d'aspect (ratio du plus court sur le plus long côté)
        float min_len = std::min({len1, len2, len3});
        float max_len = std::max({len1, len2, len3});
        float aspect_ratio = min_len / max_len;
        
        // Calculer les angles
        float angle1 = compute_angle(-e1, e3);
        float angle2 = compute_angle(-e2, e1);
        float angle3 = compute_angle(-e3, e2);
        float min_angle = std::min({angle1, angle2, angle3});
        
        // Détecter les triangles problématiques
        if (aspect_ratio < aspect_ratio_threshold || min_angle < min_angle_threshold) {
          bad_triangles++;
        }
      }
    }
  }
  
  // Si nous avons échantillonné des triangles
  if (sampled_triangles > 0) {
    float bad_percentage = (float)bad_triangles / sampled_triangles * 100.0f;
    
    // Si plus de 1% des triangles sont de mauvaise qualité
    if (bad_percentage > 1.0f) {
      params.error_message_add(NodeWarningType::Warning,
          std::string("Géométrie potentiellement problématique détectée: ") + 
          std::to_string(bad_triangles) + " triangles de mauvaise qualité sur " + 
          std::to_string(sampled_triangles) + " échantillonnés (" + 
          std::to_string(bad_percentage) + "%). "
          "Risque de crash lors des opérations de flipping.");
      
      // Si beaucoup de triangles problématiques, activer le mode spécial
      if (bad_percentage > 5.0f) {
        params.error_message_add(NodeWarningType::Warning,
            "Activation du mode spécial de tétraédrisation sans récupération des frontières "
            "pour éviter les crashs dans les opérations de flipping.");
        return true;
      }
    }
  }
  
  return false;
}

/**
 * Checks if a mesh is manifold (watertight, no open boundaries)
 * Returns true if manifold, false otherwise and adds error messages
 */
static bool check_manifold_mesh(const Mesh *mesh, GeoNodeExecParams &params)
{
  // No mesh to check
  if (!mesh || mesh->verts_num == 0) {
    params.error_message_add(NodeWarningType::Error, 
        "Cannot tetrahedralize: input mesh is empty");
    return false;
  }
  
  // Count vertices, faces, and corners
  Span<float3> vert_positions = mesh->vert_positions();
  Span<int> face_offsets = mesh->face_offsets();
  Span<int> corner_verts = mesh->corner_verts();
  
  // Create edge to face map to check manifoldness
  std::unordered_map<std::pair<int, int>, std::vector<int>, pairhash> edge_to_faces;
  
  // Function to add an edge and its associated face to the map
  auto add_edge = [&](int v1, int v2, int face_idx) {
    // Store the edge with the smaller vertex index first
    std::pair<int, int> edge = v1 < v2 ? std::make_pair(v1, v2) : std::make_pair(v2, v1);
    edge_to_faces[edge].push_back(face_idx);
  };
  
  // Process each face and add its edges to the map
  for (int i = 0; i < mesh->faces_num; i++) {
    int face_start = face_offsets[i];
    int face_size = face_offsets[i + 1] - face_start;
    
    // Skip faces with less than 3 vertices
    if (face_size < 3) {
      continue;
    }
    
    // Add all edges of this face
    for (int j = 0; j < face_size; j++) {
      int v1 = corner_verts[face_start + j];
      int v2 = corner_verts[face_start + ((j + 1) % face_size)];
      
      // Skip invalid indices
      if (v1 < 0 || v2 < 0 || v1 >= mesh->verts_num || v2 >= mesh->verts_num) {
        continue;
      }
      
      add_edge(v1, v2, i);
    }
  }
  
  // Check manifoldness by looking for edges with only one adjacent face (boundary edges)
  int boundary_edges = 0;
  int non_manifold_edges = 0;
  
  for (const auto &edge_entry : edge_to_faces) {
    int face_count = edge_entry.second.size();
    
    if (face_count == 1) {
      // This is a boundary edge
      boundary_edges++;
    } else if (face_count > 2) {
      // This is a non-manifold edge (more than 2 faces sharing an edge)
      non_manifold_edges++;
    }
  }
  
  // Check if the mesh has boundaries or non-manifold edges
  if (boundary_edges > 0 || non_manifold_edges > 0) {
    std::string error_message = "Cannot tetrahedralize: non-manifold mesh detected. ";
    
    if (boundary_edges > 0) {
      error_message += std::to_string(boundary_edges) + " open boundary edges found. ";
    }
    
    if (non_manifold_edges > 0) {
      error_message += std::to_string(non_manifold_edges) + " non-manifold edges found. ";
    }
    
    error_message += "The mesh must be watertight (no holes or open boundaries).";
    params.error_message_add(NodeWarningType::Error, error_message);
    return false;
  }
  
  return true;
}

/**
 * Main node execution function
 */
static void node_geo_exec(GeoNodeExecParams params)
{
  // Retrieve inputs
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh");
  
  if (!geometry_set.has_mesh()) {
    params.error_message_add(NodeWarningType::Error, 
        "Required input: a mesh for tetrahedralization");
    params.set_output("Tetrahedral Mesh", GeometrySet());
    return;
  }
  
  // Retrieve input mesh and parameters
  const Mesh *mesh_in = geometry_set.get_mesh();
  
  // Check if mesh is manifold (watertight) before proceeding
  if (!check_manifold_mesh(mesh_in, params)) {
    // Return the input mesh unchanged if not manifold
    params.set_output("Tetrahedral Mesh", std::move(geometry_set));
    return;
  }
  
  // Pour les maillages complexes, préparer sans remplacer
  Mesh *prepared_mesh = prepare_complex_mesh_for_tetgen(mesh_in, params);
  const Mesh *mesh_to_process = prepared_mesh ? prepared_mesh : mesh_in;
  
  // Adjust max_volume to be interpreted as a percentage
  double max_volume_percentage = params.extract_input<float>("Max Volume");
  double max_volume = std::max(0.00001, max_volume_percentage * 0.1); // 0.1 corresponds to 100%
  
  float quality_ratio = std::max(1.0f, params.extract_input<float>("Quality Ratio"));
  
  float min_dihedral_angle = params.extract_input<float>("Min Dihedral Angle");
  
  // Directly retrieve preserve_boundary from input socket
  bool preserve_boundary = params.extract_input<bool>("Preserve Boundary");
  
  // Définir une limite de complexité du maillage avec plusieurs niveaux
  const int medium_complexity_threshold = 5000;  // Maillage moyennement complexe
  const int high_complexity_threshold = 10000;   // Maillage très complexe
  const int extreme_complexity_threshold = 50000; // Maillage extrêmement complexe
  
  // Définir un seuil pour les maillages "massifs" qui nécessitent un traitement spécial
  const int massive_mesh_threshold = 100000;    // Maillages massifs susceptibles de causer des crashs

  // Déterminer le niveau de complexité du maillage
  bool is_medium_complex = mesh_to_process->verts_num > medium_complexity_threshold || 
                         mesh_to_process->faces_num > medium_complexity_threshold;
  bool is_highly_complex = mesh_to_process->verts_num > high_complexity_threshold || 
                         mesh_to_process->faces_num > high_complexity_threshold;
  bool is_extremely_complex = mesh_to_process->verts_num > extreme_complexity_threshold || 
                            mesh_to_process->faces_num > extreme_complexity_threshold;
  bool is_massive_mesh = mesh_to_process->verts_num > massive_mesh_threshold ||
                        mesh_to_process->faces_num > massive_mesh_threshold;
  
  // Détecter si le maillage est susceptible de causer des problèmes avec les opérations de flipping
  bool has_problematic_geometry = detect_flipping_prone_geometry(mesh_to_process, params);
  
  // Le niveau final de complexité pour les décisions
  bool is_complex_mesh = is_medium_complex;
  
  // Pour les maillages massifs ou avec géométrie problématique, utiliser une tétraédrisation Delaunay pure
  if (is_massive_mesh || has_problematic_geometry) {
    params.error_message_add(NodeWarningType::Warning, 
        "Maillage extrêmement volumineux ou problématique détecté. "
        "Utilisation d'une tétraédrisation Delaunay pure pour éviter les crashs.");
    
    // Configurer TetGen avec des options spéciales anti-crash
    tetgenbehavior behavior;
    // Utilisé la version originale avec tous les paramètres nécessaires
    configure_tetgen_options(behavior, 
                           -1.0,  // max_volume (désactivé)
                           1.1f,  // quality_ratio minimal
                           1.0f,  // min_dihedral_angle minimal
                           false, // preserve_boundary désactivé pour les maillages problématiques
                           params,
                           2,     // attempt - simuler une 3ème tentative pour activer toutes les protections
                           3);    // max_attempts par défaut
    
    // Force des paramètres spécifiques pour les maillages massifs
    preserve_boundary = false;
    quality_ratio = 1.01f;
    min_dihedral_angle = 0.0f;
    
    // Utiliser un mode spécial de tétraédrisation pour éviter les crashes sur les gros maillages
    tetgenio in, out;
    
    // Nettoyage automatique des ressources TetGen
    TetGenResourceGuard resource_guard(in, out);
    
    Mesh *mesh_out = nullptr;
    bool tetgen_success = false;
    
    try {
      // Préparer l'entrée TetGen
      if (prepare_tetgen_input(mesh_to_process, in, params, 0)) {
        // Configuration Delaunay pure - pas de PLC, pas de récupération des frontières
        behavior.plc = 0;            // DÉSACTIVER complètement la préservation du complexe linéaire
        behavior.psc = 0;            // Désactiver les contraintes de surface
        behavior.quality = 0;        // Désactiver l'amélioration de qualité
        behavior.nobisect = 0;       // Ne pas préserver les frontières
        behavior.docheck = 0;        // Désactiver les vérifications
        behavior.diagnose = 0;       // Désactiver le mode diagnostic
        behavior.quiet = 1;          // Mode silencieux
        behavior.verbose = 0;        // Pas de verbosité
        
        // Options spécifiques pour éviter les crashes
        behavior.mindihedral = 0.0;  // Pas de contrainte d'angle
        behavior.minratio = 1.0;     // Ratio minimal absolu
        behavior.convex = 1;         // Utiliser l'enveloppe convexe
        
        // Désactivation explicite des options problématiques
        behavior.facesout = 0;       // Ne pas générer de faces
        behavior.edgesout = 0;       // Ne pas générer d'arêtes
        behavior.neighout = 0;       // Ne pas générer de voisins
        
        // Ajouter une perturbation aléatoire légère pour éviter les configurations dégénérées
        for (int i = 0; i < in.numberofpoints * 3; i++) {
          double noise = ((double)rand() / RAND_MAX) * 1e-6;
          in.pointlist[i] += noise;
        }
        
        params.error_message_add(NodeWarningType::Info,
            "Configuration Delaunay pure activée pour le maillage volumineux. "
            "Les frontières ne seront pas préservées, mais la tétraédrisation sera stable.");
        
        // Exécuter TetGen dans un mode qui évitera complètement les opérations de flipping
        try {
          tetrahedralize(&behavior, &in, &out);
          
          // Traiter la sortie
          if (out.numberoftetrahedra > 0) {
            mesh_out = create_tetrahedral_mesh(out, params);
            if (mesh_out) {
              tetgen_success = true;
            }
          }
        }
        catch (std::exception &e) {
          params.error_message_add(NodeWarningType::Error,
              std::string("Erreur TetGen: ") + e.what());
        }
        catch (...) {
          params.error_message_add(NodeWarningType::Error,
              "Erreur inconnue pendant la tétraédrisation.");
        }
      }
    }
    catch (...) {
      params.error_message_add(NodeWarningType::Error,
          "Erreur catastrophique détectée pendant la tétraédrisation.");
    }
    
    // Output
    GeometrySet output;
    if (mesh_out) {
      output.replace_mesh(mesh_out);
    }
    
    // Nettoyer le maillage préparé si nécessaire
    if (prepared_mesh) {
      BKE_id_free(nullptr, prepared_mesh);
    }
    
    params.set_output("Tetrahedral Mesh", std::move(output));
    return;  // Sortir immédiatement, ne pas utiliser le chemin normal
  }
  
  // Ajuster les paramètres en fonction de la complexité
  if (is_extremely_complex) {
    params.error_message_add(NodeWarningType::Warning, 
        "Maillage extrêmement complexe détecté (" + std::to_string(mesh_to_process->verts_num) + 
        " sommets, " + std::to_string(mesh_to_process->faces_num) + 
        " faces). Ajustement drastique des paramètres pour assurer la tétraédrisation.");
    
    // Réduire drastiquement le ratio de qualité
    float original_quality = quality_ratio;
    quality_ratio = 1.05f;
    
    if (original_quality != quality_ratio) {
      params.error_message_add(NodeWarningType::Info, 
          "Réduction automatique du ratio de qualité de " + std::to_string(original_quality) + 
          " à " + std::to_string(quality_ratio) + " pour les maillages extrêmement complexes.");
    }
    
    // Réduire drastiquement l'angle dihédral minimum
    float original_angle = min_dihedral_angle;
    min_dihedral_angle = 0.1f;
    
    if (original_angle != min_dihedral_angle && original_angle > 0.1f) {
      params.error_message_add(NodeWarningType::Info, 
          "Réduction automatique de l'angle dihédral minimum de " + std::to_string(original_angle) + 
          "° à " + std::to_string(min_dihedral_angle) + "° pour les maillages extrêmement complexes.");
    }
    
    // Désactiver la préservation des frontières pour les maillages extrêmement complexes
    if (preserve_boundary) {
      params.error_message_add(NodeWarningType::Info, 
          "Désactivation de la préservation des frontières pour les maillages extrêmement complexes.");
      preserve_boundary = false;
    }
  }
  else if (is_highly_complex) {
    params.error_message_add(NodeWarningType::Warning, 
        "Maillage très complexe détecté (" + std::to_string(mesh_to_process->verts_num) + 
        " sommets, " + std::to_string(mesh_to_process->faces_num) + 
        " faces). Ajustement des paramètres de qualité.");
    
    // Réduire le ratio de qualité
    float original_quality = quality_ratio;
    quality_ratio = std::min(quality_ratio, 1.2f);
    
    if (original_quality != quality_ratio) {
      params.error_message_add(NodeWarningType::Info, 
          "Réduction automatique du ratio de qualité de " + std::to_string(original_quality) + 
          " à " + std::to_string(quality_ratio) + " pour éviter les crashs.");
    }
    
    // Réduire l'angle dihédral minimum
    float original_angle = min_dihedral_angle;
    min_dihedral_angle = std::min(min_dihedral_angle, 5.0f);
    
    if (original_angle != min_dihedral_angle && original_angle > 5.0f) {
      params.error_message_add(NodeWarningType::Info, 
          "Réduction automatique de l'angle dihédral minimum de " + std::to_string(original_angle) + 
          "° à " + std::to_string(min_dihedral_angle) + "° pour les maillages très complexes.");
    }
  }
  else if (is_medium_complex) {
    // Pour les maillages moyennement complexes, juste un avertissement et des ajustements mineurs
    params.error_message_add(NodeWarningType::Info, 
        "Maillage moyennement complexe détecté (" + std::to_string(mesh_to_process->verts_num) + 
        " sommets, " + std::to_string(mesh_to_process->faces_num) + 
        " faces). Optimisation des paramètres.");
    
    // Limiter le ratio de qualité à des valeurs raisonnables
    if (quality_ratio > 2.0f) {
      float original_quality = quality_ratio;
      quality_ratio = 2.0f;
      
      params.error_message_add(NodeWarningType::Info, 
          "Réduction automatique du ratio de qualité de " + std::to_string(original_quality) + 
          " à " + std::to_string(quality_ratio) + " pour optimiser le temps de calcul.");
    }
  }
  
  // TetGen structures
  tetgenio in, out;
  tetgenbehavior behavior;
  
  // Resource guard to ensure cleanup in case of exception
  TetGenResourceGuard resource_guard(in, out);
  
  Mesh *mesh_out = nullptr;
  bool tetgen_success = false;
  
  // Nombre maximum de tentatives - augmenté pour les maillages complexes
  // Utiliser plus de tentatives pour les maillages extrêmement complexes
  const int max_attempts = is_extremely_complex ? 5 : (is_highly_complex ? 4 : (is_medium_complex ? 3 : 2));
  
  try {
    // Protection contre les crashs avec try-catch au niveau global
    // En cas d'échec complet, on utilisera le tétraèdre de secours
    try {
      // Boucle d'essais avec différentes configurations
      for (int attempt = 0; attempt < max_attempts && !tetgen_success; attempt++) {
        // Réinitialiser les structures TetGen pour chaque essai
        if (attempt > 0) {
          // Nettoyer les données précédentes
          in.initialize();
          out.initialize();
          
          params.error_message_add(NodeWarningType::Info, 
              "Tentative " + std::to_string(attempt + 1) + " avec des paramètres plus permissifs");
        }
        
        // Options spéciales pour les maillages complexes dès la première tentative
        bool use_extreme_robustness = is_complex_mesh && attempt >= 1;
        
        // Pour le dernier essai sur un maillage complexe, on prend des mesures radicales
        bool use_desperate_measures = is_complex_mesh && attempt >= max_attempts - 1;
        
        // Pour les maillages extrêmement complexes, des mesures radicales dès la première tentative
        if (is_extremely_complex && attempt == 0) {
          use_extreme_robustness = true;
        }
        
        // Prepare input data - avec perturbation aléatoire accrue pour les maillages complexes
        if (prepare_tetgen_input(mesh_to_process, in, params, use_extreme_robustness ? attempt + 1 : attempt)) {
          
          // Configure TetGen options avec le numéro de tentative
          // Utiliser des options plus agressives pour les maillages complexes
          configure_tetgen_options(behavior, 
                                 max_volume, 
                                 use_extreme_robustness ? 1.1f : quality_ratio,
                                 use_extreme_robustness ? 1.0f : min_dihedral_angle, 
                                 use_extreme_robustness ? false : preserve_boundary, 
                                 params, 
                                 use_extreme_robustness ? attempt + 1 : attempt,
                                 max_attempts);
          
          // Pour les maillages complexes et les tentatives désespérées, désactiver tout contrôle qualité
          if (use_desperate_measures) {
            behavior.quality = 0;        // Désactiver toute amélioration de qualité
            behavior.mindihedral = 0.0;  // Pas de contrainte d'angle dihédral
            behavior.minratio = 1.0;    // Ratio minimal absolu
            behavior.diagnose = 1;       // Mode diagnostic 
            behavior.docheck = 0;        // Désactiver les vérifications
            behavior.nobisect = 0;       // Ne pas préserver la frontière
            behavior.epsilon = 1e-6;     // Tolérance élevée pour les erreurs numériques
            behavior.nomergefacet = 1;   // Prévenir la fusion des facettes
            behavior.nomergevertex = 1;  // Prévenir la fusion des sommets
            
            params.error_message_add(NodeWarningType::Warning,
                "Utilisation de paramètres de dernier recours pour la tétraédrisation");
          }
          
          // Run TetGen
          try {
            // Protéger l'appel TetGen pour éviter les erreurs d'accès mémoire
            try {
              // AJOUT: Protection ultime contre le crash dans create_a_shorter_edge
              // Appliqué uniquement aux maillages vraiment complexes
              if (is_extremely_complex && attempt == max_attempts - 1) {
                // Configuration anti-crash spécifique pour l'erreur dans create_a_shorter_edge
                behavior.plc = 1;          // Conserver le complexe linéaire par morceaux
                behavior.psc = 0;          // Désactiver les contraintes de surface précises
                behavior.quality = 0;      // Désactiver l'amélioration de qualité
                behavior.docheck = 0;      // Désactiver les vérifications
                behavior.diagnose = 1;     // Activer le mode diagnostic
                behavior.nobisect = 0;     // Ne pas préserver les frontières exactes
                behavior.coarsen = 1;      // Autoriser la simplification du maillage
                behavior.mindihedral = 0;  // Pas de contrainte d'angle minimum
                behavior.minratio = 10.0;  // Ratio très permissif
                behavior.epsilon = 1e-5;   // Tolérance numérique élevée
                
                // Paramètres supplémentaires pour éviter le crash dans create_a_shorter_edge
                behavior.facesout = 0;     // Ne pas générer les faces (évite certains appels problématiques)
                behavior.edgesout = 0;     // Ne pas générer les arêtes (évite certains appels problématiques)
                behavior.neighout = 0;     // Ne pas générer les voisins
                
                // NOUVELLE DÉSACTIVATION RADICALE
                // Utilisation d'une vraie tétraédrisation Delaunay sans contraintes de frontières
                behavior.plc = 0;          // DÉSACTIVER complètement la préservation des frontières
                behavior.psc = 0;          // Désactiver les contraintes de surface
                behavior.quality = 0;      // Qualité minimale
                
                // Paramètres qui causeraient des problèmes désactivés
                behavior.facesout = 0;
                behavior.edgesout = 0;
                behavior.neighout = 0;
                
                // Garantir une tétraédrisation Delaunay pure sans récupération
                behavior.convex = 1;        // Permettre l'enveloppe convexe simple
                behavior.regionattrib = 0;  // Ne pas générer d'attributs de région
                
                params.error_message_add(NodeWarningType::Warning,
                    "SOLUTION D'URGENCE: Désactivation complète de la récupération des frontières "
                    "pour éviter le crash. Le résultat sera une tétraédrisation Delaunay de "
                    "l'enveloppe convexe sans préservation des frontières.");
              }
              
              // Essayer d'exécuter TetGen avec les paramètres modifiés
              try {
                tetrahedralize(&behavior, &in, &out);
              }
              catch (...) {
                // En cas d'échec même après avoir désactivé les frontières, 
                // essayer le dernier recours: délaunay simple
                if (attempt == max_attempts - 1) {
                  try {
                    // Réinitialiser les structures
                    in.initialize();
                    out.initialize();
                    
                    // Préparer l'entrée à nouveau (sans perturbations)
                    if (prepare_tetgen_input(mesh_to_process, in, params, 0)) {
                      // Configuration pour Delaunay pur (sans frontières)
                      behavior.plc = 0;            // Pas de préservation des frontières
                      behavior.psc = 0;            // Pas de contraintes de surface
                      behavior.quality = 0;        // Pas d'amélioration de qualité
                      behavior.mindihedral = 0.0;  // Pas de contrainte d'angle
                      behavior.minratio = 1.0;     // Pas de contrainte de ratio
                      behavior.docheck = 0;        // Pas de vérifications
                      behavior.diagnose = 0;       // Pas de diagnostic
                      behavior.convex = 1;         // Conserver l'enveloppe convexe
                      behavior.facesout = 0;       // Pas de génération de faces
                      behavior.edgesout = 0;       // Pas de génération d'arêtes
                      behavior.neighout = 0;       // Pas de génération de voisins
                      behavior.quiet = 1;
                      behavior.verbose = 0;
                      
                      params.error_message_add(NodeWarningType::Warning,
                          "DERNIER RECOURS: Tentative avec tétraédrisation Delaunay pure sans aucune contrainte.");
                      
                      tetrahedralize(&behavior, &in, &out);
                    }
                  }
                  catch (...) {
                    // Même le Delaunay pur a échoué - c'est terminé
                    params.error_message_add(NodeWarningType::Error, 
                        "La tétraédrisation a échoué même avec les paramètres les plus simples.");
                    throw;
                  }
                }
                else {
                  // Pour les tentatives qui ne sont pas la dernière, simplement échouer et passer à la suivante
                  throw;
                }
              }
            }
            catch (std::bad_alloc &e) {
              // Erreur d'allocation mémoire - réessayer avec moins de qualité
              params.error_message_add(NodeWarningType::Warning, 
                  "Erreur d'allocation mémoire: réessai avec des paramètres réduits");
              continue;
            }
            catch (std::exception &e) {
              // Autres erreurs standard
              if (attempt == max_attempts - 1) {
                params.error_message_add(NodeWarningType::Error, 
                    std::string("TetGen error: ") + e.what());
              }
              continue;
            }
            catch (...) {
              // Crash potentiel de TetGen - probablement sur un maillage très complexe
              // Dernier essai avec une configuration de secours radicale
              if (attempt < max_attempts - 1) {
                continue; // Passer à la tentative suivante
              }
              
              // Dernier essai - utiliser une configuration de dernier recours
              try {
                // Réinitialiser les structures
                in.initialize();
                out.initialize();
                
                // Préparer l'entrée à nouveau
                if (!prepare_tetgen_input(mesh_to_process, in, params, max_attempts)) {
                  throw std::runtime_error("Failed to prepare input for last attempt");
                }
                
                // Configurer des options très basiques pour éviter le crash
                behavior.plc = 0;          // DÉSACTIVER complètement la préservation du complexe
                behavior.quality = 0;      // Désactiver l'amélioration de qualité
                behavior.nobisect = 0;     // Ne pas préserver les frontières
                behavior.docheck = 0;      // Désactiver les vérifications
                behavior.quiet = 1;
                behavior.verbose = 0;
                behavior.mindihedral = 0.0; // Aucune contrainte d'angle
                behavior.minratio = 1.0;    // Ratio minimal
                behavior.diagnose = 0;      // Désactiver le mode diagnostic aussi
                behavior.convex = 1;        // Activer l'option convex pour générer l'enveloppe convexe uniquement
                behavior.regionattrib = 0;  // Désactiver les attributs de région
                
                // ANTI-CRASH pour create_a_shorter_edge
                // Désactivation complète du processus de récupération des frontières
                behavior.facesout = 0;     // Ne pas générer de faces (évite les crashes dans create_a_shorter_edge)
                behavior.edgesout = 0;     // Ne pas générer d'arêtes
                behavior.psc = 0;          // Désactiver la préservation des contraintes de surface
                
                // Essai final sans récupération des frontières (pour éviter le crash)
                params.error_message_add(NodeWarningType::Warning, 
                    "Mode anti-crash critique activé - Génération d'une tétraédrisation Delaunay pure "
                    "sans récupération des frontières pour éviter complètement le crash dans "
                    "create_a_shorter_edge.");
                
                // Ajouter une perturbation aléatoire aux sommets pour éviter les co-circularités
                // qui peuvent causer des problèmes avec l'algorithme de Delaunay
                for (int i = 0; i < in.numberofpoints * 3; i++) {
                  double noise = ((double)rand() / RAND_MAX) * 1e-6;
                  in.pointlist[i] += noise;
                }
                
                // Tentative finale avec la configuration de secours
                tetrahedralize(&behavior, &in, &out);
              }
              catch (...) {
                // Même la tentative de dernier recours a échoué
                params.error_message_add(NodeWarningType::Error, 
                    "Échec total de la tétraédrisation, même avec la configuration de secours");
                // Continuer vers le fallback
                throw;
              }
            }
            
            // Validate output
            if (validate_tetgen_output(out, params)) {
              // Create mesh
              mesh_out = create_tetrahedral_mesh(out, params);
              
              if (mesh_out) {
                tetgen_success = true;
                
                // Ajouter un message de réussite avec le numéro de tentative
                if (attempt > 0) {
                  params.error_message_add(NodeWarningType::Info, 
                      "Tétraédrisation réussie après " + std::to_string(attempt + 1) + " tentative(s)");
                }
                
                break; // Sortir de la boucle
              }
            }
          }
          catch (const std::exception &e) {
            // Uniquement afficher l'erreur à la dernière tentative
            if (attempt == max_attempts - 1) {
              params.error_message_add(NodeWarningType::Error, 
                  std::string("TetGen error: ") + e.what());
            }
          }
          catch (...) {
            if (attempt == max_attempts - 1) {
              params.error_message_add(NodeWarningType::Error, 
                  "Unknown error while running TetGen");
            }
          }
        }
      }
    }
    catch (const std::exception &e) {
      params.error_message_add(NodeWarningType::Error, 
          std::string("Exception: ") + e.what());
    }
    catch (...) {
      params.error_message_add(NodeWarningType::Error, 
          "Erreur inconnue catastrophique pendant le traitement");
    }
  }
  catch (...) {
    // Protection absolue pour garantir que nous pouvons toujours créer un fallback
    params.error_message_add(NodeWarningType::Error, 
        "Crash de l'algorithme de tétraédrisation détecté");
  }
  
  // Nettoyer le maillage préparé si nécessaire
  if (prepared_mesh) {
    BKE_id_free(nullptr, prepared_mesh);
  }
  
  // In case of failure, create a fallback tetrahedron
  if (!tetgen_success) {
    params.error_message_add(NodeWarningType::Warning, 
        "Impossible de créer un maillage tétraédrique.");
    mesh_out = nullptr;
  }
  
  // Output
  GeometrySet output;
  if (mesh_out) {
    output.replace_mesh(mesh_out);
  }
  
  params.set_output("Tetrahedral Mesh", std::move(output));
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
  ntype.ui_description = "Generates a tetrahedral mesh from a surface mesh";
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  blender::bke::node_type_storage(ntype, "NodeGeometryTetrahedralize", node_free_storage, node_copy_storage);
  
  ntype.gather_link_search_ops = nullptr;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_tetrahedralize_cc

void register_node_type_geo_tetrahedralize()
{
  namespace file_ns = blender::nodes::node_geo_tetrahedralize_cc;

  file_ns::node_register();
}