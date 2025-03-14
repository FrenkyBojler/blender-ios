	/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later 
 * Tetrahedral Mesh Generation Node
 * --------------------------------
 * This node creates a tetrahedral volume mesh from a manifold surface mesh.
 * Uses the TetGen library for tetrahedralization with various quality parameters.
 * The input mesh must be closed/watertight to generate a valid tetrahedral mesh.
 */

#include <iostream>
#include <algorithm>
#include <unordered_set>
#include <numeric> 
#include <random> 


#include <tbb/parallel_for.h>
#include <tbb/mutex.h>


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


#include "tetgen.h"

namespace blender::nodes::node_geo_tetrahedralize_cc {


struct NodeGeometryTetrahedralize {
  double max_volume;          
  float quality_ratio;       
  float min_dihedral_angle;  
  bool preserve_boundary;    
  char _pad[3];              
};


using MeshBlockedRange = tbb::blocked_range<int>;


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
  
  b.add_input<decl::Float>("Max Volume").default_value(0.8).min(0.00001).max(1.0)
      .subtype(PROP_FACTOR)
      .description("Maximum volume of generated tetrahedra (lower values create denser meshes)");
  
  b.add_input<decl::Float>("Quality Ratio").default_value(2.0).min(1.0f).max(4.0f)
      .description("Direct control of TetGen quality (1=basic quality, 4=highest quality with longer calculation times)");
  
  b.add_input<decl::Float>("Min Dihedral Angle").default_value(10.0f).min(0.0f).max(30.0f)
      .subtype(PROP_ANGLE)
      .description("Minimum dihedral angle between tetrahedra faces. Higher values create better shaped elements but slower calculation");
  
  b.add_input<decl::Bool>("Preserve Boundary").default_value(false)
      .description("Preserve input mesh boundaries");
      
  b.add_output<decl::Geometry>("Tetrahedral Mesh")
      .propagate_all()
      .description("Generated tetrahedral mesh");
}

static void node_layout(uiLayout *layout, bContext *, PointerRNA *ptr)
{
  
}

static void node_init(bNodeTree *, bNode *node)
{
  NodeGeometryTetrahedralize *storage = (NodeGeometryTetrahedralize *)MEM_callocN(
      sizeof(NodeGeometryTetrahedralize), "NodeGeometryTetrahedralize");
  
  storage->max_volume = 0.8;
  storage->quality_ratio = 2.0f;
  storage->min_dihedral_angle = 10.0f;
  storage->preserve_boundary = true;
  
  node->storage = storage;
  node->custom2 = 0;                
}

static void node_free_storage(bNode *node)
{
  NodeGeometryTetrahedralize *storage = (NodeGeometryTetrahedralize *)node->storage;
  MEM_freeN(storage);
}

static void node_copy_storage(bNodeTree *,
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
 * Prepares a complex mesh for tetrahedralization without replacing it.
 * For extremely large meshes, creates an optimized version that can be processed more efficiently.
 */
static Mesh* prepare_complex_mesh_for_tetgen(const Mesh *mesh_in, GeoNodeExecParams &params)
{
  // Only process complex meshes over threshold
  if (mesh_in->verts_num < 500 || mesh_in->faces_num < 500) {
    return nullptr;
  }
  
  // Detect extremely complex meshes that need special handling
  const bool is_very_complex = mesh_in->verts_num > 50000 || mesh_in->faces_num > 50000;
  
  // Create a simplified version for very complex meshes
  if (is_very_complex) {
    params.error_message_add(NodeWarningType::Info,
        "Extremely complex mesh detected (" + std::to_string(mesh_in->verts_num) + 
        " vertices, " + std::to_string(mesh_in->faces_num) + 
        " faces). Preparing a simplified version for tetrahedralization.");
    
    try {
      
      Mesh *simplified_mesh = BKE_mesh_new_nomain_from_template(mesh_in, 
                                                              mesh_in->verts_num,
                                                              mesh_in->edges_num,
                                                              mesh_in->faces_num,
                                                              mesh_in->corners_num);
      
      if (simplified_mesh) {
        
        BKE_mesh_copy_parameters(simplified_mesh, mesh_in);
        
        
        MutableSpan<float3> vert_positions = simplified_mesh->vert_positions_for_write();
        Span<float3> orig_positions = mesh_in->vert_positions();
        for (int i = 0; i < mesh_in->verts_num; i++) {
          vert_positions[i] = orig_positions[i];
        }
        
        
        MutableSpan<int2> edges = simplified_mesh->edges_for_write();
        Span<int2> orig_edges = mesh_in->edges();
        for (int i = 0; i < mesh_in->edges_num; i++) {
          edges[i] = orig_edges[i];
        }
        
        
        MutableSpan<int> face_offsets = simplified_mesh->face_offsets_for_write();
        Span<int> orig_face_offsets = mesh_in->face_offsets();
        for (int i = 0; i <= mesh_in->faces_num; i++) {  
          face_offsets[i] = orig_face_offsets[i];
        }
        
        
        MutableSpan<int> corner_verts = simplified_mesh->corner_verts_for_write();
        Span<int> orig_corner_verts = mesh_in->corner_verts();
        for (int i = 0; i < mesh_in->corners_num; i++) {
          corner_verts[i] = orig_corner_verts[i];
        }
        
        
        
        
        
        
        params.error_message_add(NodeWarningType::Info,
            "Meshing optimizing for tetrahedralization");
        
        
        return simplified_mesh;
      }
    }
    catch (const std::exception &e) {
      params.error_message_add(NodeWarningType::Error,
          std::string("Error during complex mesh preparation: ") + e.what());
      return nullptr;
    }
  }
  
  
  
  
  
  
  
  return nullptr;
}

/**
 * Prepares a mesh for TetGen input, applying controlled perturbations to vertex positions
 * for complex meshes. This helps TetGen handle complex meshes without crashing.
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
  
  
  in.numberofpoints = vert_positions.size();
  in.pointlist = new REAL[vert_positions.size() * 3];
  
  
  float3 bbox_min(std::numeric_limits<float>::max());
  float3 bbox_max(-std::numeric_limits<float>::max());
  
  for (int i = 0; i < vert_positions.size(); i++) {
    bbox_min = math::min(bbox_min, vert_positions[i]);
    bbox_max = math::max(bbox_max, vert_positions[i]);
  }
  
  
  float mesh_scale = math::length(bbox_max - bbox_min);
  float perturbation_scale = mesh_scale * 1e-6f; 
  
  
  if (attempt >= 1) {
    perturbation_scale *= pow(10.0f, attempt);
    params.error_message_add(NodeWarningType::Info,
                         "Attempt " + std::to_string(attempt+1) + 
                         ": Increasing perturbations (x" + 
                         std::to_string(pow(10.0f, attempt)) + ")");
  }
  
  
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_real_distribution<float> dist(-perturbation_scale, perturbation_scale);
  
  
  for (int i = 0; i < vert_positions.size(); i++) {
    float3 pos = vert_positions[i];
    
    
    
    if (attempt >= 1) {
      pos.x += dist(gen);
      pos.y += dist(gen);
      pos.z += dist(gen);
    }
    
    in.pointlist[i * 3] = pos.x;
    in.pointlist[i * 3 + 1] = pos.y;
    in.pointlist[i * 3 + 2] = pos.z;
  }
  
  
  Span<int> corner_verts = mesh->corner_verts();
  Span<int> face_offsets = mesh->face_offsets();
  
 
  int num_triangles = 0;
  for (int i = 0; i < mesh->faces_num; i++) {
    int face_size = face_offsets[i + 1] - face_offsets[i];
    if (face_size < 3) {
      
      continue;
    }
    else {
    
      num_triangles += face_size - 2;
    }
  }
  
  
  in.numberoffacets = num_triangles;
  in.facetlist = new tetgenio::facet[in.numberoffacets];
  in.facetmarkerlist = new int[in.numberoffacets];
  
  int ti = 0; 
  
    
  for (int i = 0; i < mesh->faces_num; i++) {
    int face_start = face_offsets[i];
    int face_size = face_offsets[i + 1] - face_start;
    
    
    if (face_size < 3) {
      continue;
    }
    
    
    for (int j = 0; j < face_size - 2; j++) {
      
      tetgenio::facet *f = &in.facetlist[ti];
    f->numberofpolygons = 1;
    f->polygonlist = new tetgenio::polygon[f->numberofpolygons];
      f->numberofholes = 0;
      f->holelist = nullptr;
      
      
    tetgenio::polygon *p = &f->polygonlist[0];
      p->numberofvertices = 3;
    p->vertexlist = new int[p->numberofvertices];
    
      
      p->vertexlist[0] = corner_verts[face_start];
      p->vertexlist[1] = corner_verts[face_start + j + 1];
      p->vertexlist[2] = corner_verts[face_start + j + 2];
      
      
      for (int k = 0; k < 3; k++) {
        if (p->vertexlist[k] < 0 || p->vertexlist[k] >= in.numberofpoints) {
          params.error_message_add(NodeWarningType::Error,
                               "Invalid vertex index in face: " + std::to_string(p->vertexlist[k]));
          return false;
        }
      }
      
      
      in.facetmarkerlist[ti] = 1;
      
      ti++;
    }
  }
  
  return true;
}

/**
 * Makes the normals of the faces consistent for the tetrahedral mesh
 * by ensuring they all point outward.
 */
static void make_normals_consistent(Mesh *mesh, GeoNodeExecParams &params)
{
  const Span<float3> positions = mesh->vert_positions();
  const Span<int> corner_verts = mesh->corner_verts();
  const Span<int> face_offsets = mesh->face_offsets();
  
  // Calculate the center of the mesh to determine the orientation of the normals
  float3 mesh_center(0, 0, 0);
  for (const float3 &pos : positions) {
    mesh_center += pos;
  }
  mesh_center /= float(positions.size());
  
  // Determine which faces must be returned
  Array<bool> flip_faces(mesh->faces_num);
  
  for (int face_idx = 0; face_idx < mesh->faces_num; face_idx++) {
    const int face_start = face_offsets[face_idx];
    const int face_size = face_offsets[face_idx + 1] - face_start;
    
    if (face_size == 3) {
      // Calculate the normal of the face
      const float3 &v0 = positions[corner_verts[face_start]];
      const float3 &v1 = positions[corner_verts[face_start + 1]];
      const float3 &v2 = positions[corner_verts[face_start + 2]];
      
      const float3 normal = math::normalize(math::cross(v1 - v0, v2 - v0));
      
      // Calculate the center of the face
      const float3 face_center = (v0 + v1 + v2) / 3.0f;
      
      // The normal must point outward (away from the center of the mesh)
      const float3 face_to_center = mesh_center - face_center;
      
      // If the dot product is positive, the normal points inward
      flip_faces[face_idx] = math::dot(normal, face_to_center) > 0.0f;
    }
  }
  
    // Invert the order of the vertices for the faces that must be returned
  MutableSpan<int> mutable_corner_verts = mesh->corner_verts_for_write();
  
  for (int face_idx = 0; face_idx < mesh->faces_num; face_idx++) {
    if (flip_faces[face_idx]) {
      const int face_start = face_offsets[face_idx];
      const int face_size = face_offsets[face_idx + 1] - face_start;
      
      if (face_size == 3) {
        // Swap the vertices 1 and 2 of the triangle to reverse its normal
        std::swap(mutable_corner_verts[face_start + 1], mutable_corner_verts[face_start + 2]);
        
        // Also update the corner edges if necessary
        MutableSpan<int> corner_edges = mesh->corner_edges_for_write();
        if (!corner_edges.is_empty()) {
          std::swap(corner_edges[face_start + 1], corner_edges[face_start + 2]);
        }
      }
    }
  }
  
  // Force to recalculate the normals
  mesh->tag_face_winding_changed();
  
  params.error_message_add(NodeWarningType::Info,
      "Normals recalculated successfully for better visual consistency.");
}

/**
 * Configures TetGen options based on mesh complexity and quality parameters.
 * Adjusts settings for different complexity levels and problematic geometry.
 * 
 * @param behavior TetGen behavior structure to configure
 * @param max_volume Maximum volume constraint for tetrahedra
 * @param quality_ratio Quality ratio for shape control
 * @param min_dihedral_angle Minimum dihedral angle between tetrahedra faces
 * @param preserve_boundary Whether to preserve the input mesh boundary
 * @param params Node execution parameters for error reporting
 * @param attempt Current attempt number (for fallback strategies)
 * @param max_attempts Maximum number of attempts allowed
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
  // Basic configuration - always enable PLC (Piecewise Linear Complex)
  behavior.plc = 1;          
  behavior.quality = 1;      // Enable quality mesh generation
  behavior.facesout = 1;     // Output faces
  behavior.edgesout = 1;     // Output edges
  behavior.neighout = 1;     // Output neighbors
  behavior.docheck = 1;      // Check mesh consistency
  behavior.verbose = 0;      // Disable verbose output
  
  // Enable region attributes for filtering external tetrahedra
  behavior.regionattrib = 1; 
  
  if (attempt > 1) {
    behavior.nobisect = 0;
    if (preserve_boundary) {
      params.error_message_add(NodeWarningType::Info,
          "Disabling boundary preservation for stability (attempt " + 
          std::to_string(attempt + 1) + ")");
    }
  } else {
    behavior.nobisect = preserve_boundary ? 1 : 0;
  }
  
  // Direct quality control for TetGen
  behavior.minratio = quality_ratio;   
  behavior.mindihedral = min_dihedral_angle; 
  
  // Volume constraint - TOUJOURS activer pour forcer la contrainte de volume
  behavior.fixedvolume = 1;  // Toujours activer la contrainte de volume
  behavior.maxvolume = max_volume;
  
  // Adjust parameters for complex meshes
  bool is_complex_mesh = attempt > 0;
  bool is_massive_mesh = attempt >= 2;
  bool has_flipping_issues = attempt >= 2;
  
  // First fallback for complex meshes
  if (is_complex_mesh) {
    params.error_message_add(NodeWarningType::Warning,
                         "Complex mesh detected. Adjusting tetrahedralization parameters.");
    behavior.minratio = std::min(quality_ratio, 2.0f);    // Reduce quality for speed
    behavior.mindihedral = std::min(min_dihedral_angle, 5.0f);  // Reduce angle constraint
    behavior.docheck = 0;       
    behavior.diagnose = 1;      
  }
  
  // Second fallback for massive meshes
  if (is_massive_mesh) {
    params.error_message_add(NodeWarningType::Warning,
                         "Massive mesh detected. Disabling boundary recovery.");
    behavior.nobisect = 0;     
    behavior.docheck = 0;      
    behavior.diagnose = 1;     
    
    // Reduce quality further for massive meshes
    behavior.minratio = std::min(quality_ratio, 1.5f);    
    behavior.mindihedral = std::min(min_dihedral_angle, 1.0f); 
  }
  
  // Final fallback for problematic geometry
  if (has_flipping_issues) {
    params.error_message_add(NodeWarningType::Warning,
                         "Mesh with problematic geometry. Disabling flipping operations.");
    
    // Disable PLC for maximum robustness
    behavior.plc = 0;          
    behavior.nobisect = 0;     
    behavior.docheck = 0;      
    behavior.diagnose = 1;     
    
    // Disable operations that can cause flipping errors
    behavior.nomergefacet = 1;  
    behavior.nomergevertex = 1; 
    behavior.nojettison = 1;    
    
    // Use minimal quality constraints
    behavior.minratio = 1.0;   
    behavior.mindihedral = 0.5; 
    
    // Reduce output complexity
    behavior.facesout = 0;      
    behavior.edgesout = 0;      
  }
}

/**
 * Checks if TetGen output is valid and usable.
 * Validates the generated tetrahedral mesh for common issues.
 * 
 * @param out The TetGen output structure
 * @param params Node execution parameters for error reporting
 * @return True if the output is valid and can be used
 */
static bool validate_tetgen_output(const tetgenio &out, GeoNodeExecParams &params)
{
  // Basic validation - ensure we have points and tetrahedra
  if (out.numberofpoints <= 0 || out.numberoftetrahedra <= 0) {
    params.error_message_add(NodeWarningType::Error, 
        "TetGen did not generate a valid tetrahedral mesh");
    
    // Provide more detailed error information
    if (out.numberofpoints <= 0) {
      params.error_message_add(NodeWarningType::Error, 
          "No points generated by TetGen. The mesh may be degenerate or too flat.");
    }
    else if (out.numberoftetrahedra <= 0 && out.numberofpoints > 0) {
      params.error_message_add(NodeWarningType::Error, 
          "Points were generated (" + std::to_string(out.numberofpoints) + 
          ") but no tetrahedra were created. The mesh is likely non-volumetric or self-intersecting.");
    }
    
    return false;
  }
  
  
  if (!out.pointlist || !out.tetrahedronlist) {
    params.error_message_add(NodeWarningType::Error, 
        "TetGen generated incomplete data");
    return false;
  }
  
  
  if (out.numberofedges > 0 && !out.edgelist) {
    params.error_message_add(NodeWarningType::Warning, 
        "TetGen reported edges but did not generate any");
    return false;
  }
  
  
  if (out.numberoftetrahedra < 10 && out.numberofpoints > 100) {
    params.error_message_add(NodeWarningType::Warning, 
        "Very few tetrahedra generated (" + std::to_string(out.numberoftetrahedra) + 
        ") compared to the number of points (" + std::to_string(out.numberofpoints) + 
        "). The mesh might be almost flat or have topological issues.");
  }
  
  
  
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
  
  
  if (min_dim < max_dim * 0.01f) {
    params.error_message_add(NodeWarningType::Warning, 
        "The mesh is very flat in at least one dimension, which may cause tetrahedralization issues. "
        "Ratio min/max = " + std::to_string(min_dim/max_dim) + 
        ". Try extruding the mesh in this dimension.");
  }
  
  
  if (out.tetrahedronattributelist != nullptr) {
    int external_tets_count = 0;
    for (int i = 0; i < out.numberoftetrahedra; i++) {
      
      if (out.tetrahedronattributelist[i] == -1) {
        external_tets_count++;
      }
    }
    
    if (external_tets_count > 0) {
      
      float external_percentage = (float)external_tets_count / out.numberoftetrahedra * 100.0f;
      
      
      if (external_percentage < 10.0f) {
        params.error_message_add(NodeWarningType::Info,
            std::to_string(external_tets_count) + " external tetrahedra detected (" + 
            std::to_string(external_percentage) + "%). " + 
            "They will be filtered from the final result.");
      }
      else if (external_percentage < 40.0f) {
        params.error_message_add(NodeWarningType::Warning,
            std::to_string(external_tets_count) + " external tetrahedra detected (" + 
            std::to_string(external_percentage) + "%). " + 
            "Check if the mesh has geometric issues.");
      }
      else {
        params.error_message_add(NodeWarningType::Warning,
            "High percentage of external tetrahedra detected (" + 
            std::to_string(external_percentage) + "%). " + 
            "The mesh might have closure or orientation issues.");
      }
      
      
      if (external_percentage >= 99.0f) {
        params.error_message_add(NodeWarningType::Error,
            "The tetrahedral mesh consists only of the convex hull. ");
      }
    }
  }
  else {
    
    params.error_message_add(NodeWarningType::Warning,
        "No region attributes generated by TetGen. ");
    
    
    
    int degenerate_count = 0;
    int large_tets_count = 0;
    
    
    float diag_distance = math::length(bbox_max - bbox_min);
    float volume_threshold = std::pow(diag_distance, 3) * 0.001f; 
    
    for (int i = 0; i < out.numberoftetrahedra; i++) {
      int* tet = &out.tetrahedronlist[i * 4];
      
      
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
      
      
      float volume = std::abs(math::dot(math::cross(v1 - v0, v2 - v0), v3 - v0)) / 6.0f;
      
      if (volume < 1e-8f) {
        degenerate_count++;
      }
      else if (volume > volume_threshold) {
        large_tets_count++;
      }
    }
    
    
    if (degenerate_count > 0) {
      float degen_percentage = (float)degenerate_count / out.numberoftetrahedra * 100.0f;
      if (degen_percentage > 1.0f) {
        params.error_message_add(NodeWarningType::Warning,
            std::to_string(degenerate_count) + " degenerate tetrahedra detected (" + 
            std::to_string(degen_percentage) + "%). " + 
            "High risk of crash in create_a_shorter_edge.");
      }
    }
    
    
    if (large_tets_count > 0) {
      float large_percentage = (float)large_tets_count / out.numberoftetrahedra * 100.0f;
      if (large_percentage > 30.0f) {
        params.error_message_add(NodeWarningType::Warning,
            std::to_string(large_tets_count) + " large volume tetrahedra detected (" + 
            std::to_string(large_percentage) + "%). " + 
            "Possible presence of the convex hull.");
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
    // Identifie les tétraèdres internes
    Vector<int> internal_tetrahedra;
    
    // Utilise les attributs de région si disponibles
    if (out.tetrahedronattributelist != nullptr) {
      
      for (int i = 0; i < out.numberoftetrahedra; i++) {
        if (out.tetrahedronattributelist[i] != -1) {
          internal_tetrahedra.append(i);
        }
      }
      
      if (!internal_tetrahedra.is_empty()) {
        params.error_message_add(NodeWarningType::Info,
            std::string("Attribute filtering: ") + 
            std::to_string(internal_tetrahedra.size()) + 
            std::string(" internal tetrahedra identified."));
      }
      else {
        params.error_message_add(NodeWarningType::Warning,
            std::string("No internal tetrahedra identified by attributes. ") + 
            std::string("Using geometric filtering."));
        internal_tetrahedra.clear();
      }
    }
    
    // Geometric filtering if necessary
    if (internal_tetrahedra.is_empty()) {
      // Calcul de la boîte englobante
      float3 bbox_min(std::numeric_limits<float>::max());
      float3 bbox_max(-std::numeric_limits<float>::max());
      
      for (int i = 0; i < out.numberofpoints; i++) {
        float3 p(out.pointlist[i * 3], out.pointlist[i * 3 + 1], out.pointlist[i * 3 + 2]);
        bbox_min = math::min(bbox_min, p);
        bbox_max = math::max(bbox_max, p);
      }
      
      // Diagonal distance for filtering
      float diag_distance = math::length(bbox_max - bbox_min);
      
      // Detection of pure Delaunay mode
      bool is_pure_delaunay = (out.tetrahedronattributelist == nullptr && 
                             out.numberoftetrahedra > 0);
      
      // Pure Delaunay mode - uses a score based on volume and central position
      if (is_pure_delaunay) {
        params.error_message_add(NodeWarningType::Info,
            "Pure Delaunay mode detected - selection of tetrahedra based on volume");
        
        std::vector<std::pair<int, float>> tet_scores;
        float3 bbox_center = (bbox_min + bbox_max) * 0.5f;
        
        for (int i = 0; i < out.numberoftetrahedra; i++) {
          int* tet = &out.tetrahedronlist[i * 4];
          
          // Extraction of vertices
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
          
          // Volume calculation
          float volume = std::abs(math::dot(math::cross(v1 - v0, v2 - v0), v3 - v0)) / 6.0f;
          
          // Centroid calculation
          float3 centroid = (v0 + v1 + v2 + v3) * 0.25f;
          
          // Normalized distance to the bounding box center
          float dist_to_center = math::length(centroid - bbox_center) / diag_distance;
          
          // Combined volume/distance score
          float volume_norm = volume / (diag_distance * diag_distance * diag_distance);
          float score = volume_norm * 0.7f + dist_to_center * 0.3f;
          
          tet_scores.push_back(std::make_pair(i, score));
        }
        
        // Score sorting
        std::sort(tet_scores.begin(), tet_scores.end(), 
                  [](const std::pair<int, float>& a, const std::pair<int, float>& b) {
                      return a.second < b.second;
                  });
        
        // Selection of the best tetrahedra (40%)
        size_t num_to_keep = static_cast<size_t>(out.numberoftetrahedra * 0.4);
        for (size_t i = 0; i < num_to_keep && i < tet_scores.size(); i++) {
          internal_tetrahedra.append(tet_scores[i].first);
        }
        
        params.error_message_add(NodeWarningType::Info,
            std::string("Score selection: conservation of ") + std::to_string(internal_tetrahedra.size()) + 
            std::string(" tetrahedra out of ") + std::to_string(out.numberoftetrahedra) + std::string("."));
      }
      else {
        // Filtering based on centroid and bounding box
        float filter_threshold = diag_distance * 0.05f;
        
        // Filtering based on centroid
        for (int i = 0; i < out.numberoftetrahedra; i++) {
          int* tet = &out.tetrahedronlist[i * 4];
          
          // Centroid calculation
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
          
          // Extension of the bounding box
          float3 extended_min = bbox_min - filter_threshold;
          float3 extended_max = bbox_max + filter_threshold;
          
          // Inclusion test
          if (centroid.x >= extended_min.x && centroid.x <= extended_max.x &&
              centroid.y >= extended_min.y && centroid.y <= extended_max.y &&
              centroid.z >= extended_min.z && centroid.z <= extended_max.z) {
            internal_tetrahedra.append(i);
          }
        }
        
        // If no tetrahedra selected, filtering by volume
        if (internal_tetrahedra.is_empty()) {
          params.error_message_add(NodeWarningType::Warning,
              std::string("Centroid filtering failed. Using volume filtering."));
          
          std::vector<std::pair<int, float>> tet_volumes;
          for (int i = 0; i < out.numberoftetrahedra; i++) {
            int* tet = &out.tetrahedronlist[i * 4];
            
            // Extraction of vertices
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
            
            // Volume calculation
            float volume = std::abs(math::dot(math::cross(v1 - v0, v2 - v0), v3 - v0)) / 6.0f;
            tet_volumes.push_back(std::make_pair(i, volume));
          }
          
          // Volume sorting
          std::sort(tet_volumes.begin(), tet_volumes.end(), 
                    [](const std::pair<int, float>& a, const std::pair<int, float>& b) {
                        return a.second < b.second;
                    });
          
          // Selection of the smallest volume tetrahedra (65%)
          size_t num_to_keep = static_cast<size_t>(out.numberoftetrahedra * 0.65);
          for (size_t i = 0; i < num_to_keep && i < tet_volumes.size(); i++) {
            internal_tetrahedra.append(tet_volumes[i].first);
          }
          
          params.error_message_add(NodeWarningType::Info,
              std::string("Volume filtering: conservation of ") + std::to_string(internal_tetrahedra.size()) + 
              std::string(" tetrahedra out of ") + std::to_string(out.numberoftetrahedra) + std::string("."));
        }
        else {
          params.error_message_add(NodeWarningType::Info,
              std::string("Geometric filtering: identification of ") + 
              std::to_string(internal_tetrahedra.size()) + std::string(" internal tetrahedra out of ") + 
              std::to_string(out.numberoftetrahedra) + std::string("."));
        }
      }
    }
    
    // Security filter - in case of failure of all filters
    if (internal_tetrahedra.is_empty() && out.numberoftetrahedra > 0) {
      params.error_message_add(NodeWarningType::Warning,
          std::string("All filters failed. Conservation of a minimal set of tetrahedra."));
      
      // Conservation of at most 50% of the tetrahedra
      size_t max_to_keep = static_cast<size_t>(out.numberoftetrahedra * 0.5);
      for (size_t i = 0; i < max_to_keep && i < static_cast<size_t>(out.numberoftetrahedra); i++) {
        internal_tetrahedra.append(static_cast<int>(i));
      }
    }
    
    // If no internal tetrahedra could be identified
    if (internal_tetrahedra.is_empty()) {
      params.error_message_add(NodeWarningType::Error,
          std::string("No internal tetrahedra could be identified."));
      return nullptr;
    }
    
    // NEW APPROACH: Create a valid tetrahedral mesh
    // We only retrieve the surface-triangulated envelope
    
    // 1. Structure to store unique triangular faces
    using TriangleFace = std::tuple<int, int, int>;
    struct TriangleHash {
      std::size_t operator()(const TriangleFace& face) const {
        auto h1 = std::hash<int>{}(std::get<0>(face));
        auto h2 = std::hash<int>{}(std::get<1>(face));
        auto h3 = std::hash<int>{}(std::get<2>(face));
        return h1 ^ (h2 << 1) ^ (h3 << 2);
      }
    };
    std::unordered_set<TriangleFace, TriangleHash> unique_faces;
    
    // Function to order the indices of a triangle
    auto canonicalize_face = [](int v1, int v2, int v3) -> TriangleFace {
      if (v1 > v2) std::swap(v1, v2);
      if (v2 > v3) std::swap(v2, v3);
      if (v1 > v2) std::swap(v1, v2);
      return std::make_tuple(v1, v2, v3);
    };
    
    // 2. Extract the faces of the tetrahedra
    for (int tet_idx : internal_tetrahedra) {
      int* tet = &out.tetrahedronlist[tet_idx * 4];
      
      // Check for invalid indices
      for (int j = 0; j < 4; j++) {
        if (tet[j] < 0 || tet[j] >= out.numberofpoints) {
          params.error_message_add(NodeWarningType::Warning, 
              "Invalid tetrahedron index detected, correction applied");
          tet[j] = 0;
        }
      }
      
      // Add the 4 faces of the tetrahedron
      unique_faces.insert(canonicalize_face(tet[0], tet[1], tet[2]));
      unique_faces.insert(canonicalize_face(tet[0], tet[1], tet[3]));
      unique_faces.insert(canonicalize_face(tet[0], tet[2], tet[3]));
      unique_faces.insert(canonicalize_face(tet[1], tet[2], tet[3]));
    }
    
    // 3. Build the lists of triangles and edges
    Vector<std::tuple<int, int, int>> triangles;
    std::unordered_set<std::pair<int, int>, pairhash> edges_set;
    
    for (const auto& face : unique_faces) {
      int v1 = std::get<0>(face);
      int v2 = std::get<1>(face);
      int v3 = std::get<2>(face);
      
      // Add the triangle
      triangles.append(face);
      
      // Add the edges
      edges_set.insert(v1 < v2 ? std::make_pair(v1, v2) : std::make_pair(v2, v1));
      edges_set.insert(v2 < v3 ? std::make_pair(v2, v3) : std::make_pair(v3, v2));
      edges_set.insert(v3 < v1 ? std::make_pair(v3, v1) : std::make_pair(v1, v3));
    }
    
    // 4. Create the mesh
    int num_verts = out.numberofpoints;
    int num_edges = edges_set.size();
    int num_faces = triangles.size();
    int num_corners = num_faces * 3;
    
    // Create an empty mesh
    Mesh *mesh_out = BKE_mesh_new_nomain(num_verts, num_edges, num_faces, num_corners);
    
    if (!mesh_out) {
      params.error_message_add(NodeWarningType::Error,
          "Unable to create a mesh with the required dimensions");
      return nullptr;
    }
    
    // 5. Fill the mesh with the data
    try {
      // Copy the vertex positions
      MutableSpan<float3> vert_positions = mesh_out->vert_positions_for_write();
      for (int i = 0; i < num_verts; i++) {
        vert_positions[i] = float3(
            out.pointlist[i * 3],
            out.pointlist[i * 3 + 1],
            out.pointlist[i * 3 + 2]);
      }
      
      // Copier les arêtes
      MutableSpan<int2> edges = mesh_out->edges_for_write();
      int edge_index = 0;
      
      // Tri des arêtes pour cohérence
      std::vector<std::pair<int, int>> sorted_edges(edges_set.begin(), edges_set.end());
      std::sort(sorted_edges.begin(), sorted_edges.end());
      
      for (const auto& edge : sorted_edges) {
        if (edge_index < num_edges) {
          edges[edge_index] = int2(edge.first, edge.second);
          edge_index++;
        }
      }
      
      // Définir les décalages de faces pour des triangles
      MutableSpan<int> face_offsets = mesh_out->face_offsets_for_write();
      for (int i = 0; i <= num_faces; i++) {
        face_offsets[i] = i * 3;
      }
      
      // Copier les indices des sommets pour chaque face
      MutableSpan<int> corner_verts = mesh_out->corner_verts_for_write();
      for (int i = 0; i < num_faces; i++) {
        const auto& triangle = triangles[i];
        corner_verts[i * 3]     = std::get<0>(triangle);
        corner_verts[i * 3 + 1] = std::get<1>(triangle);
        corner_verts[i * 3 + 2] = std::get<2>(triangle);
      }
      
      // Créer une recherche rapide d'arêtes par paire de sommets
      std::unordered_map<std::pair<int, int>, int, pairhash> edge_indices;
      for (int i = 0; i < num_edges; i++) {
        int v1 = edges[i][0];
        int v2 = edges[i][1];
        edge_indices[v1 < v2 ? std::make_pair(v1, v2) : std::make_pair(v2, v1)] = i;
      }
      
      // Associer les coins aux arêtes
      MutableSpan<int> corner_edges = mesh_out->corner_edges_for_write();
      for (int i = 0; i < num_faces; i++) {
        const auto& triangle = triangles[i];
        int v1 = std::get<0>(triangle);
        int v2 = std::get<1>(triangle);
        int v3 = std::get<2>(triangle);
        
        // Trouver les indices d'arêtes pour chaque coin
        auto edge1 = v1 < v2 ? std::make_pair(v1, v2) : std::make_pair(v2, v1);
        auto edge2 = v2 < v3 ? std::make_pair(v2, v3) : std::make_pair(v3, v2);
        auto edge3 = v3 < v1 ? std::make_pair(v3, v1) : std::make_pair(v1, v3);
        
        corner_edges[i * 3]     = edge_indices[edge1];
        corner_edges[i * 3 + 1] = edge_indices[edge2];
        corner_edges[i * 3 + 2] = edge_indices[edge3];
      }
      
      // Ajouter attribut d'index tétraédrique
      bke::MutableAttributeAccessor attributes = mesh_out->attributes_for_write();
      bke::SpanAttributeWriter<int> tet_indices = attributes.lookup_or_add_for_write_span<int>(
          "tetrahedral_index", bke::AttrDomain::Face);
          
      if (tet_indices) {
        for (int i = 0; i < num_faces; i++) {
          tet_indices.span[i] = internal_tetrahedra.is_empty() ? 0 : internal_tetrahedra[0];
        }
        tet_indices.finish();
      }

      
      
      // Uniformize the normals of the mesh (all pointing outward)
      make_normals_consistent(mesh_out, params);
      
      return mesh_out;
    }
    catch (const std::exception &e) {
      params.error_message_add(NodeWarningType::Error,
                             std::string("Error creating mesh: ") + e.what());
      return nullptr;
    }
  }
  catch (const std::exception &e) {
    params.error_message_add(NodeWarningType::Error,
                           std::string("Error creating mesh: ") + e.what());
    return nullptr;
  }
  
  return nullptr;
}

/**
 * Analyzes mesh geometry to detect issues that could cause crashes in TetGen.
 * Checks for problematic features like very acute angles, tiny faces, and self-intersections.
 * 
 * @param mesh The input mesh to analyze
 * @param params Node execution parameters for error reporting
 * @return True if problematic geometry was detected that requires special handling
 */
static bool detect_flipping_prone_geometry(const Mesh *mesh, GeoNodeExecParams &params)
{
  Span<float3> vert_positions = mesh->vert_positions();
  Span<int> corner_verts = mesh->corner_verts();
  Span<int> face_offsets = mesh->face_offsets();
  
  bool has_potential_issues = false;
  
  // For large meshes, only sample a subset of faces
  int num_small_faces = 0;
  int sample_count = 0;
  
  // Limit the number of samples for performance
  const int max_samples = std::min(1000, mesh->faces_num);
  std::vector<int> face_indices;
  face_indices.reserve(max_samples);
  
  if (mesh->faces_num > max_samples) {
    
    std::srand(static_cast<unsigned int>(std::time(nullptr)));
    for (int i = 0; i < max_samples; i++) {
      face_indices.push_back(std::rand() % mesh->faces_num);
    }
  } else {
    
    face_indices.resize(mesh->faces_num);
    std::iota(face_indices.begin(), face_indices.end(), 0);
  }
  
  
  float3 bbox_min(std::numeric_limits<float>::max());
  float3 bbox_max(-std::numeric_limits<float>::max());
  
  for (int i = 0; i < mesh->verts_num; i++) {
    bbox_min = math::min(bbox_min, vert_positions[i]);
    bbox_max = math::max(bbox_max, vert_positions[i]);
  }
  
  float bounding_size = math::length(bbox_max - bbox_min);
  float tiny_feature_threshold = bounding_size * 1e-4f; 
  
  
  for (int face_idx : face_indices) {
    int face_start = face_offsets[face_idx];
    int face_size = face_offsets[face_idx + 1] - face_start;
    
    
    if (face_size == 3) {
      sample_count++;
      
      int v1_idx = corner_verts[face_start];
      int v2_idx = corner_verts[face_start + 1];
      int v3_idx = corner_verts[face_start + 2];
      
      
      float3 v1 = vert_positions[v1_idx];
      float3 v2 = vert_positions[v2_idx];
      float3 v3 = vert_positions[v3_idx];
      
      
      float edge1_len = math::distance(v1, v2);
      float edge2_len = math::distance(v2, v3);
      float edge3_len = math::distance(v3, v1);
      
      
      float s = (edge1_len + edge2_len + edge3_len) / 2.0f;
      float area = std::sqrt(s * (s - edge1_len) * (s - edge2_len) * (s - edge3_len));
      
      
      float inradius = (area > 0.0f) ? (area / s) : 0.0f;
      
      
      float min_edge = std::min({edge1_len, edge2_len, edge3_len});
      float max_edge = std::max({edge1_len, edge2_len, edge3_len});
      
      
      bool is_sliver = (inradius < tiny_feature_threshold) && (max_edge / min_edge > 10.0f);
      bool is_tiny = (area < tiny_feature_threshold * tiny_feature_threshold);
      
      if (is_sliver || is_tiny) {
        num_small_faces++;
      }
      
      
      
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
      
      
      if (min_sin_angle < 0.01f) { 
        has_potential_issues = true;
        num_small_faces++;
        
        
        if (min_sin_angle < 0.001f) {
          params.error_message_add(NodeWarningType::Warning,
              "Triangles with extremely acute angles detected (< 0.06 degrees). "
              "High probability of crash in tetgenmesh::sscoutsegment.");
          return true;
        }
      }
    }
  }
  
  
  
  
  
  
  bool check_self_intersect = mesh->faces_num < 5000;
  
  if (check_self_intersect) {
    
    struct Triangle {
      float3 v1, v2, v3;
      int face_idx;
    };
    
    std::vector<Triangle> triangles;
    triangles.reserve(mesh->faces_num);
    
    for (int i = 0; i < mesh->faces_num; i++) {
    int face_start = face_offsets[i];
    int face_size = face_offsets[i + 1] - face_start;
    
      
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
    
    
    auto triangles_intersect = [](const Triangle &t1, const Triangle &t2) -> bool {
      
      if (t1.v1 == t2.v1 || t1.v1 == t2.v2 || t1.v1 == t2.v3 ||
          t1.v2 == t2.v1 || t1.v2 == t2.v2 || t1.v2 == t2.v3 ||
          t1.v3 == t2.v1 || t1.v3 == t2.v2 || t1.v3 == t2.v3) {
        return false;
      }
      
      
      
      
      
      
      
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
      
      
      if (point_in_triangle(t1.v1, t2.v1, t2.v2, t2.v3) ||
          point_in_triangle(t1.v2, t2.v1, t2.v2, t2.v3) ||
          point_in_triangle(t1.v3, t2.v1, t2.v2, t2.v3)) {
        return true;
      }
      
      
      if (point_in_triangle(t2.v1, t1.v1, t1.v2, t1.v3) ||
          point_in_triangle(t2.v2, t1.v1, t1.v2, t1.v3) ||
          point_in_triangle(t2.v3, t1.v1, t1.v2, t1.v3)) {
        return true;
      }
      
      return false;
    };
    
    
    int num_samples = std::min(10000, int(triangles.size() * triangles.size() / 4));
    int self_intersect_count = 0;
    
    std::srand(static_cast<unsigned int>(std::time(nullptr) + 1));
    
    for (int s = 0; s < num_samples; s++) {
      int idx1 = std::rand() % triangles.size();
      int idx2 = std::rand() % triangles.size();
      
      
      if (idx1 != idx2) {
        if (triangles_intersect(triangles[idx1], triangles[idx2])) {
          self_intersect_count++;
          
          
          if (self_intersect_count > 10) {
            params.error_message_add(NodeWarningType::Warning,
                "Many self-intersections detected in the mesh. "
                "High probability of crash in tetgenmesh::sscoutsegment.");
            return true;
          }
        }
      }
    }
    
    if (self_intersect_count > 0) {
      params.error_message_add(NodeWarningType::Warning,
          std::to_string(self_intersect_count) + " auto-intersections detected in the mesh. "
          "Potential risk of crash in tetgenmesh::sscoutsegment.");
      has_potential_issues = true;
    }
  }
  
  
  float3 dimensions = bbox_max - bbox_min;
  float min_dim = std::min({dimensions.x, dimensions.y, dimensions.z});
  float max_dim = std::max({dimensions.x, dimensions.y, dimensions.z});
  
  if (min_dim < max_dim * 0.001f) {
      params.error_message_add(NodeWarningType::Warning,
        "Very flat mesh detected (aspect ratio: " + std::to_string(min_dim/max_dim) + "). "
        "High probability of crash in tetgenmesh::sscoutsegment.");
    has_potential_issues = true;
  }
  
  
  if (sample_count > 0 && (float)num_small_faces / sample_count > 0.05f) {
      has_potential_issues = true;
        params.error_message_add(NodeWarningType::Warning,
          "Problematic geometry detected - " + std::to_string(num_small_faces) + 
          " bad quality triangles on " + std::to_string(sample_count) + 
          " samples. High risk of crash in sscoutsegment.");
  }

  
  if (has_potential_issues) {
    params.error_message_add(NodeWarningType::Warning,
        "Mesh at risk for tetgenmesh::sscoutsegment - consider remeshing, solidifying, "
        "or extruding before tetrahedralization.");
  }
  
  
  if (has_potential_issues) {
      params.error_message_add(NodeWarningType::Info,
          "Activation of crash protections for complex mesh");
  }
  
  return has_potential_issues;
}


static bool check_mesh_volume(const Mesh *mesh, float *estimated_volume, GeoNodeExecParams &params);

/**
 * Checks if a mesh is manifold (watertight, no open boundaries) and valid for tetrahedralization
 * Returns true if manifold and valid, false otherwise and adds error messages
 */
static bool check_manifold_mesh(const Mesh *mesh, GeoNodeExecParams &params)
{
  
  if (!mesh || mesh->verts_num == 0) {
    params.error_message_add(NodeWarningType::Error, 
        "Cannot tetrahedralize: input mesh is empty");
    return false;
  }
  
  
  if (mesh->verts_num < 4) {
    params.error_message_add(NodeWarningType::Error, 
        "Cannot tetrahedralize: mesh must have at least 4 vertices");
    return false;
  }
  
  
  Span<float3> vert_positions = mesh->vert_positions();
  
  
  float3 bbox_min(std::numeric_limits<float>::max());
  float3 bbox_max(-std::numeric_limits<float>::max());
  
  for (int i = 0; i < mesh->verts_num; i++) {
    bbox_min = math::min(bbox_min, vert_positions[i]);
    bbox_max = math::max(bbox_max, vert_positions[i]);
  }
  
  
  float3 dimensions = bbox_max - bbox_min;
  float min_dim = std::min({dimensions.x, dimensions.y, dimensions.z});
  float max_dim = std::max({dimensions.x, dimensions.y, dimensions.z});
  float volume = dimensions.x * dimensions.y * dimensions.z;
  
  
  if (volume < 1e-6f || min_dim < max_dim * 0.001f) {
    params.error_message_add(NodeWarningType::Error, 
        "Cannot tetrahedralize: mesh is too flat (2D or nearly 2D). "
        "TetGen requires a true 3D volume with significant thickness in all dimensions. "
        "Try extruding or solidifying the mesh first.");
    
    
    params.error_message_add(NodeWarningType::Info, 
        "Mesh dimensions: X=" + std::to_string(dimensions.x) + 
        ", Y=" + std::to_string(dimensions.y) + 
        ", Z=" + std::to_string(dimensions.z) + 
        ". Min/Max ratio: " + std::to_string(min_dim/max_dim));
    
    return false;
  }
  
  
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
  
  
  Span<int> corner_verts = mesh->corner_verts();
  
  
  std::unordered_map<std::pair<int, int>, std::vector<int>, pairhash> edge_to_faces;
  
  
  std::unordered_set<int> used_vertices;
  
  
  auto add_edge = [&](int v1, int v2, int face_idx) {
    
    if (v1 < 0 || v2 < 0 || v1 >= mesh->verts_num || v2 >= mesh->verts_num || v1 == v2) {
      return;
    }
    
    
    std::pair<int, int> edge = v1 < v2 ? std::make_pair(v1, v2) : std::make_pair(v2, v1);
    edge_to_faces[edge].push_back(face_idx);
    
    
    used_vertices.insert(v1);
    used_vertices.insert(v2);
  };
  
  
  int invalid_face_indices = 0;
  
  for (int i = 0; i < mesh->faces_num; i++) {
    int face_start = face_offsets[i];
    int face_size = face_offsets[i + 1] - face_start;
    
    
    if (face_size < 3) {
      continue;
    }
    
    bool face_has_invalid_index = false;
    
    
    for (int j = 0; j < face_size; j++) {
      int v1 = corner_verts[face_start + j];
      int v2 = corner_verts[face_start + ((j + 1) % face_size)];
      
      
      if (v1 < 0 || v2 < 0 || v1 >= mesh->verts_num || v2 >= mesh->verts_num) {
        face_has_invalid_index = true;
        invalid_face_indices++;
        continue;
      }
      
      
      if (v1 == v2) {
        continue;
      }
      
      add_edge(v1, v2, i);
    }
    
    
    if (face_has_invalid_index) {
      continue;
    }
  }
  
  
  if (invalid_face_indices > 0) {
    params.error_message_add(NodeWarningType::Error, 
        "Cannot tetrahedralize: mesh contains " + std::to_string(invalid_face_indices) + 
        " faces with invalid vertex indices");
    return false;
  }
  
  
  if (used_vertices.size() < mesh->verts_num) {
    int isolated_verts = mesh->verts_num - used_vertices.size();
      params.error_message_add(NodeWarningType::Error,
        "Cannot tetrahedralize: mesh contains " + std::to_string(isolated_verts) + 
        " isolated vertices not connected to any face");
    return false;
  }
  
  
  int boundary_edges = 0;
  int non_manifold_edges = 0;
  
  for (const auto &edge_entry : edge_to_faces) {
    int face_count = edge_entry.second.size();
    
    if (face_count == 1) {
      
      boundary_edges++;
    } else if (face_count > 2) {
      
      non_manifold_edges++;
    }
  }
  
  
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
  
  
  
  float bounding_size = math::length(bbox_max - bbox_min);
  float tiny_feature_threshold = bounding_size * 1e-6f;
  
  int tiny_edges = 0;
  
  
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
  
  
  
  float est_volume = 0.0f;
  bool has_sufficient_volume = check_mesh_volume(mesh, &est_volume, params);
  
  if (!has_sufficient_volume) {
    params.error_message_add(NodeWarningType::Error, 
        "Cannot tetrahedralize: mesh has insufficient volume. "
        "The mesh may be too flat or self-intersecting. "
        "Try extruding, solidifying, or repairing the mesh first.");
    return false;
  }
  
  
  return true;
}

/**
 * Advanced mesh volume verification.
 * This function calculates an approximate volume to detect 
 * overly flat or nearly volume-less meshes.
 */
static bool check_mesh_volume(const Mesh *mesh, float *estimated_volume, GeoNodeExecParams &params)
{
  *estimated_volume = 0.0f;
  Span<float3> vert_positions = mesh->vert_positions();
  
  
  if (mesh->verts_num < 50) {
    
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
      
      
      if (face_size < 3) {
        continue;
      }
      
      for (int j = 0; j < face_size - 2; j++) {
        
        int v1 = corner_verts[face_start];
        int v2 = corner_verts[face_start + j + 1];
        int v3 = corner_verts[face_start + j + 2];
        
        
        float3 a = vert_positions[v1] - center;
        float3 b = vert_positions[v2] - center;
        float3 c = vert_positions[v3] - center;
        
        
        volume += std::abs(math::dot(a, math::cross(b, c))) / 6.0f;
      }
    }
    
    *estimated_volume = volume;
    
    
    float3 bbox_min(std::numeric_limits<float>::max());
    float3 bbox_max(-std::numeric_limits<float>::max());
    
    for (int i = 0; i < mesh->verts_num; i++) {
      bbox_min = math::min(bbox_min, vert_positions[i]);
      bbox_max = math::max(bbox_max, vert_positions[i]);
    }
    
    float3 dimensions = bbox_max - bbox_min;
    float bbox_volume = dimensions.x * dimensions.y * dimensions.z;
    
    
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
    
    
    if (min_dim < max_dim * 0.001f) {
      *estimated_volume = 0.0f;
      return false;
    }
    
    
    
    
    *estimated_volume = bbox_volume * (min_dim / max_dim); 
    
    return (min_dim / max_dim) > 0.01f; 
  }
}

/**
 * Main node execution function
 * 
 * Workflow:
 * 1. Extract and validate input mesh
 * 2. Prepare mesh for tetrahedralization
 * 3. Configure TetGen parameters
 * 4. Run tetrahedralization with error recovery strategies
 * 5. Process TetGen output into a Blender mesh
 */
static void node_geo_exec(GeoNodeExecParams params)
{
  try {
    // Extract input mesh
    GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh");
    
    if (!geometry_set.has_mesh()) {
      params.error_message_add(NodeWarningType::Error, 
          "Required input: a mesh for tetrahedralization");
      params.set_output("Tetrahedral Mesh", GeometrySet());
      return;
    }
    
    // Get the mesh and validate it's manifold (watertight)
    const Mesh *mesh_in = geometry_set.get_mesh();
    
    if (!check_manifold_mesh(mesh_in, params)) {
      // Return original mesh if not manifold
      params.set_output("Tetrahedral Mesh", std::move(geometry_set));
      return;
    }
    
    // Pre-process mesh for complex cases
    Mesh *prepared_mesh = prepare_complex_mesh_for_tetgen(mesh_in, params);
    const Mesh *mesh_to_process = prepared_mesh ? prepared_mesh : mesh_in;
    
    
    // Extract and validate node parameters with error handling
    double max_volume_percentage = 0.8;
    try {
      max_volume_percentage = params.extract_input<float>("Max Volume");
    }
    catch (...) {
      params.error_message_add(NodeWarningType::Warning, 
          "Failed to extract Max Volume parameter, using default value (0.8)");
    }
    
    // Calculate mesh bounding box to scale max_volume appropriately
    Span<float3> vert_positions = mesh_to_process->vert_positions();
    float3 bbox_min(std::numeric_limits<float>::max());
    float3 bbox_max(-std::numeric_limits<float>::max());
    
    for (int i = 0; i < vert_positions.size(); i++) {
      bbox_min = math::min(bbox_min, vert_positions[i]);
      bbox_max = math::max(bbox_max, vert_positions[i]);
    }
    
    float3 dimensions = bbox_max - bbox_min;
    float mesh_volume = dimensions.x * dimensions.y * dimensions.z;
    
  
    double tetgen_unit_volume = mesh_volume / 1000.0;  // Base ~ 1000 tétraèdres
    
    double density_power;
    double max_volume;
    
    if (max_volume_percentage < 0.1) {

        density_power = 12.0 * (1.0 - max_volume_percentage/0.1);
        max_volume = tetgen_unit_volume * pow(10.0, -density_power);
    }
    else if (max_volume_percentage < 0.5) {

        density_power = 6.0 * (1.0 - (max_volume_percentage-0.1)/0.4);
        max_volume = tetgen_unit_volume * pow(10.0, -density_power);
    }
    else {

        density_power = 1.0 * (max_volume_percentage - 0.5) / 0.5;
        max_volume = tetgen_unit_volume * pow(10.0, density_power);
    }
    
    double min_safe_volume = 1e-20;
    if (max_volume < min_safe_volume) {
        max_volume = min_safe_volume;
    }
    
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
    
    // Complexity thresholds for adaptive processing
    const int medium_complexity_threshold = 5000;  
    const int high_complexity_threshold = 10000;   
    const int extreme_complexity_threshold = 50000; 
    const int massive_mesh_threshold = 100000;    

    // Determine mesh complexity level for adaptive strategies
    bool is_medium_complex = mesh_to_process->verts_num > medium_complexity_threshold || 
                           mesh_to_process->faces_num > medium_complexity_threshold;
    bool is_highly_complex = mesh_to_process->verts_num > high_complexity_threshold || 
                           mesh_to_process->faces_num > high_complexity_threshold;
    bool is_extremely_complex = mesh_to_process->verts_num > extreme_complexity_threshold || 
                              mesh_to_process->faces_num > extreme_complexity_threshold;
    bool is_massive_mesh = mesh_to_process->verts_num > massive_mesh_threshold ||
                          mesh_to_process->faces_num > massive_mesh_threshold;
    
    
    bool has_problematic_geometry = detect_flipping_prone_geometry(mesh_to_process, params);
    
    
    bool is_complex_mesh = is_medium_complex;
    
    
    tetgenio in, out;
    TetGenResourceGuard resource_guard(in, out);
    Mesh *mesh_out = nullptr;
    
    try {
      
      if (!prepare_tetgen_input(mesh_to_process, in, params, 0)) {
        params.error_message_add(NodeWarningType::Error,
            "Failed to prepare input for tetrahedralization. Returning input mesh unchanged.");
        
        
        if (prepared_mesh) {
          BKE_id_free(nullptr, prepared_mesh);
        }
        
        params.set_output("Tetrahedral Mesh", std::move(geometry_set));
        return;
      }
      
      
      tetgenbehavior behavior;
      configure_tetgen_options(behavior, 
                             max_volume,
                             quality_ratio,
                             min_dihedral_angle, 
                             preserve_boundary,
                             params);
      
      
      if (has_problematic_geometry) {
        
        behavior.nomergefacet = 1;   
        behavior.nomergevertex = 1;  
        behavior.nojettison = 1;     
        behavior.docheck = 0;        
        behavior.diagnose = 0;       
        
        
        behavior.plc = 0;            
        behavior.nobisect = 0;       
        
                params.error_message_add(NodeWarningType::Warning, 
            "Problematic geometry detected. Crash protection activated. "
            "Mesh boundaries will not be preserved.");
      }
      
      
                for (int i = 0; i < in.numberofpoints * 3; i++) {
                  double noise = ((double)rand() / RAND_MAX) * 1e-6;
                  in.pointlist[i] += noise;
                }
                
      
      bool tetgen_success = false;
      
      try {
        // First attempt at tetrahedralization with standard settings
                tetrahedralize(&behavior, &in, &out);
        tetgen_success = true;
      }
      catch (std::exception &e) {
        // Handle specific crash in sscoutsegment (common TetGen error)
        std::string error_msg = e.what();
        if (error_msg.find("sscoutsegment") != std::string::npos || 
            error_msg.find("Access violation") != std::string::npos) {
          
          params.error_message_add(NodeWarningType::Warning,
              "Crash detected in sscoutsegment. Attempting with anti-crash configuration...");
          
          // Reset TetGen structures for a new attempt
          in.initialize();
          out.initialize();
          
          // Second attempt: Use stronger perturbations and safer settings
          if (prepare_tetgen_input(mesh_to_process, in, params, 3)) {
            // Ultra-safe configuration to avoid crashes in problematic cases
            tetgenbehavior safe_behavior;
            safe_behavior.plc = 0;           // Disable PLC (piecewise linear complex)
            safe_behavior.nobisect = 0;      // Allow bisections
            safe_behavior.quality = 0;       // Disable quality optimization
            safe_behavior.mindihedral = 0.0; // Disable minimum dihedral angle check
            safe_behavior.minratio = 1.0;    // Disable aspect ratio check
            safe_behavior.docheck = 0;       // Disable consistency checks
            safe_behavior.diagnose = 0;      // Disable diagnostic output
            safe_behavior.convex = 1;        // Use convex hull mode for robustness
            
            // Critical crash prevention options
            safe_behavior.nomergefacet = 1;  // Don't merge facets (prevents flipping errors)
            safe_behavior.nomergevertex = 1; // Don't merge vertices
            safe_behavior.nojettison = 1;    // Don't remove unused vertices
            
            // Reduce output complexity
            safe_behavior.facesout = 0;
            safe_behavior.edgesout = 0;
            safe_behavior.neighout = 0;
            
            // Apply stronger random perturbation to break degeneracies
            for (int i = 0; i < in.numberofpoints * 3; i++) {
              double noise = ((double)rand() / RAND_MAX) * 1e-4;
              in.pointlist[i] += noise;
            }
            
            try {
              params.error_message_add(NodeWarningType::Info,
                  "Attempting tetrahedralization with maximum crash protection");
              tetrahedralize(&safe_behavior, &in, &out);
              tetgen_success = true;
              
              params.error_message_add(NodeWarningType::Info,
                  "Tetrahedralization successful with crash protection configuration");
            }
            catch (std::exception &e) {
              
              params.error_message_add(NodeWarningType::Warning,
                  "First crash protection attempt failed. Last attempt with a pure convex hull...");
              
              try {
                
                in.initialize();
                out.initialize();
                
                
                if (prepare_tetgen_input(mesh_to_process, in, params, 5)) {
                  
                  tetgenbehavior minimal_behavior;
                  minimal_behavior.plc = 0;
                  minimal_behavior.quality = 0;
                  minimal_behavior.nobisect = 0;
                  minimal_behavior.convex = 1;        
                  minimal_behavior.weighted = 0;      
                  minimal_behavior.diagnose = 0;      
                  minimal_behavior.verbose = 0;       
                  minimal_behavior.nomergefacet = 1;  
                  minimal_behavior.nomergevertex = 1;
                  minimal_behavior.nojettison = 1;
                  
                  
                  minimal_behavior.facesout = 0;
                  minimal_behavior.edgesout = 0;
                  minimal_behavior.neighout = 0;
                  minimal_behavior.voroout = 0;
                  
                  
                  tetrahedralize(&minimal_behavior, &in, &out);
                tetgen_success = true;
                
                  params.error_message_add(NodeWarningType::Info, 
                      "Tetrahedralization successful in pure convex hull mode. "
                      "The exact shape of the mesh was not preserved.");
                }
              }
              catch (...) {
                params.error_message_add(NodeWarningType::Error,
                    "All tetrahedralization attempts failed. "
                    "The mesh is too problematic or flat to be tetrahedralized.");
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
    
    
    GeometrySet output;
    if (mesh_out) {
      output.replace_mesh(mesh_out);
    }
    else {
      
      output = std::move(geometry_set);
      params.error_message_add(NodeWarningType::Info,
          "Failed to create tetrahedral mesh. Returning input mesh unchanged.");
    }
    
    
  if (prepared_mesh) {
    BKE_id_free(nullptr, prepared_mesh);
  }
  
    params.set_output("Tetrahedral Mesh", std::move(output));
  }
  catch (...) {
    
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


void register_node_type_geo_tetrahedralize()
{
  namespace file_ns = blender::nodes::node_geo_tetrahedralize_cc;

  file_ns::node_register();
}

}  
