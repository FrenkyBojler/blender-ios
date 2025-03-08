/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_vector.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"
#include "BLI_array.hh"
#include <algorithm> // Pour std::min

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_node_types.h"

#include "BKE_attribute.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_legacy_convert.hh"
#include "BKE_lib_id.hh"
#include "BKE_customdata.hh"  // Pour les domaines d'attributs

#include "UI_interface.hh"
#include "UI_resources.hh"
#include "RNA_access.hh"
#include "RNA_define.hh"

#include "node_geometry_util.hh"
#include "NOD_register.hh"

// Inclure TetGen
#include "tetgen.h"

using namespace blender;
using namespace blender::bke;

namespace blender::nodes::node_geo_tetrahedralize_cc {

/* Stockage des données du nœud */
struct NodeGeometryTetrahedralize {
  float base_size;
  float max_tet_scale;
  float min_triangle_scale;
  float local_feature_scale;
  bool use_manual_base_size;
  char scale_attribute_name[64];
  char _pad[4];
};

/* Méthode de mise à l'échelle locale */
enum LocalScalingMethod {
  SCALING_NONE = 0,
  SCALING_FEATURE_SIZE = 1,
  SCALING_POINT_ATTRIBUTE = 2,
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Mesh").supported_type(GeometryComponent::Type::Mesh);
  b.add_input<decl::Float>("Scale").default_value(0.1f).min(0.00001f).max(10.0f)
      .description("Contrôle la taille maximale des tétraèdres générés");
  b.add_input<decl::Float>("Quality Ratio").default_value(1.4f).min(0.0f).max(2.0f)
      .description("Ratio qualité/forme des tétraèdres (0=moins strict, 2=plus strict)");
  b.add_output<decl::Geometry>("Tetrahedral Mesh").propagate_all();
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "local_scaling", static_cast<eUI_Item_Flag>(0), "", ICON_NONE);
  uiLayout *col = uiLayoutColumn(layout, false);
  uiLayoutSetActive(col, RNA_enum_get(ptr, "local_scaling") == SCALING_FEATURE_SIZE);
  uiItemR(col, ptr, "local_feature_scale", static_cast<eUI_Item_Flag>(0), std::nullopt, ICON_NONE);
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

// Fonctions utilitaires pour une intégration robuste de TetGen

/**
 * Nettoie correctement les ressources TetGen d'entrée
 */
static void cleanup_tetgen_input(tetgenio &in) {
  if (in.pointlist) {
    delete[] in.pointlist;
    in.pointlist = nullptr;
  }
  
  if (in.facetlist) {
    for (int i = 0; i < in.numberoffacets; i++) {
      if (in.facetlist[i].polygonlist) {
        for (int j = 0; j < in.facetlist[i].numberofpolygons; j++) {
          if (in.facetlist[i].polygonlist[j].vertexlist) {
            delete[] in.facetlist[i].polygonlist[j].vertexlist;
          }
        }
        delete[] in.facetlist[i].polygonlist;
      }
      if (in.facetlist[i].holelist) {
        delete[] in.facetlist[i].holelist;
      }
    }
    delete[] in.facetlist;
    in.facetlist = nullptr;
  }
}

/**
 * Nettoie correctement les ressources TetGen de sortie
 */
static void cleanup_tetgen_output(tetgenio &out) {
  if (out.pointlist) {
    delete[] out.pointlist;
    out.pointlist = nullptr;
  }
  
  if (out.tetrahedronlist) {
    delete[] out.tetrahedronlist;
    out.tetrahedronlist = nullptr;
  }
  
  if (out.neighborlist) {
    delete[] out.neighborlist;
    out.neighborlist = nullptr;
  }
  
  if (out.edgelist) {
    delete[] out.edgelist;
    out.edgelist = nullptr;
  }
  
  if (out.facetlist) {
    delete[] out.facetlist;
    out.facetlist = nullptr;
  }
}

/**
 * Prépare les données d'entrée TetGen à partir d'un maillage Blender en utilisant les APIs appropriées
 */
static bool prepare_tetgen_input(const Mesh *mesh_in, tetgenio &in, GeoNodeExecParams &params) {
  // Initialisation complète de la structure TetGen
  in.initialize();
  in.firstnumber = 0;
  
  // Vérification des données d'entrée
  if (!mesh_in->faces_num) {
    params.error_message_add(NodeWarningType::Error, 
        "Le maillage d'entrée doit contenir des faces pour la tétraèdralisation.");
    return false;
  }
  
  try {
    // Récupération des données du maillage Blender en utilisant les accesseurs Blender
    const Span<float3> positions = mesh_in->vert_positions();
    const OffsetIndices faces = mesh_in->faces();
    const Span<int> corner_verts = mesh_in->corner_verts();
    
    // Points d'entrée
    in.numberofpoints = mesh_in->verts_num;
    in.pointlist = new REAL[in.numberofpoints * 3];
    
    for (int i = 0; i < mesh_in->verts_num; i++) {
      in.pointlist[i * 3] = positions[i].x;
      in.pointlist[i * 3 + 1] = positions[i].y;
      in.pointlist[i * 3 + 2] = positions[i].z;
    }
    
    // Configuration des faces avec vérification de validité
    in.numberoffacets = mesh_in->faces_num;
    in.facetlist = new tetgenio::facet[in.numberoffacets];
    
    for (int i = 0; i < mesh_in->faces_num; i++) {
      tetgenio::facet *f = &in.facetlist[i];
      f->numberofholes = 0;
      f->holelist = nullptr;
      
      const IndexRange face = faces[i];
      int vcount = face.size();
      
      // Vérifier que c'est un polygone valide (au moins 3 sommets)
      if (vcount < 3) {
        continue;
      }
      
      f->numberofpolygons = 1;
      f->polygonlist = new tetgenio::polygon[1];
      f->polygonlist[0].numberofvertices = vcount;
      f->polygonlist[0].vertexlist = new int[vcount];
      
      // Copie des indices dans l'ordre correct
      for (int j = 0; j < vcount; j++) {
        int vertex_index = corner_verts[face[j]];
        
        // Vérification des limites des indices
        if (vertex_index >= 0 && vertex_index < mesh_in->verts_num) {
          f->polygonlist[0].vertexlist[j] = vertex_index;
        }
        else {
          f->polygonlist[0].vertexlist[j] = 0;  // Valeur de secours
        }
      }
    }
    
    return true;
  }
  catch (const std::exception &e) {
    params.error_message_add(NodeWarningType::Error, 
        std::string("Erreur lors de la préparation des données TetGen: ") + e.what());
    cleanup_tetgen_input(in);
    return false;
  }
}

/**
 * Configure les options TetGen de manière robuste en utilisant l'API TetGen
 */
static void setup_tetgen_behavior(tetgenbehavior &behavior, float scale, float quality, GeoNodeExecParams &params) {
  // Créer une chaîne d'options complète et robuste pour TetGen
  char tetgen_options[128] = "pqzQO";  // Options de base (p:PLC, q:qualité, O:optimiser, Q:quiet)
  
  // Qualité avec limite pour éviter les blocages
  if (quality > 0.0f) {
    char q_option[32];
    double safe_quality = std::min(static_cast<double>(quality), 1.5);
    snprintf(q_option, sizeof(q_option), "%g", safe_quality);
    strcat(tetgen_options, q_option);
  }
  
  // Volume maximum avec échelle
  if (scale > 0.0f) {
    strcat(tetgen_options, "a");
    char a_option[32];
    double volume = scale * scale * scale;
    snprintf(a_option, sizeof(a_option), "%g", volume);
    strcat(tetgen_options, a_option);
  }
  
  // Ajouter options pour générer faces et arêtes (important pour la visualisation)
  strcat(tetgen_options, "fe");
  
  // Conserver la cohérence des entrées/sorties
  strcat(tetgen_options, "n");
  
  // Informer l'utilisateur des options utilisées
  params.error_message_add(NodeWarningType::Info, 
      std::string("Options TetGen: ") + tetgen_options);
  
  // Configuration via l'API TetGen
  char* argv[3] = {(char*)"tetgen", tetgen_options, nullptr};
  behavior.parse_commandline(2, argv);
  
  // Configuration explicite des options clés
  behavior.plc = 1;          // préserver le complexe polyédrique linéaire (frontière)
  behavior.quality = 1;      // améliorer la qualité des tétraèdres
  behavior.quiet = 1;        // limiter la sortie console
  behavior.nobisect = 1;     // empêcher l'intersection des frontières
  behavior.edgesout = 1;     // sortie des arêtes
  behavior.facesout = 1;     // sortie des faces
  behavior.neighout = 1;     // informations sur les voisins
}

/**
 * Crée un maillage Blender à partir des résultats TetGen en utilisant l'API Blender
 */
static Mesh* create_mesh_from_tetgen_output(tetgenio &out, GeoNodeExecParams &params, int max_tets = 10000) {
  // Limiter le nombre de tétraèdres pour la stabilité et les performances
  int num_tets = std::min(out.numberoftetrahedra, max_tets);
  
  // Logs informatifs
  params.error_message_add(NodeWarningType::Info, 
      std::string("TetGen a généré ") + std::to_string(out.numberoftetrahedra) + 
      " tétraèdres et " + std::to_string(out.numberofpoints) + " points");
  
  if (num_tets < out.numberoftetrahedra) {
    params.error_message_add(NodeWarningType::Warning, 
        "Nombre de tétraèdres limité à " + std::to_string(num_tets) + 
        " pour des raisons de performance (sur " + std::to_string(out.numberoftetrahedra) + ")");
  }
  
  // Calcul des dimensions du maillage
  const int max_edges = num_tets * 6;  // 6 arêtes max par tétraèdre
  const int num_faces = num_tets * 4;  // 4 faces par tétraèdre
  const int num_loops = num_faces * 3; // 3 sommets par face triangulaire
  
  // Création du maillage avec Blender API
  Mesh *mesh_out = BKE_mesh_new_nomain(out.numberofpoints, max_edges, num_faces, num_loops);
  
  if (!mesh_out) {
    params.error_message_add(NodeWarningType::Error, 
        "Impossible de créer le maillage Blender (mémoire insuffisante?)");
    return nullptr;
  }
  
  try {
    // 1. Copie des positions des sommets avec l'API Blender
    MutableSpan<float3> vert_positions = mesh_out->vert_positions_for_write();
    for (int i = 0; i < out.numberofpoints; i++) {
      vert_positions[i].x = out.pointlist[i * 3];
      vert_positions[i].y = out.pointlist[i * 3 + 1];
      vert_positions[i].z = out.pointlist[i * 3 + 2];
    }
    
    // 2. Collection et création des arêtes et des faces
    MutableSpan<int> corner_verts_out = mesh_out->corner_verts_for_write();
    int corner_index = 0;
    int face_count = 0;
    
    // Collection des arêtes uniques
    Vector<int2> unique_edges;
    unique_edges.reserve(max_edges);
    
    // Fonction d'ajout d'arête unique
    auto add_unique_edge = [&unique_edges](int v1, int v2) {
      // Normalisation des indices (le plus petit en premier)
      if (v1 > v2) std::swap(v1, v2);
      
      // Recherche des doublons
      for (const int2 &e : unique_edges) {
        if (e.x == v1 && e.y == v2) return;
      }
      
      // Ajout uniquement si arête unique
      unique_edges.append(int2(v1, v2));
    };
    
    // Parcours des tétraèdres pour extraire les faces et arêtes
    for (int i = 0; i < num_tets && face_count < num_faces; i++) {
      // Récupération des indices avec vérification
      if ((i * 4 + 3) >= (out.numberoftetrahedra * 4)) {
        break;  // Sécurité supplémentaire
      }
      
      int v0 = out.tetrahedronlist[i * 4];
      int v1 = out.tetrahedronlist[i * 4 + 1];
      int v2 = out.tetrahedronlist[i * 4 + 2];
      int v3 = out.tetrahedronlist[i * 4 + 3];
      
      // Vérification complète des indices
      if (v0 < 0 || v0 >= out.numberofpoints ||
          v1 < 0 || v1 >= out.numberofpoints ||
          v2 < 0 || v2 >= out.numberofpoints ||
          v3 < 0 || v3 >= out.numberofpoints) {
        continue;  // Ignorer les tétraèdres invalides
      }
      
      // Ajout des arêtes uniques
      add_unique_edge(v0, v1);
      add_unique_edge(v0, v2);
      add_unique_edge(v0, v3);
      add_unique_edge(v1, v2);
      add_unique_edge(v1, v3);
      add_unique_edge(v2, v3);
      
      // Vérification de l'espace disponible pour les triangles
      if (corner_index + 12 > num_loops) {
        params.error_message_add(NodeWarningType::Warning, 
            "Limite de faces atteinte, certains tétraèdres peuvent être incomplets");
        break;
      }
      
      // Orientation cohérente des faces pour faciliter le rendu
      // Face 1: v0-v1-v2
      corner_verts_out[corner_index++] = v0;
      corner_verts_out[corner_index++] = v1;
      corner_verts_out[corner_index++] = v2;
      face_count++;
      
      // Face 2: v0-v1-v3
      corner_verts_out[corner_index++] = v0;
      corner_verts_out[corner_index++] = v1;
      corner_verts_out[corner_index++] = v3;
      face_count++;
      
      // Face 3: v0-v2-v3
      corner_verts_out[corner_index++] = v0;
      corner_verts_out[corner_index++] = v2;
      corner_verts_out[corner_index++] = v3;
      face_count++;
      
      // Face 4: v1-v2-v3
      corner_verts_out[corner_index++] = v1;
      corner_verts_out[corner_index++] = v2;
      corner_verts_out[corner_index++] = v3;
      face_count++;
    }
    
    // Copie des arêtes uniques dans le maillage
    MutableSpan<int2> edges = mesh_out->edges_for_write();
    int edge_count = std::min<int>(unique_edges.size(), max_edges);
    
    for (int i = 0; i < edge_count; i++) {
      edges[i] = unique_edges[i];
    }
    
    // Configuration des offsets de faces avec l'API Blender
    offset_indices::fill_constant_group_size(3, 0, mesh_out->face_offsets_for_write());
    
    // Ajout d'une couche de données custom pour les indices de tétraèdres si nécessaire
    // Note: On n'utilise pas AttributeIDRef qui n'est pas disponible dans cette version
    if (num_tets > 0 && face_count > 0) {
      // Créer une couche de données pour stocker l'indice du tétraèdre
      // Dans cette version de Blender, nous utilisons directement la CustomData de face
      int *tet_indices = static_cast<int *>(
          CustomData_add_layer_named(&mesh_out->face_data, CD_PROP_INT32, CD_CONSTRUCT, face_count, "tetrahedral_index"));
      
      if (tet_indices) {
        // Assignation des indices de tétraèdres aux faces (4 faces par tétraèdre)
        for (int i = 0; i < face_count; i++) {
          tet_indices[i] = i / 4;  // Division entière pour obtenir l'indice du tétraèdre
        }
      }
    }
    
    // Validation complète du maillage avec l'API Blender
    BKE_mesh_validate(mesh_out, true, true);
    
    // Force le recalcul des normales au prochain accès
    mesh_out->runtime->bounds_cache.tag_dirty();
    
    return mesh_out;
  }
  catch (const std::exception &e) {
    // En cas d'erreur, nettoyer les ressources
    if (mesh_out) {
      mesh_out->~Mesh();
      MEM_freeN(mesh_out);
    }
    params.error_message_add(NodeWarningType::Error, 
        std::string("Erreur lors de la création du maillage: ") + e.what());
    return nullptr;
  }
}

/**
 * Crée un tétraèdre de secours basé sur la boîte englobante
 */
static Mesh* create_fallback_tetrahedron(const Mesh *mesh_in, float scale, float quality) {
  // Dimensions du maillage
  const int num_verts = 4;
  const int num_edges = 6;
  const int num_faces = 4;
  const int num_loops = 12;
  
  // Créer le maillage vide
  Mesh *mesh_out = BKE_mesh_new_nomain(num_verts, num_edges, num_faces, num_loops);
  
  if (!mesh_out) {
    return nullptr;
  }
  
  // Calculer la boîte englobante du maillage d'entrée
  float3 bmin(FLT_MAX, FLT_MAX, FLT_MAX);
  float3 bmax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
  
  const Span<float3> in_positions = mesh_in->vert_positions();
  for (int i = 0; i < mesh_in->verts_num; i++) {
    bmin.x = std::min(bmin.x, in_positions[i].x);
    bmin.y = std::min(bmin.y, in_positions[i].y);
    bmin.z = std::min(bmin.z, in_positions[i].z);
    
    bmax.x = std::max(bmax.x, in_positions[i].x);
    bmax.y = std::max(bmax.y, in_positions[i].y);
    bmax.z = std::max(bmax.z, in_positions[i].z);
  }
  
  // Calculer le centre et la taille
  float3 size = bmax - bmin;
  float max_size = std::max(std::max(size.x, size.y), size.z);
  if (max_size < 0.0001f) max_size = 1.0f;
  
  float3 center = (bmax + bmin) * 0.5f;
  float s = max_size * scale * (1.0f + quality * 0.5f);
  
  // Définir les positions des sommets du tétraèdre
  MutableSpan<float3> vert_positions = mesh_out->vert_positions_for_write();
  vert_positions[0] = center;                            // Centre
  vert_positions[1] = center + float3(s, 0.0f, 0.0f);    // +X
  vert_positions[2] = center + float3(0.0f, s, 0.0f);    // +Y
  vert_positions[3] = center + float3(0.0f, 0.0f, s);    // +Z
  
  // Définir les triangles (faces)
  MutableSpan<int> corner_verts = mesh_out->corner_verts_for_write();
  
  // Face 1: 0-1-2
  corner_verts[0] = 0;
  corner_verts[1] = 1;
  corner_verts[2] = 2;
  
  // Face 2: 0-1-3
  corner_verts[3] = 0;
  corner_verts[4] = 1;
  corner_verts[5] = 3;
  
  // Face 3: 0-2-3
  corner_verts[6] = 0;
  corner_verts[7] = 2;
  corner_verts[8] = 3;
  
  // Face 4: 1-2-3
  corner_verts[9] = 1;
  corner_verts[10] = 2;
  corner_verts[11] = 3;
  
  // Définir les arêtes
  MutableSpan<int2> edges = mesh_out->edges_for_write();
  edges[0] = int2(0, 1);  // Centre à +X
  edges[1] = int2(0, 2);  // Centre à +Y
  edges[2] = int2(0, 3);  // Centre à +Z
  edges[3] = int2(1, 2);  // +X à +Y
  edges[4] = int2(1, 3);  // +X à +Z
  edges[5] = int2(2, 3);  // +Y à +Z
  
  // Configurer les offsets de faces
  offset_indices::fill_constant_group_size(3, 0, mesh_out->face_offsets_for_write());
  
  // Valider le maillage
  BKE_mesh_validate(mesh_out, true, true);
  
  return mesh_out;
}

/* Implémentation robuste de la tétraèdralisation avec TetGen */
static void node_geo_exec(GeoNodeExecParams params)
{
  // Extraction et validation des données d'entrée avec l'API Blender
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh");
  
  if (!geometry_set.has_mesh()) {
    params.error_message_add(NodeWarningType::Error, 
        "Entrée requise: un maillage pour la tétraèdralisation");
    params.set_output("Tetrahedral Mesh", GeometrySet());
    return;
  }
  
  const Mesh *mesh_in = geometry_set.get_mesh();
  if (!mesh_in || mesh_in->verts_num < 4) {
    params.error_message_add(NodeWarningType::Error, 
        "Le maillage nécessite au moins 4 sommets pour former un tétraèdre");
    params.set_output("Tetrahedral Mesh", GeometrySet());
    return;
  }

  // Préparation des paramètres
  float scale = std::max(0.0001f, params.extract_input<float>("Scale"));
  float quality = std::max(0.0f, params.extract_input<float>("Quality Ratio"));
  scale = std::min(scale, 1.0f);
  
  // Structures de sortie
  GeometrySet output;
  Mesh *mesh_out = nullptr;
  bool tetgen_success = false;
  
  // Sécurité pour les maillages très complexes
  const int max_safe_verts = 100000;
  const int max_safe_faces = 200000;
  
  if (mesh_in->verts_num > max_safe_verts || mesh_in->faces_num > max_safe_faces) {
    params.error_message_add(NodeWarningType::Warning, 
        "Maillage très complexe - tétraèdralisation limitée pour éviter les blocages");
  }
  
  // Tentative de tétraèdralisation avec TetGen et l'API Blender
  try {
    // Initialisation des structures TetGen
    tetgenio in, out;
    
    // Préparation des données avec l'API Blender
    if (prepare_tetgen_input(mesh_in, in, params)) {
      // Configuration robuste des options TetGen
      tetgenbehavior behavior;
      setup_tetgen_behavior(behavior, scale, quality, params);
      
      // Message de progression
      params.error_message_add(NodeWarningType::Info, 
          "Tétraèdralisation en cours avec TetGen, veuillez patienter...");
      
      try {
        // Exécution de TetGen avec gestion des erreurs
        tetrahedralize(&behavior, &in, &out);
        
        // Validation des résultats
        if (out.numberofpoints > 0 && out.numberoftetrahedra > 0 && 
            out.pointlist != nullptr && out.tetrahedronlist != nullptr) {
          
          // Création du maillage Blender à partir des résultats TetGen
          mesh_out = create_mesh_from_tetgen_output(out, params);
          
          if (mesh_out) {
            tetgen_success = true;
          }
        }
        else {
          params.error_message_add(NodeWarningType::Error, 
              "TetGen n'a pas produit de résultats valides. Vérifiez que le maillage d'entrée est fermé et manifold.");
        }
      }
      catch (const std::exception &e) {
        params.error_message_add(NodeWarningType::Error, 
            std::string("Exception TetGen: ") + e.what());
      }
      catch (...) {
        params.error_message_add(NodeWarningType::Error, 
            "Exception inconnue lors de l'exécution de TetGen");
      }
    }
    
    // Nettoyage sécurisé des ressources TetGen
    cleanup_tetgen_input(in);
    cleanup_tetgen_output(out);
  }
  catch (const std::exception &e) {
    params.error_message_add(NodeWarningType::Error, 
        std::string("Exception lors du traitement: ") + e.what());
  }
  catch (...) {
    params.error_message_add(NodeWarningType::Error, 
        "Exception inconnue lors du traitement");
  }
  
  // Création d'un tétraèdre de secours si TetGen a échoué
  if (!tetgen_success) {
    // Nettoyage du maillage précédent si nécessaire
    if (mesh_out) {
      mesh_out->~Mesh();
      MEM_freeN(mesh_out);
      mesh_out = nullptr;
    }
    
    params.error_message_add(NodeWarningType::Warning, 
        "Utilisation d'un tétraèdre simple comme résultat de secours");
    
    // Création du tétraèdre de secours
    mesh_out = create_fallback_tetrahedron(mesh_in, scale, quality);
  }
  
  // Finalisation de la sortie avec l'API Blender
  if (mesh_out) {
    output.replace_mesh(mesh_out);
    params.set_output("Tetrahedral Mesh", std::move(output));
  }
  else {
    params.error_message_add(NodeWarningType::Error, 
        "Échec de la création du maillage tétraédrique");
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
              
  RNA_def_float(ntype.rna_ext.srna,
              "local_feature_scale",
              1.0f,
              0.01f,
              100.0f,
              "Local Feature Scale",
              "Scaling factor for local features",
              0.01f,
              10.0f);

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