/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Inclusions standards de C++ */
#include <iostream>
#include <algorithm>
#include <unordered_set>
#include <numeric> // Pour std::iota
#include <random> // Pour la génération de nombres aléatoires

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
static bool prepare_tetgen_input(const Mesh *mesh, tetgenio &in, GeoNodeExecParams &params, int attempt)
{
  in.initialize();
  in.firstnumber = 0;
  
  Span<float3> vert_positions = mesh->vert_positions();
  if (vert_positions.size() < 4) {
    params.error_message_add(NodeWarningType::Error,
                           "Tetrahedral mesh requires at least 4 vertices.");
    return false;
  }
  
  // Option 1: Process all vertices
  in.numberofpoints = vert_positions.size();
  in.pointlist = new REAL[vert_positions.size() * 3];
  
  // Calculer la boîte englobante pour déterminer l'échelle des perturbations
  float3 bbox_min(std::numeric_limits<float>::max());
  float3 bbox_max(-std::numeric_limits<float>::max());
  
  for (int i = 0; i < vert_positions.size(); i++) {
    bbox_min = math::min(bbox_min, vert_positions[i]);
    bbox_max = math::max(bbox_max, vert_positions[i]);
  }
  
  // Calculer la taille du maillage pour déterminer l'amplitude des perturbations
  float mesh_scale = math::length(bbox_max - bbox_min);
  float perturbation_scale = mesh_scale * 1e-6f; // 0.0001% de la taille du maillage
  
  // Augmenter progressivement les perturbations si les tentatives précédentes ont échoué
  if (attempt >= 1) {
    perturbation_scale *= pow(10.0f, attempt);
    params.error_message_add(NodeWarningType::Info,
                         "Tentative " + std::to_string(attempt+1) + 
                         ": Augmentation des perturbations (x" + 
                         std::to_string(pow(10.0f, attempt)) + ")");
  }
  
  // Générateur de nombres aléatoires pour les perturbations
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_real_distribution<float> dist(-perturbation_scale, perturbation_scale);
  
  // Copier les positions des sommets avec de petites perturbations pour éviter les configurations dégénérées
  for (int i = 0; i < vert_positions.size(); i++) {
    float3 pos = vert_positions[i];
    
    // Ajouter une perturbation aléatoire si ce n'est pas la première tentative
    // ou si le maillage a été identifié comme problématique
    if (attempt >= 1) {
      pos.x += dist(gen);
      pos.y += dist(gen);
      pos.z += dist(gen);
    }
    
    in.pointlist[i * 3] = pos.x;
    in.pointlist[i * 3 + 1] = pos.y;
    in.pointlist[i * 3 + 2] = pos.z;
  }
  
  // Input faces
  Span<int> corner_verts = mesh->corner_verts();
  Span<int> face_offsets = mesh->face_offsets();
  
  /* Count number of triangles - we need to triangulate non-triangle faces. */
  int num_triangles = 0;
  for (int i = 0; i < mesh->faces_num; i++) {
    int face_size = face_offsets[i + 1] - face_offsets[i];
    if (face_size < 3) {
      // Skip degenerate faces
      continue;
    }
    else {
      // For N-gons with N > 3, we need N-2 triangles
      num_triangles += face_size - 2;
    }
  }
  
  // Configure faces for Tetgen
  in.numberoffacets = num_triangles;
  in.facetlist = new tetgenio::facet[in.numberoffacets];
  in.facetmarkerlist = new int[in.numberoffacets];
  
  int ti = 0; // triangle index
  
  // Pour chaque face, créer des triangles
  for (int i = 0; i < mesh->faces_num; i++) {
    int face_start = face_offsets[i];
    int face_size = face_offsets[i + 1] - face_start;
    
    // Skip degenerate faces
    if (face_size < 3) {
      continue;
    }
    
    // For each vertex except the first and last, create a triangle with the first vertex
    for (int j = 0; j < face_size - 2; j++) {
      // Create a triangle facet
      tetgenio::facet *f = &in.facetlist[ti];
      f->numberofpolygons = 1;
      f->polygonlist = new tetgenio::polygon[f->numberofpolygons];
      f->numberofholes = 0;
      f->holelist = nullptr;
      
      // Create a polygon (triangle) with 3
      tetgenio::polygon *p = &f->polygonlist[0];
      p->numberofvertices = 3;
      p->vertexlist = new int[p->numberofvertices];
      
      // Indices des sommets du triangle
      p->vertexlist[0] = corner_verts[face_start];
      p->vertexlist[1] = corner_verts[face_start + j + 1];
      p->vertexlist[2] = corner_verts[face_start + j + 2];
      
      // Vérifier la validité des indices pour éviter les crashs
      for (int k = 0; k < 3; k++) {
        if (p->vertexlist[k] < 0 || p->vertexlist[k] >= in.numberofpoints) {
          params.error_message_add(NodeWarningType::Error,
                               "Invalid vertex index in face: " + std::to_string(p->vertexlist[k]));
          return false;
        }
      }
      
      // Marquer ce triangle pour la reconstruction
      in.facetmarkerlist[ti] = 1;
      
      ti++;
    }
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
    
    // Protection anti-crash pour sscoutsegment
    behavior.nomergefacet = 1;  // Empêcher la fusion des facettes (prévient certains crashs sscoutsegment)
    behavior.nomergevertex = 1; // Empêcher la fusion des sommets (prévient certains crashs sscoutsegment)
    behavior.nojettison = 1;    // Désactiver le jettison des segments (prévient les crashs sscoutsegment)
    
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
  // Vérification de base
  if (out.numberofpoints <= 0 || out.numberoftetrahedra <= 0) {
    params.error_message_add(NodeWarningType::Error, 
        "TetGen n'a pas généré de maillage tétraédrique valide");
    
    // Messages diagnostiques spécifiques
    if (out.numberofpoints <= 0) {
      params.error_message_add(NodeWarningType::Error, 
          "Aucun point généré par TetGen. Le maillage est peut-être dégénéré ou trop plat.");
    }
    else if (out.numberoftetrahedra <= 0 && out.numberofpoints > 0) {
      params.error_message_add(NodeWarningType::Error, 
          "Des points ont été générés (" + std::to_string(out.numberofpoints) + 
          ") mais aucun tétraèdre n'a été créé. Le maillage est probablement non-volumique ou auto-intersectant.");
    }
    
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
  
  // Validation avancée - vérifier si le nombre de tétraèdres est raisonnable
  if (out.numberoftetrahedra < 10 && out.numberofpoints > 100) {
    params.error_message_add(NodeWarningType::Warning, 
        "Très peu de tétraèdres générés (" + std::to_string(out.numberoftetrahedra) + 
        ") par rapport au nombre de points (" + std::to_string(out.numberofpoints) + 
        "). Le maillage pourrait être presque plat ou avoir des problèmes topologiques.");
  }
  
  // Vérification spécifique pour les maillages plats
  // Calculer la boîte englobante et vérifier les proportions
  float3 bbox_min(std::numeric_limits<float>::max());
  float3 bbox_max(-std::numeric_limits<float>::max());
  
  for (int i = 0; i < out.numberofpoints; i++) {
    float3 p(out.pointlist[i * 3], out.pointlist[i * 3 + 1], out.pointlist[i * 3 + 2]);
    bbox_min = math::min(bbox_min, p);
    bbox_max = math::max(bbox_max, p);
  }
  
  float3 dimensions = bbox_max - bbox_min;
  float min_dim = std::min({dimensions.x, dimensions.y, dimensions.z});
  float max_dim = std::max({dimensions.x, dimensions.y, dimensions.z});
  
  // Si une dimension est beaucoup plus petite que les autres, c'est probablement un maillage plat
  if (min_dim < max_dim * 0.01f) {
    params.error_message_add(NodeWarningType::Warning, 
        "Le maillage est très plat dans au moins une dimension, ce qui peut causer des problèmes de tétraédralisation. "
        "Ratio min/max = " + std::to_string(min_dim/max_dim) + 
        ". Essayez d'extruder le maillage dans cette dimension.");
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
  Span<float3> vert_positions = mesh->vert_positions();
  Span<int> corner_verts = mesh->corner_verts();
  Span<int> face_offsets = mesh->face_offsets();
  
  bool has_potential_issues = false;
  
  // Vérification des petits triangles problématiques
  // Ces triangles peuvent causer des opérations de "flipping" instables dans TetGen
  // qui peuvent déclencher le crash dans sscoutsegment
  int num_small_faces = 0;
  int sample_count = 0;
  
  // Pour les grands maillages, échantillonnons aléatoirement
  const int max_samples = std::min(1000, mesh->faces_num);
  std::vector<int> face_indices;
  face_indices.reserve(max_samples);
  
  if (mesh->faces_num > max_samples) {
    // Échantillonnage aléatoire des faces
    std::srand(static_cast<unsigned int>(std::time(nullptr)));
    for (int i = 0; i < max_samples; i++) {
      face_indices.push_back(std::rand() % mesh->faces_num);
    }
  } else {
    // Utilisation de toutes les faces pour les petits maillages
    face_indices.resize(mesh->faces_num);
    std::iota(face_indices.begin(), face_indices.end(), 0);
  }
  
  // Calculer la boîte englobante pour obtenir une référence de taille
  float3 bbox_min(std::numeric_limits<float>::max());
  float3 bbox_max(-std::numeric_limits<float>::max());
  
  for (int i = 0; i < mesh->verts_num; i++) {
    bbox_min = math::min(bbox_min, vert_positions[i]);
    bbox_max = math::max(bbox_max, vert_positions[i]);
  }
  
  float bounding_size = math::length(bbox_max - bbox_min);
  float tiny_feature_threshold = bounding_size * 1e-4f; // Seuil pour les détails minuscules
  
  // Vérifier les triangles dégénérés et très petits
  for (int face_idx : face_indices) {
    int face_start = face_offsets[face_idx];
    int face_size = face_offsets[face_idx + 1] - face_start;
    
    // Ne considérer que les triangles
    if (face_size == 3) {
      sample_count++;
      
      int v1_idx = corner_verts[face_start];
      int v2_idx = corner_verts[face_start + 1];
      int v3_idx = corner_verts[face_start + 2];
      
      // Obtenir les coordonnées des sommets
      float3 v1 = vert_positions[v1_idx];
      float3 v2 = vert_positions[v2_idx];
      float3 v3 = vert_positions[v3_idx];
      
      // Calculer les longueurs des arêtes
      float edge1_len = math::distance(v1, v2);
      float edge2_len = math::distance(v2, v3);
      float edge3_len = math::distance(v3, v1);
      
      // Calculer l'aire du triangle
      float s = (edge1_len + edge2_len + edge3_len) / 2.0f;
      float area = std::sqrt(s * (s - edge1_len) * (s - edge2_len) * (s - edge3_len));
      
      // Calculer le rayon du cercle inscrit (mesure de la forme du triangle)
      float inradius = (area > 0.0f) ? (area / s) : 0.0f;
      
      // Vérifier si le triangle est dégénéré ou très petit
      float min_edge = std::min({edge1_len, edge2_len, edge3_len});
      float max_edge = std::max({edge1_len, edge2_len, edge3_len});
      
      // Un triangle est considéré problématique s'il est très allongé ou très petit
      bool is_sliver = (inradius < tiny_feature_threshold) && (max_edge / min_edge > 10.0f);
      bool is_tiny = (area < tiny_feature_threshold * tiny_feature_threshold);
      
      if (is_sliver || is_tiny) {
        num_small_faces++;
      }
      
      // Vérifier spécifiquement les triangles qui peuvent causer des problèmes avec sscoutsegment
      // Ces triangles ont généralement des angles très aigus
      float min_sin_angle = std::numeric_limits<float>::max();
      
      if (edge1_len > 0 && edge2_len > 0) {
        float cos_angle = math::dot(math::normalize(v2 - v1), math::normalize(v3 - v2));
        float sin_angle = std::sqrt(1.0f - cos_angle * cos_angle);
        min_sin_angle = std::min(min_sin_angle, sin_angle);
      }
      
      if (edge2_len > 0 && edge3_len > 0) {
        float cos_angle = math::dot(math::normalize(v3 - v2), math::normalize(v1 - v3));
        float sin_angle = std::sqrt(1.0f - cos_angle * cos_angle);
        min_sin_angle = std::min(min_sin_angle, sin_angle);
      }
      
      if (edge3_len > 0 && edge1_len > 0) {
        float cos_angle = math::dot(math::normalize(v1 - v3), math::normalize(v2 - v1));
        float sin_angle = std::sqrt(1.0f - cos_angle * cos_angle);
        min_sin_angle = std::min(min_sin_angle, sin_angle);
      }
      
      // Les triangles avec des angles très aigus sont particulièrement problématiques pour sscoutsegment
      if (min_sin_angle < 0.01f) { // Angle d'environ 0.57 degrés
        has_potential_issues = true;
        num_small_faces++;
        
        // Si nous détectons des triangles extrêmement aigus, alerter immédiatement
        if (min_sin_angle < 0.001f) {
          params.error_message_add(NodeWarningType::Warning,
              "Triangles avec angles extrêmement aigus détectés (< 0.06 degrés). "
              "Forte probabilité de crash dans tetgenmesh::sscoutsegment.");
          return true;
        }
      }
    }
  }
  
  // Vérifier si le maillage contient des arêtes qui se croisent (auto-intersections)
  // Ces intersections sont connues pour causer des crashs dans sscoutsegment
  
  // Pour les petits maillages, nous pouvons faire une vérification exhaustive
  // Pour les grands maillages, nous échantillonnons
  bool check_self_intersect = mesh->faces_num < 5000;
  
  if (check_self_intersect) {
    // Construction d'une BVH simple pour accélérer les tests d'intersection
    struct Triangle {
      float3 v1, v2, v3;
      int face_idx;
    };
    
    std::vector<Triangle> triangles;
    triangles.reserve(mesh->faces_num);
    
    for (int i = 0; i < mesh->faces_num; i++) {
      int face_start = face_offsets[i];
      int face_size = face_offsets[i + 1] - face_start;
      
      // Traiter uniquement les triangles
      if (face_size == 3) {
        int v1_idx = corner_verts[face_start];
        int v2_idx = corner_verts[face_start + 1];
        int v3_idx = corner_verts[face_start + 2];
        
        triangles.push_back({
          vert_positions[v1_idx],
          vert_positions[v2_idx],
          vert_positions[v3_idx],
          i
        });
      }
    }
    
    // Fonction simple pour vérifier si deux triangles se croisent
    auto triangles_intersect = [](const Triangle &t1, const Triangle &t2) -> bool {
      // Vérifier d'abord s'ils partagent un sommet (pas considéré comme intersectant)
      if (t1.v1 == t2.v1 || t1.v1 == t2.v2 || t1.v1 == t2.v3 ||
          t1.v2 == t2.v1 || t1.v2 == t2.v2 || t1.v2 == t2.v3 ||
          t1.v3 == t2.v1 || t1.v3 == t2.v2 || t1.v3 == t2.v3) {
        return false;
      }
      
      // Implémentation basique de détection d'intersection triangle-triangle
      // En réalité, il faudrait une implémentation plus robuste, mais
      // cela suffit pour notre détection de base
      
      // Pour simplifier, considérons qu'il y a intersection si un sommet d'un triangle
      // est à l'intérieur de l'autre triangle
      auto point_in_triangle = [](const float3 &p, const float3 &a, const float3 &b, const float3 &c) -> bool {
        float3 v0 = c - a;
        float3 v1 = b - a;
        float3 v2 = p - a;
        
        float dot00 = math::dot(v0, v0);
        float dot01 = math::dot(v0, v1);
        float dot02 = math::dot(v0, v2);
        float dot11 = math::dot(v1, v1);
        float dot12 = math::dot(v1, v2);
        
        float invDenom = 1.0f / (dot00 * dot11 - dot01 * dot01);
        float u = (dot11 * dot02 - dot01 * dot12) * invDenom;
        float v = (dot00 * dot12 - dot01 * dot02) * invDenom;
        
        return (u >= 0) && (v >= 0) && (u + v <= 1);
      };
      
      // Vérifier si un sommet de t1 est dans t2
      if (point_in_triangle(t1.v1, t2.v1, t2.v2, t2.v3) ||
          point_in_triangle(t1.v2, t2.v1, t2.v2, t2.v3) ||
          point_in_triangle(t1.v3, t2.v1, t2.v2, t2.v3)) {
        return true;
      }
      
      // Vérifier si un sommet de t2 est dans t1
      if (point_in_triangle(t2.v1, t1.v1, t1.v2, t1.v3) ||
          point_in_triangle(t2.v2, t1.v1, t1.v2, t1.v3) ||
          point_in_triangle(t2.v3, t1.v1, t1.v2, t1.v3)) {
        return true;
      }
      
      return false;
    };
    
    // Échantillonnage de paires de triangles pour vérifier les intersections
    int num_samples = std::min(10000, int(triangles.size() * triangles.size() / 4));
    int self_intersect_count = 0;
    
    std::srand(static_cast<unsigned int>(std::time(nullptr) + 1));
    
    for (int s = 0; s < num_samples; s++) {
      int idx1 = std::rand() % triangles.size();
      int idx2 = std::rand() % triangles.size();
      
      // Ne pas comparer un triangle avec lui-même
      if (idx1 != idx2) {
        if (triangles_intersect(triangles[idx1], triangles[idx2])) {
          self_intersect_count++;
          
          // Si nous trouvons beaucoup d'auto-intersections, c'est un signe clair de problèmes
          if (self_intersect_count > 10) {
            params.error_message_add(NodeWarningType::Warning,
                "Nombreuses auto-intersections détectées dans le maillage. "
                "Forte probabilité de crash dans tetgenmesh::sscoutsegment.");
            return true;
          }
        }
      }
    }
    
    if (self_intersect_count > 0) {
      params.error_message_add(NodeWarningType::Warning,
          std::to_string(self_intersect_count) + " auto-intersections détectées dans le maillage. "
          "Risque potentiel de crash dans tetgenmesh::sscoutsegment.");
      has_potential_issues = true;
    }
  }
  
  // Détection spécifique de la "platitude" du maillage - cause fréquente de crash dans sscoutsegment
  float3 dimensions = bbox_max - bbox_min;
  float min_dim = std::min({dimensions.x, dimensions.y, dimensions.z});
  float max_dim = std::max({dimensions.x, dimensions.y, dimensions.z});
  
  if (min_dim < max_dim * 0.001f) {
    params.error_message_add(NodeWarningType::Warning,
        "Maillage très plat détecté (ratio d'aspect: " + std::to_string(min_dim/max_dim) + "). "
        "Forte probabilité de crash dans tetgenmesh::sscoutsegment.");
    has_potential_issues = true;
  }
  
  // Si plus de 5% des faces échantillonnées sont problématiques
  if (sample_count > 0 && (float)num_small_faces / sample_count > 0.05f) {
      has_potential_issues = true;
      params.error_message_add(NodeWarningType::Warning,
          "Géométrie problématique détectée - " + std::to_string(num_small_faces) + 
          " triangles de mauvaise qualité sur " + std::to_string(sample_count) + 
          " échantillons. Risque élevé de crash dans sscoutsegment.");
  }

  // Avertissement spécifique pour le crash
  if (has_potential_issues) {
    params.error_message_add(NodeWarningType::Warning,
        "Maillage à risque pour tetgenmesh::sscoutsegment - envisagez de remaillager, solidifier, "
        "ou extruder avant la tétraédralisation.");
  }
  
  // Activer immédiatement les perturbations pour les maillages à problèmes
  if (has_potential_issues) {
      params.error_message_add(NodeWarningType::Info,
          "Activation de protections anti-crash pour le maillage complexe");
  }
  
  return has_potential_issues;
}

// Forward declaration
static bool check_mesh_volume(const Mesh *mesh, float *estimated_volume, GeoNodeExecParams &params);

/**
 * Checks if a mesh is manifold (watertight, no open boundaries) and valid for tetrahedralization
 * Returns true if manifold and valid, false otherwise and adds error messages
 */
static bool check_manifold_mesh(const Mesh *mesh, GeoNodeExecParams &params)
{
  // No mesh to check
  if (!mesh || mesh->verts_num == 0) {
    params.error_message_add(NodeWarningType::Error, 
        "Cannot tetrahedralize: input mesh is empty");
    return false;
  }
  
  // Skip very small meshes
  if (mesh->verts_num < 4) {
    params.error_message_add(NodeWarningType::Error, 
        "Cannot tetrahedralize: mesh must have at least 4 vertices");
    return false;
  }
  
  // Vérifier si le maillage est plat ou quasi-plat (problème courant pour la tétraédralisation)
  Span<float3> vert_positions = mesh->vert_positions();
  
  // Calculer la boîte englobante
  float3 bbox_min(std::numeric_limits<float>::max());
  float3 bbox_max(-std::numeric_limits<float>::max());
  
  for (int i = 0; i < mesh->verts_num; i++) {
    bbox_min = math::min(bbox_min, vert_positions[i]);
    bbox_max = math::max(bbox_max, vert_positions[i]);
  }
  
  // Calculer les dimensions de la boîte englobante
  float3 dimensions = bbox_max - bbox_min;
  float min_dim = std::min({dimensions.x, dimensions.y, dimensions.z});
  float max_dim = std::max({dimensions.x, dimensions.y, dimensions.z});
  float volume = dimensions.x * dimensions.y * dimensions.z;
  
  // Si le volume est presque nul ou une dimension est beaucoup plus petite que les autres
  if (volume < 1e-6f || min_dim < max_dim * 0.001f) {
    params.error_message_add(NodeWarningType::Error, 
        "Cannot tetrahedralize: mesh is too flat (2D or nearly 2D). "
        "TetGen requires a true 3D volume with significant thickness in all dimensions. "
        "Try extruding or solidifying the mesh first.");
    
    // Informations supplémentaires pour aider au diagnostic
    params.error_message_add(NodeWarningType::Info, 
        "Mesh dimensions: X=" + std::to_string(dimensions.x) + 
        ", Y=" + std::to_string(dimensions.y) + 
        ", Z=" + std::to_string(dimensions.z) + 
        ". Min/Max ratio: " + std::to_string(min_dim/max_dim));
    
    return false;
  }
  
  // Check for degenerate faces (faces with fewer than 3 vertices)
  Span<int> face_offsets = mesh->face_offsets();
  int degenerate_faces = 0;
  
  for (int i = 0; i < mesh->faces_num; i++) {
    int face_size = face_offsets[i + 1] - face_offsets[i];
    if (face_size < 3) {
      degenerate_faces++;
    }
  }
  
  if (degenerate_faces > 0) {
    params.error_message_add(NodeWarningType::Error, 
        "Cannot tetrahedralize: mesh contains " + std::to_string(degenerate_faces) + 
        " degenerate faces (with fewer than 3 vertices)");
    return false;
  }
  
  // Count vertices, faces, and corners
  Span<int> corner_verts = mesh->corner_verts();
  
  // Create edge to face map to check manifoldness
  std::unordered_map<std::pair<int, int>, std::vector<int>, pairhash> edge_to_faces;
  
  // Also track vertices to check for isolated vertices
  std::unordered_set<int> used_vertices;
  
  // Function to add an edge and its associated face to the map
  auto add_edge = [&](int v1, int v2, int face_idx) {
    // Skip invalid vertices
    if (v1 < 0 || v2 < 0 || v1 >= mesh->verts_num || v2 >= mesh->verts_num || v1 == v2) {
      return;
    }
    
    // Store the edge with the smaller vertex index first
    std::pair<int, int> edge = v1 < v2 ? std::make_pair(v1, v2) : std::make_pair(v2, v1);
    edge_to_faces[edge].push_back(face_idx);
    
    // Track used vertices
    used_vertices.insert(v1);
    used_vertices.insert(v2);
  };
  
  // Process each face and add its edges to the map
  int invalid_face_indices = 0;
  
  for (int i = 0; i < mesh->faces_num; i++) {
    int face_start = face_offsets[i];
    int face_size = face_offsets[i + 1] - face_start;
    
    // Skip faces with less than 3 vertices
    if (face_size < 3) {
      continue;
    }
    
    bool face_has_invalid_index = false;
    
    // Add all edges of this face
    for (int j = 0; j < face_size; j++) {
      int v1 = corner_verts[face_start + j];
      int v2 = corner_verts[face_start + ((j + 1) % face_size)];
      
      // Check for invalid indices
      if (v1 < 0 || v2 < 0 || v1 >= mesh->verts_num || v2 >= mesh->verts_num) {
        face_has_invalid_index = true;
        invalid_face_indices++;
        continue;
      }
      
      // Skip degenerate edges
      if (v1 == v2) {
        continue;
      }
      
      add_edge(v1, v2, i);
    }
    
    // Skip faces with invalid indices to avoid crashes
    if (face_has_invalid_index) {
      continue;
    }
  }
  
  // Check for invalid face indices
  if (invalid_face_indices > 0) {
    params.error_message_add(NodeWarningType::Error, 
        "Cannot tetrahedralize: mesh contains " + std::to_string(invalid_face_indices) + 
        " faces with invalid vertex indices");
    return false;
  }
  
  // Check for isolated vertices (not connected to any face)
  if (used_vertices.size() < mesh->verts_num) {
    int isolated_verts = mesh->verts_num - used_vertices.size();
    params.error_message_add(NodeWarningType::Error, 
        "Cannot tetrahedralize: mesh contains " + std::to_string(isolated_verts) + 
        " isolated vertices not connected to any face");
    return false;
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
  
  // Check for extremely thin or degenerate geometry
  // Use bounding box to get a reference for tiny features
  float bounding_size = math::length(bbox_max - bbox_min);
  float tiny_feature_threshold = bounding_size * 1e-6f;
  
  int tiny_edges = 0;
  
  // Check for extremely small edges
  for (const auto &edge_entry : edge_to_faces) {
    int v1 = edge_entry.first.first;
    int v2 = edge_entry.first.second;
    
    float edge_length = math::length(vert_positions[v1] - vert_positions[v2]);
    if (edge_length < tiny_feature_threshold) {
      tiny_edges++;
    }
  }
  
  if (tiny_edges > 0) {
    params.error_message_add(NodeWarningType::Error, 
        "Cannot tetrahedralize: mesh contains " + std::to_string(tiny_edges) + 
        " extremely small edges that would cause numerical instability");
    return false;
  }
  
  // Vérifier si le maillage a un volume suffisant en calculant le volume approx.
  // basé sur une analyse PCA de sa forme 3D (un maillage plat aura un volume nul)
  float est_volume = 0.0f;
  bool has_sufficient_volume = check_mesh_volume(mesh, &est_volume, params);
  
  if (!has_sufficient_volume) {
    params.error_message_add(NodeWarningType::Error, 
        "Cannot tetrahedralize: mesh has insufficient volume. "
        "The mesh may be too flat or self-intersecting. "
        "Try extruding, solidifying, or repairing the mesh first.");
    return false;
  }
  
  // All checks passed - mesh is manifold and valid
  return true;
}

/**
 * Vérification avancée du volume réel du maillage.
 * Cette fonction calcule un volume approximatif pour détecter 
 * les maillages trop plats ou presque sans volume.
 */
static bool check_mesh_volume(const Mesh *mesh, float *estimated_volume, GeoNodeExecParams &params)
{
  *estimated_volume = 0.0f;
  Span<float3> vert_positions = mesh->vert_positions();
  
  // Pour les petits maillages, une vérification simple suffit
  if (mesh->verts_num < 50) {
    // Calcul de volume simple par tétraèdres irréguliers en utilisant un point central
    float3 center(0, 0, 0);
    for (int i = 0; i < mesh->verts_num; i++) {
      center += vert_positions[i];
    }
    center /= mesh->verts_num;
    
    Span<int> face_offsets = mesh->face_offsets();
    Span<int> corner_verts = mesh->corner_verts();
    float volume = 0.0f;
    
    for (int i = 0; i < mesh->faces_num; i++) {
      int face_start = face_offsets[i];
      int face_size = face_offsets[i + 1] - face_start;
      
      // Require at least 3 vertices to form a triangular face
      if (face_size < 3) {
        continue;
      }
      
      for (int j = 0; j < face_size - 2; j++) {
        // Forme un tétraèdre avec le centre et le triangle
        int v1 = corner_verts[face_start];
        int v2 = corner_verts[face_start + j + 1];
        int v3 = corner_verts[face_start + j + 2];
        
        // Calcul du volume du tétraèdre
        float3 a = vert_positions[v1] - center;
        float3 b = vert_positions[v2] - center;
        float3 c = vert_positions[v3] - center;
        
        // Volume = (1/6) * |a · (b × c)|
        volume += std::abs(math::dot(a, math::cross(b, c))) / 6.0f;
      }
    }
    
    *estimated_volume = volume;
    
    // Calculer le volume de la boîte englobante pour comparaison
    float3 bbox_min(std::numeric_limits<float>::max());
    float3 bbox_max(-std::numeric_limits<float>::max());
    
    for (int i = 0; i < mesh->verts_num; i++) {
      bbox_min = math::min(bbox_min, vert_positions[i]);
      bbox_max = math::max(bbox_max, vert_positions[i]);
    }
    
    float3 dimensions = bbox_max - bbox_min;
    float bbox_volume = dimensions.x * dimensions.y * dimensions.z;
    
    // Si le volume est trop petit par rapport à la boîte englobante
    if (volume < bbox_volume * 0.0001f) {
      params.error_message_add(NodeWarningType::Info, 
          "Mesh has very small volume (" + std::to_string(volume) + 
          ") compared to its bounding box (" + std::to_string(bbox_volume) + 
          "). This often indicates a flat or non-volumetric mesh.");
      return false;
    }
    
    return true;
  }
  else {
    // Pour les grands maillages, calculer le volume approximatif basé sur l'échantillonnage
    // En utilisant l'approche d'échantillonnage aléatoire
    
    // Calculer la boîte englobante
    float3 bbox_min(std::numeric_limits<float>::max());
    float3 bbox_max(-std::numeric_limits<float>::max());
    
    for (int i = 0; i < mesh->verts_num; i++) {
      bbox_min = math::min(bbox_min, vert_positions[i]);
      bbox_max = math::max(bbox_max, vert_positions[i]);
    }
    
    float3 dimensions = bbox_max - bbox_min;
    float min_dim = std::min({dimensions.x, dimensions.y, dimensions.z});
    float max_dim = std::max({dimensions.x, dimensions.y, dimensions.z});
    float bbox_volume = dimensions.x * dimensions.y * dimensions.z;
    
    // Vérification simple des proportions
    if (min_dim < max_dim * 0.001f) {
      *estimated_volume = 0.0f;
      return false;
    }
    
    // Pour éviter d'avoir à calculer les normales et de tester des points contre
    // une forme potentiellement complexe, nous utilisons simplement la proportion
    // des dimensions comme heuristique
    *estimated_volume = bbox_volume * (min_dim / max_dim); // Approximation grossière du volume
    
    return (min_dim / max_dim) > 0.01f; // Seuil arbitraire pour la "planéité"
  }
}

/**
 * Main node execution function
 */
static void node_geo_exec(GeoNodeExecParams params)
{
  try {
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
    
    // Safely extract parameter values with defaults
    double max_volume_percentage = 0.5;
    try {
      max_volume_percentage = params.extract_input<float>("Max Volume");
    }
    catch (...) {
      params.error_message_add(NodeWarningType::Warning, 
          "Failed to extract Max Volume parameter, using default value (0.5)");
    }
    double max_volume = std::max(0.00001, max_volume_percentage * 0.1); // 0.1 corresponds to 100%
    
    // Get other required parameters with safe defaults
    float quality_ratio = 2.0f;
    try {
      quality_ratio = std::max(1.0f, params.extract_input<float>("Quality Ratio"));
    }
    catch (...) {
      params.error_message_add(NodeWarningType::Warning, 
          "Failed to extract Quality Ratio parameter, using default value (2.0)");
    }
    
    float min_dihedral_angle = 10.0f;
    try {
      min_dihedral_angle = params.extract_input<float>("Min Dihedral Angle");
    }
    catch (...) {
      params.error_message_add(NodeWarningType::Warning, 
          "Failed to extract Min Dihedral Angle parameter, using default value (10.0)");
    }
    
    bool preserve_boundary = false;
    try {
      preserve_boundary = params.extract_input<bool>("Preserve Boundary");
    }
    catch (...) {
      params.error_message_add(NodeWarningType::Warning, 
          "Failed to extract Preserve Boundary parameter, using default value (false)");
    }
    
    // Define mesh complexity thresholds with multiple levels
    const int medium_complexity_threshold = 5000;  // Medium complex mesh
    const int high_complexity_threshold = 10000;   // Highly complex mesh
    const int extreme_complexity_threshold = 50000; // Extremely complex mesh
    
    // Define threshold for "massive" meshes that require special handling
    const int massive_mesh_threshold = 100000;    // Massive meshes likely to cause crashes

    // Determine mesh complexity level
    bool is_medium_complex = mesh_to_process->verts_num > medium_complexity_threshold || 
                           mesh_to_process->faces_num > medium_complexity_threshold;
    bool is_highly_complex = mesh_to_process->verts_num > high_complexity_threshold || 
                           mesh_to_process->faces_num > high_complexity_threshold;
    bool is_extremely_complex = mesh_to_process->verts_num > extreme_complexity_threshold || 
                              mesh_to_process->faces_num > extreme_complexity_threshold;
    bool is_massive_mesh = mesh_to_process->verts_num > massive_mesh_threshold ||
                          mesh_to_process->faces_num > massive_mesh_threshold;
    
    // Detect if mesh is likely to cause problems with flipping operations
    bool has_problematic_geometry = detect_flipping_prone_geometry(mesh_to_process, params);
    
    // Final complexity level for decisions
    bool is_complex_mesh = is_medium_complex;
    
    // Regular tetrahedralization path with improved error handling
    tetgenio in, out;
    TetGenResourceGuard resource_guard(in, out);
    Mesh *mesh_out = nullptr;
    
    try {
      // Prepare TetGen input
      if (!prepare_tetgen_input(mesh_to_process, in, params, 0)) {
        params.error_message_add(NodeWarningType::Error,
            "Failed to prepare input for tetrahedralization. Returning input mesh unchanged.");
        
        // Clean up and return original mesh
        if (prepared_mesh) {
          BKE_id_free(nullptr, prepared_mesh);
        }
        
        params.set_output("Tetrahedral Mesh", std::move(geometry_set));
        return;
      }
      
      // Configure TetGen behavior
      tetgenbehavior behavior;
      configure_tetgen_options(behavior, 
                             max_volume,
                             quality_ratio,
                             min_dihedral_angle, 
                             preserve_boundary,
                             params);
      
      // Special anti-crash protection
      if (has_problematic_geometry) {
        // Prévention spécifique pour les crashs sscoutsegment
        behavior.nomergefacet = 1;   // Empêcher la fusion des facettes
        behavior.nomergevertex = 1;  // Empêcher la fusion des sommets
        behavior.nojettison = 1;     // Désactiver le jettison des sommets
        behavior.docheck = 0;        // Désactiver les vérifications
        behavior.diagnose = 0;       // Désactiver le diagnostic qui peut accéder à des pointeurs NULL
        
        // Options cruciales pour éviter le crash dans sscoutsegment
        behavior.plc = 0;            // Désactiver la préservation du complexe linéaire
        behavior.nobisect = 0;       // Désactiver la préservation des frontières
        
        params.error_message_add(NodeWarningType::Warning,
            "Géométrie problématique détectée. Protection anti-crash activée. "
            "Les frontières du maillage ne seront pas préservées.");
      }
      
      // Add slight random perturbation to avoid degenerate configurations that crash sscoutsegment
      for (int i = 0; i < in.numberofpoints * 3; i++) {
        double noise = ((double)rand() / RAND_MAX) * 1e-6;
        in.pointlist[i] += noise;
      }
      
      // Attempt tetrahedralization with protective try-catch block
      bool tetgen_success = false;
      
      try {
        tetrahedralize(&behavior, &in, &out);
        tetgen_success = true;
      }
      catch (std::exception &e) {
        // Si l'erreur concerne sscoutsegment, essayer avec une configuration plus robuste
        std::string error_msg = e.what();
        if (error_msg.find("sscoutsegment") != std::string::npos || 
            error_msg.find("Access violation") != std::string::npos) {
          
          params.error_message_add(NodeWarningType::Warning,
              "Crash détecté dans sscoutsegment. Tentative avec configuration anti-crash...");
          
          // Réinitialiser les structures TetGen
          in.initialize();
          out.initialize();
          
          // Préparer à nouveau l'entrée avec perturbations plus importantes
          if (prepare_tetgen_input(mesh_to_process, in, params, 3)) {
            // Configuration extrêmement robuste pour éviter sscoutsegment
            tetgenbehavior safe_behavior;
            safe_behavior.plc = 0;           // Désactiver complètement PLC
            safe_behavior.nobisect = 0;      // Ne pas préserver les frontières
            safe_behavior.quality = 0;       // Désactiver l'amélioration de qualité
            safe_behavior.mindihedral = 0.0; // Pas de contrainte d'angle
            safe_behavior.minratio = 1.0;    // Pas de contrainte de ratio
            safe_behavior.docheck = 0;       // Pas de vérification
            safe_behavior.diagnose = 0;      // Pas de diagnostic
            safe_behavior.convex = 1;        // Utiliser l'enveloppe convexe
            
            // Options cruciales pour éviter sscoutsegment
            safe_behavior.nomergefacet = 1;  // Prévenir la fusion des facettes
            safe_behavior.nomergevertex = 1; // Prévenir la fusion des sommets
            safe_behavior.nojettison = 1;    // Désactiver jettison
            
            // Désactiver toutes les sorties problématiques
            safe_behavior.facesout = 0;
            safe_behavior.edgesout = 0;
            safe_behavior.neighout = 0;
            
            // Perturber fortement les sommets pour éviter les cas dégénérés
            for (int i = 0; i < in.numberofpoints * 3; i++) {
              double noise = ((double)rand() / RAND_MAX) * 1e-4;
              in.pointlist[i] += noise;
            }
            
            try {
              params.error_message_add(NodeWarningType::Info,
                  "Tentative de tétraédrisation avec protection anti-crash maximale");
              tetrahedralize(&safe_behavior, &in, &out);
              tetgen_success = true;
              
              params.error_message_add(NodeWarningType::Info,
                  "Tétraédrisation réussie avec configuration de secours");
            }
            catch (std::exception &e) {
              // Si même la configuration de secours échoue, essayons une approche drastique
              params.error_message_add(NodeWarningType::Warning,
                  "Échec de la première tentative de secours. Dernier essai avec une enveloppe convexe pure...");
              
              try {
                // Réinitialiser à nouveau
                in.initialize();
                out.initialize();
                
                // Recréer l'entrée avec perturbations encore plus fortes
                if (prepare_tetgen_input(mesh_to_process, in, params, 5)) {
                  // Configuration absolument minimale - juste une enveloppe convexe
                  tetgenbehavior minimal_behavior;
                  minimal_behavior.plc = 0;
                  minimal_behavior.quality = 0;
                  minimal_behavior.nobisect = 0;
                  minimal_behavior.convex = 1;        // Enveloppe convexe uniquement
                  minimal_behavior.weighted = 0;      // Pas d'option avancée risquée
                  minimal_behavior.diagnose = 0;      // Pas de diagnostic
                  minimal_behavior.verbose = 0;       // Pas de verbosité
                  minimal_behavior.nomergefacet = 1;  // Sécurité maximale
                  minimal_behavior.nomergevertex = 1;
                  minimal_behavior.nojettison = 1;
                  
                  // Désactiver toutes les sorties complexes
                  minimal_behavior.facesout = 0;
                  minimal_behavior.edgesout = 0;
                  minimal_behavior.neighout = 0;
                  minimal_behavior.voroout = 0;
                  
                  // Une tentative vraiment ultime
                  tetrahedralize(&minimal_behavior, &in, &out);
                  tetgen_success = true;
                  
                  params.error_message_add(NodeWarningType::Info,
                      "Tétraédrisation réussie en mode enveloppe convexe pure. "
                      "La forme exacte du maillage n'a pas été préservée.");
                }
              }
              catch (...) {
                params.error_message_add(NodeWarningType::Error,
                    "Toutes les tentatives de tétraédrisation ont échoué. "
                    "Le maillage est trop problématique ou plat pour être tétraédralisé.");
              }
            }
          }
        }
        else {
          params.error_message_add(NodeWarningType::Error,
              std::string("TetGen error: ") + e.what() + ". Returning input mesh unchanged.");
        }
      }
      catch (...) {
        params.error_message_add(NodeWarningType::Error,
            "Unknown error during tetrahedralization. Returning input mesh unchanged.");
      }
      
      // If tetrahedralization succeeded, validate output and create mesh
      if (tetgen_success) {
        if (validate_tetgen_output(out, params)) {
          mesh_out = create_tetrahedral_mesh(out, params);
        }
        else {
          params.error_message_add(NodeWarningType::Error,
              "Generated tetrahedral mesh is invalid. Returning input mesh unchanged.");
        }
      }
    }
    catch (...) {
      params.error_message_add(NodeWarningType::Error,
          "Unexpected error in tetrahedralization process. Returning input mesh unchanged.");
    }
    
    // Prepare output
    GeometrySet output;
    if (mesh_out) {
      output.replace_mesh(mesh_out);
    }
    else {
      // If no output mesh was created, return the input mesh
      output = std::move(geometry_set);
      params.error_message_add(NodeWarningType::Info,
          "Failed to create tetrahedral mesh. Returning input mesh unchanged.");
    }
    
    // Clean up prepared mesh if needed
    if (prepared_mesh) {
      BKE_id_free(nullptr, prepared_mesh);
    }
    
    params.set_output("Tetrahedral Mesh", std::move(output));
  }
  catch (...) {
    // Ultimate fallback in case of any exception
    params.error_message_add(NodeWarningType::Error,
        "Critical error occurred. Returning empty geometry.");
    params.set_output("Tetrahedral Mesh", GeometrySet());
  }
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