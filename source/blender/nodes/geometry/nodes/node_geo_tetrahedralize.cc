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

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Mesh").supported_type(GeometryComponent::Type::Mesh)
      .description("Surface mesh to tetrahedralize");
  
  b.add_input<decl::Float>("Max Volume").default_value(0.5).min(0.0001).max(1.0)
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
 * Prepares TetGen input data from a Blender mesh
 */
static bool prepare_tetgen_input(const Mesh *mesh_in, tetgenio &in, GeoNodeExecParams &params)
{
  // Initialize TetGen
  in.initialize();
  in.firstnumber = 0;  // TetGen uses indexing starting from 0
  
  // Input data validation
  if (!mesh_in || mesh_in->verts_num < 4) {
    params.error_message_add(NodeWarningType::Error, 
        "Input mesh must contain at least 4 points for tetrahedralization");
    return false;
  }
  
  // Additional check to ensure mesh is valid
  if (mesh_in->faces_num < 4) {
    params.error_message_add(NodeWarningType::Error, 
        "Input mesh must contain at least 4 faces for tetrahedralization");
    return false;
  }
  
  try {
    // Retrieve mesh data
    const Span<float3> positions = mesh_in->vert_positions();
    const OffsetIndices faces = mesh_in->faces();
    const Span<int> corner_verts = mesh_in->corner_verts();
    
    // Input points
    in.numberofpoints = mesh_in->verts_num;
    in.pointlist = new REAL[in.numberofpoints * 3];
    
    // Copy positions in parallel if enough vertices
    if (mesh_in->verts_num > 1000) {
      tbb::parallel_for(MeshBlockedRange(0, mesh_in->verts_num, 1024), 
                        [&](const MeshBlockedRange &range) {
        for (int i = range.begin(); i < range.end(); i++) {
          in.pointlist[i * 3] = positions[i].x;
          in.pointlist[i * 3 + 1] = positions[i].y;
          in.pointlist[i * 3 + 2] = positions[i].z;
        }
      });
    }
    else {
      // Sequential version for a small number of vertices
      for (int i = 0; i < mesh_in->verts_num; i++) {
        in.pointlist[i * 3] = positions[i].x;
        in.pointlist[i * 3 + 1] = positions[i].y;
        in.pointlist[i * 3 + 2] = positions[i].z;
      }
    }
    
    // Input faces
    in.numberoffacets = mesh_in->faces_num;
    in.facetlist = new tetgenio::facet[in.numberoffacets];
    
    // Initialize facetlist in parallel
    tbb::parallel_for(MeshBlockedRange(0, mesh_in->faces_num, 512),
                     [&](const MeshBlockedRange &range) {
      for (int i = range.begin(); i < range.end(); i++) {
        tetgenio::facet *f = &in.facetlist[i];
        f->numberofholes = 0;
        f->holelist = nullptr;
        f->numberofpolygons = 0; // Will be configured later
        f->polygonlist = nullptr; // Will be allocated later if necessary
      }
    });
    
    // Iterate over faces to determine which are valid
    // This step is not easily parallelizable due to dynamic allocations
    // and dependencies between operations
    int valid_faces = 0; // Valid faces counter
    
    // Use a mutex to protect the valid_faces counter
    tbb::mutex face_mutex;
    
    // Use atomic pointers to handle allocations/deallocations safely
    auto process_face = [&](const int i) {
      const IndexRange face = faces[i];
      int vcount = face.size();
      
      // At least 3 vertices for a valid polygon
      if (vcount < 3) {
        return false;
      }
      
      tetgenio::facet *f = &in.facetlist[i];
      f->numberofpolygons = 1;
      f->polygonlist = new tetgenio::polygon[1];
      f->polygonlist[0].numberofvertices = vcount;
      f->polygonlist[0].vertexlist = new int[vcount];
      
      // Copy vertex indices with validation
      for (int j = 0; j < vcount; j++) {
        int idx = corner_verts[face[j]];
        // Bounds check
        if (idx >= 0 && idx < mesh_in->verts_num) {
          f->polygonlist[0].vertexlist[j] = idx;
        }
        else {
          // Invalid index, use 0 as fallback
          f->polygonlist[0].vertexlist[j] = 0;
          // Cannot use error_message_add here as we might be in a thread
        }
      }
      
      return true;
    };
    
    // Process faces in blocks for better load balancing
    // For small meshes, process sequentially
    if (mesh_in->faces_num > 500) {
      Array<int> face_valid(mesh_in->faces_num, 0);
      
      tbb::parallel_for(MeshBlockedRange(0, mesh_in->faces_num, 128), 
                      [&](const MeshBlockedRange &range) {
        int local_valid_count = 0;
        
        for (int i = range.begin(); i < range.end(); i++) {
          bool is_valid = process_face(i);
          face_valid[i] = is_valid ? 1 : 0;
          if (is_valid) {
            local_valid_count++;
          }
        }
        
        // Update global counter in a thread-safe manner
        tbb::mutex::scoped_lock lock(face_mutex);
        valid_faces += local_valid_count;
      });
      
      // Check invalid faces for debugging
      int invalid_count = 0;
      for (int i = 0; i < mesh_in->faces_num; i++) {
        if (face_valid[i] == 0) {
          invalid_count++;
        }
      }
      
      if (invalid_count > 0) {
        params.error_message_add(NodeWarningType::Warning,
            std::to_string(invalid_count) + " faces were ignored as they have less than 3 vertices");
      }
    }
    else {
      // Sequential version for small meshes
      for (int i = 0; i < mesh_in->faces_num; i++) {
        if (process_face(i)) {
          valid_faces++;
        }
      }
    }
    
    // Check if there are enough valid faces
    if (valid_faces < 4) {
      params.error_message_add(NodeWarningType::Error,
          "Not enough valid faces (at least 4 required) for tetrahedralization");
      return false;
    }
    
    return true;
  }
  catch (const std::exception &e) {
    params.error_message_add(NodeWarningType::Error, 
        std::string("Error while preparing data: ") + e.what());
    return false;
  }
}

/**
 * Configures TetGen options based on node parameters
 */
static void configure_tetgen_options(tetgenbehavior &behavior, 
                                    double max_volume, 
                                    float quality_ratio,
                                    float min_dihedral_angle,
                                    bool preserve_boundary,
                                    GeoNodeExecParams &params)
{
  // Basic configuration
  behavior.plc = 1;          // Preserve piecewise linear complex (boundary)
  behavior.quality = 1;      // Enable quality improvement
  behavior.quiet = 1;        // Quiet mode (no stdout output)
  behavior.verbose = 0;      // No verbosity
  
  // Preserve input boundary
  // In TetGen, nobisect=1 means not to bisect input faces
  // which corresponds to "preserve boundary" = true
  behavior.nobisect = preserve_boundary ? 1 : 0;
  
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
  
  behavior.minratio = limited_quality_ratio;
  
  // Use the dihedral angle value directly from the input
  // Convert from degrees to internal TetGen value (TetGen expects degrees)
  // Clamp to realistic values to prevent excessive slowdowns
  float clamped_angle = std::min(min_dihedral_angle, 30.0f);
  behavior.mindihedral = clamped_angle;
  
  // Maximum tetrahedra volume
  if (max_volume > 0.0001) {
    behavior.fixedvolume = 1;
    behavior.maxvolume = max_volume;
  }
  
  // Output
  behavior.edgesout = 1;     // Generate edges
  behavior.facesout = 1;     // Generate faces
  behavior.neighout = 1;     // Generate neighbor information
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
  
  return true;
}

/**
 * Creates a Blender mesh from TetGen results
 */
static Mesh* create_tetrahedral_mesh(tetgenio &out, GeoNodeExecParams &params, double max_volume, const tetgenbehavior &behavior)
{
  // Validate results
  if (!validate_tetgen_output(out, params)) {
    return nullptr;
  }

  // Check model scale
  if (max_volume < 0.0001) {
    params.error_message_add(NodeWarningType::Warning, "The max_volume value is very small, which may not be visible");
  }

  // Safety limit to avoid performance issues
  const int max_tets = 3000000;
  int num_tets = std::min(out.numberoftetrahedra, max_tets);
  
  if (num_tets < out.numberoftetrahedra) {
    params.error_message_add(NodeWarningType::Warning, 
        "Number of tetrahedra limited to " + std::to_string(num_tets) + 
        " for performance reasons (out of " + std::to_string(out.numberoftetrahedra) + ")");
  }
  
  // Calculate mesh dimensions
  const int num_verts = out.numberofpoints;
  const int num_edges = out.numberofedges > 0 ? out.numberofedges : 0;
  const int num_faces = num_tets * 4;  // 4 faces per tetrahedron
  const int num_loops = num_faces * 3; // 3 vertices per triangular face
  
  // Create mesh
  Mesh *mesh_out = BKE_mesh_new_nomain(num_verts, num_edges, num_faces, num_loops);
  
  if (!mesh_out) {
    params.error_message_add(NodeWarningType::Error, "Unable to create mesh");
    return nullptr;
  }
  
  // 1. Copy vertices (parallelized)
  MutableSpan<float3> vert_positions = mesh_out->vert_positions_for_write();
  
  tbb::parallel_for(MeshBlockedRange(0, num_verts, 1024),
                   [&](const MeshBlockedRange &range) {
    for (int i = range.begin(); i < range.end(); i++) {
      vert_positions[i].x = out.pointlist[i * 3];
      vert_positions[i].y = out.pointlist[i * 3 + 1];
      vert_positions[i].z = out.pointlist[i * 3 + 2];
    }
  });
  
  // 2. Copy edges (parallelized if sufficient number)
  if (num_edges > 0 && out.edgelist) {
    MutableSpan<int2> edges = mesh_out->edges_for_write();
    
    if (num_edges > 1000) {
      tbb::parallel_for(MeshBlockedRange(0, num_edges, 1024),
                       [&](const MeshBlockedRange &range) {
        for (int i = range.begin(); i < range.end(); i++) {
          // Check that indices are valid
          int v1 = out.edgelist[i * 2];
          int v2 = out.edgelist[i * 2 + 1];
          
          if (v1 >= 0 && v1 < num_verts && v2 >= 0 && v2 < num_verts) {
            edges[i].x = v1;
            edges[i].y = v2;
          }
          else {
            // In case of invalid index, use safe values
            edges[i].x = 0;
            edges[i].y = std::min(1, num_verts - 1);
          }
        }
      });
    }
    else {
      // Sequential version for a small number of edges
      for (int i = 0; i < num_edges; i++) {
        int v1 = out.edgelist[i * 2];
        int v2 = out.edgelist[i * 2 + 1];
        
        if (v1 >= 0 && v1 < num_verts && v2 >= 0 && v2 < num_verts) {
          edges[i].x = v1;
          edges[i].y = v2;
        }
        else {
          edges[i].x = 0;
          edges[i].y = std::min(1, num_verts - 1);
          params.error_message_add(NodeWarningType::Warning,
              "Invalid edge index detected and corrected");
        }
      }
    }
  }
  
  // 3. Create tetrahedral faces (using temporary structure for parallelization)
  MutableSpan<int> corner_verts = mesh_out->corner_verts_for_write();
  
  if (num_tets > 1000) {
    // Parallel version for a large number of tetrahedra
    // Use Array which are thread-safe to build the data
    Array<int> tet_valid(num_tets, 1); // 1 if valid, 0 otherwise
    
    tbb::parallel_for(MeshBlockedRange(0, num_tets, 256),
                     [&](const MeshBlockedRange &range) {
      for (int i = range.begin(); i < range.end(); i++) {
        // Retrieve tetrahedron indices
        int v0 = out.tetrahedronlist[i * 4];
        int v1 = out.tetrahedronlist[i * 4 + 1];
        int v2 = out.tetrahedronlist[i * 4 + 2];
        int v3 = out.tetrahedronlist[i * 4 + 3];
        
        // Validate indices
        if (v0 < 0 || v0 >= num_verts || v1 < 0 || v1 >= num_verts ||
            v2 < 0 || v2 >= num_verts || v3 < 0 || v3 >= num_verts) {
          tet_valid[i] = 0; // Mark as invalid
          continue;
        }
        
        // Calculate offset for this tetrahedron
        int offset = i * 12; 
        

        corner_verts[offset] = v0;
        corner_verts[offset + 1] = v1;
        corner_verts[offset + 2] = v2;
        

        corner_verts[offset + 3] = v0;
        corner_verts[offset + 4] = v3;
        corner_verts[offset + 5] = v1;
        

        corner_verts[offset + 6] = v0;
        corner_verts[offset + 7] = v2;
        corner_verts[offset + 8] = v3;
        

        corner_verts[offset + 9] = v1;
        corner_verts[offset + 10] = v3;
        corner_verts[offset + 11] = v2;
      }
    });
    
    // Check if there are invalid tetrahedra and warn
    int invalid_count = 0;
    for (int i = 0; i < num_tets; i++) {
      if (tet_valid[i] == 0) {
        invalid_count++;
      }
    }
    
    if (invalid_count > 0) {
      params.error_message_add(NodeWarningType::Warning,
          std::to_string(invalid_count) + " tetrahedra were ignored due to invalid indices");
    }
  }
  else {
    // Sequential version for a small number of tetrahedra
    int corner_index = 0;
    
    for (int i = 0; i < num_tets; i++) {
      // Retrieve tetrahedron indices
      int v0 = out.tetrahedronlist[i * 4];
      int v1 = out.tetrahedronlist[i * 4 + 1];
      int v2 = out.tetrahedronlist[i * 4 + 2];
      int v3 = out.tetrahedronlist[i * 4 + 3];
      
      // Validate indices
      if (v0 < 0 || v0 >= num_verts || v1 < 0 || v1 >= num_verts ||
          v2 < 0 || v2 >= num_verts || v3 < 0 || v3 >= num_verts) {
        continue;
      }
      

      corner_verts[corner_index++] = v0;
      corner_verts[corner_index++] = v1;
      corner_verts[corner_index++] = v2;
      

      corner_verts[corner_index++] = v0;
      corner_verts[corner_index++] = v3;
      corner_verts[corner_index++] = v1;
      

      corner_verts[corner_index++] = v0;
      corner_verts[corner_index++] = v2;
      corner_verts[corner_index++] = v3;
      

      corner_verts[corner_index++] = v1;
      corner_verts[corner_index++] = v3;
      corner_verts[corner_index++] = v2;
    }
  }
  
  // Configure face offsets
  offset_indices::fill_constant_group_size(3, 0, mesh_out->face_offsets_for_write());
  
  // Create an attribute to store the tetrahedron ID for each face
  // Use Blender's attribute API
  bke::MutableAttributeAccessor attributes = mesh_out->attributes_for_write();
  bke::SpanAttributeWriter<int> tet_indices = attributes.lookup_or_add_for_write_span<int>(
      "tetrahedral_index", bke::AttrDomain::Face);
  
  if (tet_indices) {
    tbb::parallel_for(MeshBlockedRange(0, num_faces, 1024),
                     [&](const MeshBlockedRange &range) {
      for (int i = range.begin(); i < range.end(); i++) {
        tet_indices.span[i] = i / 4;  // Integer division to get tetrahedron ID
      }
    });
    tet_indices.finish();
  }
  
  // Validate mesh
  BKE_mesh_validate(mesh_out, true, true);

  // Check mesh visualization
  if (!mesh_out) {
    params.error_message_add(NodeWarningType::Error, "Output mesh was not created correctly");
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
  if (max_size < 0.0001f) max_size = 1.0f;
  
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
  
  // Adjust max_volume to be interpreted as a percentage
  double max_volume_percentage = params.extract_input<float>("Max Volume");
  double max_volume = std::max(0.0001, max_volume_percentage * 0.1); // 0.1 corresponds to 100%
  
  float quality_ratio = std::max(1.0f, params.extract_input<float>("Quality Ratio"));
  
  float min_dihedral_angle = params.extract_input<float>("Min Dihedral Angle");
  
  // Directly retrieve preserve_boundary from input socket
  bool preserve_boundary = params.extract_input<bool>("Preserve Boundary");
  
  // TetGen structures
  tetgenio in, out;
  tetgenbehavior behavior;
  
  // Resource guard to ensure cleanup in case of exception
  TetGenResourceGuard resource_guard(in, out);
  
  Mesh *mesh_out = nullptr;
  bool tetgen_success = false;
  
  try {
    // Prepare input data
    if (prepare_tetgen_input(mesh_in, in, params)) {
      // Configure TetGen options
      configure_tetgen_options(behavior, max_volume, quality_ratio, min_dihedral_angle, preserve_boundary, params);
      
      // Run TetGen
      try {
        tetrahedralize(&behavior, &in, &out);
        
        // Validate output
        if (validate_tetgen_output(out, params)) {
          // Create mesh
          mesh_out = create_tetrahedral_mesh(out, params, max_volume, behavior);
          
          if (mesh_out) {
            tetgen_success = true;
          }
        }
      }
      catch (const std::exception &e) {
        params.error_message_add(NodeWarningType::Error, 
            std::string("TetGen error: ") + e.what());
      }
      catch (...) {
        params.error_message_add(NodeWarningType::Error, 
            "Unknown error while running TetGen");
      }
    }
  }
  catch (const std::exception &e) {
    params.error_message_add(NodeWarningType::Error, 
        std::string("Exception: ") + e.what());
  }
  
  // In case of failure, create a fallback tetrahedron
  if (!tetgen_success) {
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