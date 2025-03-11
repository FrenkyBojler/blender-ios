/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_vector.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"
#include "BLI_array.hh"
#include <algorithm>

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_node_types.h"

#include "BKE_attribute.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_legacy_convert.hh"
#include "BKE_lib_id.hh"
#include "BKE_customdata.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"
#include "RNA_access.hh"
#include "RNA_define.hh"

#include "node_geometry_util.hh"
#include "NOD_register.hh"

// Inclure TetGen
#include "tetgen.h"

namespace blender::nodes::node_geo_tetrahedralize_cc {

/* Structure pour les paramètres du nœud */
struct NodeGeometryTetrahedralize {
  float max_volume;          // Volume maximum des tétraèdres
  float quality_ratio;       // Ratio qualité (min radius-edge ratio)
  float coarsen_percent;     // Pourcentage de simplification (comme dans Houdini)
  bool preserve_boundary;    // Préserver la frontière (Houdini: preserve input)
  bool optimize_quality;     // Optimiser la qualité des tétraèdres
  char attribute_name[64];   // Nom de l'attribut pour la densité locale
  char _pad[4];              // Padding pour l'alignement
};

/* Types de méthodes de tétraédralisation */
enum TetrahedralizationMethod {
  METHOD_DELAUNAY = 0,      // Tétraédralisation de Delaunay standard
  METHOD_CONSTRAINED = 1,   // Tétraédralisation de Delaunay contrainte
  METHOD_REFINE = 2,        // Raffiner un maillage tétraédrique existant
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Mesh").supported_type(GeometryComponent::Type::Mesh)
      .description("Maillage de surface à tétraédraliser");
  
  b.add_input<decl::Float>("Max Volume").default_value(0.1f).min(0.00001f).max(10.0f)
      .subtype(PROP_FACTOR)
      .description("Volume maximum des tétraèdres générés");
  
  b.add_input<decl::Float>("Quality Ratio").default_value(1.4f).min(1.0f).max(2.0f)
      .description("Ratio qualité/forme des tétraèdres (1=minimum, 2=élevé)");
  
  b.add_input<decl::Float>("Coarsen").default_value(0.0f).min(0.0f).max(100.0f)
      .subtype(PROP_PERCENTAGE)
      .description("Pourcentage de simplification du maillage d'entrée avant tétraédralisation");
      
  b.add_output<decl::Geometry>("Tetrahedral Mesh")
      .propagate_all()
      .description("Maillage tétraédrique généré");
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "method", static_cast<eUI_Item_Flag>(0), "", ICON_NONE);
  
  uiLayout *col = uiLayoutColumn(layout, false);
  uiItemR(col, ptr, "preserve_boundary", static_cast<eUI_Item_Flag>(0), std::nullopt, ICON_NONE);
  uiItemR(col, ptr, "optimize_quality", static_cast<eUI_Item_Flag>(0), std::nullopt, ICON_NONE);
  
  uiLayout *row = uiLayoutRow(layout, true);
  uiItemR(row, ptr, "use_attribute", static_cast<eUI_Item_Flag>(0), std::nullopt, ICON_NONE);
  
  uiLayout *sub = uiLayoutRow(row, true);
  uiLayoutSetActive(sub, RNA_boolean_get(ptr, "use_attribute"));
  uiItemR(sub, ptr, "attribute_name", static_cast<eUI_Item_Flag>(0), "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryTetrahedralize *storage = (NodeGeometryTetrahedralize *)MEM_callocN(
      sizeof(NodeGeometryTetrahedralize), "NodeGeometryTetrahedralize");
  
  storage->max_volume = 0.1f;
  storage->quality_ratio = 1.4f;
  storage->coarsen_percent = 0.0f;
  storage->preserve_boundary = true;
  storage->optimize_quality = true;
  
  strcpy(storage->attribute_name, "density");
  
  node->storage = storage;
  node->custom1 = METHOD_DELAUNAY;  // Méthode par défaut
  node->custom2 = 0;                // use_attribute = false
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
 * Classe utilitaire pour gérer automatiquement les ressources TetGen (RAII)
 */
class TetGenResourceGuard {
private:
  tetgenio &in_;
  tetgenio &out_;

public:
  TetGenResourceGuard(tetgenio &in, tetgenio &out) : in_(in), out_(out) {}

  ~TetGenResourceGuard() {
    // Nettoyage des entrées
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
    
    // Nettoyage des sorties
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
 * Prépare les données d'entrée pour TetGen à partir d'un maillage Blender
 */
static bool prepare_tetgen_input(const Mesh *mesh_in, tetgenio &in, GeoNodeExecParams &params)
{
  // Initialisation de TetGen
  in.initialize();
  in.firstnumber = 0;  // TetGen utilise l'indexation à partir de 0
  
  // Vérification des données d'entrée
  if (!mesh_in || mesh_in->verts_num < 4) {
    params.error_message_add(NodeWarningType::Error, 
        "Le maillage d'entrée doit contenir au moins 4 points pour la tétraédralisation");
    return false;
  }
  
  // Vérification supplémentaire pour s'assurer que le maillage est valide
  if (mesh_in->faces_num < 4) {
    params.error_message_add(NodeWarningType::Error, 
        "Le maillage d'entrée doit contenir au moins 4 faces pour la tétraédralisation");
    return false;
  }
  
  try {
    // Récupération des données du maillage
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
    
    // Faces d'entrée
    in.numberoffacets = mesh_in->faces_num;
    in.facetlist = new tetgenio::facet[in.numberoffacets];
    
    int valid_faces = 0; // Compteur de faces valides
    
    for (int i = 0; i < mesh_in->faces_num; i++) {
      tetgenio::facet *f = &in.facetlist[i];
      f->numberofholes = 0;
      f->holelist = nullptr;
      
      const IndexRange face = faces[i];
      int vcount = face.size();
      
      // Au moins 3 sommets pour un polygone valide
      if (vcount < 3) {
        continue;
      }
      
      valid_faces++;
      
      f->numberofpolygons = 1;
      f->polygonlist = new tetgenio::polygon[1];
      f->polygonlist[0].numberofvertices = vcount;
      f->polygonlist[0].vertexlist = new int[vcount];
      
      // Copie des indices de sommets avec vérification
      for (int j = 0; j < vcount; j++) {
        int idx = corner_verts[face[j]];
        // Vérification des limites
        if (idx >= 0 && idx < mesh_in->verts_num) {
          f->polygonlist[0].vertexlist[j] = idx;
        }
        else {
          // Index invalide, utiliser 0 comme secours
          f->polygonlist[0].vertexlist[j] = 0;
          params.error_message_add(NodeWarningType::Warning,
              "Index de sommet invalide détecté et corrigé");
        }
      }
    }
    
    // Vérifier qu'il y a suffisamment de faces valides
    if (valid_faces < 4) {
      params.error_message_add(NodeWarningType::Error,
          "Pas assez de faces valides (au moins 4 nécessaires) pour la tétraédralisation");
      return false;
    }
    
    return true;
  }
  catch (const std::exception &e) {
    params.error_message_add(NodeWarningType::Error, 
        std::string("Erreur lors de la préparation des données : ") + e.what());
    return false;
  }
}

/**
 * Applique la simplification du maillage si nécessaire
 */
static void apply_coarsening(tetgenio &in, float coarsen_percent, GeoNodeExecParams &params)
{
  // Si le pourcentage de simplification est trop faible, ne pas l'appliquer
  if (coarsen_percent < 1.0f) {
    return;
  }
  
  // Pour l'instant, simplement informer l'utilisateur que cette fonctionnalité n'est pas disponible
  // La simplification réelle nécessiterait un prétraitement ou une option TetGen spécifique
  params.error_message_add(NodeWarningType::Info,
      "Option de simplification définie à " + std::to_string(coarsen_percent) + 
      "% (sera implémentée dans une future version)");
}

/**
 * Configure les options TetGen en fonction des paramètres du nœud
 */
static void configure_tetgen_options(tetgenbehavior &behavior, 
                                    float max_volume, 
                                    float quality_ratio,
                                    bool preserve_boundary,
                                    bool optimize_quality,
                                    TetrahedralizationMethod method)
{
  // Configuration de base
  behavior.plc = 1;          // Préserver le complexe linéaire par morceaux (boundary)
  behavior.quality = 1;      // Activer l'amélioration de la qualité
  behavior.nobisect = 1;     // Ne pas bissecter les faces d'entrée
  behavior.quiet = 1;        // Mode silencieux (pas de sortie stdout)
  behavior.verbose = 0;      // Pas de verbosité
  
  // Options selon la méthode
  switch (method) {
    case METHOD_DELAUNAY:
      break;  // Options par défaut
      
    case METHOD_CONSTRAINED:
      behavior.refine = 0;   // Pas de raffinement
      break;
      
    case METHOD_REFINE:
      behavior.refine = 1;   // Mode raffinement
      break;
  }
  
  // Préservation de la frontière d'entrée
  if (preserve_boundary) {
    behavior.nobisect = 1;
  }
  else {
    behavior.nobisect = 0;
  }
  
  // Options de qualité
  if (optimize_quality) {
    // Dans TetGen, l'optimisation de la qualité est contrôlée via quality=1 et minratio
    behavior.quality = 1;    // Activer l'optimisation de la qualité
    
    // Ratio qualité (min radius-edge ratio)
    if (quality_ratio > 1.0f) {
      behavior.minratio = quality_ratio;
    }
  }
  
  // Volume maximum des tétraèdres
  if (max_volume > 0.00001f) {
    behavior.fixedvolume = 1;
    behavior.maxvolume = max_volume;
  }
  
  // Sortie
  behavior.edgesout = 1;     // Générer les arêtes
  behavior.facesout = 1;     // Générer les faces
  behavior.neighout = 1;     // Générer les informations de voisinage
}

/**
 * Vérifie si les résultats de TetGen sont valides et utilisables
 */
static bool validate_tetgen_output(const tetgenio &out, GeoNodeExecParams &params)
{
  // Vérification de base
  if (out.numberofpoints <= 0 || out.numberoftetrahedra <= 0) {
    params.error_message_add(NodeWarningType::Error, 
        "TetGen n'a pas généré de maillage tétraédrique valide");
    return false;
  }
  
  // Vérification des données requises
  if (!out.pointlist || !out.tetrahedronlist) {
    params.error_message_add(NodeWarningType::Error, 
        "TetGen a généré des données incomplètes");
    return false;
  }
  
  // Vérification des arêtes si edgesout a été utilisé
  if (out.numberofedges > 0 && !out.edgelist) {
    params.error_message_add(NodeWarningType::Warning, 
        "TetGen a signalé des arêtes mais n'en a pas généré");
    return false;
  }
  
  return true;
}

/**
 * Crée un maillage Blender à partir des résultats TetGen
 */
static Mesh* create_tetrahedral_mesh(tetgenio &out, GeoNodeExecParams &params, bool use_attribute, const char *attribute_name)
{
  // Vérification des résultats
  if (!validate_tetgen_output(out, params)) {
    return nullptr;
  }
  
  // Limite de sécurité pour éviter les problèmes de performance
  const int max_tets = 1000000;
  int num_tets = std::min(out.numberoftetrahedra, max_tets);
  
  if (num_tets < out.numberoftetrahedra) {
    params.error_message_add(NodeWarningType::Warning, 
        "Nombre de tétraèdres limité à " + std::to_string(num_tets) + 
        " pour des raisons de performance (sur " + std::to_string(out.numberoftetrahedra) + ")");
  }
  
  // Calcul des dimensions du maillage
  const int num_verts = out.numberofpoints;
  const int num_edges = out.numberofedges > 0 ? out.numberofedges : 0;
  const int num_faces = num_tets * 4;  // 4 faces par tétraèdre
  const int num_loops = num_faces * 3; // 3 sommets par face triangulaire
  
  // Création du maillage
  Mesh *mesh_out = BKE_mesh_new_nomain(num_verts, num_edges, num_faces, num_loops);
  
  if (!mesh_out) {
    params.error_message_add(NodeWarningType::Error, "Impossible de créer le maillage");
    return nullptr;
  }
  
  // 1. Copie des sommets
  MutableSpan<float3> vert_positions = mesh_out->vert_positions_for_write();
  for (int i = 0; i < num_verts; i++) {
    vert_positions[i].x = out.pointlist[i * 3];
    vert_positions[i].y = out.pointlist[i * 3 + 1];
    vert_positions[i].z = out.pointlist[i * 3 + 2];
  }
  
  // 2. Copie des arêtes
  if (num_edges > 0 && out.edgelist) {
    MutableSpan<int2> edges = mesh_out->edges_for_write();
    for (int i = 0; i < num_edges; i++) {
      // Vérifier que les indices sont valides
      int v1 = out.edgelist[i * 2];
      int v2 = out.edgelist[i * 2 + 1];
      
      if (v1 >= 0 && v1 < num_verts && v2 >= 0 && v2 < num_verts) {
        edges[i].x = v1;
        edges[i].y = v2;
      }
      else {
        // En cas d'indice invalide, utiliser des valeurs sûres
        edges[i].x = 0;
        edges[i].y = std::min(1, num_verts - 1);
        params.error_message_add(NodeWarningType::Warning,
            "Indice d'arête invalide détecté et corrigé");
      }
    }
  }
  
  // 3. Création des faces tétraédriques
  MutableSpan<int> corner_verts = mesh_out->corner_verts_for_write();
  int corner_index = 0;
  
  // Construction d'un ensemble pour éviter les faces dupliquées
  Vector<Vector<int>> unique_faces;
  unique_faces.reserve(num_faces);
  
  for (int i = 0; i < num_tets; i++) {
    // Récupération des indices du tétraèdre
    int v0 = out.tetrahedronlist[i * 4];
    int v1 = out.tetrahedronlist[i * 4 + 1];
    int v2 = out.tetrahedronlist[i * 4 + 2];
    int v3 = out.tetrahedronlist[i * 4 + 3];
    
    // Vérification des indices
    if (v0 < 0 || v0 >= num_verts || v1 < 0 || v1 >= num_verts ||
        v2 < 0 || v2 >= num_verts || v3 < 0 || v3 >= num_verts) {
      continue;
    }
    
    // Face 1: triangle (v0, v1, v2)
    corner_verts[corner_index++] = v0;
    corner_verts[corner_index++] = v1;
    corner_verts[corner_index++] = v2;
    
    // Face 2: triangle (v0, v1, v3)
    corner_verts[corner_index++] = v0;
    corner_verts[corner_index++] = v1;
    corner_verts[corner_index++] = v3;
    
    // Face 3: triangle (v0, v2, v3)
    corner_verts[corner_index++] = v0;
    corner_verts[corner_index++] = v2;
    corner_verts[corner_index++] = v3;
    
    // Face 4: triangle (v1, v2, v3)
    corner_verts[corner_index++] = v1;
    corner_verts[corner_index++] = v2;
    corner_verts[corner_index++] = v3;
  }
  
  // Configuration des offsets de faces
  offset_indices::fill_constant_group_size(3, 0, mesh_out->face_offsets_for_write());
  
  // Création d'un attribut pour stocker l'ID du tétraèdre pour chaque face
  // Utilisation de l'API d'attributs de Blender
  bke::MutableAttributeAccessor attributes = mesh_out->attributes_for_write();
  bke::SpanAttributeWriter<int> tet_indices = attributes.lookup_or_add_for_write_span<int>(
      "tetrahedral_index", bke::AttrDomain::Face);
  
  if (tet_indices) {
    for (int i = 0; i < num_faces; i++) {
      tet_indices.span[i] = i / 4;  // Division entière pour obtenir l'ID du tétraèdre
    }
    tet_indices.finish();
  }
  
  // Ajouter un attribut pour la densité si nécessaire
  if (use_attribute && attribute_name && attribute_name[0] != '\0') {
    bke::SpanAttributeWriter<float> density = attributes.lookup_or_add_for_write_span<float>(
        attribute_name, bke::AttrDomain::Point);
        
    if (density) {
      // Remplir avec des valeurs par défaut (1.0)
      for (int i = 0; i < num_verts; i++) {
        density.span[i] = 1.0f;
      }
      
      // Si TetGen a généré des informations de qualité, les utiliser
      if (out.numberoftetrahedra > 0 && out.tetrahedronattributelist) {
        // Attributs propres aux tétraèdres, à mapper sur les sommets
        // Ceci est une simplification - un vrai mappeur serait plus complexe
        for (int i = 0; i < num_tets && i < num_verts; i++) {
          float quality = out.tetrahedronattributelist[i];
          density.span[i] = quality > 0.0f ? quality : 1.0f;
        }
      }
      
      density.finish();
    }
  }
  
  // Validation du maillage
  BKE_mesh_validate(mesh_out, true, true);
  
  return mesh_out;
}

/**
 * Crée un tétraèdre de secours en cas d'échec
 */
static Mesh* create_fallback_tetrahedron(const Mesh *mesh_in)
{
  // Dimensions du maillage
  const int num_verts = 4;
  const int num_edges = 6;
  const int num_faces = 4;
  const int num_loops = 12;
  
  // Création du maillage
  Mesh *mesh_out = BKE_mesh_new_nomain(num_verts, num_edges, num_faces, num_loops);
  
  if (!mesh_out) {
    return nullptr;
  }
  
  // Calcul de la boîte englobante
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
  
  // Centre et taille
  float3 center = (bmin + bmax) * 0.5f;
  float3 size = bmax - bmin;
  float max_size = std::max(std::max(size.x, size.y), size.z);
  if (max_size < 0.0001f) max_size = 1.0f;
  
  // Positions des sommets
  MutableSpan<float3> vert_positions = mesh_out->vert_positions_for_write();
  vert_positions[0] = center + float3(0, 0, max_size * 0.5f);
  vert_positions[1] = center + float3(-max_size * 0.5f, -max_size * 0.5f, -max_size * 0.5f);
  vert_positions[2] = center + float3(max_size * 0.5f, -max_size * 0.5f, -max_size * 0.5f);
  vert_positions[3] = center + float3(0, max_size * 0.5f, -max_size * 0.5f);
  
  // Arêtes
  MutableSpan<int2> edges = mesh_out->edges_for_write();
  edges[0] = int2(0, 1);
  edges[1] = int2(0, 2);
  edges[2] = int2(0, 3);
  edges[3] = int2(1, 2);
  edges[4] = int2(1, 3);
  edges[5] = int2(2, 3);
  
  // Faces
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
  
  // Configuration des offsets de faces
  offset_indices::fill_constant_group_size(3, 0, mesh_out->face_offsets_for_write());
  
  // Ajouter un attribut d'ID tétraèdre
  bke::MutableAttributeAccessor attributes = mesh_out->attributes_for_write();
  bke::SpanAttributeWriter<int> tet_indices = attributes.lookup_or_add_for_write_span<int>(
      "tetrahedral_index", bke::AttrDomain::Face);
      
  if (tet_indices) {
    for (int i = 0; i < num_faces; i++) {
      tet_indices.span[i] = 0;  // Un seul tétraèdre
    }
    tet_indices.finish();
  }
  
  // Validation
  BKE_mesh_validate(mesh_out, true, true);
  
  return mesh_out;
}

/**
 * Fonction principale d'exécution du nœud
 */
static void node_geo_exec(GeoNodeExecParams params)
{
  // Récupération des entrées
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh");
  
  if (!geometry_set.has_mesh()) {
    params.error_message_add(NodeWarningType::Error, 
        "Entrée requise: un maillage pour la tétraédralisation");
    params.set_output("Tetrahedral Mesh", GeometrySet());
    return;
  }
  
  // Récupération du maillage d'entrée et des paramètres
  const Mesh *mesh_in = geometry_set.get_mesh();
  
  float max_volume = std::max(0.00001f, params.extract_input<float>("Max Volume"));
  float quality_ratio = std::max(1.0f, params.extract_input<float>("Quality Ratio"));
  float coarsen_percent = std::max(0.0f, params.extract_input<float>("Coarsen"));
  
  // Récupération des paramètres du nœud
  const bNode &node = params.node();
  const NodeGeometryTetrahedralize *storage = static_cast<const NodeGeometryTetrahedralize *>(node.storage);
  
  TetrahedralizationMethod method = static_cast<TetrahedralizationMethod>(node.custom1);
  bool preserve_boundary = storage->preserve_boundary;
  bool optimize_quality = storage->optimize_quality;
  bool use_attribute = node.custom2 != 0;
  
  // Structures TetGen
  tetgenio in, out;
  tetgenbehavior behavior;
  
  // Gestionnaire de ressources pour assurer le nettoyage en cas d'exception
  TetGenResourceGuard resource_guard(in, out);
  
  Mesh *mesh_out = nullptr;
  bool tetgen_success = false;
  
  try {
    // Préparation des données d'entrée
    if (prepare_tetgen_input(mesh_in, in, params)) {
      // Appliquer la simplification si nécessaire
      if (coarsen_percent > 0.0f) {
        apply_coarsening(in, coarsen_percent, params);
      }
      
      // Configuration des options TetGen
      configure_tetgen_options(behavior, max_volume, quality_ratio, 
                              preserve_boundary, optimize_quality, method);
      
      // Exécution de TetGen
      try {
        tetrahedralize(&behavior, &in, &out);
        
        // Vérification des résultats
        if (validate_tetgen_output(out, params)) {
          mesh_out = create_tetrahedral_mesh(out, params, use_attribute, storage->attribute_name);
          if (mesh_out) {
            tetgen_success = true;
          }
        }
      }
      catch (const std::exception &e) {
        params.error_message_add(NodeWarningType::Error, 
            std::string("Erreur TetGen: ") + e.what());
      }
      catch (...) {
        params.error_message_add(NodeWarningType::Error, 
            "Erreur inconnue lors de l'exécution de TetGen");
      }
    }
  }
  catch (const std::exception &e) {
    params.error_message_add(NodeWarningType::Error, 
        std::string("Exception: ") + e.what());
  }
  
  // En cas d'échec, création d'un tétraèdre de secours
  if (!tetgen_success) {
    params.error_message_add(NodeWarningType::Warning, 
        "Création d'un tétraèdre simple comme résultat de secours");
    
    mesh_out = create_fallback_tetrahedron(mesh_in);
  }
  
  // Sortie
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
  ntype.ui_description = "Génère un maillage tétraédrique à partir d'un maillage de surface";
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  blender::bke::node_type_storage(ntype, "NodeGeometryTetrahedralize", node_free_storage, node_copy_storage);
  
  // Enregistrement des propriétés RNA
  static const EnumPropertyItem method_items[] = {
    {METHOD_DELAUNAY, "DELAUNAY", 0, "Delaunay", "Tétraédralisation de Delaunay standard"},
    {METHOD_CONSTRAINED, "CONSTRAINED", 0, "Constrained", "Tétraédralisation de Delaunay contrainte"},
    {METHOD_REFINE, "REFINE", 0, "Refine", "Raffiner un maillage tétraédrique existant"},
    {0, nullptr, 0, nullptr, nullptr},
  };
  
  RNA_def_enum(ntype.rna_ext.srna, 
              "method", 
              method_items, 
              METHOD_DELAUNAY, 
              "Méthode", 
              "Méthode de tétraédralisation");
              
  RNA_def_boolean(ntype.rna_ext.srna,
                 "preserve_boundary",
                 true,
                 "Preserve Boundary",
                 "Préserver les limites du maillage d'entrée");
                 
  RNA_def_boolean(ntype.rna_ext.srna,
                 "optimize_quality",
                 true,
                 "Optimize Quality",
                 "Optimiser la qualité des tétraèdres générés");
                 
  RNA_def_boolean(ntype.rna_ext.srna,
                 "use_attribute",
                 false,
                 "Use Attribute",
                 "Utiliser un attribut pour contrôler la densité locale");
                 
  RNA_def_string(ntype.rna_ext.srna,
                "attribute_name",
                "density",
                64,
                "Attribute",
                "Nom de l'attribut pour la densité locale");

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
