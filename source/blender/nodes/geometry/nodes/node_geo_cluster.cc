/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_mesh.hh"
#include "BKE_attribute_math.hh"
#include "BKE_attribute.hh"

#include "BLI_math_vector_types.hh"
#include "BLI_rand.h"
#include "BLI_kdtree.h"
#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_cluster_cc {

/* Pour stocker les informations sur les vertex et clusters */
struct NodeGeometryCluster {
  int vertices_per_cluster;
  int total_vertices;
  int cluster_count;
  bool random_colors;
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Mesh").supported_type(GeometryComponent::Type::Mesh);
  b.add_input<decl::Int>("Vertices Per Cluster").default_value(10).min(1).max(10000);
  b.add_input<decl::Bool>("Random Colors").default_value(true);
  b.add_output<decl::Geometry>("Mesh").propagate_all();
  b.add_output<decl::Color>("Cluster Colors").field_source_reference_all();
  b.add_output<decl::Int>("Cluster ID").field_source_reference_all();
}

/* Fonction qui crée des clusters spatialement cohérents en utilisant des points de départ 
 * aléatoires (k-means simplifié) */
static void create_spatial_clusters(
    const Span<float3> positions,
    const int desired_cluster_count,
    MutableSpan<int> cluster_ids)
{
  const int total_vertices = positions.size();
  
  /* Utiliser au maximum le nombre souhaité de clusters ou le nombre de vertex */
  const int actual_cluster_count = std::min(desired_cluster_count, total_vertices);
  
  /* Créer un tableau pour stocker les positions des centres de clusters */
  Array<float3> cluster_centers(actual_cluster_count);
  
  /* Initialiser un générateur de nombres aléatoires */
  RNG *rng = BLI_rng_new(42);
  
  /* Sélectionner des vertices aléatoires comme centres initiaux de clusters */
  Array<int> selected_indices(actual_cluster_count);
  for (int i = 0; i < actual_cluster_count; i++) {
    int random_index;
    bool index_already_selected;
    
    /* Éviter de sélectionner deux fois le même point */
    do {
      index_already_selected = false;
      random_index = BLI_rng_get_int(rng) % total_vertices;
      
      for (int j = 0; j < i; j++) {
        if (selected_indices[j] == random_index) {
          index_already_selected = true;
          break;
        }
      }
    } while (index_already_selected);
    
    selected_indices[i] = random_index;
    cluster_centers[i] = positions[random_index];
  }
  
  /* Construire un KD-tree pour une recherche rapide des sommets les plus proches */
  KDTree_3d *tree = BLI_kdtree_3d_new(actual_cluster_count);
  
  /* Ajouter les centres de clusters au KD-tree */
  for (int i = 0; i < actual_cluster_count; i++) {
    BLI_kdtree_3d_insert(tree, i, cluster_centers[i]);
  }
  
  /* Construire l'arbre */
  BLI_kdtree_3d_balance(tree);
  
  /* Assigner chaque vertex au cluster le plus proche */
  threading::parallel_for(IndexRange(total_vertices), 1024, [&](IndexRange range) {
    for (const int i : range) {
      KDTreeNearest_3d nearest;
      
      /* Trouver le centre de cluster le plus proche */
      if (BLI_kdtree_3d_find_nearest(tree, positions[i], &nearest) != -1) {
        /* Assigner ce vertex au cluster le plus proche */
        cluster_ids[i] = nearest.index;
      }
      else {
        /* En cas d'erreur, assigner au cluster 0 (ne devrait pas arriver) */
        cluster_ids[i] = 0;
      }
    }
  });
  
  /* Libérer les ressources */
  BLI_kdtree_3d_free(tree);
  BLI_rng_free(rng);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  /* Get inputs from sockets. */
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh");
  const int vertices_per_cluster = std::max(1, params.extract_input<int>("Vertices Per Cluster"));
  const bool random_colors = params.extract_input<bool>("Random Colors");

  int total_vertices = 0;
  int cluster_count = 0;

  /* Process mesh if available. */
  if (Mesh *mesh = geometry_set.get_mesh_for_write()) {
    /* Get vertex positions and count */
    const Span<float3> positions = mesh->vert_positions();
    total_vertices = positions.size();
    
    /* Calculate number of clusters */
    cluster_count = (total_vertices + vertices_per_cluster - 1) / vertices_per_cluster;
    
    /* Create cluster ID attribute */
    bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
    
    /* Créer un attribut de cluster_id identifiable */
    bke::SpanAttributeWriter<int> cluster_id_attribute =
        attributes.lookup_or_add_for_write_span<int>("cluster_id", bke::AttrDomain::Point);
    
    /* Assign cluster IDs to each vertex using spatial clustering */
    create_spatial_clusters(positions, cluster_count, cluster_id_attribute.span);
    
    /* Finish cluster ID attribute writing */
    cluster_id_attribute.finish();
    
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
      
      /* Get corner to vertex mapping to determine face clusters */
      const Span<int> corner_verts = mesh->corner_verts();
      const OffsetIndices faces = mesh->faces();
      
      /* Assign face colors based on dominant cluster of vertices */
      for (const int face_idx : faces.index_range()) {
        /* Count clusters in this face */
        Array<int> cluster_counts(cluster_count, 0);
        int dominant_cluster = 0;
        int max_count = 0;
        
        /* Calculate the dominant cluster for this face */
        for (const int vert_idx : corner_verts.slice(faces[face_idx])) {
          if (vert_idx < total_vertices) {
            const int cluster_id = cluster_id_attribute.span[vert_idx];
            if (cluster_id >= 0 && cluster_id < cluster_count) {
              cluster_counts[cluster_id]++;
              
              if (cluster_counts[cluster_id] > max_count) {
                max_count = cluster_counts[cluster_id];
                dominant_cluster = cluster_id;
              }
            }
          }
        }
        
        /* Assign the color of the dominant cluster to this face */
        face_color_attribute.span[face_idx] = cluster_colors[dominant_cluster];
      }
      
      /* Finish color attribute writing */
      face_color_attribute.finish();
      
      /* Créer une copie de la couleur pour le domaine des vertices (utile pour le Viewer) */
      bke::SpanAttributeWriter<ColorGeometry4f> vert_color_attribute =
          attributes.lookup_or_add_for_write_span<ColorGeometry4f>("VertexColor", bke::AttrDomain::Point);
      
      /* Assign vertex colors based on their cluster */
      for (int i = 0; i < total_vertices; i++) {
        int cluster_id = cluster_id_attribute.span[i];
        if (cluster_id >= 0 && cluster_id < cluster_count) {
          vert_color_attribute.span[i] = cluster_colors[cluster_id];
        }
      }
      
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
      std::to_string(total_vertices) + " vertices in " + std::to_string(cluster_count) + " clusters");
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeCluster");
  ntype.ui_name = "Vertex Clusters";
  ntype.ui_description = "Create clusters of vertices from a mesh";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_cluster_cc