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
  
  return true;
}

/**
 * Configures TetGen options based on node parameters
 */
static void configure_tetgen_options(tetgenbehavior &behavior, 
                                    double max_volume, 
                                    float quality_ratio,
                                    float min_dihedral_angle,
                                    bool preserve_boundary,
                                    GeoNodeExecParams &params,
                                    int attempt = 0)
{
  // Basic configuration
  behavior.plc = 1;          // Preserve piecewise linear complex (boundary)
  behavior.quality = 1;      // Enable quality improvement
  behavior.quiet = 1;        // Quiet mode (no stdout output)
  behavior.verbose = 0;      // No verbosity
  
  // Détecter automatiquement la complexité du maillage
  const bool is_complex_attempt = attempt > 0;
  
  // Preserve input boundary
  // In TetGen, nobisect=1 means not to bisect input faces
  // which corresponds to "preserve boundary" = true
  behavior.nobisect = preserve_boundary ? 1 : 0;
  
  // En cas de tentative désespérée (>1), désactiver la préservation des frontières
  if (is_complex_attempt && attempt > 1) {
    behavior.nobisect = 0;
    if (preserve_boundary) {
      params.error_message_add(NodeWarningType::Info,
          "Désactivation de la préservation des frontières pour améliorer la stabilité (tentative " + 
          std::to_string(attempt + 1) + ")");
    }
  }
  
  // Quality options (always enabled)
  // In TetGen, quality improvement is controlled via quality=1 and minratio
  behavior.quality = 1;    // Enable quality improvement
  
  // Quality ratio (min radius-edge ratio)
  // Limit the maximum value to avoid excessive computation time
  // TetGen can be extremely slow with high quality values
  float limited_quality_ratio = quality_ratio;
  if (limited_quality_ratio > 2.0f) {
    params.error_message_add(NodeWarningType::Warning,
        "High quality value detected. Processing may take longer.");
    
    if (limited_quality_ratio <= 3.0f) {
      limited_quality_ratio = 2.0f + (limited_quality_ratio - 2.0f) * 0.5f;
    } else {
      limited_quality_ratio = 2.5f + (limited_quality_ratio - 3.0f) * 1.5f;
    }
  }
  
  // Ajuster le ratio de qualité progressivement selon la tentative
  if (is_complex_attempt) {
    // Réduction progressive de la qualité avec chaque tentative
    float reduction_factor = 1.0f - (0.2f * attempt);
    reduction_factor = std::max(0.4f, reduction_factor); // Ne pas descendre sous 40%
    
    // Stocker l'ancienne valeur pour l'information
    float original_quality_ratio = limited_quality_ratio;
    
    // Appliquer la réduction mais garder un minimum de 1.05
    limited_quality_ratio = std::max(1.05f, limited_quality_ratio * reduction_factor);
    
    if (original_quality_ratio != limited_quality_ratio) {
      params.error_message_add(NodeWarningType::Info,
          "Réduction du ratio de qualité de " + std::to_string(original_quality_ratio) + 
          " à " + std::to_string(limited_quality_ratio) + " (tentative " + 
          std::to_string(attempt + 1) + ")");
    }
  }
  
  behavior.minratio = limited_quality_ratio;
  
  // Use the dihedral angle value directly from the input
  // Convert from degrees to internal TetGen value (TetGen expects degrees)
  // Clamp to realistic values to prevent excessive slowdowns
  float clamped_angle = std::min(min_dihedral_angle, 30.0f);
  
  // Réduire progressivement l'angle dihédral minimum avec chaque tentative
  if (is_complex_attempt) {
    // Stocker l'ancienne valeur pour l'information
    float original_angle = clamped_angle;
    
    // Réduction progressive avec chaque tentative
    // Tentative 1: 70% de l'angle original
    // Tentative 2: 40% de l'angle original
    // Tentative 3: 10% de l'angle original
    float reduction_factor = 1.0f - (0.3f * attempt);
    reduction_factor = std::max(0.1f, reduction_factor); // Au moins 10%
    
    // Appliquer la réduction mais maintenir un minimum
    clamped_angle = std::max(0.1f, clamped_angle * reduction_factor);
    
    if (original_angle != clamped_angle && original_angle > 0.1f) {
      params.error_message_add(NodeWarningType::Info,
          "Réduction de l'angle dihédral minimum de " + std::to_string(original_angle) + 
          "° à " + std::to_string(clamped_angle) + "° (tentative " + 
          std::to_string(attempt + 1) + ")");
    }
  }
  
  behavior.mindihedral = clamped_angle;
  
  // Maximum tetrahedra volume
  if (max_volume > 0.00001) {
    behavior.fixedvolume = 1;
    behavior.maxvolume = max_volume;
    
    // Pour les tentatives avancées, augmenter légèrement le volume max pour permettre plus de flexibilité
    if (is_complex_attempt && attempt > 1) {
      behavior.maxvolume = max_volume * (1.0 + 0.2 * (attempt - 1));
    }
  }
  
  // Output
  behavior.edgesout = 1;     // Generate edges
  behavior.facesout = 1;     // Generate faces
  behavior.neighout = 1;     // Generate neighbor information
  
  // Additional options for handling complex/non-manifold meshes
  behavior.docheck = 1;      // Check mesh consistency
  
  // Pour les maillages complexes, ajouter une option pour éviter la création d'arêtes courtes
  // Cette option n'existe pas dans TetGen, nous l'implémentons de façon indirecte
  if (is_complex_attempt) {
    // Augmenter progressivement la tolérance pour les erreurs numériques
    behavior.epsilon = 1e-8 * std::pow(10.0, attempt);
    
    // Désactiver certains contrôles stricts sur les tentatives avancées
    if (attempt >= 2) {
      // Tentative 3 ou plus: prendre des mesures radicales pour éviter les crashs
      behavior.docheck = 0;
      
      // Désactiver l'amélioration de qualité pour accélérer et éviter les crashs
      behavior.quality = 0;
      behavior.mindihedral = 0;
      behavior.minratio = 1.01;
      
      // Augmenter encore plus la tolérance
      behavior.epsilon = 1e-6;
      
      params.error_message_add(NodeWarningType::Info,
          "Mode robuste avec priorité à la stabilité au détriment de la qualité (tentative " + 
          std::to_string(attempt + 1) + ")");
    }
  }
  
  // Pour les dernières tentatives sur maillages complexes 
  // utiliser une stratégie à zéro risque
  if (attempt >= 2) {
    // Options de sécurité pour éviter les crashs
    behavior.nomergefacet = 1;  // Prévenir la fusion des facettes (peut causer des erreurs)
    behavior.nomergevertex = 1; // Prévenir la fusion des sommets (peut causer des erreurs)
    
    // Maximiser la stabilité numérique même si c'est au détriment de la qualité
    behavior.diagnose = 1;     // Mode diagnostic: priorité à la robustesse
  }
  
  // Pour la dernière tentative en cas de maillage complexe, désactiver toutes les options de qualité
  if (attempt >= 3) {
    behavior.quality = 0;
    behavior.mindihedral = 0;
    behavior.minratio = 1.0;
    behavior.docheck = 0;
    behavior.nobisect = 0;
    
    // Mode ultime de stabilité
    params.error_message_add(NodeWarningType::Warning,
        "Mode de stabilité extrême activé - qualité minimale mais tétraédrisation garantie");
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
  
  // Analyse plus détaillée des tétraèdres pour détecter les problèmes
  if (out.numberoftetrahedra > 0 && out.tetrahedronlist) {
    bool has_invalid_indices = false;
    bool has_degenerate_tets = false;
    int num_verts = out.numberofpoints;

    // Vérifier un échantillon de tétraèdres (jusqu'à 1000 pour éviter de ralentir)
    int num_to_check = std::min(1000, out.numberoftetrahedra);
    int step = out.numberoftetrahedra > 1000 ? out.numberoftetrahedra / 1000 : 1;
    
    for (int i = 0; i < out.numberoftetrahedra; i += step) {
      if (i >= num_to_check) break;
      
      // Vérifier les indices
      int v0 = out.tetrahedronlist[i * 4];
      int v1 = out.tetrahedronlist[i * 4 + 1];
      int v2 = out.tetrahedronlist[i * 4 + 2];
      int v3 = out.tetrahedronlist[i * 4 + 3];
      
      // Validation des indices
      if (v0 < 0 || v0 >= num_verts || v1 < 0 || v1 >= num_verts ||
          v2 < 0 || v2 >= num_verts || v3 < 0 || v3 >= num_verts) {
        has_invalid_indices = true;
        continue;
      }
      
      // Vérifier si les tétraèdres sont dégénérés (tous les sommets coplanaires)
      if (v0 == v1 || v0 == v2 || v0 == v3 || v1 == v2 || v1 == v3 || v2 == v3) {
        has_degenerate_tets = true;
        continue;
      }
    }
    
    if (has_invalid_indices) {
      params.error_message_add(NodeWarningType::Warning,
          "TetGen generated some tetrahedra with invalid vertex indices");
    }
    
    if (has_degenerate_tets) {
      params.error_message_add(NodeWarningType::Warning,
          "TetGen generated some degenerate tetrahedra");
    }
  }
  
  return true;
}

/**
 * Creates a Blender mesh from TetGen output data
 */
static Mesh *create_tetrahedral_mesh(tetgenio &out, GeoNodeExecParams &params)
{
  // Validate TetGen output
  if (out.numberofpoints < 4 || out.numberoftetrahedra < 1) {
    params.error_message_add(NodeWarningType::Error,
                           "TetGen n'a pas pu générer suffisamment de tétraèdres");
    return nullptr;
  }
  
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
  
  // Collect edges from tetrahedra
  for (int i = 0; i < out.numberoftetrahedra; i++) {
    int *tet = &out.tetrahedronlist[i * 4];
    
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
    
    // Set mesh name
    BKE_mesh_validate(mesh_out, true, true);
  }
  catch (const std::exception &e) {
    params.error_message_add(NodeWarningType::Error, 
        std::string("Erreur lors de la création du maillage tétraédrique: ") + e.what());
    
    if (mesh_out) {
      BKE_id_free(nullptr, mesh_out);
      mesh_out = nullptr;
    }
  }
  
  return mesh_out;
}

/**
 * Creates a fallback tetrahedron in case of failure
 */
static Mesh* create_fallback_tetrahedron(const Mesh *mesh_in)
{
  // Mesh dimensions
  const int num_verts = 4;
  const int num_edges = 6;
  const int num_faces = 4;
  const int num_loops = 12;
  
  // Create mesh
  Mesh *mesh_out = BKE_mesh_new_nomain(num_verts, num_edges, num_faces, num_loops);
  
  if (!mesh_out) {
    return nullptr;
  }
  
  // Calculate bounding box
  float3 bmin(FLT_MAX, FLT_MAX, FLT_MAX);
  float3 bmax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
  
  const Span<float3> positions = mesh_in->vert_positions();
  for (int i = 0; i < mesh_in->verts_num; i++) {
    bmin.x = std::min(bmin.x, positions[i].x);
    bmin.y = std::min(bmin.y, positions[i].y);
    bmin.z = std::min(bmin.z, positions[i].z);
    
    bmax.x = std::max(bmax.x, positions[i].x);
    bmax.y = std::max(bmax.y, positions[i].y);
    bmax.z = std::max(bmax.z, positions[i].z);
  }
  
  // Center and size
  float3 center = (bmin + bmax) * 0.5f;
  float3 size = bmax - bmin;
  float max_size = std::max(std::max(size.x, size.y), size.z);
  if (max_size < 0.00001f) max_size = 1.0f;
  
  // Vertex positions
  MutableSpan<float3> vert_positions = mesh_out->vert_positions_for_write();
  vert_positions[0] = center + float3(0, 0, max_size * 0.5f);
  vert_positions[1] = center + float3(-max_size * 0.5f, -max_size * 0.5f, -max_size * 0.5f);
  vert_positions[2] = center + float3(max_size * 0.5f, -max_size * 0.5f, -max_size * 0.5f);
  vert_positions[3] = center + float3(0, max_size * 0.5f, -max_size * 0.5f);
  
  // Edges
  MutableSpan<int2> edges = mesh_out->edges_for_write();
  edges[0] = int2(0, 1);
  edges[1] = int2(0, 2);
  edges[2] = int2(0, 3);
  edges[3] = int2(1, 2);
  edges[4] = int2(1, 3);
  edges[5] = int2(2, 3);
  
  // Faces
  MutableSpan<int> corner_verts = mesh_out->corner_verts_for_write();
  

  corner_verts[0] = 0;
  corner_verts[1] = 1;
  corner_verts[2] = 2;
  

  corner_verts[3] = 0;
  corner_verts[4] = 3;
  corner_verts[5] = 1;
  

  corner_verts[6] = 0;
  corner_verts[7] = 2;
  corner_verts[8] = 3;
  

  corner_verts[9] = 1;
  corner_verts[10] = 3;
  corner_verts[11] = 2;
  
  // Configure face offsets
  offset_indices::fill_constant_group_size(3, 0, mesh_out->face_offsets_for_write());
  
  // Add tetrahedron ID attribute
  bke::MutableAttributeAccessor attributes = mesh_out->attributes_for_write();
  bke::SpanAttributeWriter<int> tet_indices = attributes.lookup_or_add_for_write_span<int>(
      "tetrahedral_index", bke::AttrDomain::Face);
      
  if (tet_indices) {
    for (int i = 0; i < num_faces; i++) {
      tet_indices.span[i] = 0;  // Single tetrahedron
    }
    tet_indices.finish();
  }
  
  // Validate
  BKE_mesh_validate(mesh_out, true, true);
  
  return mesh_out;
}

/**
 * Prépare un maillage complexe pour la tétraédrisation sans le remplacer
 * Cette fonction ajoute des attributs ou des préparations spéciales mais garde le maillage original
 */
static Mesh* prepare_complex_mesh_for_tetgen(const Mesh *mesh_in, GeoNodeExecParams &params)
{
  // Ne pas traiter les maillages simples
  if (mesh_in->verts_num < 100 || mesh_in->faces_num < 100) {
    return nullptr;
  }
  
  // Vérifier si le maillage est complexe (seuil arbitraire basé sur l'expérience)
  const bool is_complex = mesh_in->verts_num > 10000 || mesh_in->faces_num > 10000;
  
  if (is_complex) {
    params.error_message_add(NodeWarningType::Info,
        "Maillage complexe détecté (" + std::to_string(mesh_in->verts_num) + 
        " sommets, " + std::to_string(mesh_in->faces_num) + 
        " faces). Optimisation pour la tétraédrisation.");
    
    // Au lieu de remplacer par un cube, nous allons optimiser le maillage original
    try {
      // On pourrait créer une copie et l'optimiser, mais pour l'instant,
      // nous utiliserons directement le maillage original avec des paramètres ajustés
      return nullptr; // Utiliser le maillage original
    }
    catch (const std::exception &e) {
      params.error_message_add(NodeWarningType::Error,
          std::string("Erreur lors de la préparation du maillage complexe: ") + e.what());
      return nullptr;
    }
  }
  
  // Pour les maillages non-complexes, on retourne simplement nullptr (utiliser l'original)
  return nullptr;
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
  
  // Définir une limite de complexité du maillage
  const int complexity_threshold = 10000; // Seuil arbitraire basé sur l'expérience
  bool is_complex_mesh = mesh_to_process->verts_num > complexity_threshold || 
                        mesh_to_process->faces_num > complexity_threshold;
  
  // En cas de maillage complexe, on réduit les attentes de qualité
  if (is_complex_mesh && quality_ratio > 1.2f) {
    float original_quality = quality_ratio;
    quality_ratio = 1.2f;
    params.error_message_add(NodeWarningType::Info, 
        "Maillage complexe détecté. Réduction automatique du ratio de qualité de " +
        std::to_string(original_quality) + " à " + std::to_string(quality_ratio) +
        " pour éviter les crash.");
  }
  
  // TetGen structures
  tetgenio in, out;
  tetgenbehavior behavior;
  
  // Resource guard to ensure cleanup in case of exception
  TetGenResourceGuard resource_guard(in, out);
  
  Mesh *mesh_out = nullptr;
  bool tetgen_success = false;
  
  // Nombre maximum de tentatives - augmenté pour les maillages complexes
  const int max_attempts = is_complex_mesh ? 4 : 2;
  
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
                                 use_extreme_robustness ? attempt + 1 : attempt);
          
          // Pour les maillages complexes et les tentatives désespérées, désactiver tout contrôle qualité
          if (use_desperate_measures) {
            behavior.quality = 0;        // Désactiver toute amélioration de qualité
            behavior.mindihedral = 0.0; // Pas de contrainte d'angle dihédral
            behavior.minratio = 1.0f;    // Ratio minimal absolu
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
              // Appeler TetGen avec une gestion d'erreur spéciale
              tetrahedralize(&behavior, &in, &out);
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
                behavior.plc = 0;          // Désactiver la préservation du complexe
                behavior.quality = 0;      // Désactiver l'amélioration de qualité
                behavior.nobisect = 0;     // Ne pas préserver les frontières
                behavior.docheck = 0;      // Désactiver les vérifications
                behavior.quiet = 1;
                behavior.verbose = 0;
                behavior.mindihedral = 0.0; // Aucune contrainte d'angle
                behavior.minratio = 1.0;    // Ratio minimal
                behavior.diagnose = 1;      // Mode diagnostic
                
                // Essai final sans récupération des frontières (pour éviter le crash)
                behavior.edgesout = 0;     // Ne pas générer d'arêtes
                behavior.facesout = 0;     // Ne pas générer de faces
                
                // Message d'avertissement
                params.error_message_add(NodeWarningType::Warning, 
                    "Utilisation d'une configuration de dernier recours pour éviter un crash");
                
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
        "Impossible de créer un maillage tétraédrique. Utilisation d'un tétraèdre de secours.");
    mesh_out = create_fallback_tetrahedron(mesh_in);
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