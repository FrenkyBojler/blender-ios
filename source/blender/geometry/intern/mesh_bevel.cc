/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <iostream>
#include <numeric>
#include <optional>
#include <utility>

#include <fmt/format.h>

#include "BLI_array.hh"
#include "BLI_array_utils.hh"
#include "BLI_atomic_disjoint_set.hh"
#include "BLI_index_mask.hh"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_base.h"
#include "BLI_math_base.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_numbers.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_multi_value_map.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "BKE_attribute.hh"
#include "BKE_curveprofile.h"
#include "BKE_deform.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.hh"

#include "atomic_ops.h"

#include "GEO_mesh_bevel.hh"

#define DEBUG_BEVEL
#ifdef DEBUG_BEVEL
#  include "draw_debug.hh"
#endif

namespace blender::geometry {

/**
 * The un-transformed 2D storage of profile vertex locations. Also, for non-custom profiles
 * this serves as a cache for the results of the expensive calculation of u parameter values to
 * get even spacing on superellipse for current BevelParams seg and pro_super_r.
 */
struct ProfileSpacing {
  /** The profile's seg+1 x values. */
  Array<double> xvals;
  /** The profile's seg+1 y values. */
  Array<double> yvals;
  /** The profile's segments_power_2+1 x values. */
  Array<double> xvals_2;
  /** The profile's segments_power_2+1 y values, (seg_2 = power of 2 >= seg). */
  Array<double> yvals_2;
  /** The power of two greater than or equal to the number of segments. */
  int segments_power_2;
  /** How far "out" the profile is, used at the start of subdivision. */
  float fullness;
};

/** Different kinds of patterns for mesh fragments that will replace bevel-involved vertices. */
enum class MeshKind {
  None,         /* No mesh. */
  Line,         /* A line of connected vertices. */
  TerminalPoly, /* A simple polygon, one side is a terminal beveled edge. */
  Adj,          /* "Adjacent edges" mesh pattern. */
  TriFan,       /* A simple polygon - fan filled. */
  Cutoff,       /* A triangulated face at the end of each profile. */
};

/** Different kinds of vertices in an Adj Mesh. Relevant for attribute assignment. */
enum class AdjVertKind {
  Interior,           /* Unambiguously inside the vertices for a given anchor. */
  CenterLineNear,     /* On center line after anchor (near side, if odd segs). */
  CenterLineFar,      /* On center line after anchor (far side, if odd segs). */
  PrevCenterLineNear, /* Like CenterLineNear, but before anchor. */
  PrevCenterLineFar,  /* Like CenterLineFar, but before anchor. */
  Center,             /* The center vertex (even segs) or in center poly (odd segs). */
};

/** An Array<int> with an inline capacity big enough to hold the number of vertices, etc. */
using SmallIntArray = Array<int, 20>;
class MeshPattern {
 public:
  MeshKind kind;
  int num_anchors;
  int num_segs;

  MeshPattern() : kind(MeshKind::None), num_anchors(0), num_segs(0) {}
  MeshPattern(MeshKind kind, int num_anchors, int num_segs)
      : kind(kind), num_anchors(num_anchors), num_segs(num_segs)
  {
  }

  /** Return a 4-tuple with the number of vertices, edges, faces, corners.  */
  int4 num_elements() const;

  /** Return the vertex index of the give anchor vertex. */
  int anchor_vert(const int anchor_index) const;

  /** Return tne next cyclical anchor index. */
  int next_anchor(const int anchor_index) const
  {
    return anchor_index == num_anchors - 1 ? 0 : anchor_index + 1;
  }

  /** Return the previous cyclic anchor index. */
  int prev_anchor(const int anchor_index) const
  {
    return anchor_index == 0 ? num_anchors - 1 : anchor_index - 1;
  }

  /** Return the anchor index corresponding to the given vertex index,
   * assuming \a v is a vertex that is also an anchor (on the outer boundary).
   */
  int vert_to_anchor(const int v) const;

  /** Return arrays of the verts and edges for pattern face \a face,
   * where \a first_v and \a first_e give the offsets to add to pattern space indices.
   */
  std::pair<SmallIntArray, SmallIntArray> face_verts_and_edges(const int face,
                                                               const int first_v,
                                                               const int first_e) const;

  /** Assuming \a v is a vert on the outer boundary, return the boundary vert that is \a delta away
   * in the ccw direction along the boundary.
   * Assume \a v is offset from 0 by \a first_v, and return a similarly offset answer.
   */
  int next_boundary_vert(const int v, const int ccw_delta, const int first_v) const;

  /** Assuming \a v is a vert on the outer boundary, return the edge that leaves \a v in the ccw
   * direction around the boundary.
   * Assume \a v is offset by \a first_v, and return an edge offset by \a first_e.
   */
  int boundary_edge_from_vert(const int v, const int first_v, const int first_e) const;

  /** Given \a firstpos and \a lastpos, vertex indices in pattern space, and
   * assuming they are on the outer boundary, return arrays of verts and edges that
   * trace the boundary between those positions (inclusive), where \a first_v
   * and \a first_e give the offsets to add to pattern space indices.
   */
  std::pair<SmallIntArray, SmallIntArray> boundary_vert_and_edges(const int firstpos,
                                                                  const int lastpos,
                                                                  const int first_v,
                                                                  const int first_e) const;

  /** Return the face indices (if any) that are unabiguously closested to anchor. */
  SmallIntArray faces_for_anchor(const int anchor) const;

  /** Return the face indices on the center line between first_anchor and the one after that.
   * This will only be non-empty for Adj with odd segments, and excludes the center polygon.
   */
  SmallIntArray faces_for_centerline(const int first_anchor) const;

  /** Return the AdjVertKind for pattern vert \a v relative to anchor \a anchor.
   * Return Interior if not an Adj kind. */
  AdjVertKind adj_vert_kind(const int v, const int anchor) const;

  /** Return the anchor or anchors that own(s) (is nearest to) \a f  (in pattern space).
   * For faces on the mid-strip (when odd segments), return both anchors on either
   * side of the mid-strip, otherwise put -1 in the second component of the answer.
   * For the center polygon (when odd segments), return (-1, -1)..
   */
  int2 face_anchor_owner(const int f) const;
};

/** Helper for keeping track of angle kind. */
enum class AngleKind {
  /** Angle less than 180 degrees. */
  Smaller = -1,
  /** 180 degree angle. */
  Straight = 0,
  /** Angle greater than 180 degrees. */
  Larger = 1,
};

/** Return what range[i+1] would when wrapping around at end. */
static inline int index_range_next(const IndexRange range, const int index)
{
  return (index >= range.size() - 1) ? range.first() : range[index + 1];
}

/** Return what range[i-1] would when wraping around at the beginning. */
static inline int index_range_prev(const IndexRange range, const int index)
{
  return (index <= 0) ? range.last() : range[index - 1];
}

/** Mesh information, including some topology information. */
class MeshInfo {
 public:
  const Mesh &mesh;

  MeshInfo(const Mesh &mesh);

  /** For each Mesh vert v, vert_edges[v] is a Span of edge indices incident on v. */
  GroupedSpan<int> vert_edges() const
  {
    return vert_edges_;
  }

  /** For each Mesh edge e, edge_faces[e] is a Span of face indices incident on e. */
  GroupedSpan<int> edge_faces() const
  {
    return edge_faces_;
  };

  /** For each Mesh vert v, vert_corners[v] is a Span of corner indices incident on v. */
  GroupedSpan<int> vert_corners() const
  {
    return vert_corners_;
  }

  /** For a corner c, return the face containing it. */
  int corner_face(const int c) const
  {
    return corner_to_face_map_[c];
  }

 private:
  index_mask::IndexMaskMemory memory_;
  Array<int> vert_to_edge_offsets_;
  Array<int> vert_to_edge_indices_;
  Array<int> edge_to_face_map_offsets_;
  Array<int> edge_to_face_map_indices_;
  Array<int> vert_to_corner_offsets_;
  Array<int> vert_to_corner_indices_;
  Array<int> corner_to_face_map_;
  GroupedSpan<int> vert_edges_;
  GroupedSpan<int> edge_faces_;
  GroupedSpan<int> vert_corners_;
};

/** Information about a UV map. */
class UVMapInfo {
 public:
  std::string uv_attr_name;
  UVMapInfo(const std::string &uv_attr_name, const Mesh &mesh);

  /** Return the UV Map value at the \a corner. */
  float2 value(const int corner) const
  {
    return values_[corner];
  }

  /** Which connected component is a given Mesh face in?
   * Only valid after find_components() has been called. */
  int component_id(const int face) const
  {
    return face_to_component_id_[face];
  }

  /** Find the connected components (by face) for this UV map. */
  void find_components(const MeshInfo &mesh_info);

 private:
  Array<int> face_to_component_id_;
  Array<float2> values_;
};

/** Bevel parameters and state.
 * A note on some terminology used in the state variables:
 *
 * A "beveled vertex" is, when we are vertex-beveling, a vertex selected by the params
 * and is eligible for beveling (attached to at least two edges).
 * A "beveled edge" is, when we are edge-beveling, an edge selected by the params
 * and is eligibile for beveling (attached to exactly two faces).
 *
 * A bevvert is a "bevel involved vertex". This means:
 *    If we are doing a vertex-only bevel, then it is a beveled vertex.
 *    If we are doing an edge bevel, then it is attached to a beveled edge (not, not a
 *    bevel-involved edge, just a beveled edge).`
 *
 * A bevedge is a "bevel involved edge". This means:
 *    If we are doing a vertex-only bevel, then it is attached to a beveled vertex.
 *    If we are doing an edge bevel, then it is a beveled edge, or another edge
 *    attached to a bevvert (i.e., attached to a vert where a beveled edge is attached).
 *    We can tell which edges are actually beveled because they will have a nonzero
 *    bevel weight (which is a property of bevedges).
 *
 * There is a separate indexing scheme for bevverts and bevedges, each going from zero
 * to the total number of them minus one. There are member functions to go back and
 * forth between the two indexing systems.
 *
 * There are also newverts, newedges, newfaces, and newcorners, all of which will be
 * added to the argument Mesh as part of making the new beveled mesh.
 * During construction, these all have indices starting at zero and going to the number
 * of such elements, minus 1. When we need to mix new and old elements, we offset
 * the new element indices by the size of the corresponding elements in the source Mesh.
 *
 * Both bevverts and bevedges will have their corresponding verts and edges replaced by
 * new mesh fragments, where each fragment is made up of newvertes, newedges, etc.
 * The associated new elements form a particular patterns that, in the case of a bevvert,
 * can have several kinds (see #MeshKind, above).
 */
class BevelState {
 public:
  /** Input parameters. */
  BevelParameters params;
  /** Input Mesh with some topology information.. */
  MeshInfo mesh_info;
  /** Should we attempt to adjust offsets? */
  bool offset_adjust = false;
  /** Number of bevel-involved verts. */
  int bevverts_num = 0;
  /** Number of bevel-involved edges. */
  int bevedges_num = 0;
  /** Number of bevel-involved faces. */
  int bevfaces_num = 0;
  /** Number of new verts. */
  int newverts_num = 0;
  /** Number of new edges. */
  int newedges_num = 0;
  /** Number of new faces. */
  int newfaces_num = 0;
  /** Number of new corners. */
  int newcorners_num = 0;
  /** Parameter values for evenly spaced profile points. */
  ProfileSpacing pro_spacing;
  /** Parameter values for evenly spaced profile points for the miter profiles. */
  ProfileSpacing pro_spacing_miter;
  /** Profile shape parameter converted to a superellipse exponent. */
  float pro_super_r = 2;
  /** How many UV maps are there? */
  int uvmaps_num = 0;
  /** Are there any vertex attributes to be copied (besidies position)? */
  bool any_vert_attributes = false;
  /** Are there any ege attributes to be copied? */
  bool any_edge_attributes = false;
  /** Are there any face attributes to be copied? */
  bool any_face_attributes = false;
  /** Are there any corner attributes (besodes UV maps) to be copied? */
  bool any_corner_attributes = false;

  /** Construct initial state from input. */
  BevelState(const Mesh &src_mesh,
             const IndexMask &selection,
             const BevelParameters &bevel_params);

  void initialize_profile_data();

  void order_bevedges();

  void determine_needed_new_elements();

  void determine_needed_attribute_data();

  void build_vertex_meshes();

  void build_edge_meshes();

  void build_face_meshes();

  /** Build the edge e from vertices v0 and v1, where those indices are in a "new element" space.
   * Negative input values are just copied as is (used to encode original elements).
   */
  void build_newedge(const int e, const int v0, const int v1);

  /** Build the face f from verts and edges, where those indices are in "new" element" space.
   * Negative input elements are just copied as is (used to encode original elements).
   */
  void build_newface(const int f, Span<int> verts, Span<int> edges);

  /** Indices of Mesh vertices that have corresponding bevverts.
   */
  const IndexMask &bevverts_mask() const
  {
    return bevverts_mask_;
  }

  /** Indices of Mesh edges that have corresponding bevedges. */
  const IndexMask &bevedges_mask() const
  {
    return bevedges_mask_;
  }

  /** Indices of Mesh faces that have corresponding bevfaces. */
  const IndexMask &bevfaces_mask() const
  {
    return bevfaces_mask_;
  }

  /** Map from Mesh vertex index to bevverts index. */
  Span<int> vert_bevverts() const
  {
    return vert_bevverts_;
  }

  /** Map from Mesh edge index to bevedges index. */
  Span<int> edge_bevedges() const
  {
    return edge_bevedges_;
  }

  /** Map from Mesh face index to bevfaces index. */
  Span<int> face_bevfaces() const
  {
    return face_bevfaces_;
  }

  /* The following are indexed by a bevvert index, 0 to bevverts_num - 1. */

  /** The Mesh vert associated with a given bevvert. */
  Span<int> bevvert_mesh_verts() const
  {
    return bevvert_mesh_verts_;
  }

  /** The bevedges associated with a given bevvert.
   * The are sorted to be CCW around the vertex, as much as possible. */
  GroupedSpan<int> bevvert_bevedges() const
  {
    return bevvert_bevedges_;
  }

  /** Corresponding to each bevedge in bevvert_bevvedges, this gives
   * the common face between that edge and the next one cyclically,
   * if there is such an edge, else it will be -1. */
  GroupedSpan<int> bevvert_faces() const
  {
    return bevvert_faces_;
  }

  /** What pattern of Mesh will replace the given bevvert. */
  Span<MeshPattern> bevvert_meshpatterns() const
  {
    return bevvert_meshpatterns_;
  }

  /** The range of newvert indices used in the mesh pattern for the given bevvert. */
  OffsetIndices<int> bevvert_newverts() const
  {
    return bevvert_newverts_;
  }

  /** The range of newedge indices used in the mesh pattern for the given bevvert. */
  OffsetIndices<int> bevvert_newedges() const
  {
    return bevvert_newedges_;
  }

  /** The range of newface indices used in the mesh pattern for the given bevvert. */
  OffsetIndices<int> bevvert_newfaces() const
  {
    return bevvert_newfaces_;
  }

  /* The following are indexed by a bevedge index, 0 to bevvedges_num - 1. */

  /** The Mesh edge associated with the given bevedge. */
  Span<int> bevedge_mesh_edges() const
  {
    return bevedge_mesh_edges_;
  }

  /** Return the offset paramenter for bevedge \a be.
   * The \a width_index parameter says which offset is desired: one of four in the order:
   *  [0] left side (looking at vertex) at source end of the mesh edge
   *  [1] right side at source end of the mesh edge
   *  [2] left side of the destination edge of the mesh edge
   *  [3] right side of the destiaation edge of the mesh edge.
   *  If edge beveling, these are perpendicular offsets.
   *  If vertex beveling, only the 0 and 2 indices are used, and the give the amount
   *  to move along the edge away from the vertex at the source and destination edges.
   */
  float bevedge_offset(const int be, const int width_index) const
  {
    return params.offsets[width_index][bevedge_mesh_edges_[be]];
  }

  /** Near and far end indices in the vmesh index space where the given bevedge is attached.
   * If the edge is beveled, this is where the left side of the beveled edge is attached. */
  Span<int2> bevedge_attach_verts() const
  {
    return bevedge_attach_verts_;
  }

  /** The newedges that are needed to form the edge polygons for the given bevvedge. */
  OffsetIndices<int> bevedge_newedges() const
  {
    return bevedge_newedges_;
  }

  /** The newfaces that are  the edge polygons for the given bevvedge. */
  OffsetIndices<int> bevedge_newfaces() const
  {
    return bevedge_newfaces_;
  }

  /* The following are indexed by a bevface index, 0..bevfaces_num - 1. */

  /** The Mesh face associated with the given bevedge. */
  Span<int> bevface_mesh_faces() const
  {
    return bevface_mesh_faces_;
  }

  /** The newface indices used to rebuild a given bevface. Currently, there will be only one per
   * bevface. */
  OffsetIndices<int> bevface_newfaces() const
  {
    return bevface_newfaces_;
  }

  /* The following are indexed by a newvert index, 0 to newverts_num - 1. */

  /** The cooredinates of the given newvert. */
  Span<float3> newvert_positions() const
  {
    return newvert_positions_;
  }

  /** The representative (for attributes) original vertex of the given newvert. */
  Span<int> newvert_repverts() const
  {
    return newvert_repverts_;
  }

  /* The following are indexed by a newedge index, 0 to newedges_num - 1. */

  /** The start and end  indices of vertices for the given newedge. If the indices are in the
   * range of vertices in the source mesh, they come from that; else they are newvert indices
   * offset by the number of vertices in the source mesh. */
  Span<int2> newedge_vertpairs() const
  {
    return newedge_vertpairs_;
  }

  /** The representative (for attributrs) original edge of the given newedge. */
  Span<int> newedge_repedges() const
  {
    return newedge_repedges_;
  }

  /* The following are indexed by a newface index, 0 to newfaces_num - 1. */

  /** Returns a contiguous chunk of face corners, represented as an #IndexRange, like a #Mesh
   * faces(). The corners are newcorner indices. */
  OffsetIndices<int> newface_faces_face() const
  {
    return newface_faces_face_;
  }

  /** The corner offsets for the newfaces faces. Mostly used to construct newface_faces_face(). */
  Span<int> newface_faces_face_offsets() const
  {
    return newface_faces_face_offsets_;
  }

  /** The representative (for attributes) original face of the given newface. */
  Span<int> newface_repfaces() const
  {
    return newface_repfaces_;
  }

  /* The following are indexed by a newcorner index, 0 to newcorners_num - 1. */

  /** Analog of a #Mesh corner_verts() function. The indices are either in the
   * range of vertices of the source mesh, or they are newvert indices offset by
   * the number of vertices in the source mesh. */
  Span<int> newcorner_verts() const
  {
    return newcorner_verts_;
  }

  /** Analog of a #Mesh corner_edges() function. The indices are either in the
   * range of edges of the source mesh, or they are newedge indices offset by
   * the number of edges in the source mesh. */
  Span<int> newcorner_edges() const
  {
    return newcorner_edges_;
  }

  /** The representative (for non-uv attributes) original corners of the given newcorner. */
  Span<int> newcorner_repcorners() const
  {
    return newcorner_repcorners_;
  }

  /** Is the edge corresponding to bevedge \a be beveled? */
  bool bevedge_is_beveled(const int be) const
  {
    return bevedge_is_beveled_[be];
  }

  /** Return the number of beveled eddges attached to \a bv. */
  int bevvert_beveled_edges_num(const int bv) const
  {
    Span<int> bes = bevvert_bevedges_[bv];
    return std::accumulate(bes.begin(), bes.end(), 0, [this](int sum, int be) {
      return sum + bevedge_is_beveled_[be];
    });
  }

  /** Return the next bevedge position around bevvert \a bv after \a edge_pos. */
  int next_edge_pos(const int bv, const int edge_pos) const
  {
    return edge_pos == bevvert_bevedges_[bv].size() - 1 ? 0 : edge_pos + 1;
  }

  /** Return the previous bevedge position around bevvert \a bv before \a edge_pos. */
  int prev_edge_pos(const int bv, const int edge_pos) const
  {
    return edge_pos == 0 ? bevvert_bevedges_[bv].size() - 1 : edge_pos - 1;
  }

  /** Return the next face after the bevedge in position \a edge_pos of bevvert \a bv. */
  int face_next(const int bv, const int edge_pos) const
  {
    return bevvert_faces_[bv][edge_pos];
  }

  /** Return the previous face before the bevedge in position \a edge_pos of bevvert \a bv. */
  int face_prev(const int bv, const int edge_pos) const
  {
    return bevvert_faces_[bv][prev_edge_pos(bv, edge_pos)];
  }

  /** Return the left and right anchors for the edge at position \a edge_pos of bevvert \a bv.
   * These will be the same if the edge isn't beveled. If it is beveled, the first  of the pair is
   * the anchor position where the left side of the edge (looking towards bv) is attached and the
   * second of the pair is where the right end is atttached. */
  int2 bevedge_anchors(const int bv, const int edge_pos) const;

  /** Return the edge positions of the first and last edges attached at anchor position \a anchor.
   * Return (-1, -1) if no edge is attached there. */
  int2 anchor_bevedge_positions(const int bv, const int anchor) const;

  /** Return the end of the bevedge \a be edge that the bevvert \a bv is
   * on: 0 if it is at the start of the edge, 1 if it is at the end.
   */
  int bevedge_vert_end(const int bv, const int be) const
  {
    return bevvert_mesh_verts_[bv] == mesh_info.mesh.edges()[bevedge_mesh_edges_[be]][0] ? 0 : 1;
  }

  /** Return the two bevverts associated with bevedge \a be. Near end first, then far end. */
  int2 bevedge_bevverts(const int be) const
  {
    const int2 vs = mesh_info.mesh.edges()[bevedge_mesh_edges_[be]];
    return int2(vert_bevverts_[vs[0]], vert_bevverts_[vs[1]]);
  }

  /** Return the bevedge position of the last bevedge attached to newvert, or -1 if none. */
  int last_attached_bevedge_pos(const int bv, const int newvert) const;

  /** Return the edge position for bevedge \a be around bevvert \a bv. */
  int bevedge_pos(const int be, const int bv) const;

  /** Return the UVMapInfo corresponding to the UV map with index \a uv_map_index. */
  const UVMapInfo &uv_map_info(const int uv_map_index) const
  {
    return uv_map_infos_[uv_map_index];
  }

  /** Return the newly calculated UV values for newcorners.
   *. The outer Vector is indexed by the uv map index,
   * and the inner Array is indexed by a newcorner index. */
  const Vector<Array<float2>> &uv_attributes() const
  {
    return uv_attributes_;
  }

  /** Return the index of the UV map with the given attribute name, or -1 if not found. */
  int find_uv_map_index(const std::string &attr_name) const
  {
    for (const int i : uv_map_infos_.index_range()) {
      if (uv_map_infos_[i].uv_attr_name == attr_name) {
        return i;
      }
    }
    return -1;
  }

  /** Return the material index for face \a f. */
  int material(const int f) const
  {
    return materials_.size() > f ? materials_[f] : -1;
  }

 private:
  IndexMaskMemory memory_;
  IndexMask bevedges_mask_;
  IndexMask bevverts_mask_;
  IndexMask bevfaces_mask_;
  Array<int> vert_bevverts_;
  Array<int> edge_bevedges_;
  Array<int> face_bevfaces_;

  Array<int> bevvert_mesh_verts_;
  Array<bool> bevvert_is_beveled_;
  GroupedSpan<int> bevvert_bevedges_;
  Array<int> bevvert_bevedges_offsets_;
  Array<int> bevvert_bevedges_indices_;
  GroupedSpan<int> bevvert_faces_;
  Array<int> bevvert_faces_indices_;
  Array<MeshPattern> bevvert_meshpatterns_;
  OffsetIndices<int> bevvert_newverts_;
  OffsetIndices<int> bevvert_newedges_;
  OffsetIndices<int> bevvert_newfaces_;

  Array<int> bevedge_mesh_edges_;
  Array<bool> bevedge_is_beveled_;
  Array<int2> bevedge_attach_verts_;
  OffsetIndices<int> bevedge_newedges_;
  OffsetIndices<int> bevedge_newfaces_;

  Array<int> bevface_mesh_faces_;
  OffsetIndices<int> bevface_newfaces_;

  Array<int> newverts_offsets_;
  Array<float3> newvert_positions_;
  Array<int> newvert_repverts_;

  Array<int> newedges_offsets_;
  Array<int2> newedge_vertpairs_;
  Array<int> newedge_repedges_;

  Array<int> newfaces_offsets_;
  OffsetIndices<int> newface_faces_;
  Array<int> newface_faces_face_offsets_;
  OffsetIndices<int> newface_faces_face_;
  Array<int> newface_repfaces_;

  Array<int> newcorner_verts_;
  Array<int> newcorner_edges_;
  Array<int> newcorner_repcorners_;

  Vector<UVMapInfo> uv_map_infos_;
  Vector<Array<float2>> uv_attributes_;

  Array<int> materials_;
};

/** Construct a MeshInfo from a given Mesh.  This mostly involves calculating some topology maps.
 */
MeshInfo::MeshInfo(const Mesh &mesh) : mesh(mesh)
{
  vert_edges_ = bke::mesh::build_vert_to_edge_map(
      mesh.edges(), mesh.verts_num, vert_to_edge_offsets_, vert_to_edge_indices_);
  edge_faces_ = bke::mesh::build_edge_to_face_map(mesh.faces(),
                                                  mesh.corner_edges(),
                                                  mesh.edges_num,
                                                  edge_to_face_map_indices_,
                                                  edge_to_face_map_offsets_);
  vert_corners_ = bke::mesh::build_vert_to_corner_map(
      mesh.corner_verts(), mesh.verts_num, vert_to_corner_offsets_, vert_to_corner_indices_);
  corner_to_face_map_ = bke::mesh::build_corner_to_face_map(mesh.faces());
}

/** Some Mesh utility functions. */

/** Return the two edges incident on vertex \a v in face \a f in \a mesh. */
static int2 get_incident_edges(int f, int v, const Mesh &mesh)
{
  int2 ans(-1, -1);
  const int corner = bke::mesh::face_find_corner_from_vert(
      mesh.faces()[f], mesh.corner_verts(), v);
  ans[0] = mesh.corner_edges()[corner];
  const int corner_prev = bke::mesh::face_corner_prev(mesh.faces()[f], corner);
  ans[1] = mesh.corner_edges()[corner_prev];
  return ans;
}

/** Functions for debug printing. */

template<typename T> static void print_span(Span<T> span, const char *label)
{
  if (span.size() == 0) {
    return;
  }
  fmt::print("{}:", label);
  for (const int i : span.index_range()) {
    if (i % 10 == 0) {
      fmt::print("\n[{}] ", i);
    }
    fmt::print("{} ", span[i]);
  }
  fmt::println("");
}

static void print_float3(const float3 &v)
{
  fmt::print("({},{},{})", v[0], v[1], v[2]);
}

static void print_float3_span(Span<float3> span, const char *label)
{
  if (span.size() == 0) {
    return;
  }
  fmt::print("{}:", label);
  for (const int i : span.index_range()) {
    if (i % 10 == 0) {
      fmt::print("\n[{}] ", i);
    }
    print_float3(span[i]);
    fmt::print(" ");
  }
  fmt::println("");
}

static void print_int2(const int2 pair)
{
  fmt::print("({},{})", pair[0], pair[1]);
}

static void print_int2_span(Span<int2> span, const char *label)
{
  if (span.size() == 0) {
    return;
  }
  fmt::print("{}:", label);
  for (const int i : span.index_range()) {
    if (i % 10 == 0) {
      fmt::print("\n[{}] ", i);
    }
    print_int2(span[i]);
    fmt::print(" ");
  }
  fmt::println("");
}

[[maybe_unused]] static void print_int3(const int3 triple)
{
  fmt::print("({},{},{})", triple[0], triple[1], triple[2]);
}

[[maybe_unused]] static void print_int4(const int4 quad)
{
  fmt::print("({},{},{},{})", quad[0], quad[1], quad[2], quad[3]);
}

static void print_indexrange(const IndexRange &range)
{
  if (range.size() == 0) {
    fmt::print("[]");
  }
  else {
    fmt::print("[{}..{}]", range.first(), range.last());
  }
}

static void print_indexmask(const IndexMask &index_mask, const char *label)
{
  if (index_mask.size() == 0) {
    return;
  }
  fmt::print("{}:", label);
  index_mask.foreach_index([&](const int v, const int mask) {
    if (mask % 10 == 0) {
      fmt::print("\n[{}] ", mask);
    }
    fmt::print("{} ", v);
  });
  fmt::println("");
}

static void print_groupedspan(const GroupedSpan<int> &groupedspan, const char *label)
{
  if (groupedspan.size() == 0) {
    return;
  }
  fmt::println("{}:", label);
  for (const int i : groupedspan.index_range()) {
    fmt::print("[{}] ", i);
    for (int v : groupedspan[i]) {
      fmt::print("{} ", v);
    }
    fmt::println("");
  }
}

static void print_offsetindices(const OffsetIndices<int> &offsetindices, const char *label)
{
  if (offsetindices.size() == 0) {
    return;
  }
  fmt::print("{}", label);
  for (const int i : offsetindices.index_range()) {
    if (i % 10 == 0) {
      fmt::print("\n[{}] ", i);
    }
    print_indexrange(offsetindices[i]);
    fmt::print(" ");
  }
  fmt::println("");
}

static void print_meshpattern(const MeshPattern &pat)
{
  static const char *kind_names[] = {"None", "Line", "TerminalPoly", "Adj", "TriFan", "Cutoff"};
  fmt::println("{} anchors={} segs={}", kind_names[int(pat.kind)], pat.num_anchors, pat.num_segs);
}

static void print_anchor_newvert_positions(const BevelState &bs, const char *label)
{
  if (bs.newvert_positions().size() == 0) {
    return;
  }
  fmt::println("{}", label);
  for (const int bv : IndexRange(bs.bevverts_num)) {
    const MeshPattern &pat = bs.bevvert_meshpatterns()[bv];
    fmt::print("bv {}:", bv);
    for (const int a : IndexRange(pat.num_anchors)) {
      const int apos = pat.anchor_vert(a);
      const int nv_index = bs.bevvert_newverts()[bv][apos];
      float3 co = bs.newvert_positions()[nv_index];
      fmt::print(" [{}]({}.{},{})", apos, co[0], co[1], co[2]);
    }
    fmt::println("");
  }
}

[[maybe_unused]] static void dump_bevel_state(const BevelState &bs, const char *label)
{
  fmt::println("\nBevelState {}", label);
  print_indexmask(bs.bevverts_mask(), "bevverts_mask");
  print_indexmask(bs.bevedges_mask(), "bevedges_mask");
  print_indexmask(bs.bevfaces_mask(), "bevfaces_mask");
  print_span(bs.vert_bevverts(), "vert_bevverts");
  print_span(bs.edge_bevedges(), "edge_bevedges");
  print_span(bs.face_bevfaces(), "face_bevfaces");
  print_span(bs.bevvert_mesh_verts(), "bevvert_mesh_verts");
  print_span(bs.bevface_mesh_faces(), "bevface_mesh_faces");
  print_groupedspan(bs.bevvert_bevedges(), "bevvert_bevedges");
  print_groupedspan(bs.bevvert_faces(), "bevvert_faces");
  print_int2_span(bs.bevedge_attach_verts(), "bevedge_attach_verts");
  print_anchor_newvert_positions(bs, "anchor newvert positions");
  if (bs.bevvert_meshpatterns().size() > 0) {
    fmt::println("meshpatterns");
    for (const int bv : bs.bevvert_meshpatterns().index_range()) {
      fmt::print("{}: ", bv);
      print_meshpattern(bs.bevvert_meshpatterns()[bv]);
    }
  }
  print_offsetindices(bs.bevvert_newverts(), "bevvert_newverts");
  print_offsetindices(bs.bevvert_newedges(), "bevvert_newedges");
  print_offsetindices(bs.bevvert_newfaces(), "bevvert_newfaces");
  print_offsetindices(bs.bevedge_newedges(), "bevedge_newedges");
  print_offsetindices(bs.bevedge_newfaces(), "bevedge_newfaces");
  print_offsetindices(bs.bevface_newfaces(), "bevface_newfaces");
  print_offsetindices(bs.newface_faces_face(), "newface_faces_face");
  print_float3_span(bs.newvert_positions(), "newvert_positions");
  print_int2_span(bs.newedge_vertpairs(), "newedge_vertpairs");
  print_span(bs.newcorner_verts(), "newcorner_verts");
  print_span(bs.newcorner_edges(), "newcorner_edges");
}

[[maybe_unused]] static void dump_bevvert(const int bv, const BevelState &bs)
{
  fmt::println("Bevvert {} for Mesh vert {}", bv, bs.bevvert_mesh_verts()[bv]);
  if (bs.bevvert_meshpatterns().size() > bv) {
    print_meshpattern(bs.bevvert_meshpatterns()[bv]);
  }
  Span<int> bevedges = bs.bevvert_bevedges()[bv];
  Span<int> faces = bs.bevvert_faces()[bv];
  fmt::println("Edges");
  for (const int epos : bevedges.index_range()) {
    const int be = bevedges[epos];
    const int be_end = bs.bevedge_vert_end(bv, be);
    const int mesh_e = bs.bevedge_mesh_edges()[be];
    fmt::print("{}: ", epos);
    fmt::println(" be={}, meshe={}, end={}", be, mesh_e, be_end);
    if (bs.bevedge_is_beveled(be)) {
      fmt::println(" BEV offsets=({},{})",
                   bs.bevedge_offset(be, 2 * be_end),
                   bs.bevedge_offset(be, 2 * be_end + 1));
    }
    fmt::println(
        " face_next={} attch_vert={}", faces[epos], bs.bevedge_attach_verts()[be][be_end]);
  }
  print_span(bevedges, "bevedges");
  print_span(bs.bevvert_faces()[bv], "faces");
}

/** For debugging: draws some colored "points" on the screen to help visualize the state of a
 * bevvert. The original vertex is drawn in orig_vert_col (red). The first anchor (boundary point)
 * is drawn in first_bndv_col (blue), and the rest of them are drawn in bndv_col (blue). These
 * points will persist on the screen as long as Blender is running.
 */
static void draw_bevvert(int bv, const BevelState &bs)
{
  constexpr uint life = draw::drw_debug_persistent_lifetime;
  constexpr float pntsize = 0.04f;
  const float4 orig_vert_col = {1, 0, 0, 1};
  const float4 first_bndv_col = {0, 0, 1, 1};
  const float4 bndv_col = {0, 1, 0, 1};
  const MeshPattern &pat = bs.bevvert_meshpatterns()[bv];
  float3 bevvert_co = bs.mesh_info.mesh.vert_positions()[bs.bevvert_mesh_verts()[bv]];
  draw::drw_debug_point(bevvert_co, pntsize, orig_vert_col, life);
  for (const int a : IndexRange(pat.num_anchors)) {
    const int apos = pat.anchor_vert(a);
    const int nv_index = bs.bevvert_newverts()[bv][apos];
    float3 co = bs.newvert_positions()[nv_index];
    draw::drw_debug_point(co, pntsize, a == 0 ? first_bndv_col : bndv_col, life);
  }
}

/** For debugging: call draw_bevvert() (see above) on all bevverts. */
[[maybe_unused]] static void draw_all_bevverts(const BevelState &bs)
{
  for (const int bv : IndexRange(bs.bevverts_num)) {
    draw_bevvert(bv, bs);
  }
}

namespace geom {
/** Functions and data for geometric calculations. */

constexpr double bevel_epsilon_d = 1e-6;
// constexpr float bevel_epsilon = 1e-6f;
constexpr float bevel_epsilon_sq = 1e-12f;
constexpr float bevel_epsilon_big = 1e-4f;
// constexpr float bevel_epsilon_big_sq = 1e-8f;
constexpr float bevel_epsilon_ang = DEG2RADF(2.0f);
constexpr float bevel_small_ang = DEG2RADF(10.0f);
/** Difference in dot products that corresponds to 10 degree difference between vectors. */
const float bevel_small_ang_dot = (1.0f - math::cos(bevel_small_ang));
/** Difference in dot products that corresponds to 2.0 degree difference between vectors. */
const float bevel_epsilon_ang_dot = (1.0f - cosf(bevel_epsilon_ang));
// constexpr float bevel_max_adjust_pct = 10.0f;
// constexpr float bevel_max_auto_adjust_pct = 300.0f;
// constexpr double bevel_match_spec_weight = 0.2;

/** Return the direction of the edge leaving bevvert \a bv and position \a epos. */
static float3 edge_dir(const int bv, const int epos, const BevelState &bs)
{
  const Mesh &mesh = bs.mesh_info.mesh;
  int be = bs.bevvert_bevedges()[bv][epos];
  int2 e = mesh.edges()[bs.bevedge_mesh_edges()[be]];
  float3 dir = mesh.vert_positions()[e[1]] - mesh.vert_positions()[e[0]];
  return bs.bevedge_vert_end(bv, be) == 0 ? dir : -dir;
}

/** Return the length of the edge for bevvert \a bv and position \a epos. */
[[maybe_unused]] static float edge_length(const int bv, const int epos, const BevelState &bs)
{
  return math::length(edge_dir(bv, epos, bs));
}

/** Return the angle going counterclockwise when viewed from the vertex normal side,
 * from the edge in position \a e1_pos to the edge in position \a e2_pos of bevedges
 * connected to Bevvert \a bv.
 */
static float angle_between_edges(const int bv,
                                 const int e1_pos,
                                 const int e2_pos,
                                 const BevelState &bs)
{
  const Mesh &mesh = bs.mesh_info.mesh;
  const int v = bs.bevvert_mesh_verts()[bv];
  float3 dir1 = math::normalize(edge_dir(bv, e1_pos, bs));
  float3 dir2 = math::normalize(edge_dir(bv, e2_pos, bs));
  float ang = angle_normalized_v3v3(dir1, dir2);
  /* Angles are in [0,pi]. Need to compare cross product with normal to see if it is reflex. */
  const int face_next = bs.face_next(bv, e1_pos);
  const int face_prev = bs.face_prev(bv, e2_pos);
  float3 normal = face_next != -1 ?
                      mesh.face_normals()[face_next] :
                      (face_prev != -1 ? mesh.face_normals()[face_prev] : mesh.vert_normals()[v]);
  if (math::dot(math::cross(dir1, dir2), normal) < 0.0f) {
    /* Angle is reflex. */
    ang = 2 * math::numbers::pi - ang;
  }
  return ang;
}

/**
 * \return True if d1 and d2 are parallel or nearly parallel.
 */
static bool nearly_parallel_normalized(const float3 d1, const float3 d2)
{
  return compare_ff(math::abs(math::dot(d1, d2)), 1.0f, bevel_epsilon_ang_dot);
}

/** Return whether the angle is less than, equal to, or larger than 180 degrees when viewed from
 * the positive normal side of faces (if available) or vertex. */
[[maybe_unused]] static AngleKind edges_angle_kind(const int bv,
                                                   const int e1_pos,
                                                   const int e2_pos,
                                                   const BevelState &bs)
{
  float ang = angle_between_edges(bv, e1_pos, e2_pos, bs);
  if (math::abs(ang - math::numbers::pi) < bevel_epsilon_ang) {
    return AngleKind::Straight;
  }
  else if (ang < math::numbers::pi) {
    return AngleKind::Smaller;
  }
  return AngleKind::Larger;
}

/** Return the angle between the two faces adjacent to the bevedge that
 * is at position \a edge_pos of bevvert \a bv.
 * If there are not two, return 0. */
[[maybe_unused]] static float edge_face_angle(const int bv,
                                              const int edge_pos,
                                              const BevelState &bs)
{
  const int face_next = bs.face_next(bv, edge_pos);
  const int face_prev = bs.face_prev(bv, edge_pos);
  if (face_prev == -1 || face_next == -1) {
    return 0.0f;
  }
  const float3 &norm_prev = bs.mesh_info.mesh.face_normals()[face_prev];
  const float3 &norm_next = bs.mesh_info.mesh.face_normals()[face_next];
  return math::numbers::pi - angle_normalized_v3v3(norm_prev, norm_next);
}

/** Find the center axis of the given bevel vert, for use in vertex beveling.
 * Note; Don't use the vertex normal -- it can give unwanted results.
 */
[[maybe_unused]] static float3 bevvert_axis(const int bv, const BevelState &bs)
{
  float3 vert_axis(0.0f, 0.0f, 0.0f);
  Span<int> edges = bs.bevvert_bevedges()[bv];
  for (const int be : edges) {
    float3 edir = edge_dir(bv, be, bs);
    if (bs.bevedge_vert_end(bv, be) == 0) {
      edir = -edir;
    }
    vert_axis += math::normalize(edir);
  }
  return vert_axis;
}

/** Assuming \a co is on the edge defined by \a be and \a epos,
 * find out if is on or between those two vertices. If it is, return -1.
 * Otherwise return the index of the vertex that is the end that is nearer \a co.
 */
static bool is_outside_edge(
    const int bv, const int epos, const float3 co, const BevelState &bs, float3 *r_closer)
{
  const Mesh &mesh = bs.mesh_info.mesh;
  int be = bs.bevvert_bevedges()[bv][epos];
  int2 edge = mesh.edges()[bs.bevedge_mesh_edges()[be]];
  float3 l1 = mesh.vert_positions()[edge[0]];
  float3 l2 = mesh.vert_positions()[edge[1]];
  float lenu;
  float3 u = math::normalize_and_get_length(l2 - l1, lenu);
  float lambda = math::dot(u, co - l1);
  if (lambda <= -bevel_epsilon_big * lenu) {
    *r_closer = l1;
    return true;
  }
  if (lambda > (1.0 + bevel_epsilon_big) * lenu) {
    *r_closer = l2;
    return true;
  }
  return false;
}

/** \a co should be approximately on the plane between the edges in positions \a e1_pos
 * and \a e2_pos at bevvert \a bv. They should share face \a face (which cannot be -1).
 * Is it between those edges, sweeping CCW?
 */
static bool point_between_edges(const int bv,
                                const int e1_pos,
                                const int e2_pos,
                                const float3 co,
                                const int face,
                                const BevelState &bs)
{
  BLI_assert(face != -1);
  const Mesh &mesh = bs.mesh_info.mesh;
  float3 dir1 = -math::normalize(edge_dir(bv, e1_pos, bs));
  float3 dir2 = -math::normalize(edge_dir(bv, e2_pos, bs));
  const int v = bs.bevvert_mesh_verts()[bv];
  float3 dirco = math::normalize(mesh.vert_positions()[v] - co);

  float ang11 = angle_normalized_v3v3(dir1, dir2);
  float ang1co = angle_normalized_v3v3(dir1, dirco);
  /* Angles are in [0,pi]. Need to compare cross product with normal to see if they are reflex.
   */
  float3 norm = math::cross(dir1, dir2);
  float3 face_norm = mesh.face_normals()[face];
  if (math::dot(norm, face_norm) < 0.0f) {
    ang11 = float(math::numbers::pi * 2.0) - ang11;
  }
  norm = math::cross(dir1, dirco);
  if (math::dot(norm, face_norm) < 0.0f) {
    ang1co = float(math::numbers::pi * 2.0) - ang1co;
  }
  return (ang11 - ang1co > -bevel_epsilon_ang);
}

/** Return coordinates of a point a distance \a dist along the edge specified by \a bv and \a
 * epos. However, clamp it to be no more than the length of the edge.
 */
static float3 slide_dist(const int bv, const int epos, const float dist, const BevelState &bs)
{
  float len;
  float3 normalized_dir = math::normalize_and_get_length(edge_dir(bv, epos, bs), len);
  float d = dist;
  if (d >= len) {
    /* Don't go quite all the way to the other end. */
    d = len - float(50.0 - bevel_epsilon_d);
  }
  int origin_v = bs.bevvert_mesh_verts()[bv];
  return bs.mesh_info.mesh.vert_positions()[origin_v] + d * normalized_dir;
}

/** Return the coordinates of the offset by \a dist of the origin end of the edge specified
 * by \a bv and \a epos,
 * on the left side if \a left is true, else the right side.
 * Do the offset in the plane that is on that side if it exists, else choose an arbitrary
 * plane normal.
 */
static float3 offset_bevedge(
    const int bv, const int epos, const float dist, bool left, const BevelState &bs)
{
  const Mesh &mesh = bs.mesh_info.mesh;
  float3 dir = math::normalize(edge_dir(bv, epos, bs));
  float3 normal;
  int face = left ? bs.face_prev(bv, epos) : bs.face_next(bv, epos);
  if (face != -1) {
    normal = mesh.face_normals()[face];
  }
  else {
    /* An arbitrary direction not the same as dir. */
    if (math::abs(dir[0]) < math::abs(dir[1])) {
      normal = {1.0f, 0.0f, 0.0f};
    }
    else {
      normal = {0.0f, 1.0f, 0.0f};
    }
  }
  float3 fdir = math::normalize(left ? math::cross(dir, normal) : math::cross(normal, dir));
  int origin_v = bs.bevvert_mesh_verts()[bv];
  return mesh.vert_positions()[origin_v] + dist * fdir;
}

/** When the offset type is Percent or Absolute, fill in the coordinates
 * of the lines whose intersection defines the boundary point between e1 and e2 with common
 * vert v, as defined in the parameters of offset_meet.
 */
[[maybe_unused]] static void offset_meet_lines_percent_or_absolute(const int /*bv*/,
                                                                   const int /*e1_pos*/,
                                                                   const int /*e2_pos*/,
                                                                   MutableSpan<float3> /*pts*/,
                                                                   const BevelState & /*bs*/)
{
  /* Get points the specified distance along each leg.
   * The legs we need are:
   *   e0 : the next edge around e1->fnext (==f1) after e1.
   *   e3 : the prev edge around e2->fprev (==f2) before e2.
   *   e4 : the previous edge around f1 before e1 (may be e2).
   *   e5 : the next edge around f2 after e2 (may be e1).
   */
  /* TODO */
  BLI_assert(false);
}

/** Return the meeting point between the offset edges for edges at positions \a e1_pos and \a
 * e2_pos. Do this in \a face, if that is not -1. Offset edge is on right of both edges, where e1
 * enters bv and e2 leave it. When offsets are equal, the new point is on the edge bisector, with
 * length offset/sin(angle/2), but if the offsets are not equal (we allow for because the bevel
 * modifier has edge weights that may lead to different offsets) then the meeting point can be
 * found by intersecting offset lines.
 *
 * \a edges_between: If this is true, there are edges between e1 and e2 in CCW order so they
 * do not share a common face. We want the meeting point to be on an existing face so it
 * should be dropped onto one of the intermediate faces, if possible.
 * \a e_in_plane: If we need to drop from the calculated offset lines to one of the faces,
 * we do not want to drop onto the 'in plane' face, so if this is not -1 skip this edge's faces.
 */
static float3 offset_meet(const int bv,
                          const int e1_pos,
                          const int e2_pos,
                          const float e1_spec_r,
                          const float e2_spec_l,
                          const int face,
                          const bool edges_between,
                          const int e_in_plane_pos,
                          const BevelState &bs)
{
  const Mesh &mesh = bs.mesh_info.mesh;
  const int v = bs.bevvert_mesh_verts()[bv];
  const float3 bevvert_pos = mesh.vert_positions()[v];
  /* Get direction along e1 into bv and along e2 from bv. */
  const float3 dir1 = -edge_dir(bv, e1_pos, bs);
  const float3 dir2 = edge_dir(bv, e2_pos, bs);
  Span<int> bevedges = bs.bevvert_bevedges()[bv];
  const int e1_next_pos = index_range_next(bevedges.index_range(), e1_pos);
  const int e2_prev_pos = index_range_prev(bevedges.index_range(), e2_pos);
  const float3 dir1n = -edge_dir(bv, e1_next_pos, bs);
  const float3 dir2p = edge_dir(bv, e2_prev_pos, bs);
  float3 vert_norm = mesh.vert_normals()[v];
  float ang = angle_v3v3(dir1, dir2);
  if (ang < bevel_epsilon_ang) {
    /* Special case: e1 and e2 are parallel, or nearly so;
     * put offset point perp to both, from bevvert.v on a suitable plane.
     * This code used to just use offset and dir1, but that makes for visible errors
     * on a circle with > 200 sides, which trips this "nearly perp" code (see #61214).
     * so use the average of the two, and the offset formula for angle bisector.
     * If offsets are different, we're out of luck:
     * Use the max of the two (so get consistent looking results if the same situation
     * arises elsewhere in the object but with opposite roles for e1 and e2.
     */
    float3 norm_v;
    if (face != -1) {
      norm_v = mesh.face_normals()[face];
    }
    else {
      /* Get average of face normals between e and e2. */
      norm_v = {0.0f, 0.0f, 0.0f};
      int fcount = 0;
      for (int i = e1_pos; i != e2_pos; i = (i + 1) % bevedges.size()) {
        const int face = bs.face_next(bv, i);
        if (face != -1) {
          norm_v += mesh.face_normals()[face];
          fcount++;
        }
      }
      if (fcount == 0) {
        norm_v = vert_norm;
      }
      else {
        norm_v = norm_v / float(fcount);
      }
    }
    float3 norm_perp1 = math::normalize(math::cross(dir1 + dir2, norm_v));
    float d = math::max(e1_spec_r, e2_spec_l) / math::cos(ang / 2.0);
    return bevvert_pos + d * norm_perp1;
  }
  if (math::abs(ang - math::numbers::pi) < bevel_epsilon_ang) {
    /* Special case: e1 and e2 are anti-parallel, so bevel is into a zero-area face.
     * Just make the offset point on the common line, at offset distance from v.
     */
    return slide_dist(bv, e2_pos, math::max(e1_spec_r, e2_spec_l), bs);
  }
  /* Get normal to plane where meet point should be, using cross product instead of
   * face's normal, in case f is non-planar.
   * Except: sometimes locally there can be a small angle between dir1 and dir2 that leads
   * to a normal that is actually almost perpendicular to the face normal;
   * in this case it looks wrong to use the local (cross-product) normal, so use the face normal
   * if the angle between dir1 and dir2 is smallish.
   * If e1-v-e2 is a reflex angle (viewed from vertex normal side), need to flip.
   * Use face's normal to figure out which side to look at angle from, as even if face is
   * non-planar, this will be more accurate than the vertex normal.
   */
  float3 norm_v1;
  float3 norm_v2;
  if (face != -1 && ang < bevel_small_ang) {
    norm_v1 = mesh.face_normals()[face];
    norm_v2 = norm_v1;
  }
  else if (!edges_between) {
    norm_v1 = math::normalize(math::cross(dir2, dir1));
    if (math::dot(norm_v1, face != -1 ? mesh.face_normals()[face] : vert_norm) < 0.0f) {
      norm_v1 = -norm_v1;
    }
    norm_v2 = norm_v1;
  }
  else {
    /* Separate faces; get face norms at corners for each separately. */
    norm_v1 = math::normalize(math::cross(dir1n, dir1));
    int f = bs.face_next(bv, e1_pos);
    if (math::dot(norm_v1, f != -1 ? mesh.face_normals()[f] : vert_norm) < 0.0f) {
      norm_v1 = -norm_v1;
    }
    norm_v2 = math::normalize(math::cross(dir2, dir2p));
    f = bs.face_prev(bv, e2_pos);
    if (math::dot(norm_v2, f != -1 ? mesh.face_normals()[f] : vert_norm) < 0.0f) {
      norm_v2 = -norm_v2;
    }
  }
  /* Get vectors perp to each edge, perp to norm_v, and pointing into face. */
  float3 norm_perp1 = math::normalize(math::cross(dir1, norm_v1));
  float3 norm_perp2 = math::normalize(math::cross(dir2, norm_v2));
  /* Get points on two lines to intersect in order to find the meet point. */
  Array<float3, 4> off_pts(4);
  off_pts[0] = bevvert_pos + e1_spec_r * norm_perp1;
  off_pts[1] = off_pts[0] + dir1;
  off_pts[2] = bevvert_pos + e2_spec_l * norm_perp2;
  off_pts[3] = off_pts[2] + dir2;

  /* Intersect the offset lines. */
  float3 meetco;
  float3 isect2;
  int isect_kind = isect_line_line_v3(
      off_pts[0], off_pts[1], off_pts[2], off_pts[3], meetco, isect2);
  if (isect_kind == 0) {
    /* Lines are collinear: we already tested for this, but this used a different epsilon. */
    return off_pts[0]; /* Just do something. */
  }
  /* The lines intersect, but is it at a reasonable place?
   * One problem to check: if one of the offsets is 0, then we don't want an intersection
   * that is outside that edge itself. This can happen if angle between them is > 180 degrees,
   * or if the offset amount is > the edge length.
   */
  float3 vcloser;
  if (e1_spec_r == 0.0f && is_outside_edge(bv, e1_pos, meetco, bs, &vcloser)) {
    meetco = vcloser;
  }
  if (e2_spec_l == 0.0f && is_outside_edge(bv, e2_pos, meetco, bs, &vcloser)) {
    meetco = vcloser;
  }
  if (edges_between && e1_spec_r > 0.0f && e2_spec_l > 0.0f) {
    /* Try to drop meetco to a face between e1 and e2. */
    if (isect_kind == 2) {
      /* Lines didn't meet in 3d: get average of meetco and isect2. */
      mid_v3_v3v3(meetco, meetco, isect2);
    }

    for (int i = e1_pos; i != e2_pos; i = (i + 1) % bevedges.size()) {
      const int fnext = bs.face_next(bv, i);
      if (fnext == -1) {
        continue;
      }
      const float3 fnext_norm = mesh.face_normals()[fnext];
      float4 plane;
      plane_from_point_normal_v3(plane, bevvert_pos, fnext_norm);
      float3 dropco;
      closest_to_plane_normalized_v3(dropco, plane, meetco);
      /* Don't drop to the faces next to the in plane edge. */
      if (e_in_plane_pos != -1) {
        const int face = bs.face_next(bv, e_in_plane_pos);
        if (face != -1) {
          ang = angle_v3v3(fnext_norm, mesh.face_normals()[face]);
          if (math::abs(ang) < bevel_small_ang ||
              math::abs(ang - math::numbers::pi) < bevel_small_ang)
          {
            continue;
          }
        }
      }
      if (point_between_edges(bv, i, (i + 1) % bevedges.size(), dropco, fnext, bs)) {
        meetco = dropco;
        break;
      }
    }
  }

  return meetco;
}

/* This was changed from 0.25f to fix bug #86768.
 * Original bug #44961 remains fixed with this value.
 * Update: changed again from 0.0001f to fix bug #95335.
 * Original two bugs remained fixed.
 */
const float good_angle = 0.1f;

/** Calculate the meeting point between the right offset edge  of the edges a position \a
 * e1_pos and the left offset edge of the edge at position \a e2_pos, where \a e1 precedes \a
 * e2 in CCW order. Assume that at most of the specified widths is non-zero. It is possible
 * that no such meeting point exists (if the angle between is reflex), in which case false is
 * returned.  It is also possible that the angle is so close to a straight angle that the
 * position of the meet point would be a big spike, so return false in that case too.
 * Otherwise, return the meeting point in *r_co and the angle between in *r_angle and return
 * true.
 */
static bool try_offset_meet_edge(const int bv,
                                 const int e1_pos,
                                 const int e2_pos,
                                 const float e1_spec_r,
                                 const float e2_spec_l,
                                 float3 *r_co,
                                 float *r_angle,
                                 const BevelState &bs)
{
  BLI_assert(e1_spec_r == 0.0f || e2_spec_l == 0.0f);
  float ang = angle_between_edges(bv, e1_pos, e2_pos, bs);
  if (r_angle) {
    *r_angle = ang;
  }
  if (math::abs(ang) < good_angle || math::numbers::pi - ang < good_angle) {
    return false;
  }
  if (r_co == nullptr) {
    return true;
  }
  const Mesh &mesh = bs.mesh_info.mesh;
  float3 co = mesh.vert_positions()[bs.bevvert_mesh_verts()[bv]];
  float sinang = math::sin(ang);
  BLI_assert(sinang != 0.0f);
  if (e1_spec_r == 0.0f) {
    float3 dir1 = math::normalize(edge_dir(bv, e1_pos, bs));
    *r_co = co + (e2_spec_l / sinang) * dir1;
  }
  else {
    float3 dir2 = math::normalize(edge_dir(bv, e2_pos, bs));
    *r_co = co + (e1_spec_r / sinang) * dir2;
  }
  return true;
}

/** Return true if it will look good to put the meeting point where try_offset_on_edge_between
 * would put it. This means that neither side sees a reflex angle or too close to a straight
 * angle.
 */
[[maybe_unused]] static bool good_slide(const int bv,
                                        const int e1_pos,
                                        const int e2_pos,
                                        const int eslide_pos,
                                        const float e1_spec_r,
                                        const float e2_spec_l,
                                        const BevelState &bs)
{
  return try_offset_meet_edge(bv, e1_pos, eslide_pos, e1_spec_r, 0.0f, nullptr, nullptr, bs) &&
         try_offset_meet_edge(bv, eslide_pos, e2_pos, 0.0f, e2_spec_l, nullptr, nullptr, bs);
}

/** Calculate the best place for a meeting point for the offsets from edges at \a e1_pos and \a
 * e2_pos on the in-between edge at \a eon_pos. Viewed from the vertex normal side, the CCW
 * order of these edges is e1, eon, e2. Return true if we placed in *r_co as compromise between
 * where two edges met. If we did, put the ratio of sines of angles in *r_sin_ratio too.
 * However, if the offset_type is Percent or Absolute, we just slide along eon by the specified
 * amount.
 */
static bool try_offset_on_edge_between(const int bv,
                                       const int e1_pos,
                                       const int e2_pos,
                                       const int eon_pos,
                                       const float e1_spec_r,
                                       const float e2_spec_l,
                                       float3 *r_co,
                                       float *r_sin_ratio,
                                       const BevelState &bs)
{
  BLI_assert(bs.bevedge_is_beveled(bs.bevvert_bevedges()[bv][e1_pos]) &&
             bs.bevedge_is_beveled(bs.bevvert_bevedges()[bv][e2_pos]) &&
             !bs.bevedge_is_beveled(bs.bevvert_bevedges()[bv][eon_pos]));
  BLI_assert(r_co != nullptr);
  float ang1, ang2;
  float3 meet1, meet2;
  bool ok1 = try_offset_meet_edge(bv, e1_pos, eon_pos, e1_spec_r, 0.0f, &meet1, &ang1, bs);
  bool ok2 = try_offset_meet_edge(bv, eon_pos, e2_pos, 0.0f, e2_spec_l, &meet2, &ang2, bs);
  if (r_sin_ratio != nullptr) {
    *r_sin_ratio = ang1 == 0.0f ? 1.0f : math::sin(ang2) / math::sin(ang1);
  }
  if (ok1 && ok2) {
    /* The two sides will likely lead to different meet points.
     * Compromise on the midpint between them.
     */
    mid_v3_v3v3(*r_co, meet1, meet2);
    return true;
  }
  else if (ok1 && !ok2) {
    *r_co = meet1;
  }
  else if (!ok1 && ok2) {
    *r_co = meet2;
  }
  else {
    /* Neither offset line met eon.
     * This should only happen if all three lines are on top of each other.
     */
    *r_co = slide_dist(bv, eon_pos, e1_spec_r, bs);
  }
  return false;
}

/** Return true if the specified edge is between two faces with 180 degres between their normals.
 */
static bool bevedge_on_plane(const int bv, const int epos, const BevelState &bs)
{
  const int face_prev = bs.face_prev(bv, epos);
  const int face_next = bs.face_next(bv, epos);
  const Mesh &mesh = bs.mesh_info.mesh;
  if (face_prev != -1 && face_next != -1) {
    float dot = math::dot(mesh.face_normals()[face_prev], mesh.face_normals()[face_next]);
    return math::abs(dot + 1.0f) <= bevel_epsilon_big ||
           math::abs(dot - 1.0f) <= bevel_epsilon_big;
  }
  return false;
}

/** Return the closest point on the line (a, b) to the given bevedge specified by \a bv and \a
 * epos. */
static float3 project_to_edge(
    const int bv, const int epos, const float3 &a, const float3 &b, const BevelState &bs)
{
  float3 co1, co2;
  const int be = bs.bevvert_bevedges()[bv][epos];
  const Mesh &mesh = bs.mesh_info.mesh;
  const int2 mesh_e = mesh.edges()[bs.bevedge_mesh_edges()[be]];
  const float3 e1 = mesh.vert_positions()[mesh_e[0]];
  const float3 e2 = mesh.vert_positions()[mesh_e[1]];
  if (!isect_line_line_v3(e1, e2, a, b, co1, co2)) {
    return e1;
  }
  return co1;
}

}  // end namespace geom

namespace profile {
/** Structures, functions and constants related to superellipse profiles. */

/**
 * Profile specification:
 * The profile is a path defined with start, middle, and end control points projected onto a
 * plane (plane_no is normal, plane_co is a point on it) via lines in a given direction (proj_dir).
 *
 * Many interesting profiles are in family of superellipses:
 *     (abs(x/a))^r + abs(y/b))^r = 1
 * r==2 => ellipse; r==1 => line; r < 1 => concave; r > 1 => bulging out.
 * Special cases: let r==0 mean straight-inward, and r==4 mean straight outward.
 *
 * After the parameters are all set, the actual profile points are calculated and pointed to
 * by prof_co. We also may need profile points for a higher resolution number of segments
 * for the subdivision while making the ADJ vertex mesh pattern, and that goes in prof_co_2.
 */
struct Profile {
  /** Superellipse r parameter. */
  float super_r;
  /** Height for profile cutoff face sides. */
  float height;
  /** Start control point for profile. */
  float3 start;
  /** Mid control point for profile. */
  float3 middle;
  /** End control point for profile. */
  float3 end;
  /** Normal of plane to project to. */
  float3 plane_no;
  /** Coordinate on plane to project to. */
  float3 plane_co;
  /** Direction of projection line. */
  float3 proj_dir;
  /** segments+1 profile coordinates. */
  Array<float3, 10> prof_co;
  /** Like prof_co, but for seg power of 2 >= seg. */
  Array<float3, 10> prof_co_2;
  /** Mark a special case so the these parameters aren't reset with others. */
  bool special_params;
};

/** Holds the profiles for each anchor vertex in a vertex mesh pattern.
 * Given an inline capacity to make the need for allocates rare.
 */
typedef Array<Profile, 20> AnchorProfiles;

/* Values for super_r to give particular special shapes. */
constexpr float pro_square_r = 1e4f;
constexpr float pro_circle_r = 2.0f;
constexpr float pro_line_r = 1.0f;
constexpr float pro_square_in_r = 0.0f;

/**
 * Get the coordinate on the superellipse (x^r + y^r = 1), at parameter value x
 * (or, if !rbig, mirrored (y=x)-line).
 * rbig should be true if r > 1.0 and false if <= 1.0.
 * Assume r > 0.0.
 */
static double superellipse_co(double x, float r, bool rbig)
{
  BLI_assert(r > 0.0f);

  /* If r<1, mirror the superellipse function by (y=x)-line to get a numerically stable range
   * Possible because of symmetry, later mirror back. */
  double dr = r;
  if (rbig) {
    return math::pow((1.0 - math::pow(x, dr)), (1.0 / dr));
  }
  return 1.0 - math::pow((1.0 - math::pow(1.0 - x, dr)), (1.0 / dr));
}

/** Find xnew > x0 so that distance((x0,y0), (xnew, ynew)) = dtarget.
 * False position Illinois method used because the function is somewhat linear
 * -> linear interpolation converges fast.
 * Assumes that the gradient is always between 1 and -1 for x in [x0, x0+dtarget].
 */
static double find_superellipse_chord_endpoint(double x0, double dtarget, float r, bool rbig)
{
  double y0 = superellipse_co(x0, r, rbig);
  const double tol = 1e-13; /* accumulates for many segments so use low value. */
  const int maxiter = 10;

  /* For gradient between -1 and 1, xnew can only be in [x0 + sqrt(2)/2*dtarget, x0 + dtarget].
   */
  double xmin = x0 + math::numbers::sqrt2 / 2.0 * dtarget;
  xmin = std::min(xmin, 1.0);
  double xmax = x0 + dtarget;
  xmax = std::min(xmax, 1.0);
  double ymin = superellipse_co(xmin, r, rbig);
  double ymax = superellipse_co(xmax, r, rbig);

  /* NOTE: using distance**2 (no sqrt needed) does not converge that well. */
  double dmaxerr = math::sqrt(math::pow((xmax - x0), 2.0) + math::pow((ymax - y0), 2.0)) - dtarget;
  double dminerr = math::sqrt(math::pow((xmin - x0), 2.0) + math::pow((ymin - y0), 2.0)) - dtarget;

  double xnew = xmax - dmaxerr * (xmax - xmin) / (dmaxerr - dminerr);
  bool lastupdated_upper = true;

  for (int iter = 0; iter < maxiter; iter++) {
    double ynew = superellipse_co(xnew, r, rbig);
    double dnewerr = math::sqrt(math::pow((xnew - x0), 2.0) + math::pow((ynew - y0), 2.0)) -
                     dtarget;
    if (fabs(dnewerr) < tol) {
      break;
    }
    if (dnewerr < 0) {
      xmin = xnew;
      ymin = ynew;
      dminerr = dnewerr;
      if (!lastupdated_upper) {
        xnew = (dmaxerr / 2 * xmin - dminerr * xmax) / (dmaxerr / 2 - dminerr);
      }
      else {
        xnew = xmax - dmaxerr * (xmax - xmin) / (dmaxerr - dminerr);
      }
      lastupdated_upper = false;
    }
    else {
      xmax = xnew;
      ymax = ynew;
      dmaxerr = dnewerr;
      if (lastupdated_upper) {
        xnew = (dmaxerr * xmin - dminerr / 2 * xmax) / (dmaxerr - dminerr / 2);
      }
      else {
        xnew = xmax - dmaxerr * (xmax - xmin) / (dmaxerr - dminerr);
      }
      lastupdated_upper = true;
    }
  }
  return xnew;
}

/**
 * This search procedure to find equidistant points (x,y) in the first
 * superellipse quadrant works for every superellipse exponent but is more
 * expensive than known solutions for special cases.
 * Call the point on superellipse that intersects x=y line mx.
 * For r>=1 use only the range x in [0,mx] and mirror the rest along x=y line,
 * for r<1 use only x in [mx,1]. Points are initially spaced and iteratively
 * repositioned to have the same distance.
 */
static void find_even_superellipse_chords_general(int seg,
                                                  float r,
                                                  MutableSpan<double> xvals,
                                                  MutableSpan<double> yvals)
{
  const int smoothitermax = 10;
  const double error_tol = 1e-7;
  int imax = (seg + 1) / 2 - 1; /* Ceiling division - 1. */

  bool seg_odd = seg % 2;

  bool rbig;
  double mx;
  if (r > 1.0f) {
    rbig = true;
    mx = math::pow(0.5, 1.0 / r);
  }
  else {
    rbig = false;
    mx = 1 - math::pow(0.5, 1.0 / r);
  }

  /* Initial positions, linear spacing along x axis. */
  for (int i = 0; i <= imax; i++) {
    xvals[i] = i * mx / seg * 2;
    yvals[i] = superellipse_co(xvals[i], r, rbig);
  }
  yvals[0] = 1;

  /* Smooth distance loop. */
  for (int iter = 0; iter < smoothitermax; iter++) {
    double sum = 0.0;
    double dmin = 2.0;
    double dmax = 0.0;
    /* Update distances between neighbor points. Store the highest and
     * lowest to see if the maximum error to average distance (which isn't
     * known yet) is below required precision. */
    for (int i = 0; i < imax; i++) {
      double d = math::sqrt(math::pow((xvals[i + 1] - xvals[i]), 2.0) +
                            math::pow((yvals[i + 1] - yvals[i]), 2.0));
      sum += d;
      dmax = std::max(d, dmax);
      dmin = std::min(d, dmin);
    }
    /* For last distance, weight with 1/2 if seg_odd. */
    double davg;
    if (seg_odd) {
      sum += math::numbers::sqrt2 / 2 * (yvals[imax] - xvals[imax]);
      davg = sum / (imax + 0.5);
    }
    else {
      sum += math::sqrt(math::pow((xvals[imax] - mx), 2.0) + math::pow((yvals[imax] - mx), 2.0));
      davg = sum / (imax + 1.0);
    }
    /* Max error in tolerance? -> Quit. */
    bool precision_reached = true;
    if (dmax - davg > error_tol) {
      precision_reached = false;
    }
    if (dmin - davg < error_tol) {
      precision_reached = false;
    }
    if (precision_reached) {
      break;
    }

    /* Update new coordinates. */
    for (int i = 1; i <= imax; i++) {
      xvals[i] = find_superellipse_chord_endpoint(xvals[i - 1], davg, r, rbig);
      yvals[i] = superellipse_co(xvals[i], r, rbig);
    }
  }

  /* Fill remaining. */
  if (!seg_odd) {
    xvals[imax + 1] = mx;
    yvals[imax + 1] = mx;
  }
  for (int i = imax + 1; i <= seg; i++) {
    yvals[i] = xvals[seg - i];
    xvals[i] = yvals[seg - i];
  }

  if (!rbig) {
    for (int i = 0; i <= seg; i++) {
      double temp = xvals[i];
      xvals[i] = 1.0 - yvals[i];
      yvals[i] = 1.0 - temp;
    }
  }
}

/**
 * Find equidistant points `(x0,y0), (x1,y1)... (xn,yn)` on the superellipse
 * function in the first quadrant. For special profiles (linear, arc,
 * rectangle) the point can be calculated easily, for any other profile a more
 * expensive search procedure must be used because there is no known closed
 * form for equidistant parametrization.
 * `xvals` and `yvals` should be size `n+1`.
 */
static void find_even_superellipse_chords(int n,
                                          float r,
                                          MutableSpan<double> xvals,
                                          MutableSpan<double> yvals)
{
  bool seg_odd = n % 2;
  int n2 = n / 2;

  /* Special cases. */
  if (r == pro_line_r) {
    /* Linear spacing. */
    for (int i = 0; i <= n; i++) {
      xvals[i] = double(i) / n;
      yvals[i] = 1.0 - double(i) / n;
    }
    return;
  }
  if (r == pro_circle_r) {
    double temp = M_PI_2 / n;
    /* Angle spacing. */
    for (int i = 0; i <= n; i++) {
      xvals[i] = math::sin(i * temp);
      yvals[i] = math::cos(i * temp);
    }
    return;
  }
  if (r == pro_square_in_r) {
    /* n is even, distribute first and second half linear. */
    if (!seg_odd) {
      for (int i = 0; i <= n2; i++) {
        xvals[i] = 0.0;
        yvals[i] = 1.0 - double(i) / n2;
        xvals[n - i] = yvals[i];
        yvals[n - i] = xvals[i];
      }
    }
    /* n is odd, so get one corner-cut chord. */
    else {
      double temp = 1.0 / (n2 + math::numbers::sqrt2 / 2.0);
      for (int i = 0; i <= n2; i++) {
        xvals[i] = 0.0;
        yvals[i] = 1.0 - double(i) * temp;
        xvals[n - i] = yvals[i];
        yvals[n - i] = xvals[i];
      }
    }
    return;
  }
  if (r == pro_square_r) {
    /* n is even, distribute first and second half linear. */
    if (!seg_odd) {
      for (int i = 0; i <= n2; i++) {
        xvals[i] = double(i) / n2;
        yvals[i] = 1.0;
        xvals[n - i] = yvals[i];
        yvals[n - i] = xvals[i];
      }
    }
    /* n is odd, so get one corner-cut chord. */
    else {
      double temp = 1.0 / (n2 + math::numbers::sqrt2 / 2);
      for (int i = 0; i <= n2; i++) {
        xvals[i] = double(i) * temp;
        yvals[i] = 1.0;
        xvals[n - i] = yvals[i];
        yvals[n - i] = xvals[i];
      }
    }
    return;
  }
  /* For general case use the more expensive search algorithm. */
  find_even_superellipse_chords_general(n, r, xvals, yvals);
}

/**
 * Find the profile's "fullness," which is the fraction of the space it takes up way from the
 * boundvert's centroid to the original vertex for a non-custom profile, or in the case of a
 * custom profile, the average "height" of the profile points along its centerline.
 */
static float find_profile_fullness(BevelState *bs)
{
  int nseg = bs->params.segments;

  /* Precalculated fullness for circle profile radius and more common low seg values. */
  constexpr int circle_fullness_segs = 11;
  static const float circle_fullness[circle_fullness_segs] = {
      0.0f,   /* nsegs == 1 */
      0.559f, /* 2 */
      0.642f, /* 3 */
      0.551f, /* 4 */
      0.646f, /* 5 */
      0.624f, /* 6 */
      0.646f, /* 7 */
      0.619f, /* 8 */
      0.647f, /* 9 */
      0.639f, /* 10 */
      0.647f, /* 11 */
  };

  float fullness;
  if (bs->params.custom_profile) {
    /* Set fullness to the average "height" of the profile's sampled points. */
    fullness = 0.0f;
    for (int i = 0; i < nseg; i++) { /* Don't use the end points. */
      fullness += float(bs->pro_spacing.xvals[i] + bs->pro_spacing.yvals[i]) / (2.0f * nseg);
    }
  }
  else {
    /* An offline optimization process found fullness that led to closest fit to sphere as
     * a function of r and ns (for case of cube corner). */
    if (bs->pro_super_r == pro_line_r) {
      fullness = 0.0f;
    }
    else if (bs->pro_super_r == pro_circle_r && nseg > 0 && nseg <= circle_fullness_segs) {
      fullness = circle_fullness[nseg - 1];
    }
    else {
      /* Linear regression fit found best linear function, separately for even/odd segs. */
      if (nseg % 2 == 0) {
        fullness = 2.4506f * bs->params.shape - 0.00000300f * nseg - 0.6266f;
      }
      else {
        fullness = 2.3635f * bs->params.shape + 0.000152f * nseg - 0.6060f;
      }
    }
  }
  return fullness;
}

/**
 * Fill matrix r_mat so that a point in the sheared parallelogram with corners
 * va, vmid, vb (and the 4th that is implied by it being a parallelogram)
 * is the result of transforming the unit square by multiplication with r_mat.
 * If it can't be done because the parallelogram is degenerate, return false,
 * else return true.
 * Method:
 * Find vo, the origin of the parallelogram with other three points va, vmid, vb.
 * Also find vd, which is in direction normal to parallelogram and 1 unit away
 * from the origin.
 * The quarter circle in first quadrant of unit square will be mapped to the
 * quadrant of a sheared ellipse in the parallelogram, using a matrix.
 * The matrix mat is calculated to map:
 *    (0,1,0) -> va
 *    (1,1,0) -> vmid
 *    (1,0,0) -> vb
 *    (0,1,1) -> vd
 * We want M to make M*A=B where A has the left side above, as columns
 * and B has the right side as columns - both extended into homogeneous coords.
 * So M = B*(Ainverse).  Doing Ainverse by hand gives the code below.
 */
static bool make_unit_square_map(const float3 va,
                                 const float3 vmid,
                                 const float3 vb,
                                 float4x4 &r_mat)
{
  const float3 va_vmid = vmid - va;
  const float3 vb_vmid = vmid - vb;

  if (math::is_zero(va_vmid) || math::is_zero(vb_vmid)) {
    return false;
  }

  if (math::abs(angle_v3v3(va_vmid, vb_vmid) - math::numbers::pi) <= geom::bevel_epsilon_ang) {
    return false;
  }

  const float3 vo = va - vb_vmid;
  const float3 vddir = math::normalize(math::cross(vb_vmid, va_vmid));
  const float3 vd = vo + vddir;

  /* The cols of m are: `vmid - va, vmid - vb, vmid + vd - va -vb, va + vb - vmid`;
   * Blender transform matrices are stored such that `m[i][*]` is `i-th` column;
   * the last elements of each col remain as they are in unity matrix. */
  const float3 col0 = vmid - va;
  const float3 col1 = vmid - vb;
  const float3 col2 = vmid + vd - va - vb;
  const float3 col3 = va + vb - vmid;
  r_mat[0] = float4(col0[0], col0[1], col0[2], 0.0f);
  r_mat[1] = float4(col1[0], col1[1], col1[2], 0.0f);
  r_mat[2] = float4(col2[0], col2[1], col2[2], 0.0f);
  r_mat[3] = float4(col3[0], col3[1], col3[2], 1.0f);

  return true;
}

/**
 * Helper for #calculate_profiles that builds the 3D locations for the segments
 * and the higher power of 2 segments.
 */
static void calculate_profile_segments(const Profile &profile,
                                       const float4x4 map,
                                       const bool use_map,
                                       const bool reversed,
                                       const int ns,
                                       Span<double> xvals,
                                       Span<double> yvals,
                                       MutableSpan<float3> r_prof_co)
{
  /* Iterate over the vertices along the boundary arc. */
  for (int k = 0; k <= ns; k++) {
    float3 co;
    if (k == 0) {
      co = profile.start;
    }
    else if (k == ns) {
      co = profile.end;
    }
    else {
      if (use_map) {
        const float3 p = reversed ? float3(yvals[ns - k], xvals[ns - k], 0.0f) :
                                    float3(xvals[k], yvals[k], 0.0f);
        /* Do the 2D->3D transformation of the profile coordinates. */
        co = math::transform_point(map, p);
      }
      else {
        co = math::interpolate(profile.start, profile.end, float(k) / float(ns));
      }
    }
    /* Finish the 2D->3D transformation by projecting onto the final profile plane. */
    float3 &prof_co_k = r_prof_co[k];
    if (!math::is_zero(profile.proj_dir)) {
      float3 co2 = co + profile.proj_dir;
      if (!isect_line_plane_v3(prof_co_k, co, co2, profile.plane_co, profile.plane_no)) {
        /* Shouldn't happen. */
        prof_co_k = co;
      }
    }
    else {
      prof_co_k = co;
    }
  }
}

/**
 * Fills the ProfileSpacing struct with the 2D coordinates for the profile's vertices.
 * The superellipse used for multi-segment profiles does not have a closed-form way
 * to generate evenly spaced points along an arc. We use an expensive search procedure
 * to find the parameter values that lead to bp->seg even chords.
 * We also want spacing for a number of segments that is a power of 2 >= bp->seg (but at least
 * 4). Use doubles because otherwise we cannot come close to float precision for final results.
 *
 * \param pro_spacing: The struct to fill. Changes depending on whether there needs
 * to be a separate miter profile.
 */
static void set_profile_spacing(BevelState *bs, ProfileSpacing *pro_spacing, bool custom)
{
  int segments = bs->params.segments;

  if (segments <= 1) {
    /* Only 1 segment, we don't need any profile information. */
    pro_spacing->segments_power_2 = 0;
    return;
  }

  int segments_power_2 = std::max(power_of_2_max_i(bs->params.segments), 4);

  /* Sample the seg_2 segments used during vertex mesh subdivision. */
  bs->pro_spacing.segments_power_2 = segments_power_2;
  if (segments_power_2 == segments) {
    pro_spacing->xvals_2 = pro_spacing->xvals;
    pro_spacing->yvals_2 = pro_spacing->yvals;
  }
  else {
    pro_spacing->xvals_2 = Array<double>(segments_power_2 + 1);
    pro_spacing->yvals_2 = Array<double>(segments_power_2 + 1);
    if (custom) {
      /* Make sure the curve profile widget's sample table is full of the segments_power_2
       * samples.
       */
      BKE_curveprofile_init((CurveProfile *)bs->params.custom_profile, short(segments_power_2));

      /* Copy segment locations into the profile spacing struct. */
      for (const int i : IndexRange(segments_power_2 + 1)) {
        pro_spacing->xvals_2[i] = double(bs->params.custom_profile->segments[i].y);
        pro_spacing->yvals_2[i] = double(bs->params.custom_profile->segments[i].x);
      }
    }
    else {
      find_even_superellipse_chords(
          segments_power_2, bs->pro_super_r, pro_spacing->xvals_2, pro_spacing->yvals_2);
    }
  }

  /* Sample the input number of segments. */
  pro_spacing->xvals = Array<double>(segments + 1);
  pro_spacing->yvals = Array<double>(segments + 1);
  if (custom) {
    /* Make sure the curve profile's sample table is full. */
    if (bs->params.custom_profile->segments_len != segments ||
        !bs->params.custom_profile->segments)
    {
      BKE_curveprofile_init((CurveProfile *)bs->params.custom_profile, short(segments));
    }

    /* Copy segment locations into the profile spacing struct. */
    for (const int i : IndexRange(segments + 1)) {
      pro_spacing->xvals[i] = double(bs->params.custom_profile->segments[i].y);
      pro_spacing->yvals[i] = double(bs->params.custom_profile->segments[i].x);
    }
  }
  else {
    find_even_superellipse_chords(
        segments, bs->pro_super_r, pro_spacing->xvals, pro_spacing->yvals);
  }
}

static void calculate_profiles(const int bv, AnchorProfiles &profiles, const BevelState &bs)
{
  if (bs.params.segments == 1) {
    /* Profiles are unnecessary for 1-segment bevels. */
    return;
  }
  const MeshPattern &pat = bs.bevvert_meshpatterns()[bv];
  Span<int> edges = bs.bevvert_bevedges()[bv];
  IndexRange newverts = bs.bevvert_newverts()[bv];
  const float3 bv_v_pos = bs.mesh_info.mesh.vert_positions()[bs.bevvert_mesh_verts()[bv]];
  for (const int a : IndexRange(pat.num_anchors)) {
    Profile &profile = profiles[a];
    /* First fill in the profile parameters. */
    const int apos = pat.anchor_vert(a);
    const int anext = (a + 1) % pat.num_anchors;
    const int aposnext = pat.anchor_vert(anext);
    profile.start = bs.newvert_positions()[newverts[apos]];
    profile.end = bs.newvert_positions()[newverts[aposnext]];
    /* Fallback will be linear interopolation bewtween start and end. */
    profile.middle = math::interpolate(profile.start, profile.end, 0.5f);
    profile.super_r = pro_line_r;
    profile.plane_co = float3(0.0, 0.0, 0.0);
    profile.plane_no = float3(0.0, 0.0, 0.0);
    profile.proj_dir = float3(0.0, 0.0, 0.0);
    if (bs.params.affect_type == BevelAffect::Vertices) {
      profile.middle = bv_v_pos;
      continue;
    }
    const int be_pos = bs.last_attached_bevedge_pos(bv, apos);
    if (be_pos == -1) {
      /* TODO: this shouldn't happen. */
      BLI_assert(false);
      continue;
    }
    const int be = edges[be_pos];
    if (bs.bevedge_is_beveled(be)) {
      /* Projection direction is along be towards bv. */
      profile.proj_dir = -math::normalize(geom::edge_dir(bv, be_pos, bs));
      profile.middle = geom::project_to_edge(bv, be_pos, profile.start, profile.end, bs);
      /* Usual plane to project to is the one containing start, middle, and end. */
      const float3 d1 = math::normalize(profile.middle - profile.start);
      const float3 d2 = math::normalize(profile.middle - profile.end);
      if (!geom::nearly_parallel_normalized(d1, d2)) {
        /* Usual plane is fine. */
        profile.plane_no = math::normalize(math::cross(d1, d2));
        profile.plane_co = profile.start;
        profile.super_r = bs.pro_super_r;
      }
      else {
        /* It seems that the beveled edge is coplanar with the two anchor verts.
         * We want to make that plane the profile plane, if possible.
         */
        const int be_prev_pos = be_pos == 0 ? edges.size() - 1 : be_pos - 1;
        const int be_next_pos = be_pos == edges.size() - 1 ? 0 : be_pos + 1;
        const int be_prev = edges[be_prev_pos];
        const int be_next = edges[be_next_pos];
        if (bs.bevedge_is_beveled(be_prev) && bs.bevedge_is_beveled(be_next) && be_prev != be_next)
        {
          const float3 d3 = math::normalize(geom::edge_dir(bv, be_prev_pos, bs));
          const float3 d4 = math::normalize(geom::edge_dir(bv, be_next_pos, bs));
          if (!geom::nearly_parallel_normalized(d3, d4)) {
            const float3 co3 = profile.start + d3;
            const float3 co4 = profile.end + d4;
            float3 meetco, isect2;
            if (isect_line_line_v3(profile.start, co3, profile.end, co4, meetco, isect2)) {
              profile.middle = meetco;
              profile.super_r = bs.pro_super_r;
            }
          }
        }
        else {
          profile.middle = bv_v_pos;
          profile.super_r = bs.pro_super_r;
        }
        if (profile.super_r != pro_line_r) {
          const float3 d5 = math::normalize(profile.middle - profile.start);
          const float3 d6 = math::normalize(profile.middle - profile.end);
          if (!geom::nearly_parallel_normalized(d5, d6)) {
            profile.plane_no = math::normalize(math::cross(d5, d6));
            profile.plane_co = profile.start;
          }
        }
      }
    }
    /* TODO: miters */

    /* Now that the profile parameters are set, we can calculate the positions
     */
    profile.prof_co.reinitialize(bs.params.segments + 1);
    profile.prof_co_2.reinitialize(bs.pro_spacing.segments_power_2 + 1);
    float4x4 map;
    bool use_map = !bs.params.custom_profile && profile.super_r != pro_line_r;
    if (use_map) {
      use_map = make_unit_square_map(profile.start, profile.middle, profile.end, map);
    }
    /* TODO: cutoff method. */
    /* Calculate the 3D locations for the profile points. */
    calculate_profile_segments(profile,
                               map,
                               use_map,
                               false,
                               bs.params.segments,
                               bs.pro_spacing.xvals,
                               bs.pro_spacing.yvals,
                               profile.prof_co.as_mutable_span());
    if (bs.params.segments != bs.pro_spacing.segments_power_2) {
      calculate_profile_segments(profile,
                                 map,
                                 use_map,
                                 false,
                                 bs.pro_spacing.segments_power_2,
                                 bs.pro_spacing.xvals_2,
                                 bs.pro_spacing.yvals_2,
                                 profile.prof_co_2.as_mutable_span());
    }
    else {
      std::copy(profile.prof_co.begin(), profile.prof_co.end(), profile.prof_co_2.begin());
    }
  }
}

/**
 * Find the point on given profile at parameter \a at_index which goes from 0 to \a nseg as
 * the profile moves from `profile.start` to `profile.end`.
 * We assume that nseg is either the profile's seg number or a power of 2 less than
 * or equal to the power of 2 >= seg.
 * In the latter case, we subsample the profile.prof_co_2, which will not necessarily
 * give equal spaced chords, but is in fact more what is desired by the cubic subdivision
 * method used to make the adj pattern.
 */
static float3 get_profile_point(const Profile &profile, const int at_index, const int nseg)
{
  const int profile_nseg = profile.prof_co.size() - 1;
  if (profile_nseg == 1) {
    return at_index == 0 ? profile.start : profile.end;
  }
  if (nseg == profile_nseg) {
    return profile.prof_co[at_index];
  }
  BLI_assert(is_power_of_2_i(nseg) && nseg < profile.prof_co_2.size());
  const int subsample_spacing = (profile.prof_co_2.size() - 1) / nseg;
  return profile.prof_co_2[at_index * subsample_spacing];
}

}  // end namespace profile

/** Hole Filling Mesh Pattern. */

namespace adj {
/** The "Adj" pattern is the only really complicated pattern.
 * The pattern for nv anchor verts and ns segments is a set of
 * concentric rings of vertices, which form rings of faces.
 * The outer most ring has a skeleton of nv anchor verts, and
 * between successive anchors there are ns edge segments and
 * hence (ns - 2) non-anchor verts (sometimes we'll call them
 * "span" verts) between the successive anchor verts.
 *
 * Each successive inner ring has 2 less verts than the next
 * outward ring.  If ns is odd, we end up with an nv-gon as
 * the innermost ring; if ns is even, we end up with a single
 * vertex in the innermost ring.
 *
 * The edges that go between the verts of the same ring are
 * called "ring edges". There are also edges that go between
 * two successive rings, called "cross-ring" edges. The span
 * vertices are connected one-to-one between the rings, while
 * an anchor vertex has edges to the just-preceding and
 * just-succeeding span vert in the next outer ring.
 * The ring edges and cross ring edges together form rings
 * of faces, which are all quads except possibly the center
 * face, which is an nv-gon ns is odd.
 *
 * The verts, edges, and faces are indexed by starting at the
 * innermost ring and going counterclockwise from the 0th
 * anchor vertex, and continuing with successive rings.
 * The edges index the ring edges first, then the cross ring
 * edges.
 *
 * TODO: link to external doc that has a picture of all this.
 */

static inline int odd(int i)
{
  return i % 2 == 1;
}

/** Odd vertex rings start with ring 0 = the innermost polygon. */
static inline int v_ringstart_odd(int r, int nv)
{
  return nv * r * r;
}

static inline int v_ringlen_odd(int r, int nv)
{
  return nv * (2 * r + 1);
}

static inline int v_num_rings_odd(int ns)
{
  return (ns - 1) / 2 + 1;
}

/** Even vertex ring 0 is the single vertex in the center. */
static inline int v_ringstart_even(int r, int nv)
{
  return r == 0 ? 0 : 1 + nv * (r - 1) * r;
}

static inline int v_ringlen_even(int r, int nv)
{
  return r == 0 ? 1 : 2 * nv * r;
}

static inline int v_num_rings_even(int ns)
{
  return ns / 2 + 1;
}

static int v_ringstart(int r, int nv, int ns)
{
  return odd(ns) ? v_ringstart_odd(r, nv) : v_ringstart_even(r, nv);
}

static int v_ringlen(int r, int nv, int ns)
{
  return odd(ns) ? v_ringlen_odd(r, nv) : v_ringlen_even(r, nv);
}

static int v_num_rings(int ns)
{
  return odd(ns) ? v_num_rings_odd(ns) : v_num_rings_even(ns);
}

static int v_total_verts(int nv, int ns)
{
  return v_ringstart(v_num_rings(ns), nv, ns);
}

/** Odd face ring 0 is the polygon in the center. */
static inline int f_ringstart_odd(int r, int nv)
{
  return v_ringstart_even(r, nv);
}

static inline int f_ringlen_odd(int r, int nv)
{
  return v_ringlen_even(r, nv);
}

static inline int f_num_rings_odd(int ns)
{
  return (ns - 1) / 2 + 1;
}

/** Even face ring 0 is the face ring around the center. */
static inline int f_ringstart_even(int r, int nv)
{
  return v_ringstart_odd(r, nv);
}

static inline int f_ringlen_even(int r, int nv)
{
  return v_ringlen_odd(r, nv);
}

static inline int f_num_rings_even(int ns)
{
  return ns / 2;
}

static int f_ringstart(int r, int nv, int ns)
{
  return odd(ns) ? f_ringstart_odd(r, nv) : f_ringstart_even(r, nv);
}

static int f_ringlen(int r, int nv, int ns)
{
  return odd(ns) ? f_ringlen_odd(r, nv) : f_ringlen_even(r, nv);
}

static int f_num_rings(int ns)
{
  return odd(ns) ? f_num_rings_odd(ns) : f_num_rings_even(ns);
}

static int f_total_faces(int nv, int ns)
{
  return f_ringstart(f_num_rings(ns), nv, ns);
}

/** Even edge ring 0 is the concentric ring of edges surrounding the center polygon. */
static inline int e_ringstart_odd(int r, int nv)
{
  return v_ringstart_odd(r, nv);
}

static inline int e_ringlen_odd(int r, int nv)
{
  return v_ringlen_odd(r, nv);
}

static inline int e_num_rings_odd(int ns)
{
  return (ns - 1) / 2 + 1;
}

/** Odd edge cross ring 0 is the set of cross-ring edges going from edge ring 1 to
 * edge ring 0 (i.e., they end on the vertices of the center polygon).
 */
static inline int e_crossring_start_odd(int r, int nv, int ns)
{
  return e_ringstart_odd(e_num_rings_odd(ns), nv) + nv * r * (r + 1);
}

static inline int e_crossring_len_odd(int r, int nv)
{
  return 2 * nv * (r + 1);
}

static inline int e_num_crossrings_odd(int ns)
{
  return ns > 1 ? (ns - 1) / 2 : 0;
}

/** Even edge ring 0 is concentric ring of edges surrounding the center vertex. */
static inline int e_ringstart_even(int r, int nv)
{
  return v_ringstart_even(r + 1, nv) - 1;
}

static inline int e_ringlen_even(int r, int nv)
{
  return v_ringlen_even(r + 1, nv);
}

static inline int e_num_rings_even(int ns)
{
  return ns / 2;
}

/** Even cross edge ring 0 is the set of cross edges ending at the center vertex. */
static inline int e_crossring_start_even(int r, int nv, int ns)
{
  return e_ringstart_even(e_num_rings_even(ns), nv) + nv * r * r;
}

static inline int e_crossring_len_even(int r, int nv)
{
  return nv * (2 * r + 1);
}

static inline int e_num_crossrings_even(int ns)
{
  return ns / 2;
}

static int e_ringstart(int r, int nv, int ns)
{
  return odd(ns) ? e_ringstart_odd(r, nv) : e_ringstart_even(r, nv);
}

static int e_ringlen(int r, int nv, int ns)
{
  return odd(ns) ? e_ringlen_odd(r, nv) : e_ringlen_even(r, nv);
}

static int e_num_rings(int ns)
{
  return odd(ns) ? e_num_rings_odd(ns) : e_num_rings_even(ns);
}

static int e_crossring_start(int r, int nv, int ns)
{
  return odd(ns) ? e_crossring_start_odd(r, nv, ns) : e_crossring_start_even(r, nv, ns);
}

static int e_crossring_len(int r, int nv, int ns)
{
  return odd(ns) ? e_crossring_len_odd(r, nv) : e_crossring_len_even(r, nv);
}

static int e_num_crossrings(int ns)
{
  return odd(ns) ? e_num_crossrings_odd(ns) : e_num_crossrings_even(ns);
}

static int e_num_edges(int nv, int ns)
{
  return e_crossring_start(e_num_crossrings(ns), nv, ns);
}

static int face_ring(int f, int nv, int ns)
{
  if (odd(ns)) {
    return f < 1 ? 0 : int(((nv + math::sqrt(nv * nv + 4 * nv * (f - 1))) / (2 * nv)));
  }
  else {
    return f < 0 ? 0 : int(math::sqrt(f / nv));
  }
}

static int vertex_ring(int v, int nv, int ns)
{
  if (odd(ns)) {
    return v < 0 ? 0 : int(math::sqrt(v / nv));
  }
  else {
    return v < 1 ? 0 : int(((nv + math::sqrt(nv * nv + 4 * nv * (v - 1))) / (2 * nv)));
  }
}

static inline int v_anchor_div(int r, int nv, int ns)
{
  return v_ringlen(r, nv, ns) / nv;
}

static inline int f_anchor_div(int r, int nv, int ns)
{
  return f_ringlen(r, nv, ns) / nv;
}

[[maybe_unused]] static int3 v_ring_anchor_offset(int v, int nv, int ns)
{
  int ring = vertex_ring(v, nv, ns);
  int ring_offset = v - v_ringstart(ring, nv, ns);
  int anchor_index = ring_offset / v_anchor_div(ring, nv, ns);
  int offset = ring_offset % v_anchor_div(ring, nv, ns);
  return int3(ring, anchor_index, offset);
}

static int3 f_ring_anchor_offset(int f, int nv, int ns)
{
  int ring = face_ring(f, nv, ns);
  int ring_offset = f - f_ringstart(ring, nv, ns);
  int anchor_index = ring_offset / f_anchor_div(ring, nv, ns);
  int offset = ring_offset % f_anchor_div(ring, nv, ns);
  return int3(ring, anchor_index, offset);
}

static int rao_to_vert(int r, int a, int o, int nv, int ns)
{
  int delta = (v_anchor_div(r, nv, ns) * a + o) % v_ringlen(r, nv, ns);
  return v_ringstart(r, nv, ns) + delta;
}

[[maybe_unused]] static int rao_to_face(int r, int a, int o, int nv, int ns)
{
  int delta = (f_anchor_div(r, nv, ns) * a + o) % f_ringlen(r, nv, ns);
  return f_ringstart(r, nv, ns) + delta;
}

static int face_outer_vertex_ring(int f, int nv, int ns)
{
  return odd(ns) ? face_ring(f, nv, ns) : face_ring(f, nv, ns) + 1;
}

/** Return the pattern indices for vertices and edges making up the pattern face \a f.
 * \a nv is the number of anchor verts and \a ns is the number of segments.  */
static std::pair<int4, int4> face_vertices_and_edges(int f, int nv, int ns)
{
  if (f == 0 and odd(ns)) {
    /* The center ngon has vertex and edge indices 0, ..., nv-1,
     * and we expect the caller to know this as this API doesn't let
     * us return an ngon. */
    return std::pair<int4, int4>(int4(0, 1, 2, 3), int4(0, 1, 2, 3));
  }
  const int3 rao = f_ring_anchor_offset(f, nv, ns);
  const int fr = rao[0];
  const int fa = rao[1];
  const int fo = rao[2];
  int outer_r = face_outer_vertex_ring(f, nv, ns);
  int inner_r = outer_r - 1;
  int vr = outer_r;
  int va = fa;
  int vo = fo;
  int vroot = rao_to_vert(vr, va, vo, nv, ns);
  int vafter = vroot + 1;
  int vopposite, vbefore;
  if (fo == 0) {
    vopposite = rao_to_vert(inner_r, va, vo, nv, ns);
    vbefore = va > 0 ? vroot - 1 : vroot + v_ringlen(outer_r, nv, ns) - 1;
  }
  else {
    vopposite = rao_to_vert(inner_r, va, vo, nv, ns);
    vbefore = vopposite > v_ringstart(inner_r, nv, ns) ?
                  vopposite - 1 :
                  v_ringstart(inner_r, nv, ns) + v_ringlen(inner_r, nv, ns) - 1;
  }
  int4 vertices = int4(vroot, vafter, vopposite, vbefore);
  int e0 = odd(ns) ? vroot : vroot - 1;
  int f_ring_pos = f - f_ringstart(fr, nv, ns);
  int e_cross_r = odd(ns) ? fr - 1 : fr;
  int e_outer_r = fr;
  int e_inner_r = e_outer_r - 1;
  int e1 = e_crossring_start(e_cross_r, nv, ns) + f_ring_pos;
  int e2, e3;
  if (fo == 0) {
    e2 = fa > 0 ? e1 - 1 : e1 + e_crossring_len(e_cross_r, nv, ns) - 1;
    e3 = e0 > e_ringstart(e_outer_r, nv, ns) ?
             e0 - 1 :
             e_ringstart(e_outer_r, nv, ns) + e_ringlen(e_outer_r, nv, ns) - 1;
  }
  else {
    if (odd(ns)) {
      e2 = vopposite > e_ringstart(e_inner_r, nv, ns) ?
               vopposite - 1 :
               e_ringstart(e_inner_r, nv, ns) + e_ringlen(e_inner_r, nv, ns) - 1;
    }
    else {
      e2 = vopposite - 1 > e_ringstart(e_inner_r, nv, ns) ?
               vopposite - 2 :
               e_ringstart(e_inner_r, nv, ns) + e_ringlen(e_inner_r, nv, ns) - 1;
    }
    e3 = e1 - 1;
  }
  int4 edges = int4(e0, e1, e2, e3);
  return std::pair<int4, int4>(vertices, edges);
}

[[maybe_unused]] static void print_adj_pattern(int nv, int ns)
{
  fmt::println("\nnv = {}, ns = {}", nv, ns);
  int nvrings = v_num_rings(ns);
  fmt::println("verts");
  fmt::println("v_num_rings = {}", nvrings);
  for (const int r : IndexRange(nvrings)) {
    fmt::println("r = {}: start = {}, len = {}", r, v_ringstart(r, nv, ns), v_ringlen(r, nv, ns));
  }
  fmt::println("total verts = {}", v_total_verts(nv, ns));
  fmt::println("faces");
  int nfrings = f_num_rings(ns);
  fmt::println("f_num_rings = {}", nfrings);
  for (const int r : IndexRange(nfrings)) {
    fmt::println("r = {}: start = {}, len = {}", r, f_ringstart(r, nv, ns), f_ringlen(r, nv, ns));
  }
  fmt::println("total faces = {}", f_total_faces(nv, ns));
  fmt::println("edges");
  int nerings = e_num_rings(ns);
  fmt::println("e_num_rings = {}", nerings);
  for (const int r : IndexRange(nerings)) {
    fmt::println("r = {}: start = {}, len = {}", r, e_ringstart(r, nv, ns), e_ringlen(r, nv, ns));
  }
  int necrossrings = e_num_crossrings(ns);
  fmt::println("e_num_crossrings = {}", necrossrings);
  for (const int r : IndexRange(necrossrings)) {
    fmt::println("r = {}: start = {}, len = {}",
                 r,
                 e_crossring_start(r, nv, ns),
                 e_crossring_len(r, nv, ns));
  }
  fmt::println("total edges = {}", e_num_edges(nv, ns));
  fmt::println("");
  for (const int f : IndexRange(f_total_faces(nv, ns))) {
    auto [fv, fe] = face_vertices_and_edges(f, nv, ns);
    fmt::println("f = {}, fverts = ({},{},{},{}), fedges = ({},{},{},{})",
                 f,
                 fv[0],
                 fv[1],
                 fv[2],
                 fv[3],
                 fe[0],
                 fe[1],
                 fe[2],
                 fe[3]);
  }
}

/** A structure to hold the vertex positions of an Adj pattern. */
struct AdjVerts {
  /** The coordinates of vertices arranged according to indexing given above.
   * The inline size 125 will accommodate anchors=4, segments=10. */
  Array<float3, 125> verts;
  /** How many anchors (often abbreviated nv). */
  int anchors;
  /** How many segments (often abbreviated ns). */
  int segments;

  AdjVerts(const int anchors, const int segments) : anchors(anchors), segments(segments)
  {
    verts.reinitialize(v_total_verts(anchors, segments));
    verts.fill(float3(0.0f, 0.0f, 0.0f));
  }

  /** Given an anchor index and an offset from that anchor to a verterx on the outer ring,
   * return the pattern vertex index for that identified vertex. */
  const int anchor_offset_to_outer_ring_vert(const int anchor, const int offset) const;

  /** Return the 3d position of the vertex given by an anchor and an offset from that anchor, in
   * the outer ring. */
  const float3 &outer_ring_vert(const int anchor, const int offset) const
  {
    return verts[anchor_offset_to_outer_ring_vert(anchor, offset)];
  }

  /** Like outer_ring_vert(), but return a mutable position. */
  float3 &mutable_outer_ring_vert(const int anchor, const int offset)
  {
    return verts[anchor_offset_to_outer_ring_vert(anchor, offset)];
  }

  const int ring_anchor_offset_to_vert(const int ring, const int anchor, const int offset) const;

  const float3 &vert(const int ring, const int anchor, const int offset) const
  {
    return verts[ring_anchor_offset_to_vert(ring, anchor, offset)];
  }

  float3 &mutable_vert(const int ring, const int anchor, const int offset)
  {
    return verts[ring_anchor_offset_to_vert(ring, anchor, offset)];
  }
};

/** Given an anchor index and an offset from that anchor to a verterx on the outer ring,
 * return the pattern vertex index for that identified vertex. */
const int AdjVerts::anchor_offset_to_outer_ring_vert(const int anchor, const int offset) const
{
  const int ring = v_num_rings(this->segments) - 1;
  return rao_to_vert(ring, anchor, offset, this->anchors, this->segments);
}

/** Given a ring, an anchor index, and an offset from that anchor, return the vertex pattern index
 * for the corresponding vertex. */
const int AdjVerts::ring_anchor_offset_to_vert(const int ring,
                                               const int anchor,
                                               const int offset) const
{
  return rao_to_vert(ring, anchor, offset, this->anchors, this->segments);
}

/** For debugging: draw the Adj Pattern anchors of the \a adjvertes object. */
[[maybe_unused]] static void draw_adj(const AdjVerts &adjverts)
{
  constexpr uint life = draw::drw_debug_persistent_lifetime;
  for (const int i : adjverts.verts.index_range()) {
    float3 co = adjverts.verts[i];
    draw::drw_debug_point(co, 0.01f, {1, 0, 0, 1}, life);
  }
}

/** Return the average of four 3d positions. */
static float3 avg4(const float3 &v0, const float3 &v1, const float3 &v2, const float3 &v3)
{
  return 0.25f * (v0 + v1 + v2 + v3);
}

/* Gamma needed for smooth Catmull-Clark, Sabin modification. */
static float sabin_gamma(int n)
{
  /* Precalculated for common cases of n. */
  if (n < 3) {
    return 0.0f;
  }
  if (n == 3) {
    return 0.065247584f;
  }
  if (n == 4) {
    return 0.25f;
  }
  if (n == 5) {
    return 0.401983447f;
  }
  if (n == 6) {
    return 0.523423277f;
  }
  const double k = cos(math::numbers::pi / double(n));
  /* Need x, real root of x^3 + (4k^2 - 3)x - 2k = 0.
   * Answer calculated via Wolfram Alpha. */
  const double k2 = k * k;
  const double k4 = k2 * k2;
  const double k6 = k4 * k2;
  const double y = pow(
      math::numbers::sqrt3 * math::sqrt(64.0 * k6 - 144.0 * k4 + 135.0 * k2 - 27.0) + 9.0 * k,
      1.0 / 3.0);
  const double x = 0.480749856769136 * y - (0.231120424783545 * (12.0 * k2 - 9.0)) / y;
  return (k * x + 2.0 * k2 - 1.0) / (x * x * (k * x + 1.0));
}

/** Fill \a adjverts using recursive cubic subdivision, until reach the
 * base case where \a adjverts_2_segs is the answer.
 * adjverts.segments should be a power of 2, and >= 2.
 */
static void fill_adjverts(AdjVerts &adjverts,
                          const AdjVerts &adjverts_2_segs,
                          const profile::AnchorProfiles &profiles)
{
  BLI_assert(adjverts_2_segs.segments == 2 && adjverts.segments % 2 == 0);
  BLI_assert(adjverts.segments < 1000); /* TODO: stop too-deep recursion. */
  if (adjverts.segments < 2) {
    return;
  }
  if (adjverts.segments == 2) {
    std::copy(adjverts_2_segs.verts.begin(), adjverts_2_segs.verts.end(), adjverts.verts.begin());
    return;
  }
  int nhalf = adjverts.segments / 2;
  BLI_assert(nhalf % 2 == 0);
  AdjVerts adjverts_half = AdjVerts(adjverts.anchors, nhalf);
  fill_adjverts(adjverts_half, adjverts_2_segs, profiles);
  /* Do a step of cubic subdivision (Catmull-Clark) with special rules
   * at boundaries. See Levin 1999 paper "Filling an N-sided hole using combined
   * subdivision schemes".
   */
  const int n_boundary = adjverts_half.anchors;
  const int ns_in = adjverts_half.segments;
  const int ns_out = adjverts.segments;

  /* First adjust the boundary vertices of the input, storing in the output. */
  for (const int a : IndexRange(n_boundary)) {
    adjverts.mutable_outer_ring_vert(a, 0) = adjverts_half.outer_ring_vert(a, 0);
    for (const int o : IndexRange::from_begin_end(1, ns_in)) {
      float3 co = adjverts_half.outer_ring_vert(a, o);
      /* Smooth boundary rule for even verts. TODO: Custom profiles. */
      const float3 co1 = adjverts_half.outer_ring_vert(a, o - 1);
      const float3 co2 = adjverts_half.outer_ring_vert(a, o + 1);
      co = co - (1.0f / 6.0f) * (co1 + co2 - 2.0f * co);
      adjverts.mutable_outer_ring_vert(a, 2 * o) = co;
    }
  }
  /* Now set odd boundary verts, using input profiles. */
  for (const int a : IndexRange(n_boundary)) {
    const profile::Profile &profile = profiles[a];
    for (int o = 1; o < ns_out; o += 2) {
      float3 co = profile::get_profile_point(profile, o, ns_out);
      /* Smooth boundary rule for odd verts. TODO: Custom profiles. */
      const float3 co1 = adjverts.outer_ring_vert(a, o - 1);
      const float3 co2 = adjverts.outer_ring_vert(a, o + 1);
      co = co - (1.0f / 6.0f) * (co1 + co2 - 2.0f * co);
      adjverts.mutable_outer_ring_vert(a, o) = co;
    }
  }
  /* Copy adjusted boundary verts back into adjverts_half, prior to subdivision. */
  for (const int a : IndexRange(n_boundary)) {
    for (const int o : IndexRange(ns_in)) {
      adjverts_half.mutable_outer_ring_vert(a, o) = adjverts.outer_ring_vert(a, 2 * o);
    }
  }
  /* Now we do the internal vertices, using standard Catmull-Clark
   * and assuming all boundary vertices have valence 4. */

  /* The new face-center vertices.
   * Made as average of four corners of quads of the input mesh.
   */
  const int nrings_in = v_num_rings(ns_in);
  for (const int r : IndexRange::from_begin_end(1, nrings_in)) {
    const int side = v_anchor_div(r, n_boundary, ns_in);
    for (const int a : IndexRange(n_boundary)) {
      const int aprev = a == 0 ? n_boundary - 1 : a - 1;
      /* Corner faces have a different indexing pattern. */
      adjverts.mutable_vert(2 * r - 1, a, 0) = avg4(adjverts_half.vert(r, a, 0),
                                                    adjverts_half.vert(r, a, 1),
                                                    adjverts_half.vert(r - 1, r == 1 ? 0 : a, 0),
                                                    adjverts_half.vert(r, aprev, side - 1));
      for (int o = 1; o < side - 1; o++) {
        adjverts.mutable_vert(2 * r - 1, a, 2 * o) = avg4(adjverts_half.vert(r, a, o),
                                                          adjverts_half.vert(r, a, o + 1),
                                                          adjverts_half.vert(r - 1, a, o),
                                                          adjverts_half.vert(r - 1, a, o - 1));
      }
    }
  }

  /* The new cross-ring edge vertices.
   * Made as average of ends of cross-ring edges of the input mesh and the adjacent newly-made
   * face-center vertices of the output mesh left and right of that edge.
   */
  for (const int r : IndexRange::from_begin_end(1, nrings_in)) {
    const int side = v_anchor_div(r, n_boundary, ns_in);
    for (const int a : IndexRange(n_boundary)) {
      for (int o = 1; o < side; o++) {
        adjverts.mutable_vert(2 * r - 1, a, 2 * o - 1) = avg4(
            adjverts_half.vert(r, a, o),
            adjverts_half.vert(r - 1, a, o - 1),
            adjverts.vert(2 * r - 1, a, 2 * o - 2),
            adjverts.vert(2 * r - 1, a, 2 * o));
      }
    }
  }

  /* The new edge-ring edge vertices.
   * Made as the average of the ends of the edge-ring edges of input mesh and the
   * face-center vertices of the output mesh above and below that edge.
   */
  for (int r = 1; r < nrings_in - 1; r++) {
    const int side = v_anchor_div(r, n_boundary, ns_in);
    for (const int a : IndexRange(n_boundary)) {
      for (const int o : IndexRange(side)) {
        adjverts.mutable_vert(2 * r, a, 2 * o + 1) = avg4(adjverts_half.vert(r, a, o),
                                                          adjverts_half.vert(r, a, o + 1),
                                                          adjverts.vert(2 * r + 1, a, 2 * o + 2),
                                                          adjverts.vert(2 * r - 1, a, 2 * o));
      }
    }
  }

  /* The new vertex-vertex vertices (transformation of input interior vertices).
   * Made as a function of the average of the ends of the four new edges leading into the
   * vertex, and the average of the four new face vertices surrounding the vertex.
   */
  float gamma = sabin_gamma(4);
  float beta = -gamma;
  for (int r = 1; r < nrings_in - 1; r++) {
    const int iside = v_anchor_div(r, n_boundary, ns_in);
    const int oside = v_anchor_div(2 * r, n_boundary, ns_out);
    for (const int a : IndexRange(n_boundary)) {
      const int aprev = a == 0 ? n_boundary - 1 : a - 1;
      float3 ev_centroid = avg4(adjverts.vert(2 * r, aprev, oside - 1),
                                adjverts.vert(2 * r, a, 1),
                                adjverts.vert(2 * r + 1, aprev, oside + 1),
                                adjverts.vert(2 * r + 1, a, 1));
      float3 ef_centroid = avg4(adjverts.vert(2 * r - 1, a, 0),
                                adjverts.vert(2 * r + 1, a, 0),
                                adjverts.vert(2 * r + 1, a, 2),
                                adjverts.vert(2 * r + 1, aprev, oside));
      adjverts.mutable_vert(2 * r, a, 0) = ev_centroid + beta * ef_centroid +
                                           gamma * adjverts_half.vert(r, a, 0);
      for (int o = 1; o < iside; o++) {
        ev_centroid = avg4(adjverts.vert(2 * r, a, 2 * o - 1),
                           adjverts.vert(2 * r, a, 2 * o + 1),
                           adjverts.vert(2 * r + 1, a, 2 * o + 1),
                           adjverts.vert(2 * r - 1, a, 2 * o - 1));
        ef_centroid = avg4(adjverts.vert(2 * r - 1, a, 2 * o - 2),
                           adjverts.vert(2 * r - 1, a, 2 * o),
                           adjverts.vert(2 * r + 1, a, 2 * o),
                           adjverts.vert(2 * r + 1, a, 2 * o + 2));
        adjverts.mutable_vert(2 * r, a, 2 * o) = ev_centroid + beta * ef_centroid +
                                                 gamma * adjverts_half.vert(r, a, o);
      }
    }
  }

  /* The center vertex-vertex is like the ones calculated for the quads, but
   * the gamma is different because the valence is not necessarily 4.
   */
  gamma = sabin_gamma(n_boundary);
  beta = -gamma;
  /* Get centroids of new edge-verts and new face-verts adjacent to center. */
  float3 ev_centroid(0.0f, 0.0f, 0.0f);
  float3 ef_centroid(0.0f, 0.0f, 0.0f);
  for (const int a : IndexRange(n_boundary)) {
    ev_centroid = ev_centroid + adjverts.vert(1, a, 0);
    ef_centroid = ef_centroid + adjverts.vert(1, a, 1);
  }
  ev_centroid = ev_centroid / float(n_boundary);
  ef_centroid = ef_centroid / float(n_boundary);
  adjverts.mutable_vert(0, 0, 0) = ev_centroid + beta * ef_centroid +
                                   gamma * adjverts_half.vert(0, 0, 0);

  /* Final step: Copy the profile vertices to the output boundary. */
  for (const int a : IndexRange(n_boundary)) {
    const profile::Profile &profile = profiles[a];
    for (const int o : IndexRange(ns_out)) {
      adjverts.mutable_outer_ring_vert(a, o) = profile::get_profile_point(profile, o, ns_out);
    }
  }
}

/** Given a ruler whose values are \a div apart and start at 0, what is the index of the
 * tick on that ruler such that  \a value is between the tick of that index (inclusive) and
 * the next tick (exclusive), and also what is the remainder of value past that tick?
 * Assume value >= 0 and div > 0. */
static std::pair<int, float> tick_and_remainder(const float value, const float div)
{
  BLI_assert(value >= 0.0f && div > 0);
  const int tick = int(std::floor(value / div));
  return std::pair<int, float>(tick, value - tick * div);
}

/** Return the index of the first interval in \a breaks (assumed ascending) that is
 * less than or equal to \a value and either at the end, or less than the next one.
 */
static int which_interval(const float value, Span<float> breaks)
{
  int i = 0;
  while ((i + 1) < breaks.size() && value >= breaks[i + 1]) {
    i++;
  }
  return i;
}

/** Interpolate four coordinates by u along the co00 -> c01 direction and v along the c01 -> c10
 * direction. */
static float3 interp_bilinear_quad(const float3 &co00,
                                   const float3 &co01,
                                   const float3 &co11,
                                   const float3 &co10,
                                   const float u,
                                   const float v)
{
  return (1 - u) * (1 - v) * co00 + u * (1 - v) * co01 + u * v * co11 + (1 - u) * v * co10;
}

/** Interpolate given \a adjverts_in to make one with a different number of segments, in \a
 * adjverts. Assume they both have the same number of anchors, that adjverts_in has more segments,
 * and that adjverts_in has an even number of segments.
 */
static void interp_adj(AdjVerts &adjverts,
                       const AdjVerts &adjverts_in,
                       const profile::AnchorProfiles &profiles)
{
  const int na = adjverts_in.anchors;
  const int ns_in = adjverts_in.segments;
  const int ns_out = adjverts.segments;
  BLI_assert(adjverts.anchors == na && (ns_in % 2) == 0 && ns_out < ns_in);
  const int num_rings_in = v_num_rings(ns_in);
  const int num_rings_out = v_num_rings(ns_out);
  bool odd_out = (ns_out % 2) == 1;
  /* The radial_divs are the fractional spacing between rings.
   * For odd number of segments, there is a half space at the center. */
  const float radial_div_in = 2.0f / ns_in;
  const float radial_div_out = odd_out ? 1.0f / ((ns_out / 2) + 0.5f) : 2.0f / ns_out;
  for (int r_out = 0; r_out < num_rings_out - 1; r_out++) {
    if (r_out == 0 && !odd_out) {
      /* Just copy the center vertex. */
      adjverts.mutable_vert(0, 0, 0) = adjverts_in.vert(0, 0, 0);
      continue;
    }
    const float radial_frac_out = (num_rings_out - r_out - 1) * radial_div_out;
    auto [tick_in, remainder_in] = tick_and_remainder(radial_frac_out, radial_div_in);
    const int r_in = num_rings_in - 1 - tick_in;
    const int iside = v_anchor_div(r_in, na, ns_in);
    const int oside = v_anchor_div(r_out, na, ns_out);
    for (const int a : IndexRange(na)) {
      const int aprev = a == 0 ? na - 1 : a - 1;
      const float3 co00 = adjverts_in.vert(r_in, a, 0);
      const float3 co01 = adjverts_in.vert(r_in, a, 1);
      const float3 co11 = adjverts_in.vert(r_in - 1, r_in == 1 ? 0 : a, 0);
      const float3 co10 = adjverts_in.vert(r_in, aprev, iside - 1);
      const float3 co_interp = interp_bilinear_quad(
          co00, co01, co11, co10, remainder_in, remainder_in);
      adjverts.mutable_vert(r_out, a, 0) = co_interp;
    }
    if (oside > 1) {
      BLI_assert(iside >= oside);
      /* Make #i_breaks be cumulative lengths of segments in #adverts_in's anchor #a ring. */
      Array<float, 20> i_breaks(iside + 1);
      for (const int a : IndexRange(na)) {
        const int anext = a == na - 1 ? 0 : a + 1;
        i_breaks[0] = 0.0f;
        for (const int o : IndexRange().from_begin_end(1, iside + 1)) {
          i_breaks[o] = i_breaks[o - 1] + math::length(adjverts_in.vert(r_in, a, o) -
                                                       adjverts_in.vert(r_in, a, o - 1));
        }
        /* We'll put the output vertices at equal intervals along #advert's anchor #a ring. */
        const float olen = math::length(adjverts.vert(r_out, anext, 0) -
                                        adjverts.vert(r_out, a, 0));
        const float odiv = olen / oside;
        for (int o_out = 1; o_out < oside; o_out++) {
          const float o_out_pos = (o_out + remainder_in) * odiv;
          /* Where does #o_out_pos fit amount #i_breaks? */
          const int o_in = which_interval(o_out_pos, i_breaks);
          bool o_in_last = o_in == i_breaks.size() - 1;
          const float rem = (o_in == i_breaks.size() - 1) ? 0.0f : o_out_pos - i_breaks[o_in];
          const float o_in_seg_len = o_in_last ? 0.0f : i_breaks[o_in + 1] - i_breaks[o_in];
          const float rem_frac = (o_in_seg_len == 0.0f) ? 0.0f : rem / o_in_seg_len;
          const float3 co00 = adjverts_in.vert(r_in, a, o_in);
          const float3 co01 = adjverts_in.vert(r_in, a, o_in_last ? o_in : o_in + 1);
          const float3 co11 = adjverts_in.vert(r_in - 1, a, o_in_last ? o_in : o_in + 1);
          const float3 co10 = adjverts_in.vert(r_in - 1, a, o_in);
          const float3 co_interp = interp_bilinear_quad(
              co00, co01, co11, co10, rem_frac, remainder_in);
          adjverts.mutable_vert(r_out, a, o_out) = co_interp;
        }
      }
    }
  }
  /* Outer ring comes from the boundary profile. */
  const int oside = v_anchor_div(num_rings_out - 1, na, ns_out);
  for (const int a : IndexRange(na)) {
    for (const int o : IndexRange(oside)) {
      float3 co = profile::get_profile_point(profiles[a], o, ns_out);
      adjverts.mutable_outer_ring_vert(a, o) = co;
    }
  }
}

}  // namespace adj

namespace facerep {

static int choose_face_rep(Span<int> faces, const BevelState &bs)
{
  /* The tie breaking values are, in order:
   *  + component id of face in each uv_map, in the order we found them
   *  + material index
   *  + z component of face center
   *  + x component of face center
   *  + y component of face center
   * We want the face that has the lexicographically lowest value of these.
   */
  if (faces.size() == 1) {
    return faces[0];
  }
  const int value_len = bs.uvmaps_num + 4;
  const int numf = faces.size();
  using ValueVec = Array<float, 6>;
  Array<ValueVec, 20> values(numf, ValueVec(value_len));
  const Mesh &mesh = bs.mesh_info.mesh;
  for (const int i : IndexRange(numf)) {
    const int f = faces[i];
    if (f == -1) {
      values[i][0] = 1e10f;
      continue;
    }
    int value_index = 0;
    for (int j : IndexRange(bs.uvmaps_num)) {
      values[i][value_index++] = float(bs.uv_map_info(j).component_id(f));
    }
    values[i][value_index++] = bs.material(f);
    float3 center = bke::mesh::face_center_calc(mesh.vert_positions(),
                                                mesh.corner_verts().slice(mesh.faces()[f]));
    values[i][value_index++] = center[2];
    values[i][value_index++] = center[0];
    values[i][value_index++] = center[1];
    BLI_assert(value_index == value_len);
  }
  auto it = std::min_element(
      values.begin(), values.end(), [](const ValueVec &a, const ValueVec &b) {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
      });
  if (it != values.end()) {
    return faces[std::distance(values.begin(), it)];
  }
  return faces[0];
}

/**
 * Return a good representative face (for materials, etc.) for faces
 * created around/near anchor \a anchor of bevvert \a bv.
 * Sometimes care about a second choice, if there is one.
 * If r_fother parameter is non-nullptr and there is another, different,
 * possible frep, return the other one in that parameter.
 *
 * This code copies the old BMesh bevel boundvert_rep_face()
 * but I don't think it is particularly "right".  TODO: look again.
 */
static int anchor_rep_face(const int bv, const int anchor, int *r_fother, const BevelState &bs)
{
  /* make an analog of the code in bmesh_bevel.c, boundvert_rep_face() */
  int frep = -1;
  /* Get positions of first and last edges attached to anchor. */
  const int2 edge_positions = bs.anchor_bevedge_positions(bv, anchor);
  const int pos_first = edge_positions[0];
  const int pos_last = edge_positions[1];
  const int be_last = bs.bevvert_bevedges()[bv][pos_last];
  if (pos_first != -1) {
    BLI_assert(pos_last != -1);
    if (bs.bevedge_is_beveled(be_last)) {
      frep = bs.face_prev(bv, pos_last);
    }
    else {
      frep = bs.face_prev(bv, pos_first);
      if (frep == -1) {
        frep = bs.face_next(bv, pos_first);
        if (frep == -1) {
          frep = bs.face_prev(bv, pos_last);
        }
      }
    }
  }
  else {
    const int anchor_prev = bs.bevvert_meshpatterns()[bv].prev_anchor(anchor);
    const int2 prev_edge_positions = bs.anchor_bevedge_positions(bv, anchor_prev);
    frep = bs.face_next(bv, prev_edge_positions[1]);
    if (frep == -1) {
      const int anchor_next = bs.bevvert_meshpatterns()[bv].next_anchor(anchor);
      const int2 next_edge_positions = bs.anchor_bevedge_positions(bv, anchor_next);
      frep = bs.face_prev(bv, next_edge_positions[0]);
    }
  }
  if (r_fother && frep != -1) {
    int frep_other = -1;
    if (pos_first != -1) {
      if (bs.bevedge_is_beveled(be_last)) {
        frep_other = bs.face_prev(bv, pos_first);
      }
      else {
        frep_other = bs.face_next(bv, pos_last);
        if (frep_other == -1 || frep_other == frep) {
          frep_other = bs.face_next(bv, pos_first);
        }
        if (frep_other == -1 || frep_other == frep) {
          frep_other = bs.face_prev(bv, pos_first);
        }
      }
    }
    else {
      const int anchor_next = bs.bevvert_meshpatterns()[bv].next_anchor(anchor);
      const int2 next_edge_positions = bs.anchor_bevedge_positions(bv, anchor_next);
      if (next_edge_positions[0] != -1) {
        frep_other = bs.face_prev(bv, next_edge_positions[0]);
      }
    }
    *r_fother = frep_other == frep ? -1 : frep_other;
  }
  return frep;
}

/**
 * If we make a poly out of anchor verts around \a bv, snapping to face \a f,
 * will uv poly have zero area?
 * The uv poly is made by snapping all outside-of-f vertices to the closest edge in \a f.
 * Sometimes this results in a zero or very small area polygon, which translates to a zero
 * or very small area polygon in UV space -- not good for interpolating textures.
 */
static bool is_bad_uv_poly(const int bv, const int f, const BevelState &bs)
{
  const Mesh &mesh = bs.mesh_info.mesh;
  Span<float3> vert_positions = mesh.vert_positions();
  Span<int> corner_verts = mesh.corner_verts();
  float3x3 axis_mat;
  /* Project the vertices of f to 2d along its dominant axis. */
  const int mesh_v = bs.bevvert_mesh_verts()[bv];
  axis_dominant_v3_to_m3(axis_mat.ptr(), mesh.face_normals()[f]);
  IndexRange face = mesh.faces()[f];
  Array<float2, 20> face_co_proj(face.size());
  for (const int i : face_co_proj.index_range()) {
    const float3 co = vert_positions[corner_verts.slice(face)[i]];
    face_co_proj[i] = float2(transform_point(axis_mat, co));
  }
  /* Find the adjacent edges to bv, and the positions of their ends. */
  int2 v_edges = get_incident_edges(f, mesh_v, mesh);
  float3 v_edge_ends[2][2];
  for (const int i : IndexRange(2)) {
    const int2 e = mesh.edges()[v_edges[i]];
    for (const int j : IndexRange(2)) {
      v_edge_ends[i][j] = vert_positions[e[j]];
    }
  }
  /* Project the anchor verts for bv onto the same 3d plane.
   * If the projection isn't inside the projected face, snap to an
   * edge first and then reproject.
   */
  const MeshPattern pat = bs.bevvert_meshpatterns()[bv];
  Array<float2, 20> anchor_co_proj(pat.num_anchors);
  const int first_newv = bs.bevvert_newverts()[bv][0];
  for (int a : IndexRange(pat.num_anchors)) {
    const int a_newv = first_newv + pat.anchor_vert(a);
    float3 anchor_co = bs.newvert_positions()[a_newv];
    anchor_co_proj[a] = float2(transform_point(axis_mat, anchor_co));
    if (!isect_point_poly_v2(anchor_co_proj[a],
                             reinterpret_cast<const float (*)[2]>(face_co_proj.data()),
                             pat.num_anchors))
    {
      /* Need to snap anchor_co to the nearest incident edge. */
      float3 snap1, snap2;
      closest_to_line_segment_v3(snap1, anchor_co, v_edge_ends[0][0], v_edge_ends[0][1]);
      closest_to_line_segment_v3(snap1, anchor_co, v_edge_ends[0][0], v_edge_ends[0][1]);
      const float d1_sq = distance_squared(snap1, anchor_co);
      const float d2_sq = distance_squared(snap2, anchor_co);
      if (d1_sq <= d2_sq) {
        anchor_co_proj[a] = float2(transform_point(axis_mat, snap1));
      }
      else {
        anchor_co_proj[a] = float2(transform_point(axis_mat, snap2));
      }
    }
  }
  const float area = area_poly_v2(reinterpret_cast<const float (*)[2]>(anchor_co_proj.data()),
                                  anchor_co_proj.size());
  return area < geom::bevel_epsilon_big;
}

/**
 * Pick a good face from all the faces around \a bv to use for
 * a representative face, using choose_rep_face.
 * We want to choose from among the faces that would be
 * chosen for a single-segment edge polygon between two successive
 * anchor verts.
 * But if \a for_interp is true, we also want to make sure it will
 * make an acceptable polygon in UV space if we interpolate here.
 *
 * The single beveled edge is a special case,
 * where we also want to consider the third face (else can get
 * zero-area UV interpolated face).
 *
 * If there are uv maps, then don't include faces that would result
 * in zero-area UV polygons if chosen as the rep.
 */
static int find_center_face_rep(const int bv, const bool for_interp, const BevelState &bs)
{
  int any_face = -1;
  bool consider_all_faces = for_interp && bs.bevvert_beveled_edges_num(bv) == 1;
  Span<int> bes = bs.bevvert_bevedges()[bv];
  const int edge_count = bes.size();
  VectorSet<int, 20> fchoices;
  for (const int epos : IndexRange(edge_count)) {
    if (!bs.bevedge_is_beveled(bes[epos]) && !consider_all_faces) {
      continue;
    }
    const int f1 = bs.face_prev(bv, epos);
    const int f2 = bs.face_next(bv, epos);
    if (f1 == -1 && f2 == -1) {
      continue;
    }
    const int f = choose_face_rep({f1, f2}, bs);
    if (any_face == -1) {
      any_face = f;
    }
    if (for_interp && bs.uvmaps_num > 0 && is_bad_uv_poly(bv, f, bs)) {
      continue;
    }
    fchoices.add(f);
  }
  if (fchoices.size() == 0) {
    return any_face;
  }
  return choose_face_rep(fchoices.as_span(), bs);
}

}  // namespace facerep

namespace uv {

/** Return true if all uv's in the given uv_map are contiguous at bevvert \a bv.
 * If \a uv_map_index is -1, return true if all uv_maps are contiguous there.  */
[[maybe_unused]] static bool bevvert_is_uv_contiguous(const int bv,
                                                      const int uv_map_index,
                                                      const BevelState &bs)
{
  const int mesh_v = bs.bevvert_mesh_verts()[bv];
  for (const int i : IndexRange(bs.uvmaps_num)) {
    if (uv_map_index != -1 && i != uv_map_index) {
      continue;
    }
    const UVMapInfo &info = bs.uv_map_info(uv_map_index);
    Span<int> corners = bs.mesh_info.vert_corners()[mesh_v];
    if (corners.size() == 0) {
      continue;
    }
    float2 first_val = info.value(corners[0]);
    for (int i = 1; i < corners.size(); i++) {
      if (info.value(corners[i]) != first_val) {
        return false;
      }
    }
  }
  return true;
}

/** Return a pair of bools, where says whether the edge at position \a epos of bevvert \a bv
 * has contiguous UVs for the UV map \a uv_map_index, at the near and far ends, respectively..
 * This means that the two faces (prev and next) have corners at corresponding end with equal UV
 * values.  */
static std::pair<bool, bool> uv_contiguousness(const int bv,
                                               const int epos,
                                               const int uv_map_index,
                                               const BevelState &bs)
{
  const int fprev = bs.face_prev(bv, epos);
  const int fnext = bs.face_next(bv, epos);
  if (fprev == -1 || fnext == -1) {
    return {false, false};
  }
  const int be = bs.bevvert_bevedges()[bv][epos];
  const int mesh_e = bs.bevedge_mesh_edges()[be];
  const int2 mesh_e_verts = bs.mesh_info.mesh.edges()[mesh_e];
  const int end = bs.bevedge_vert_end(bv, be);
  const int other_end = 1 - end;
  const int2 mesh_vs = {mesh_e_verts[end], mesh_e_verts[other_end]};
  const UVMapInfo &info = bs.uv_map_info(uv_map_index);
  OffsetIndices<int> faces = bs.mesh_info.mesh.faces();
  Span<int> corner_verts = bs.mesh_info.mesh.corner_verts();
  Array<bool, 2> ans(2);
  for (const int i : IndexRange(2)) {
    const int cprev = bke::mesh::face_find_corner_from_vert(
        faces[fprev], corner_verts, mesh_vs[i]);
    const int cnext = bke::mesh::face_find_corner_from_vert(
        faces[fnext], corner_verts, mesh_vs[i]);
    BLI_assert(cprev != -1 && cnext != -1);
    float2 uvprev = info.value(cprev);
    float2 uvnext = info.value(cnext);
    ans[i] = uvprev == uvnext;
  }
  return std::pair<bool, bool>(ans[0], ans[1]);
}

enum class UVGapKind { NoGap, WedgeGap, WideGap, UnknownGap };

/** Find the kind of UV gap is there between the faces separated by each edge around bevvert \a bv.
 * Assuming there is a previous and next Face separated by bevedge, thre should be two corners
 * that share bevvert's mesh vertex. If the UV values are the same at those two corners, and if it
 * is the same at the other end of bevedge, then there is NoGap.
 * If they are the same at the bevvert end but not at the far end, and also the angle in UV space
 * is is not obtuse or straight, then this is a WedgeGap. Otherwise this is a WideGap.
 */
static Array<UVGapKind, 20> bevvert_bevedge_uv_gaps(const int bv,
                                                    const int uv_map_index,
                                                    const BevelState &bs)
{
  BLI_assert(uv_map_index != -1);
  Span<int> bevedges = bs.bevvert_bevedges()[bv];
  Array<UVGapKind, 20> ans(bevedges.size(), UVGapKind::UnknownGap);
  for (const int epos : bevedges.index_range()) {
    std::pair<bool, bool> uv_contigs = uv_contiguousness(bv, epos, uv_map_index, bs);
    if (uv_contigs.first) {
      if (uv_contigs.second) {
        ans[epos] = UVGapKind::NoGap;
      }
      else {
        /* TODO: non-obtuse angle check. */
        ans[epos] = UVGapKind::WedgeGap;
      }
    }
    else {
      ans[epos] = UVGapKind::WideGap;
    }
  }
  return ans;
}

/** Give a 3D position and a Span of Mesh face indices,  find the face (if any) where the
 * perpendicular projection of \a pos  is inside it. There may be another (if the projection is on
 * the edge between two faces) -- if so, return that too. Return the index of the "over" face in
 * *r_over_face, and the 3D position of its projection in *r_over_pos. If there is an alternative
 * over face, use *r_alt_face and *r_alt_pos to store those. If there is no suitable face for the
 * main or alternate, return -1 in the index. Assume all pointers are non-null.
 * TODO: fix this. projection can equally intersect edge of adjacent face.
 * The real test should be to find which face between the posible anchors for the vert it is over.
 */
static void find_over_faces(const float3 &pos,
                            const Span<int> mesh_faces,
                            const BevelState &bs,
                            int *r_over_face,
                            float3 *r_over_pos,
                            int *r_alt_face,
                            float3 *r_alt_pos)
{
  fmt::println("find_over_faces pos=({},{},{}", pos[0], pos[1], pos[2]);
  const Mesh &mesh = bs.mesh_info.mesh;
  BLI_assert(r_over_face && r_alt_face && r_over_pos && r_alt_pos);

  *r_over_face = -1;
  *r_alt_face = -1;

  for (const int mesh_f : mesh_faces) {
    BLI_assert(mesh_f != -1);
    fmt::println("  try face {}", mesh_f);

    const float3 &face_no = mesh.face_normals()[mesh_f];
    const IndexRange face_corners_indices = mesh.faces()[mesh_f];
    BLI_assert(face_corners_indices.size() >= 3);
    const int first_vert_index = mesh.corner_verts()[face_corners_indices[0]];
    const float3 face_co = mesh.vert_positions()[first_vert_index];
    float4 plane;
    plane_from_point_normal_v3(plane, face_co, face_no);
    fmt::println("    plane=({},{},{},{}", plane[0], plane[1], plane[2], plane[3]);

    float3 projected_pos;
    closest_to_plane_normalized_v3(projected_pos, plane, pos);
    fmt::println("    projected_pos=({},{},{})", projected_pos[0], projected_pos[1], projected_pos[2]);

    float3x3 axis_mat;
    axis_dominant_v3_to_m3(axis_mat.ptr(), face_no);

    const int face_len = face_corners_indices.size();
    Array<float2, 20> poly_2d(face_len);

    for (int i = 0; i < face_len; i++) {
      const int v_idx = mesh.corner_verts()[face_corners_indices[i]];
      mul_v2_m3v3(poly_2d[i], axis_mat.ptr(), mesh.vert_positions()[v_idx]);
      fmt::println("      {}: ({},{})", i, poly_2d[i][0], poly_2d[i][1]);
    }

    float2 p_2d;
    mul_v2_m3v3(p_2d, axis_mat.ptr(), projected_pos);
    fmt::println("    p_2d=({},{})", p_2d[0], p_2d[1]);

    /* TODO: do better when the intersection is "near" the edge between two faces. */
    if (isect_point_poly_v2(p_2d, (const float (*)[2])poly_2d.data(), face_len)) {
      fmt::println("    intersects!");
      if (*r_over_face == -1) {
        *r_over_face = mesh_f;
        *r_over_pos = projected_pos;
      }
      else if (*r_alt_face == -1) {
        *r_alt_face = mesh_f;
        *r_alt_pos = projected_pos;
      }
    }
  }
}

/** Project the given Mesh face to 2d and return the array of 2d positions of its vertices.
 * Also return in \a r_axis_mat the transformation matrix that will project any other 3d point
 * onto the same plane.
 */
static Array<float2, 20> project_face_to_2d(const int mesh_face,
                                            const Mesh &mesh,
                                            float3x3 &r_axis_mat)
{
  const IndexRange face_corners = mesh.faces()[mesh_face];
  const int face_size = face_corners.size();
  Array<float2, 20> pos_2d(face_size);
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<float3> vert_positions = mesh.vert_positions();

  const float3 face_normal = mesh.face_normals()[mesh_face];
  axis_dominant_v3_to_m3(r_axis_mat.ptr(), face_normal);
  for (const int i : face_corners.index_range()) {
    const int c = face_corners[i];
    float3 pos_3d = vert_positions[corner_verts[c]];
    pos_2d[i] = float2(transform_point(r_axis_mat, pos_3d));
  }
  return pos_2d;
}

/** Given a Span of 2d positions of a face, and a Span of corresponding values,
 * return the interpolation of those values at another 2d point, \a interp_pos.
 */
static float2 interp_uv_2d(const Span<float2> &positions,
                           const Span<float2> values,
                           const float2 interp_pos)
{
  const int face_size = positions.size();
  BLI_assert(face_size >= 3 && values.size() == face_size);
  Array<float, 20> bary_weights(face_size);
  if (face_size == 3) {
    barycentric_weights_v2(
        positions[0], positions[1], positions[2], interp_pos, bary_weights.data());
  }
  else if (face_size == 4) {
    barycentric_weights_v2_quad(
        positions[0], positions[1], positions[2], positions[3], interp_pos, bary_weights.data());
  }
  else {
    interp_weights_poly_v2(bary_weights.data(),
                           reinterpret_cast<float (*)[2]>(const_cast<float2 *>(positions.data())),
                           face_size,
                           interp_pos);
  }
  float2 ans(0.0f, 0.0f);
  for (const int i : IndexRange(face_size)) {
    ans += bary_weights[i] * values[i];
  }
  return ans;
}

/** For the face with index \a f in the adj pattern for bevvert \a bv, calculate the UV position
 * for each of its corners in the UV map with the given \a uv_map_index, and store them in
 * uv_attributes[uv_map_index] at the corresponding newcorner indices.
 */
static void calculate_adj_face_uvs(const int f,
                                   const int bv,
                                   const Array<UVGapKind, 20> &gaps,
                                   const int uv_map_index,
                                   Vector<Array<float2>> &uv_attributes,
                                   const BevelState &bs)
{
  // DEBUG!!
  fmt::println("calculate_adj_face_uvs, bv={}, f={}", bv, f);
  const IndexRange newfaces = bs.bevvert_newfaces()[bv];
  const IndexRange newface_corners_range = bs.newface_faces_face()[newfaces[f]];
  const Span<int> fverts = bs.newcorner_verts().slice(newface_corners_range);
  const int num_fverts = fverts.size();
  SmallIntArray over_face(num_fverts);
  SmallIntArray alt_over_face(num_fverts);
  Array<float3, 20> over_pos(num_fverts);
  Array<float3, 20> alt_over_pos(num_fverts);
  for (const int i : fverts.index_range()) {
    const float3 pos = bs.newvert_positions()[fverts[i]];
    find_over_faces(pos,
                    bs.bevvert_faces()[bv],
                    bs,
                    &over_face[i],
                    &over_pos[i],
                    &alt_over_face[i],
                    &alt_over_pos[i]);
    // DEBUG!!
    fmt::println("i={}, nv={}, pos=({},{},{}), over_f={}, over_pos=({},{},{})",
                 i,
                 fverts[i],
                 pos[0],
                 pos[1],
                 pos[2],
                 over_face[i],
                 over_pos[i][0],
                 over_pos[i][1],
                 over_pos[i][2]);
  }

  const UVMapInfo &uv_info = bs.uv_map_info(uv_map_index);
  const Mesh &mesh = bs.mesh_info.mesh;
  const OffsetIndices<int> mesh_faces = mesh.faces();

  Array<float2, 20> face_uvs(num_fverts);

  for (const int i : fverts.index_range()) {
    const int over_f = over_face[i];
    const float3 over_p = over_pos[i];

    if (over_f == -1) {
      face_uvs[i] = float2(0.0f, 0.0f); /* Default UV. */
      continue;
    }

    const IndexRange over_face_corners = mesh_faces[over_f];
    const int num_over_face_corners = over_face_corners.size();

    if (num_over_face_corners < 3) { /* Degenerate face. */
      face_uvs[i] = float2(0.0f, 0.0f);
      continue;
    }

    float3x3 axis_mat;
    Array<float2, 20> over_face_vert_pos_2d = project_face_to_2d(over_f, mesh, axis_mat);
    Array<float2, 20> over_face_corner_uvs(num_over_face_corners);

    for (const int j : over_face_corners.index_range()) {
      const int corner_idx = over_face_corners[j];
      over_face_corner_uvs[j] = uv_info.value(corner_idx);
    }

    float2 over_pos_2d = float2(transform_point(axis_mat, over_p));
    face_uvs[i] = interp_uv_2d(over_face_vert_pos_2d, over_face_corner_uvs, over_pos_2d);
  }

  for (const int i : fverts.index_range()) {
    const int newcorner_idx = newface_corners_range[i];
    uv_attributes[uv_map_index][newcorner_idx] = face_uvs[i];
    //! DEBUG
    fmt::println(
        "uv[{}][{}] = ({},{})", uv_map_index, newcorner_idx, face_uvs[i][0], face_uvs[i][1]);
  }
}

static void calculate_vertex_mesh_face_uvs(const int bevvert,
                                           const int uv_map_index,
                                           Vector<Array<float2>> &uv_attributes,
                                           const BevelState &bs)
{
  // DEBUG!!
  fmt::println("calculate vertex mesh uvs for bevvert {}, uv map {}", bevvert, uv_map_index);
  Array<UVGapKind, 20> gaps = bevvert_bevedge_uv_gaps(bevvert, uv_map_index, bs);
  const MeshPattern &pat = bs.bevvert_meshpatterns()[bevvert];
  int4 nums = pat.num_elements();
  switch (pat.kind) {
    case MeshKind::Adj: {
      for (const int f : IndexRange(nums[2])) {
        calculate_adj_face_uvs(f, bevvert, gaps, uv_map_index, uv_attributes, bs);
      }
      break;
    }
    case MeshKind::Line: {
      break;
    }
    default: {
      // TODO
      fmt::println("Implement me: other cases in calculate_vertex_mesh_face_uvs");
    }
  }
}

static void calculate_edge_mesh_uvs(const int bevedge,
                                    const int uv_map_index,
                                    Vector<Array<float2>> &uv_attributes,
                                    const BevelState &bs)
{
  // TODO: Implement me.
  fmt::println("calculate_edge_mesh_face_uvs not implemented, bevedge={}, uv_map_index={}",
               bevedge,
               uv_map_index);
}

static void calculate_face_mesh_uvs(const int bevface,
                                    const int uv_map_index,
                                    Vector<Array<float2>> &uv_attributes,
                                    const BevelState &bs)
{
  fmt::println("calculate_face_mesh_face_uvs, bevface={}, uv_map_index={}", bevface, uv_map_index);
  const UVMapInfo &uv_info = bs.uv_map_info(uv_map_index);
  const Mesh &mesh = bs.mesh_info.mesh;
  const Span<float3> mesh_vert_positions = mesh.vert_positions();
  const OffsetIndices<int> mesh_faces = mesh.faces();
  const int mesh_face = bs.bevface_mesh_faces()[bevface];
  const int new_face = bs.bevface_newfaces()[bevface][0];
  fmt::println("mesh_face = {}, new_face = {}", mesh_face, new_face);
  const IndexRange orig_face_corners = mesh_faces[mesh_face];
  const int num_orig_face_corners = orig_face_corners.size();
  const IndexRange newface_corners = bs.newface_faces_face()[new_face];
  const Span<float3> new_positions = bs.newvert_positions();

  /* Values needed for interpolation. */
  float3x3 axis_mat;
  Array<float2, 20> orig_face_vert_pos_2d = project_face_to_2d(mesh_face, mesh, axis_mat);

  Array<float2, 20> orig_face_corner_uvs(num_orig_face_corners);
  for (const int i : orig_face_corners.index_range()) {
    orig_face_corner_uvs[i] = uv_info.value(orig_face_corners[i]);
  }

  for (const int c : newface_corners) {
    const int v = bs.newcorner_verts()[c];
    fmt::println("process corner c={}, v={}", c, v);
    /* In the newfaces, corner vertices are encoded where newverts are as is,
     * but original verts are encoded as -(meshv + 1). */
    const float3 pos = (v < 0) ? mesh_vert_positions[-(v + 1)] : new_positions[v];
    float2 pos_2d = float2(transform_point(axis_mat, pos));
    fmt::println(
        "   pos=({},{},{}); pos_2d=({},{})", pos[0], pos[1], pos[2], pos_2d[0], pos_2d[1]);
    /* TOOD: use original values of UVs for corners that are original verts. */
    uv_attributes[uv_map_index][c] = interp_uv_2d(
        orig_face_vert_pos_2d, orig_face_corner_uvs, pos_2d);
    fmt::println(
        "   uv = ({},{})", uv_attributes[uv_map_index][c][0], uv_attributes[uv_map_index][c][1]);
  }
}

}  // end namespace uv

UVMapInfo::UVMapInfo(const std::string &uv_attr_name, const Mesh &mesh)
    : uv_attr_name(uv_attr_name)
{
  const bke::AttributeAccessor attrs = mesh.attributes();
  const bke::AttributeReader<float2> uv_reader = attrs.lookup<float2>(uv_attr_name,
                                                                      bke::AttrDomain::Corner);
  if (uv_reader) {
    values_ = Array<float2>(mesh.corners_num);
    uv_reader.varray.materialize(values_.as_mutable_span());
  }
}

/** Each face in 3d space maps to a face in UV space. Find ids such that if two faces have
 * the same ID, then they are in the same UV island.
 */
void UVMapInfo::find_components(const MeshInfo &mesh_info)
{
  const Mesh &mesh = mesh_info.mesh;
  face_to_component_id_ = Array<int>(mesh.faces_num, -1);
  if (mesh.faces_num == 0) {
    return;
  }

  GroupedSpan<int> vert_to_corners = mesh_info.vert_corners();
  AtomicDisjointSet dsu(mesh.faces_num);

  threading::parallel_for(IndexRange(mesh.verts_num), 5000, [&](IndexRange range) {
    for (const int v : range) {

      Span<int> corners = vert_to_corners[v];
      if (corners.size() <= 1) {
        continue;
      }

      /* Group corners by UV value. */
      struct CornerUV {
        float2 uv;
        int corner_idx;
      };
      Array<CornerUV> corner_uvs(corners.size());
      for (const int i : corners.index_range()) {
        corner_uvs[i] = {values_[corners[i]], corners[i]};
      }

      std::sort(corner_uvs.begin(), corner_uvs.end(), [](const CornerUV &a, const CornerUV &b) {
        if (a.uv[0] != b.uv[0]) {
          return a.uv[0] < b.uv[0];
        }
        return a.uv[1] < b.uv[1];
      });

      /* Join faces in the same group. */
      for (int i = 0; i < corner_uvs.size();) {
        int j = i + 1;
        while (j < corner_uvs.size() && corner_uvs[j].uv == corner_uvs[i].uv) {
          j++;
        }
        /* Group is corner_uvs[i..j-1] */
        if (j > i + 1) {
          const int first_face_idx = mesh_info.corner_face(corner_uvs[i].corner_idx);
          for (int k = i + 1; k < j; k++) {
            const int other_face_idx = mesh_info.corner_face(corner_uvs[k].corner_idx);
            dsu.join(first_face_idx, other_face_idx);
          }
        }
        i = j;
      }
    }
  });

  /* Normalize component IDs. */
  dsu.calc_reduced_ids(face_to_component_id_.as_mutable_span());
}

/** Return a 4-tuple with the number of vertices, edges, faces, corners.  */
int4 MeshPattern::num_elements() const
{
  int4 ans;
  switch (this->kind) {
    case MeshKind::None:
      ans = int4(0, 0, 0, 0);
      break;
    case MeshKind::Line:
      ans = int4(num_anchors + num_segs - 1, num_segs, 0, 0);
      break;
    case MeshKind::TerminalPoly:
      ans = int4(
          num_anchors + num_segs - 2, num_anchors + num_segs - 2, 1, num_anchors + num_segs - 1);
      break;
    case MeshKind::Adj: {
      int totf = adj::f_total_faces(num_anchors, num_segs);
      ans = int4(adj::v_total_verts(num_anchors, num_segs),
                 adj::e_num_edges(num_anchors, num_segs),
                 totf,
                 4 * totf + (adj::odd(num_segs) ? num_anchors - 4 : 0));
      break;
    }
    case MeshKind::TriFan:
      ans = int4(num_segs + 2, num_segs + 2, num_segs, 3 * num_segs);
      break;
    case MeshKind::Cutoff:
      /* TODO */
      BLI_assert(false);
      ans = int4(0, 0, 0, 0);
  }
  return ans;
}

/** Return the index of the vertex in this MeshPattern for the given anchor vertex. */
int MeshPattern::anchor_vert(int anchor_index) const
{
  int ans;
  BLI_assert(0 <= anchor_index && anchor_index < this->num_anchors);
  switch (this->kind) {
    case MeshKind::None:
      ans = 0;
      break;
    case MeshKind::Line:
      ans = anchor_index == 0 ? 0 : this->num_segs;
      break;
    case MeshKind::TerminalPoly:
      ans = anchor_index == 0 ? 0 : this->num_segs + anchor_index;
      break;
    case MeshKind::Adj: {
      const int ring = adj::v_num_rings(this->num_segs) - 1;
      ans = adj::v_ringstart(ring, this->num_anchors, this->num_segs);
      ans += anchor_index * adj::v_anchor_div(ring, this->num_anchors, this->num_segs);
      break;
    }
    case MeshKind::TriFan:
      ans = anchor_index == 0 ? 0 : this->num_segs + anchor_index;
      break;
    case MeshKind::Cutoff:
      /* TODO */
      BLI_assert(false);
      ans = 0;
      break;
  }
  BLI_assert(0 <= ans && ans < this->num_elements()[0]);
  return ans;
}

/** Inverse of #anchor_vert. Return -1 if there is no such anchor vert. */
int MeshPattern::vert_to_anchor(const int v) const
{
  int ans;
  BLI_assert(0 <= v && v < this->num_elements()[0]);
  switch (this->kind) {
    case MeshKind::Adj: {
      const int ring = adj::v_num_rings(this->num_segs) - 1;
      const int ringstart = adj::v_ringstart(ring, this->num_anchors, this->num_segs);
      const int k = v - ringstart;
      const int div = adj::v_anchor_div(ring, this->num_anchors, this->num_segs);
      ans = (k < 0 || (k % div) != 0) ? -1 : k / div;
      break;
    }
    case MeshKind::TerminalPoly:
    case MeshKind::TriFan:
      ans = v == 0 ? 0 : (v > this->num_segs ? v - this->num_segs : -1);
      break;
    case MeshKind::Cutoff:
      /* TODO */
      BLI_assert(false);
    default:
      ans = -1;
      break;
  }
  return ans;
}

/** Return arrays of the verts and edges for pattern face \a face,
 * where \a first_v and \a first_e give the offsets to add to pattern space indices.
 */
std::pair<SmallIntArray, SmallIntArray> MeshPattern::face_verts_and_edges(const int face,
                                                                          const int first_v,
                                                                          const int first_e) const
{
  int4 vefc_nums = this->num_elements();
  BLI_assert(0 <= face && face < vefc_nums[2]);
  std::pair<SmallIntArray, SmallIntArray> ans;
  SmallIntArray &ans_verts = ans.first;
  SmallIntArray &ans_edges = ans.second;
  switch (this->kind) {
    case MeshKind::TerminalPoly: {
      /* There is only one face. */
      BLI_assert(face == 0);
      const int n = num_anchors + num_segs - 2;
      ans_verts.reinitialize(n);
      ans_edges.reinitialize(n);
      std::iota(ans_verts.begin(), ans_verts.end(), 0);
      std::iota(ans_edges.begin(), ans_edges.end(), 0);
      break;
    }
    case MeshKind::Adj: {
      if (face == 0 && (num_segs % 2) == 1) {
        /* Center polygon is an ngon with num_anchors sides. */
        ans_verts.reinitialize(num_anchors);
        ans_edges.reinitialize(num_anchors);
        std::iota(ans_verts.begin(), ans_verts.end(), 0);
        std::iota(ans_edges.begin(), ans_edges.end(), 0);
      }
      else {
        std::pair<int4, int4> vs_es = adj::face_vertices_and_edges(face, num_anchors, num_segs);
        ans_verts.reinitialize(4);
        ans_edges.reinitialize(4);
        for (const int i : IndexRange(4)) {
          ans_verts[i] = vs_es.first[i];
          ans_edges[i] = vs_es.second[i];
        }
      }
      break;
    }
    case MeshKind::TriFan: {
      BLI_assert(0 <= face && face < num_segs);
      ans_verts.reinitialize(3);
      ans_edges.reinitialize(3);
      ans_verts[0] = face;
      ans_verts[1] = face + 1;
      ans_verts[3] = num_segs + 1;
      ans_edges[0] = face;
      ans_edges[1] = face + num_segs;
      ans_edges[2] = face + num_segs + 1;
      break;
    }
    case MeshKind::Cutoff:
      /* TODO */
      BLI_assert(false);
      break;
    case MeshKind::Line:
    case MeshKind::None:
      /* These have no faces. */
      BLI_assert_unreachable();
      break;
  }
  for (int &v : ans_verts) {
    v = v + first_v;
  }
  for (int &e : ans_edges) {
    e = e + first_e;
  }
  return ans;
}

/** Return arrays of the verts and edges for pattern boundary, going clockwise
 * from \a firtpos to \a lastpos, and where \a first_v and \a first_e give the
 * offsets to add to pattern space indices.
 * Note that in the current use of this function, the last element of the returned
 * edges array will not be used, as the final edge of a face reconstruction leaves
 * the bevvert rather than continuing around it.
 */
std::pair<SmallIntArray, SmallIntArray> MeshPattern::boundary_vert_and_edges(
    const int firstpos, const int lastpos, const int first_v, const int first_e) const
{
  int4 vefc_nums = this->num_elements();
  const int v_num = vefc_nums[0];
  std::pair<SmallIntArray, SmallIntArray> ans;
  SmallIntArray &ans_verts = ans.first;
  SmallIntArray &ans_edges = ans.second;
  switch (this->kind) {
    case MeshKind::TerminalPoly: {
      const int ans_size = (firstpos - lastpos + 1 + v_num) % v_num;
      ans_verts.reinitialize(ans_size);
      ans_edges.reinitialize(ans_size);
      int pos = firstpos;
      for (const int i : IndexRange(ans_size)) {
        ans_verts[i] = pos;
        const int pos_prev = (pos - 1 + v_num) % v_num;
        ans_edges[i] = pos_prev;
        pos = pos_prev;
      }
      break;
    }
    case MeshKind::Adj: {
      const int ring = adj::v_num_rings(num_segs) - 1;
      const int ring_len = adj::v_ringlen(ring, num_anchors, num_segs);
      const int ans_size = (firstpos - lastpos + 1 + ring_len) % ring_len;
      const int ring_first_v = adj::v_ringstart(ring, num_anchors, num_segs);
      const int ring_first_e = adj::e_ringstart(ring, num_anchors, num_segs);
      ans_verts.reinitialize(ans_size);
      ans_edges.reinitialize(ans_size);
      int pos = firstpos;
      for (const int i : IndexRange(ans_size)) {
        ans_verts[i] = pos;
        ans_edges[i] = pos > 0 ? ring_first_e + (pos - ring_first_v) - 1 :
                                 ring_first_e + ring_len - 1;
        pos = (pos - 1 + ring_len) % ring_len;
      }
      break;
    }
    case MeshKind::TriFan: {
      const int ans_size = (firstpos - lastpos + 1 + v_num) % v_num;
      const int num_e = vefc_nums[1];
      ans_verts.reinitialize(ans_size);
      ans_edges.reinitialize(ans_size);
      int pos = firstpos;
      for (const int i : IndexRange(ans_size)) {
        ans_verts[i] = pos;
        ans_edges[i] = (pos == 0) ? num_segs : (pos == v_num - 1 ? num_e - 1 : pos - 1);
        pos = (pos - 1 + v_num) % v_num;
      }
      break;
    }
    case MeshKind::Line: {
      /* Since there are two sides: we are going up or down depending
       * on which of firstpos and lastpos is bigger. Wraparound is not possible.
       */
      int diff = firstpos - lastpos;
      if (diff >= 0) {
        const int ans_size = diff + 1;
        ans_verts.reinitialize(ans_size);
        ans_edges.reinitialize(ans_size);
        int pos = firstpos;
        for (const int i : IndexRange(ans_size)) {
          ans_verts[i] = pos;
          /* Note: ok if the last number is -1, since it won't be used. */
          ans_edges[i] = pos - 1;
          pos--;
        }
      }
      else {
        const int ans_size = -diff + 1;
        ans_verts.reinitialize(ans_size);
        ans_edges.reinitialize(ans_size);
        int pos = firstpos;
        for (const int i : IndexRange(ans_size)) {
          ans_verts[i] = pos;
          ans_edges[i] = pos;
          pos++;
        }
      }
      break;
    }
    case MeshKind::Cutoff:
      /* TODO */
      BLI_assert(false);
      break;
    case MeshKind::None:
      BLI_assert_unreachable();
      break;
  }
  for (int &v : ans_verts) {
    v = v + first_v;
  }
  for (int &e : ans_edges) {
    e = e + first_e;
  }
  return ans;
}

/** Return the faces (in pattern space) that are unambiguously nearest to \a anchor. */
SmallIntArray MeshPattern::faces_for_anchor(const int anchor) const
{
  if (num_segs <= 1 || kind != MeshKind::Adj) {
    return SmallIntArray(0);
  }
  const int floor_n2 = num_segs / 2;
  const bool ns_odd = (num_segs % 2) == 1;
  const int num_face_rings = adj::f_num_rings(num_segs);
  SmallIntArray ans(floor_n2 * floor_n2);
  int ans_pos = 0;
  for (int r = ns_odd ? 1 : 0; r < num_face_rings; r++) {
    const int ring_side = adj::f_ringlen(r, num_anchors, num_segs) / num_anchors + 1;
    const int half_ring_side = ring_side / 2;
    const int anchor_prev = anchor > 0 ? anchor - 1 : num_anchors - 1;
    const int v = adj::rao_to_face(r, anchor, 0, num_anchors, num_segs);
    ans[ans_pos++] = v;
    for (int o = 1; o < half_ring_side; o++) {
      ans[ans_pos++] = adj::rao_to_face(r, anchor, o, num_anchors, num_segs);
      ans[ans_pos++] = adj::rao_to_face(r, anchor_prev, ring_side - o - 1, num_anchors, num_segs);
    }
  }
  BLI_assert(ans_pos == ans.size());
  return ans;
}

/** Return the faces (in pattern space) that are on the centerline between \a first_anchor
 * and its successor. Don't include the center face, for odd number of segments. */
SmallIntArray MeshPattern::faces_for_centerline(const int first_anchor) const
{
  if (kind != MeshKind::Adj || num_segs <= 1 || (num_segs % 2) == 0) {
    return SmallIntArray(0);
  }
  const int floor_n2 = num_segs / 2;
  const int num_face_rings = adj::f_num_rings(num_segs);
  SmallIntArray ans(floor_n2);
  BLI_assert(num_face_rings == floor_n2 + 1);
  for (int r = 1; r < num_face_rings; r++) {
    const int ring_side = adj::f_ringlen(r, num_anchors, num_segs) / num_anchors + 1;
    const int half_ring_side = ring_side / 2;
    const int v = adj::rao_to_face(r, first_anchor, half_ring_side, num_anchors, num_segs);
    ans[r - 1] = v;
  }
  return ans;
}

AdjVertKind MeshPattern::adj_vert_kind(const int v, const int anchor) const
{
  if (kind != MeshKind::Adj) {
    return AdjVertKind::Interior;
  }
  int3 rao = adj::v_ring_anchor_offset(v, num_anchors, num_segs);
  const int ring = rao[0];
  const int a = rao[1];
  const int floor_n2 = num_segs / 2;
  if (num_segs % 2 == 0) {
    if (v == 0) {
      return AdjVertKind::Center;
    }
    if (v < floor_n2 || v > floor_n2) {
      return AdjVertKind::Interior;
    }
    return a == anchor ? AdjVertKind::CenterLineNear : AdjVertKind::PrevCenterLineNear;
  }
  else {
    if (ring == 0) {
      return AdjVertKind::Center;
    }
    const int ceil_n2 = (num_segs + 1) / 2;
    if (v < floor_n2 || v > ceil_n2) {
      return AdjVertKind::Interior;
    }
    if (v == floor_n2) {
      return a == anchor ? AdjVertKind::CenterLineNear : AdjVertKind::PrevCenterLineNear;
    }
    return a == anchor ? AdjVertKind::CenterLineFar : AdjVertKind::PrevCenterLineFar;
  }
}

/** Return the ancho or anchorsr that owns (is nearest to) \a f  (in pattern space).
 * For faces on the mid-strip (when odd segments), return both anchors on either
 * side of the mid-strip, otherwise put -1 in the second component of the answer.
 * For the center polygon (when odd segments), return (-1, -1)..
 */
int2 MeshPattern::face_anchor_owner(const int f) const
{
  if (kind != MeshKind::Adj) {
    return int2(-1. -1);
  }
  int3 rao = adj::f_ring_anchor_offset(f, num_anchors, num_segs);
  const int r= rao[0];
  const int a = rao[1];
  const int offset = rao[2];
  const int ring_side = adj::f_ringlen(r, num_anchors, num_segs) / num_anchors + 1;
  const int floor_ring_side_2 = ring_side / 2;
  if (num_segs % 2 == 0) {
    return offset < floor_ring_side_2 ? int2(a, -1) : int2(next_anchor(a), -1);
  }
  if (f == 0) {
    return int2(-1, -1);
  }
  if (offset == floor_ring_side_2) {
    return int2(a, next_anchor(a));
  }
  return offset < floor_ring_side_2 ? int2(a, -1) : int2(next_anchor(a), 1);
}

/** Return an array of the faces that go between the first and last edges at \a anchor.
 * If \a include_prev_and_next is true, also include the faces just before and just after the anchor. */
static SmallIntArray faces_to_next_anchor(const int bv, const int anchor, const bool include_prev_and_next, const BevelState &bs) const
{
  const MeshPattern &pat = bs.bevvert_meshpatterns()[bv];
  const int next anchor = pat.next_anchor(anchor);
  const int prev_anchor = pat.prev_anchor(anchor);
  const int2 edge_poses = bs.anchor_bevedge_positions(bv, anchor);
  const int2 edge_poses_prev = bs.anchor_bevedge_positions(bv, prev_anchor);
  const int2 edge_poses_next = bs.anchor_bevedge_positions(bv, next_anchor);
  Vector<int, 20> faces;
  
}

/** Return a 4-tuple with the number of vertices, edges, faces, corners needed for edge mesh. */
static int4 bevedge_num_elements(const int be, const BevelState &bs)
{
  /* There are ns parallel quad faces, needed.
   * The verts and end edges come from the bevvert meshes, so
   * don't get counted here.
   * Unbeveled edges get 1 new edge, because at least one end of the edge will change
   * and it is cleaner to make a new edge then to do surgery on the original mesh edges.
   */
  if (bs.params.affect_type == BevelAffect::Vertices || !bs.bevedge_is_beveled(be)) {
    return int4(0, 1, 0, 0);
  }
  const int ns = bs.params.segments;
  return int4(0, ns + 1, ns, 4 * ns);
}

/** Return a 4-tuple with the number of vertices, edges, faces, corners needed for face mesh
 * (at the momemt, 'face mesh' just means a single reconstructed face). */
static int4 bevface_num_elements(const int bf, const BevelState &bs)
{
  /* The only tricky part is the number of corners needed. */
  int ncorners = 0;
  const Mesh &mesh = bs.mesh_info.mesh;
  const int mesh_face = bs.bevface_mesh_faces()[bf];
  bool vertex_only = bs.params.affect_type == BevelAffect::Vertices;
  const int segments = bs.params.segments;
  IndexRange face_corners = mesh.faces()[mesh_face];
  int prev_corner = face_corners.last();
  for (const int corner : face_corners) {
    const int mesh_vertex = mesh.corner_verts()[corner];
    const int bv = bs.vert_bevverts()[mesh_vertex];
    if (bv == -1) {
      /* Non-bevel-involved vertex contributes one corner. */
      ncorners += 1;
    }
    else {
      if (vertex_only) {
        ncorners += segments;
      }
      else {
        const MeshPattern &pat = bs.bevvert_meshpatterns()[bv];
        if (pat.kind == MeshKind::Adj) {
          /* TODO: miters. */
          ncorners += 1;
        }
        else {
          /* Is this corner adjacent to a beveled edges? */
          const int mesh_edge = mesh.corner_edges()[corner];
          const int bevedge = bs.edge_bevedges()[mesh_edge];
          const int prev_mesh_edge = mesh.corner_edges()[prev_corner];
          const int prev_bevedge = bs.edge_bevedges()[prev_mesh_edge];
          bool bev_adjacent = (bevedge != -1 && bs.bevedge_is_beveled(bevedge)) ||
                              (prev_bevedge != -1 && bs.bevedge_is_beveled(prev_bevedge));
          if (pat.kind == MeshKind::Line) {
            ncorners += bev_adjacent ? 1 : segments + 1;
          }
          else if (pat.kind == MeshKind::TriFan) {
            const int numedges = bs.bevvert_bevedges()[bv].size();
            ncorners += (numedges == 2 || !bev_adjacent) ? 2 : 1;
          }
          else if (pat.kind == MeshKind::TerminalPoly) {
            ncorners += bev_adjacent ? 1 : 2;
          }
          else {
            BLI_assert_unreachable();
          }
        }
      }
    }
    prev_corner = corner;
  }
  return int4(0, 0, 1, ncorners);
}

/** Assuming \a v is a vert on the outer boundary, return the boundary vert that is \a delta away
 * in the ccw direction along the boundary. Return -1 if there is no such vert.
 * Assume \a v is offset from 0 by \a first_v, and return a similarly offset answer.
 */
int MeshPattern::next_boundary_vert(const int v, const int ccw_delta, const int first_v) const
{
  int ans;
  const int v_num = this->num_elements()[0];
  const int pat_v = v - first_v;
  switch (kind) {
    case MeshKind::None:
      ans = ccw_delta == 0 ? pat_v : -1;
      break;
    case MeshKind::Line:
      ans = pat_v + ccw_delta;
      if (ccw_delta < 0 && ans < 0) {
        /* We want the "prev" of position 0 to be the prev of position v_num.
         * This allows us to traverse both sides of the line.
         */
        ans += v_num - 1;
      }
      else if (ans >= v_num && ccw_delta > 0) {
        /* Go back down the other side. */
        ans = pat_v - ccw_delta;
      }
      break;
    case MeshKind::TerminalPoly:
    case MeshKind::TriFan:
      ans = num_segs == 0 ? pat_v : (pat_v + v_num + ccw_delta) % v_num;
      break;
    case MeshKind::Adj: {
      const int ring = adj::v_num_rings(num_segs) - 1;
      const int ring_len = adj::v_ringlen(ring, num_anchors, num_segs);
      const int v_start = adj::v_ringstart(ring, num_anchors, num_segs);
      ans = v_start + (((pat_v - v_start) + ring_len + ccw_delta) % ring_len);
      break;
    }
    case MeshKind::Cutoff:
      /* TODO */
      ans = -1;
      BLI_assert(false);
  }
  BLI_assert(0 <= ans && ans < v_num);
  return ans + first_v;
}

/** Assuming \a v is a vert on the outer boundary, return the edge that leaves \a v in the ccw
 * direction around the boundary. Return -1 if there is no such edge.
 * Assume \a v is offset from 0 by \a first_v, and return a similarly offset answer.
 */
int MeshPattern::boundary_edge_from_vert(const int v, const int first_v, const int first_e) const
{
  int ans;
  const int pat_v = v - first_v;
  const int4 vefc = this->num_elements();
  const int e_num = vefc[1];
  switch (this->kind) {
    case MeshKind::None:
      ans = -1;
      break;
    case MeshKind::Line:
    case MeshKind::TerminalPoly:
      ans = pat_v;
      break;
    case MeshKind::TriFan:
      ans = pat_v < num_segs ? pat_v : (pat_v == num_segs ? e_num - 1 : num_segs);
      break;
    case MeshKind::Adj: {
      const int v_ring = adj::v_num_rings(num_segs) - 1;
      const int e_ring = adj::e_num_rings(num_segs) - 1;
      const int v_start = adj::v_ringstart(v_ring, num_anchors, num_segs);
      const int e_start = adj::e_ringstart(e_ring, num_anchors, num_segs);
      ans = e_start + (pat_v - v_start);
      break;
    }
    case MeshKind::Cutoff:
      /* TODO */
      ans = 0;
      BLI_assert(false);
  }
  return ans + first_e;
}

namespace topology {
/** Functions for analyzing the topology around bevels and setting up main bevel data structures
 * before construction. */

/** if there is common member of both values1 and values2, return the first lexicographically
 * where that is so. Else return -1.
 */
static int find_in_both(Span<int> values1, Span<int> values2)
{
  for (const int v1 : values1) {
    if (values2.contains(v1)) {
      return v1;
    }
  }
  return -1;
}

/** If \a edge is in the mesh face \a face, return the position (amount the corners of the face)
 * where it is. Else return -1. */
static int find_edge_pos_in_face(int edge, int face, const Mesh &mesh)
{
  const Span<int> face_edges = mesh.corner_edges().slice(mesh.faces()[face]);
  for (int e_index : face_edges.index_range()) {
    if (edge == face_edges[e_index]) {
      return e_index;
    }
  }
  return -1;
}

/** Assuming edge1 and edge2 are both in face, are they ccw around a common vertex in that face?
 */
static int edges_ccw_in_face(const int edge1, const int edge2, const int face, const Mesh &mesh)
{
  int pos1 = find_edge_pos_in_face(edge1, face, mesh);
  int pos2 = find_edge_pos_in_face(edge2, face, mesh);
  BLI_assert(pos1 != -1 && pos2 != -1);
  return ((pos1 + 1) % mesh.faces()[face].size()) == pos2;
}

/** Are edges counter clockwise around vert when seen from normal side
 * of the majority of faces involved? */
static bool edges_are_ccw(Span<int> edges, const BevelState &bs)
{
  if (edges.size() <= 1) {
    return true;
  }
  int ccw_test_sum = 0;
  for (const int e_index : edges.index_range()) {
    int edge = edges[e_index];
    int edge_next = edges[(e_index + 1) % edges.size()];
    int face = find_in_both(bs.mesh_info.edge_faces()[edge], bs.mesh_info.edge_faces()[edge_next]);
    if (face != -1) {
      bool ccw_in_face = edges_ccw_in_face(edge, edge_next, face, bs.mesh_info.mesh);
      /* Note that if edges are ccw in the face then that means the are
       * going cw around the bevel vertex.
       */
      ccw_test_sum += ccw_in_face ? -1 : 1;
    }
  }
  return ccw_test_sum >= 0;
}

static MultiValueMap<int, int> build_edge_to_edge_map(Span<int> edges, const BevelState &bs)
{
  GroupedSpan<int> edge_faces = bs.mesh_info.edge_faces();
  MultiValueMap<int, int> face_to_edge;
  for (const int e : edges) {
    for (const int f : edge_faces[e]) {
      face_to_edge.add(f, e);
    }
  }
  MultiValueMap<int, int> map;
  for (const int e : edges) {
    for (const int f : edge_faces[e]) {
      map.add_multiple(e, face_to_edge.lookup(f));
    }
  }
  return map;
}

/** Order the \a edges as specified in #bevvert_order_edges in the general
 * case where not all edges are manifold.
 * Most of the time, try_all_manifold_order will succeed. This is a fallback.
 * Whatever permutation is applied to edges, do the same thing to weights.
 */
static void general_edge_order(MutableSpan<int> edges, const BevelState &bs)
{
  MultiValueMap<int, int> edge_to_edge = build_edge_to_edge_map(edges, bs);
  int n = edges.size();
  Map<int, int> edge_to_index;
  /* An edge should appear at most once in edges. Make an edge -> position map. */
  edge_to_index.reserve(n);
  for (const int i : edges.index_range()) {
    edge_to_index.add_new(edges[i], i);
  }
  const Mesh &mesh = bs.mesh_info.mesh;
  GroupedSpan<int> edge_faces = bs.mesh_info.edge_faces();
  Array<bool, 20> placed(n, false);
  Vector<int, 20> chain_cw(n);
  Vector<int, 20> chain_ccw(n);
  Vector<int, 20> ordered_edges;
  ordered_edges.reserve(n);
  while (ordered_edges.size() != n) {
    /* Loop invariant: edges in ordered_edges are placed.
     * The loop always adds at least first_e to ordered_edges, so will not loop forever.
     * Start by finding the first index #i of an edge that has not yet been placed.
     */
    int first_i = std::find_if(placed.begin(), placed.end(), [](bool b) { return !b; }) -
                  placed.begin();
    BLI_assert(first_i < n);
    int first_e = edges[first_i];
    placed[first_i] = true;
    chain_cw.resize(0);
    chain_ccw.resize(0);
    /* Grow chain in two directions from first_e: cw when dir == 0, ccw when dir == 1. */
    for (const int dir : {0, 1}) {
      /* Follow chain from first_e going in dir as far as possible. */
      int cur_e = first_e;
      while (cur_e != -1) {
        Span<int> neighbors = edge_to_edge.lookup(cur_e);
        int next_e = -1;
        for (const int e : neighbors) {
          if (placed[edge_to_index.lookup(e)]) {
            continue;
          }
          int common_face = find_in_both(edge_faces[cur_e], edge_faces[e]);
          if (common_face != -1) {
            /* Note: ccw in face means cw around common vertex. */
            bool correct_dir = dir == 0 ? edges_ccw_in_face(cur_e, e, common_face, mesh) :
                                          !edges_ccw_in_face(cur_e, e, common_face, mesh);
            if (correct_dir) {
              next_e = e;
              break;
            }
          }
        }
        if (next_e != -1) {
          if (dir == 0) {
            chain_cw.append(next_e);
          }
          else {
            chain_ccw.append(next_e);
          }
          placed[edge_to_index.lookup(next_e)] = true;
        }
        cur_e = next_e;
      }
    }
    /* Append to ordered_edges: rev(chain_cw), first_e, chain_ccw. */
    for (int i = chain_cw.size() - 1; i >= 0; i--) {
      ordered_edges.append(chain_cw[i]);
    }
    ordered_edges.append(first_e);
    ordered_edges.extend(chain_ccw);
  }
  BLI_assert(ordered_edges.size() == n);
  std::copy(ordered_edges.begin(), ordered_edges.end(), edges.begin());
}

/** See if the "all manifold" strategy for ordering edges applies, and if it does
 * order the edges CCW around the vertex.
 */
static bool try_all_manifold_order(MutableSpan<int> edges, const BevelState &bs)
{
  bool edges_all_manifold = std::all_of(edges.begin(), edges.end(), [&](const int e) {
    return bs.mesh_info.edge_faces()[e].size() == 2;
  });
  if (!edges_all_manifold) {
    return false;
  }
  int n = edges.size();
  bool found_order;
  if (n <= 3) {
    /* There is only one order, modulo possible reversal. */
    found_order = true;
  }
  else if (n <= 6) {
    GroupedSpan<int> edge_faces = bs.mesh_info.edge_faces();
    /* Use O(n^2) method that needs no extra map. */
    for (int i = 1; i < n - 1; i++) {
      /* Loop invariant: edges[0..i) form a chain through faces and are placed.
       * We only loop to i == n-2 because at that point, whether the last edge
       * forms part of the chain or not, there isn't a better order.
       */
      int prev_e = edges[i - 1];
      BLI_assert(edge_faces[prev_e].size() == 2);
      int f1 = edge_faces[prev_e][0];
      int f2 = edge_faces[prev_e][1];
      /* Find an edge another edge among the unplaced edges, adjacent to f1 or f2.
       * If find one, swap the edge in position i with the found one.
       */
      int next_e_index = -1;
      for (int j = i; j < n; j++) {
        int e = edges[j];
        BLI_assert(edge_faces[e].size() == 2);
        int g1 = edge_faces[e][0];
        int g2 = edge_faces[e][1];
        if (f1 == g1 || f1 == g2 || f2 == g1 || f2 == g2) {
          next_e_index = j;
          break;
        }
      }
      if (next_e_index == -1) {
        found_order = false;
        break;
      }
      if (i != next_e_index) {
        std::swap(edges[i], edges[next_e_index]);
      }
    }
    found_order = true;
  }
  else {
    /* For larger n, speed up the search by making a map from
     * edges to other edges that share a face.
     */
    MultiValueMap<int, int> edge_to_edge = build_edge_to_edge_map(edges, bs);
    /* Similar loop to above, but using edge_to_edge map. */
    for (int i = 1; i < n - 1; i++) {
      int prev_e = edges[i - 1];
      int next_e_index = -1;
      for (int j = i; j < n; j++) {
        int e = edges[j];
        if (edge_to_edge.lookup(prev_e).contains(e)) {
          next_e_index = j;
          break;
        }
      }
      if (next_e_index == -1) {
        found_order = false;
        break;
      }
      if (i != next_e_index) {
        std::swap(edges[i], edges[next_e_index]);
      }
    }
    found_order = true;
  }
  if (found_order) {
    /* Possibly reverse the order. */
    if (!edges_are_ccw(edges, bs)) {
      std::reverse(edges.begin(), edges.end());
    }
  }
  return found_order;
}

/** Order the \a bevedges so that, as much as possible,
 * the edges are in a sequence such that they faces between them.
 * Also, so that with viewed from the positive face normal side of those faces,
 * we generally move counterclockwise around the bevel vertex.
 * The \a faces argument will be filled in with indices of shared faces
 * between the corresponding bevedge and its successor (cyclicly) if there is one,
 * else -1.
 */
static void bevvert_order_edges(BevelState &bs, MutableSpan<int> bevedges, MutableSpan<int> faces)
{
  SmallIntArray edges(bevedges.size());
  for (const int i : bevedges.index_range()) {
    edges[i] = bs.bevedge_mesh_edges()[bevedges[i]];
  }
  if (!try_all_manifold_order(edges.as_mutable_span(), bs)) {
    general_edge_order(edges.as_mutable_span(), bs);
  }
  /* If we are edge beveling, we need to rotate so that a beveled edge is first. */
  if (bs.params.affect_type == BevelAffect::Edges) {
    auto first_beveled_edge_iter = std::find_if(edges.begin(), edges.end(), [&](int e) {
      int be = bs.edge_bevedges()[e];
      return bs.bevedge_is_beveled(be);
    });
    BLI_assert(first_beveled_edge_iter != edges.end());
    std::rotate(edges.begin(), first_beveled_edge_iter, edges.end());
  }
  /* Convert the ordered edges to bevedges and copy the new order to bevedges argument.
   * Also set the shared face between bevedges, if any.
   */
  GroupedSpan<int> edge_faces = bs.mesh_info.edge_faces();
  for (const int i : bevedges.index_range()) {
    const int e = edges[i];
    const int next_e = edges[(i == bevedges.size() - 1) ? 0 : i + 1];
    bevedges[i] = bs.edge_bevedges()[e];
    faces[i] = find_in_both(edge_faces[e], edge_faces[next_e]);
  }
}

/** How many edges attached to BevVert \a bv are beveled? */
static int bevvert_num_beveled_edges(int bv, const BevelState &bs)
{
  Span<int> bevedges = bs.bevvert_bevedges()[bv];
  return std::accumulate(bevedges.begin(), bevedges.end(), 0, [&](int sum, int be) {
    return sum + (bs.bevedge_is_beveled(be) > 0.0f ? 1 : 0);
  });
}

/** What MeshPattern will be used for BevVert \a bv ? */
static MeshPattern find_meshpattern(int bv, BevelState &bs)
{
  MeshPattern pat;
  pat.num_segs = bs.params.segments;
  int num_edges = bs.bevvert_bevedges()[bv].size();
  BLI_assert(num_edges > 1 && pat.num_segs >= 1);
  if (bs.params.affect_type == BevelAffect::Vertices) {
    if (num_edges < 3) {
      pat.kind = MeshKind::Line;
      /* Anchors are on each attached edge.
       * They will be connected by a line of pat.num_segs segments. */
      pat.num_anchors = 2;
    }
    else {
      /* Anchors are on each attached edge. */
      pat.kind = MeshKind::Adj;
      pat.num_anchors = num_edges;
    }
  }
  else {
    int num_beveled = bevvert_num_beveled_edges(bv, bs);
    BLI_assert(num_beveled > 0 && num_beveled <= num_edges);
    if (num_beveled == 1) {
      /* Terminal edge case. */
      if (num_edges == 2) {
        /* Need to add an artificial vertex on the unbeveled edge. */
        pat.kind = MeshKind::TriFan;
        pat.num_anchors = 3;
      }
      else if (num_edges == 3) {
        pat.kind = MeshKind::Line;
        pat.num_anchors = 2;
      }
      else if (num_edges == 4) {
        pat.kind = MeshKind::TriFan;
        pat.num_anchors = 3;
      }
      else {
        pat.kind = MeshKind::TerminalPoly;
        pat.num_anchors = num_edges - 1;
      }
    }
    else {
      /* Edge bevel with at least two beveled edges. */
      if (num_beveled == 2) {
        /* The two beveled edges butt up against each other, along a line. */
        pat.kind = MeshKind::Line;
        pat.num_anchors = 2;
      }
      else {
        /* Edge bevel with three or more beveled edges.
         * There are anchors at the point where the right side of one beveled
         * edge attaches to the left side of the next beveled edge. */
        pat.kind = MeshKind::Adj;
        pat.num_anchors = num_beveled;
        /* However if there is mitering, we put additional anchor points
         * between the beveled edges: one additional for "arc" miters and two
         * additional for "patch" miters. We can only have pacth miters on
         * outer miters (where there is an obtuse angle). */
#if 0
        TODO
        if (bs.params.miter_inner != BevelMiterType::Sharp) {
          pat.num_anchors += num_beveled;
        }
        if (bs.params.miter_outer == BevelMiterType::Patch) {
          /* TODO: this is wrong if there is no obtuse angle between two
           * successive beveled edges! */
          pat.num_anchors += 1;
        }
#endif
      }
    }
  }
  return pat;
}

/* Helper function to find the coordinates between two beveled edges in the adj pattern. */
static float3 adj_edge_anchor_co(int bv,
                                 int cur_anchor_pos,
                                 int next_anchor_pos,
                                 int num_in_plane,
                                 int be_in_plane_pos,
                                 int num_not_in_plane,
                                 int be_not_in_plane_pos,
                                 const BevelState &bs)
{
  const int be_cur = bs.bevvert_bevedges()[bv][cur_anchor_pos];
  const int be_next = bs.bevvert_bevedges()[bv][next_anchor_pos];
  const int be_cur_end = bs.bevedge_vert_end(bv, be_cur);
  const int be_next_end = bs.bevedge_vert_end(bv, be_next);
  BLI_assert(bs.bevedge_is_beveled(be_cur) && bs.bevedge_is_beveled(be_next));
  bool offset_edge_between = false;
  bool offset_meet_edges_between = false;
  float3 co;
  float sin_ratio = 1.0f;
  int eon_pos = -1;
  const float spec_r = bs.bevedge_offset(be_cur, 2 * be_cur_end + 1);
  const float spec_next_l = bs.bevedge_offset(be_next, 2 * be_next_end);
  int offset_bepos_in_plane = -1;
  int offset_meet_face = bs.face_next(bv, cur_anchor_pos);
  if (num_not_in_plane > 0) {
#if 0
    TODO: either delete this or revive it if we need loop_slide
    if (bs.params.loop_slide && num_not_in_plane == 1 &&
        geom::good_slide(
            bv, cur_anchor_pos, next_anchor_pos, be_not_in_plane_pos, spec_r, spec_next_l, bs))
    {
      offset_edge_between = true;
      offset_bepos_in_plane = be_not_in_plane_pos;
      eon_pos = be_not_in_plane_pos;
    }
    else {
      offset_meet_face = -1;
      offset_meet_edges_between = true;
    }
#else
    offset_meet_face = -1;
    offset_meet_edges_between = true;
#endif
  }
  else if (num_in_plane > 0) {
#if 0
    TODO: either delete this or revive it if we need loop_slide
    if (bs.params.loop_slide && num_in_plane == 1 &&
        geom::good_slide(
            bv, cur_anchor_pos, next_anchor_pos, be_in_plane_pos, spec_r, spec_next_l, bs))
    {
      offset_edge_between = true;
      eon_pos = be_in_plane_pos;
    }
    else {
      offset_meet_edges_between = false;
    }
#else
    offset_meet_edges_between = false;
#endif
  }
  if (offset_edge_between) {
    if (!geom::try_offset_on_edge_between(bv,
                                          cur_anchor_pos,
                                          next_anchor_pos,
                                          eon_pos,
                                          spec_r,
                                          spec_next_l,
                                          &co,
                                          &sin_ratio,
                                          bs))
    {
      eon_pos = -1;
      offset_edge_between = false;
    }
  }
  if (!offset_edge_between) {
    co = geom::offset_meet(bv,
                           cur_anchor_pos,
                           next_anchor_pos,
                           spec_r,
                           spec_next_l,
                           offset_meet_face,
                           offset_meet_edges_between,
                           offset_bepos_in_plane,
                           bs);
  }
  /* TODO: stuff with eon and mitering. */
  return co;
}

/** Set the positions of the anchor vertices for bv, and the bevedge attachment points.  */
static void build_vmesh_skeleton(int bv,
                                 MutableSpan<float3> bv_newvert_positions,
                                 MutableSpan<int2> be_attach_verts,
                                 const BevelState &bs)
{
  bv_newvert_positions.fill(float3(0.0f, 0.0f, 0.0f));
  Span<int> bevedges = bs.bevvert_bevedges()[bv];
  const int num_edges = bevedges.size();
  BLI_assert(num_edges > 1);
  MeshPattern pat = bs.bevvert_meshpatterns()[bv];
  if (bs.params.affect_type == BevelAffect::Vertices) {
    /* Vertex bevel. */
    for (const int epos : bevedges.index_range()) {
      int be = bevedges[epos];
      int be_end = bs.bevedge_vert_end(bv, be);
      float spec = bs.bevedge_offset(be, be_end * 2);
      float3 co = geom::slide_dist(bv, epos, spec, bs);
      int nv_index = pat.anchor_vert(epos);
      bv_newvert_positions[nv_index] = co;
      be_attach_verts[be][be_end] = nv_index;
    }
  }
  else {
    /* Edge bevel. */;
    /* First gather the positions of all the actual beveled edges and their width specs. */
    Vector<int, 20> bevel_pos;
    Vector<float2, 20> widths;
    for (const int epos : bevedges.index_range()) {
      const int be = bevedges[epos];
      if (bs.bevedge_is_beveled(be)) {
        bevel_pos.append(epos);
        const int end = bs.bevedge_vert_end(bv, be);
        widths.append(float2(bs.bevedge_offset(be, 2 * end), bs.bevedge_offset(be, 2 * end + 1)));
      }
      else {
        widths.append(float2(0.0f, 0.0f));
      }
    }
    int num_beveled = bevel_pos.size();
    BLI_assert(num_beveled > 0 && bevel_pos[0] == 0);
    const int be0 = bevedges[0];
    const int be0_end = bs.bevedge_vert_end(bv, be0);
    const int be1 = bevedges[1];
    const int be1_end = bs.bevedge_vert_end(bv, be1);
    const int anchor0 = pat.anchor_vert(0);
    const int anchor1 = pat.anchor_vert(1);
    /* All cases have be0 attached to anchor 0. */
    be_attach_verts[be0][be0_end] = anchor0;
    bool leg_side = false; /* TODO: remove if not necessary. */
    if (num_beveled == 1) {
      /* Terminal edge cases. */
      if (num_edges == 2) {
        BLI_assert(pat.kind == MeshKind::TriFan && pat.num_anchors == 3);
        be_attach_verts[be1][be1_end] = pat.anchor_vert(2);
        bv_newvert_positions[anchor0] = geom::offset_bevedge(bv, 0, widths[0][0], true, bs);
        bv_newvert_positions[anchor1] = geom::offset_bevedge(bv, 0, widths[0][1], false, bs);
        bv_newvert_positions[pat.anchor_vert(2)] = geom::slide_dist(bv, 1, widths[0][0], bs);
      }
      else if (num_edges == 3) {
        BLI_assert(pat.kind == MeshKind::Line && pat.num_anchors == 2);
        be_attach_verts[be1][be1_end] = anchor1;
        const int be2 = bevedges[2];
        be_attach_verts[be2][bs.bevedge_vert_end(bv, be2)] = anchor0;
        if (leg_side) {
          bv_newvert_positions[anchor0] = geom::slide_dist(bv, 2, widths[0][0], bs);
          bv_newvert_positions[anchor1] = geom::slide_dist(bv, 1, widths[0][1], bs);
        }
        else {
          bv_newvert_positions[anchor0] = geom::offset_meet(
              bv, 2, 0, 0.0f, widths[0][0], bs.face_prev(bv, 0), false, -1, bs);
          bv_newvert_positions[anchor1] = geom::offset_meet(
              bv, 0, 1, widths[0][1], 0.0f, bs.face_next(bv, 0), false, -1, bs);
        }
      }
      else if (num_edges == 4) {
        BLI_assert(pat.kind == MeshKind::TriFan && pat.num_anchors == 3);
        be_attach_verts[be1][be1_end] = anchor1;
        const int be2 = bevedges[2];
        be_attach_verts[be2][bs.bevedge_vert_end(bv, be2)] = pat.anchor_vert(2);
        const int be3 = bevedges[3];
        be_attach_verts[be3][bs.bevedge_vert_end(bv, be3)] = anchor0;
        if (leg_side) {
          bv_newvert_positions[anchor0] = geom::slide_dist(bv, 3, widths[0][0], bs);
          bv_newvert_positions[anchor1] = geom::slide_dist(bv, 1, widths[0][1], bs);
        }
        else {
          bv_newvert_positions[anchor0] = geom::offset_meet(
              bv, 3, 0, 0.0f, widths[0][0], bs.face_prev(bv, 0), false, -1, bs);
          bv_newvert_positions[anchor1] = geom::offset_meet(
              bv, 0, 1, widths[0][1], 0.0f, bs.face_next(bv, 0), false, -1, bs);
        }
      }
      else {
        BLI_assert(pat.kind == MeshKind::TerminalPoly && pat.num_anchors == bevedges.size() - 1);
        for (int i = 1; i < num_edges; i++) {
          const int be = bevedges[i];
          be_attach_verts[be][bs.bevedge_vert_end(bv, be)] = pat.anchor_vert(i);
        }
        if (leg_side) {
          bv_newvert_positions[anchor0] = geom::slide_dist(bv, num_edges - 1, widths[0][0], bs);
          bv_newvert_positions[anchor1] = geom::slide_dist(bv, 1, widths[0][1], bs);
        }
        else {
          bv_newvert_positions[anchor0] = geom::offset_meet(
              bv, num_edges - 1, 0, 0.0f, widths[0][0], bs.face_prev(bv, 0), false, -1, bs);
          bv_newvert_positions[anchor1] = geom::offset_meet(
              bv, 0, 1, widths[0][1], 0.0f, bs.face_next(bv, 0), false, -1, bs);
        }
        for (int i = 2; i < num_edges - 1; i++) {
          bv_newvert_positions[anchor1 + i - 1] = geom::slide_dist(bv, i, widths[0][0], bs);
        }
      }
    }
    else if (num_beveled == 2) {
      BLI_assert(pat.kind == MeshKind::Line && pat.num_anchors == 2);
      be_attach_verts[be1][be1_end] = anchor1;
      bool edges_between10 = !(anchor0 == (anchor1 + pat.num_segs) % num_edges);
      bool edges_between01 = !(anchor1 == (anchor0 + pat.num_segs) % num_edges);
      bv_newvert_positions[anchor0] = geom::offset_meet(
          bv, 1, 0, widths[1][1], widths[0][0], bs.face_prev(bv, 0), edges_between10, -1, bs);
      bv_newvert_positions[anchor1] = geom::offset_meet(
          bv, 0, 1, widths[0][1], widths[1][0], bs.face_next(bv, 0), edges_between01, -1, bs);
      bool seen_second_bevel = false;
      for (int i = 1; i < num_edges; i++) {
        if (i == bevel_pos[1]) {
          seen_second_bevel = true;
        }
        else {
          const int be = bevedges[i];
          be_attach_verts[be][bs.bevedge_vert_end(bv, be)] = seen_second_bevel ? anchor0 : anchor1;
        }
      }
    }
    else {
      BLI_assert(pat.kind == MeshKind::Adj && pat.num_anchors >= 3);
      /* TODO: miters. */
      int cur_anchor = 0;
      int next_anchor = 1;
      /* We need to know the number of edges between the current beveled edge and the
       * next one that are "in plane" (coplanar with their two adjactent faces) or
       * "not in plane". */
      int num_in_plane = 0;
      int num_not_in_plane = 0;
      int be_in_plane_pos = -1;
      int be_not_in_plane_pos = -1;
      for (int i = 1; i < num_edges; i++) {
        const int be = bevedges[i];
        const int be_end = bs.bevedge_vert_end(bv, be);
        be_attach_verts[be][be_end] = pat.anchor_vert(next_anchor);
        if (i == bevel_pos[next_anchor]) {
          /* We have reached the next beveled edge. */
          const int anchor_pos = pat.anchor_vert(next_anchor);
          bv_newvert_positions[anchor_pos] = adj_edge_anchor_co(bv,
                                                                bevel_pos[cur_anchor],
                                                                bevel_pos[next_anchor],
                                                                num_in_plane,
                                                                be_in_plane_pos,
                                                                num_not_in_plane,
                                                                be_not_in_plane_pos,
                                                                bs);
          cur_anchor = next_anchor;
          next_anchor = (cur_anchor + 1) % num_beveled;
          num_in_plane = 0;
          num_not_in_plane = 0;
          be_in_plane_pos = -1;
          be_not_in_plane_pos = -1;
        }
        else {
          if (geom::bevedge_on_plane(bv, i, bs)) {
            num_in_plane++;
            be_in_plane_pos = i;
          }
          else {
            num_not_in_plane++;
            be_not_in_plane_pos = i;
          }
        }
      }
      const int anchor_pos = pat.anchor_vert(next_anchor);
      bv_newvert_positions[anchor_pos] = adj_edge_anchor_co(bv,
                                                            bevel_pos[cur_anchor],
                                                            bevel_pos[next_anchor],
                                                            num_in_plane,
                                                            be_in_plane_pos,
                                                            num_not_in_plane,
                                                            be_not_in_plane_pos,
                                                            bs);
    }
  }
}

static void build_internal_adj(const int bv,
                               const profile::AnchorProfiles &profiles,
                               MutableSpan<float3> bv_newvert_positions,
                               const BevelState &bs)
{
  const MeshPattern &pat = bs.bevvert_meshpatterns()[bv];
  const int ns = bs.params.segments;
  const int na = pat.num_anchors;
  BLI_assert(ns > 1 && pat.kind == MeshKind::Adj);
  const int ns_power_2 = bs.pro_spacing.segments_power_2;

  /* First construct an initial control mesh with 2 segments. */
  adj::AdjVerts adj2(na, 2);
  float3 center(0.0f, 0.0f, 0.0f);
  for (const int a : IndexRange(na)) {
    float3 a_pos = bv_newvert_positions[pat.anchor_vert(a)];
    adj2.mutable_outer_ring_vert(a, 0) = a_pos;
    adj2.mutable_outer_ring_vert(a, 1) = profile::get_profile_point(profiles[a], 1, 2);
    center = center + a_pos;
  }
  center = center / float(na);

  /* To place the center vertex, let:
   * 'negative_fullest' = the original vertex across the boundverts' center.
   * 'fullness' = fraction of the way from the boundvert's centroid
   *  to the original vertex (if positive) or to negative_fullest (if negative).
   */
  const float3 orig_v = bs.mesh_info.mesh.vert_positions()[bs.bevvert_mesh_verts()[bv]];
  const float fullness = bs.pro_spacing.fullness;
  const float3 center_dir = orig_v - center;
  if (math::length_squared(center_dir) > geom::bevel_epsilon_sq) {
    adj2.mutable_vert(0, 0, 0) = center + fullness * center_dir;
    /* TODO: custom profile is different here. */
    /* const float3 negative_fullest = center + (center - orig_v); */
  }
  else {
    adj2.mutable_vert(0, 0, 0) = center;
  }

  /* Make and fill adj mesh with #ns_power_2 segements. */
  adj::AdjVerts adj_sup_power_2(na, ns_power_2);
  adj::fill_adjverts(adj_sup_power_2, adj2, profiles);

  /* Interpolate the mesh to the needed number of eegments, if necessary. */
  adj::AdjVerts adj(na, ns);
  if (ns == ns_power_2) {
    BLI_assert(adj_sup_power_2.verts.size() == adj.verts.size());
    std::copy(adj_sup_power_2.verts.begin(), adj_sup_power_2.verts.end(), adj.verts.begin());
  }
  else {
    adj::interp_adj(adj, adj_sup_power_2, profiles);
  }

  BLI_assert(adj.verts.size() == bv_newvert_positions.size());
  std::copy(adj.verts.begin(), adj.verts.end(), bv_newvert_positions.begin());
}

static void build_internal_vmesh(const int bv,
                                 const profile::AnchorProfiles &profiles,
                                 MutableSpan<float3> bv_newvert_positions,
                                 const BevelState &bs)
{
  const MeshPattern &pat = bs.bevvert_meshpatterns()[bv];
  const int ns = bs.params.segments;
  if (ns == 1) {
    /* The newverts are all anchor points, so already set. */
    return;
  }
  if (pat.kind == MeshKind::Line) {
    BLI_assert(pat.num_anchors == 2 && pat.num_elements()[0] == ns + 1);
    const profile::Profile pro = profiles[0];
    for (int i = 1; i <= ns; i++) {
      bv_newvert_positions[i] = pro.prof_co[i];
    }
  }
  else if (pat.kind == MeshKind::Adj) {
    build_internal_adj(bv, profiles, bv_newvert_positions, bs);
  }
  else {
    /* TODO: implement me. */
    BLI_assert(false);
  }
}

}  // end of namespace topology

/** Initialize the bevel_edges and bevel_verts IndexMasks, and also some fields
 * that will control the basic operaiton of the whole bevel.
 */
BevelState::BevelState(const Mesh &src_mesh,
                       const IndexMask &selection,
                       const BevelParameters &bevel_params)
    : mesh_info(src_mesh)
{
  /* Copy the parameters so we can safely modify them if necessary. */
  this->params = bevel_params;
  bool vertex_only = params.affect_type == BevelAffect::Vertices;

  /* There is an adjustment procedure that modifies bevel amounts to make the best compromise
   * when all specifications cannot be met simultaneously. That procedure cannot be applied
   * in all circumstances. */
  this->offset_adjust = false;
#if 0
  this->offset_adjust = !(vertex_only || params.offset_type == BevelOffsetType::Percent ||
                          params.offset_type == BevelOffsetType::Absolute);
#endif

  /* Get the IndexMasks for bevel-involved edges, bevel-involved vertices,
   * and bevel-involved faces.
   * Set the weights that modify the bevel amount for those elements
   * actually beveled.
   *
   * The selection parameter selectes what is beveled depending on the affect type.
   * We filter the selection to only select legal elements to bevel:
   * for edge bevels, the edge must be manifold (attached to exactly 2 faces);
   * for vertex bevels, the vertex must have at least two edges attached to it.
   *
   * Note: the legacy bmesh bevel code assumed that the caller made these things true.
   */
  IndexMask beveled_edges_mask;
  if (vertex_only) {
    bevverts_mask_ = index_mask::IndexMask::from_predicate(
        selection, GrainSize(4096), this->memory_, [&](const int v) {
          return this->mesh_info.vert_edges()[v].size() >= 2;
        });
  }
  else {
    beveled_edges_mask = index_mask::IndexMask::from_predicate(
        selection, GrainSize(4096), this->memory_, [&](const int e) {
          return this->mesh_info.edge_faces()[e].size() == 2;
        });
    Array<bool> bevel_involved_vert(src_mesh.verts_num, false);
    beveled_edges_mask.foreach_index([&](const int e) {
      int2 edge_vs = src_mesh.edges()[e];
      bevel_involved_vert[edge_vs[0]] = true;
      bevel_involved_vert[edge_vs[1]] = true;
    });
    bevverts_mask_ = IndexMask::from_bools(bevel_involved_vert, memory_);
  }
  Array<bool> bevel_involved_edge(src_mesh.edges_num, false);
  bevverts_mask_.foreach_index([&](const int v) {
    Span<int> edges = this->mesh_info.vert_edges()[v];
    for (const int e : edges) {
      bevel_involved_edge[e] = true;
    }
  });
  bevedges_mask_ = IndexMask::from_bools(bevel_involved_edge, memory_);

  Array<bool> bevel_involved_face(src_mesh.faces_num, false);
  bevedges_mask_.foreach_index([&](const int e) {
    Span<int> faces = mesh_info.edge_faces()[e];
    for (const int f : faces) {
      bevel_involved_face[f] = true;
    }
  });
  bevfaces_mask_ = IndexMask::from_bools(bevel_involved_face, memory_);

  /* Create the arrays for bevverts and maps between verts and bevverts. */
  this->bevverts_num = bevverts_mask_.size();
  bevvert_mesh_verts_ = Array<int>(this->bevverts_num);
  vert_bevverts_ = Array<int>(this->mesh_info.mesh.verts_num, -1);
  if (vertex_only) {
    bevvert_is_beveled_ = Array<bool>(this->bevverts_num, false);
  }
  bevverts_mask_.foreach_index([&](const int v, const int mask) {
    vert_bevverts_[v] = mask;
    bevvert_mesh_verts_[mask] = v;
    if (vertex_only) {
      bevvert_is_beveled_[mask] = selection.contains(v);
    }
  });

  /* Create the arrays for the bevedges and maps between edges and bevedges. */
  this->bevedges_num = bevedges_mask_.size();
  bevedge_mesh_edges_ = Array<int>(this->bevedges_num);
  edge_bevedges_ = Array<int>(this->mesh_info.mesh.edges_num, -1);
  if (!vertex_only) {
    bevedge_is_beveled_ = Array<bool>(this->bevedges_num);
  }
  bevedges_mask_.foreach_index([&](const int e, const int mask) {
    edge_bevedges_[e] = mask;
    bevedge_mesh_edges_[mask] = e;
    if (!vertex_only) {
      bevedge_is_beveled_[mask] = selection.contains(e);
    }
  });

  /* Create the arrays for the bevfaces and maps between faces and bevfaces. */
  this->bevfaces_num = bevfaces_mask_.size();
  bevface_mesh_faces_ = Array<int>(this->bevfaces_num);
  face_bevfaces_ = Array<int>(this->mesh_info.mesh.faces_num, -1);
  bevfaces_mask_.foreach_index([&](const int f, const int mask) {
    face_bevfaces_[f] = mask;
    bevface_mesh_faces_[mask] = f;
  });

  /* Find the bevedges attached to each bevvert. */

  /* First fill in the counts of the number of attached edges, then convert to offsets. */
  bevvert_bevedges_offsets_ = Array<int>(this->bevverts_num + 1);
  threading::parallel_for(IndexRange(this->bevverts_num), 20000, [&](IndexRange range) {
    for (const int bv : range) {
      int v = bevvert_mesh_verts_[bv];
      bevvert_bevedges_offsets_[bv] = this->mesh_info.vert_edges()[v].size();
    }
  });
  offset_indices::accumulate_counts_to_offsets(bevvert_bevedges_offsets_);
  OffsetIndices<int> offsets(bevvert_bevedges_offsets_);
  bevvert_bevedges_indices_ = Array<int>(offsets.total_size());
  threading::parallel_for(IndexRange(this->bevverts_num), 20000, [&](IndexRange range) {
    for (const int bv : range) {
      const int v = bevvert_mesh_verts_[bv];
      Span<int> v_edges = this->mesh_info.vert_edges()[v];
      int offset_first = offsets[bv].first();
      for (const int i : IndexRange(offsets[bv].size())) {
        bevvert_bevedges_indices_[offset_first + i] = edge_bevedges_[v_edges[i]];
      }
    }
  });
  bevvert_bevedges_ = GroupedSpan<int>(offsets, bevvert_bevedges_indices_);
}

/** Return the bevedge poisition of the last bevedge attached to newvert, or -1 if none. */
int BevelState::last_attached_bevedge_pos(const int bv, const int newvert) const
{
  Span<int> edges = bevvert_bevedges_[bv];
  int ans = -1;
  for (const int epos : edges.index_range()) {
    const int be = edges[epos];
    const int be_end = bevedge_vert_end(bv, be);
    const int attach_vert = bevedge_attach_verts_[be][be_end];
    if (attach_vert == newvert) {
      ans = epos;
    }
    else if (ans != -1) {
      /* We reached the end of the range of edges having newvert as attachment. */
      break;
    }
  }
  return ans;
}

/** Return the edge position for bevedge \a be around bevvert \a bv. */
int BevelState::bevedge_pos(const int be, const int bv) const
{
  Span<int> edges = bevvert_bevedges_[bv];
  for (const int epos : edges.index_range()) {
    if (be == edges[epos]) {
      return epos;
    }
  }
  return -1;
}

/** Return the left and right anchors for the edge at position \a edge_pos of bevvert \a bv.
 * These will be the same if the edge isn't beveled. If it is beveled, the first  of the pair is
 * the anchor position where the left side of the edge (looking towards bv) is attached and the
 * second of the pair is where the right end is atttached. */
int2 BevelState::bevedge_anchors(const int bv, const int edge_pos) const
{
  const int be = bevvert_bevedges_[bv][edge_pos];
  const int be_end = bevedge_vert_end(bv, be);
  const int attach_vert = bevedge_attach_verts_[be][be_end];
  const MeshPattern &pat = bevvert_meshpatterns_[bv];
  const int attach_anchor = pat.vert_to_anchor(attach_vert);
  if (bevedge_is_beveled_[be]) {
    return int2(attach_anchor, pat.next_anchor(attach_anchor));
  }
  return int2(attach_anchor, attach_anchor);
}

/** Return the edge positions of the first and last edges attached at anchor position \a anchor.
 * Return (-1, -1) if no edge is attached there. */
int2 BevelState::anchor_bevedge_positions(const int bv, const int anchor) const
{
  int2 ans(-1, -1);
  Span<int> bevedges = bevvert_bevedges()[bv];
  for (const int pos : bevedges.index_range()) {
    int2 be_anchors = bevedge_anchors(bv, pos);
    if (anchor == be_anchors[0] || anchor == be_anchors[1]) {
      if (ans[0] == -1) {
        ans[0] = ans[1] = pos;
      }
      else {
        ans[1] = pos;
      }
    }
  }
  if (anchor == 0) {
    /* The first encountered edge will actually be the last one in this case. */
    std::swap(ans[0], ans[1]);
  }
  return ans;
}

/** Initialize the part of the state related to the profile curves that will be used in the
 * non-custom shapes of multisegment bevels.
 */
void BevelState::initialize_profile_data()
{
  /* Convert the input profile shape parameter to the actual exponent of a superellipse. */
  const float psr = -std::log(2.0) /
                    std::log(math::sqrt(this->params.shape > 0 ? this->params.shape : 1e-20f));
  this->pro_super_r = psr;

  /* Snap some ranges of profile to particular shapes. */
  if (this->params.shape >= 0.950f) { /* r ~ 692, so pro_square_r is 1e4 */
    this->pro_super_r = profile::pro_square_r;
  }
  else if (abs(psr - profile::pro_circle_r) < 1e-4f) {
    this->pro_super_r = profile::pro_circle_r;
  }
  else if (abs(psr - profile::pro_line_r) < 1e-4f) {
    this->pro_super_r = profile::pro_line_r;
  }
  else if (abs(psr) < 1e-4f) {
    this->pro_super_r = profile::pro_square_in_r;
  }

  profile::set_profile_spacing(this, &this->pro_spacing, this->params.custom_profile != nullptr);

  if (this->params.segments > 1) {
    this->pro_spacing.fullness = profile::find_profile_fullness(this);
  }

#if 0
  TODO
  /* Get separate non-custom profile samples for the miter profiles if they are needed */
  /* TODO: check: bmesh code seems wrong here: the first check is ==, not != */
  if (this->params.profile_type != BevelProfileType::Custom &&
      (this->params.miter_inner != BevelMiterType::Sharp ||
       this->params.miter_outer != BevelMiterType::Sharp))
  {
    profile::set_profile_spacing(this, &this->pro_spacing_miter, false);
  }
#endif
}

/** Order the bevedges around each bevvert so that they form manifold caps
 * or pieces of manifold caps, as much as possible.
 * In the simple usual case, there is one manifold cap (a cyclic ordering of edges
 * where there is exactly one face between each successive pair of edges).
 * In that simple case, the ordering is counterclockwise when looking at it from
 * the positive normal side of the vertex and faces.
 *
 * As well as ordering the bevedges, set up the bevvert_bevfaces GroupedSpan,
 * which gives the faces shared between a bevvedge in bevvert_bevedges and its
 * immediately following (in cyclic order) bevedge.
 */
void BevelState::order_bevedges()
{
  /* We'll shared the bevvert_bevedges_offsets_ for bevvert_faces_, since the faces
   * are parallel to the bevedges. */
  OffsetIndices<int> offsets(bevvert_bevedges_offsets_);
  bevvert_faces_indices_ = Array<int>(offsets.total_size());
  threading::parallel_for(IndexRange(this->bevverts_num), 4096, [&](IndexRange range) {
    for (const int bv : range) {
      IndexRange r = OffsetIndices<int>(bevvert_bevedges_offsets_)[bv];
      MutableSpan<int> edges = bevvert_bevedges_indices_.as_mutable_span().slice(r);
      MutableSpan<int> faces = bevvert_faces_indices_.as_mutable_span().slice(r);
      topology::bevvert_order_edges(*this, edges, faces);
    }
  });
  bevvert_faces_ = GroupedSpan<int>(offsets, bevvert_faces_indices_);
}

/** Figure out number of each new element type needed, allocate those arrays,
 * and set up the OffsetIndices to map bevverts, bevedges, and bevfafces to the
 * corrsponding new elements (newverts, newedges, newfaces, newcorners0.
 * To do this, we need to know the mesh patterns for each bevverrt, so determint that too.
 */
void BevelState::determine_needed_new_elements()
{
  bevvert_meshpatterns_ = Array<MeshPattern>(bevverts_num);
  /* We need newverts only for bevvert meshes.
   * We need newedges only for bevvert and bevedges meshes.
   * We need newfaces for all three.
   */
  newverts_offsets_ = Array<int>(bevverts_num + 1);
  newedges_offsets_ = Array<int>(bevverts_num + bevedges_num + 1);
  newfaces_offsets_ = Array<int>(bevverts_num + bevedges_num + bevfaces_num + 1);
  int32_t total_corners = 0;
  threading::parallel_for(IndexRange(this->bevverts_num), 20'000, [&](IndexRange range) {
    int32_t bevvert_tot_corners = 0;
    for (const int bv : range) {
      bevvert_meshpatterns_[bv] = topology::find_meshpattern(bv, *this);
      const int4 nums = bevvert_meshpatterns_[bv].num_elements();
      /* In this loop, accumulate counts. Will covert to offsets later. */
      newverts_offsets_[bv] = nums[0];
      newedges_offsets_[bv] = nums[1];
      newfaces_offsets_[bv] = nums[2];
      bevvert_tot_corners += nums[3];
    }
    atomic_add_and_fetch_int32(&total_corners, bevvert_tot_corners);
  });
  threading::parallel_for(IndexRange(this->bevedges_num), 20'000, [&](IndexRange range) {
    int32_t bevedge_tot_corners = 0;
    for (const int be : range) {
      const int4 nums = bevedge_num_elements(be, *this);
      BLI_assert(nums[0] == 0);
      newedges_offsets_[bevverts_num + be] = nums[1];
      newfaces_offsets_[bevverts_num + be] = nums[2];
      bevedge_tot_corners += nums[3];
    }
    atomic_add_and_fetch_int32(&total_corners, bevedge_tot_corners);
  });
  threading::parallel_for(IndexRange(this->bevfaces_num), 20'000, [&](IndexRange range) {
    int32_t bevface_tot_corners = 0;
    for (const int bf : range) {
      const int4 nums = bevface_num_elements(bf, *this);
      BLI_assert(nums[0] == 0 && nums[1] == 0);
      newfaces_offsets_[bevverts_num + bevedges_num + bf] = nums[2];
      bevface_tot_corners += nums[3];
    }
    atomic_add_and_fetch_int32(&total_corners, bevface_tot_corners);
  });

  offset_indices::accumulate_counts_to_offsets(newverts_offsets_);
  offset_indices::accumulate_counts_to_offsets(newedges_offsets_);
  offset_indices::accumulate_counts_to_offsets(newfaces_offsets_);

  this->newverts_num = newverts_offsets_.last();
  this->newedges_num = newedges_offsets_.last();
  this->newfaces_num = newfaces_offsets_.last();

  bevvert_newverts_ = OffsetIndices<int>(newverts_offsets_.as_span().take_front(bevverts_num + 1));
  bevvert_newedges_ = OffsetIndices<int>(newedges_offsets_.as_span().take_front(bevverts_num + 1));
  bevvert_newfaces_ = OffsetIndices<int>(newfaces_offsets_.as_span().take_front(bevverts_num + 1));

  bevedge_newedges_ = OffsetIndices<int>(
      newedges_offsets_.as_span().drop_front(bevverts_num).take_front(bevedges_num + 1));
  bevedge_newfaces_ = OffsetIndices<int>(
      newfaces_offsets_.as_span().drop_front(bevverts_num).take_front(bevedges_num + 1));

  bevface_newfaces_ = OffsetIndices<int>(
      newfaces_offsets_.as_span().drop_front(bevverts_num + bevedges_num));

  newvert_positions_ = Array<float3>(newverts_num);
  newedge_vertpairs_ = Array<int2>(newedges_num);
  newcorner_verts_ = Array<int>(total_corners, -1);
  newcorner_edges_ = Array<int>(total_corners, -1);
  this->newcorners_num = total_corners;

  /* Now set up corners for new faces. */
  newface_faces_face_offsets_ = Array<int>(newfaces_num + 1);
  threading::parallel_for(IndexRange(bevverts_num), 20'000, [&](IndexRange range) {
    for (const int bv : range) {
      const MeshPattern &pat = bevvert_meshpatterns_[bv];
      for (const int patf : IndexRange(bevvert_newfaces_[bv].size())) {
        const int newf = bevvert_newfaces_[bv][patf];
        auto [verts, edges] = pat.face_verts_and_edges(patf, 0, 0);
        /* TODO: make function that just returns face size for f. */
        newface_faces_face_offsets_[newf] = verts.size();
      }
    }
  });
  threading::parallel_for(IndexRange(bevedges_num), 20'000, [&](IndexRange range) {
    for (const int be : range) {
      if (bevedge_newfaces_[be].size() > 0) {
        for (const int newf : bevedge_newfaces_[be]) {
          newface_faces_face_offsets_[newf] = 4;
        }
      }
    }
  });
  threading::parallel_for(IndexRange(bevfaces_num), 20'000, [&](IndexRange range) {
    for (const int bf : range) {
      int newf = bevface_newfaces_[bf][0];
      const int4 vefc = bevface_num_elements(bf, *this);
      newface_faces_face_offsets_[newf] = vefc[3];
    }
  });
  offset_indices::accumulate_counts_to_offsets(newface_faces_face_offsets_);
  newface_faces_face_ = OffsetIndices<int>(newface_faces_face_offsets_);
}

/** Find out which elements have attributes beyond the special builtin ones, and then
 * allocate the arrays needed to keep track of how to calculate attribute values
 * for the new elements.
 */
void BevelState::determine_needed_attribute_data()
{
  const Mesh &mesh = mesh_info.mesh;
  const bke::AttributeAccessor attrs = mesh.attributes();
  attrs.foreach_attribute([&](const bke::AttributeIter &iter) {
    if (ELEM(iter.name, "position", ".edge_verts", ".corner_vert", ".corner_edge")) {
      return;
    }
    switch (iter.domain) {
      case bke::AttrDomain::Point:
        this->any_vert_attributes = true;
        break;
      case bke::AttrDomain::Edge:
        this->any_edge_attributes = true;
        break;
      case bke::AttrDomain::Face:
        this->any_face_attributes = true;
        break;
      case bke::AttrDomain::Corner:
        if (iter.data_type == bke::AttrType::Float2) {
          /* Assume this is a UV map attribute. */
          this->uv_map_infos_.append(UVMapInfo(iter.name, mesh));
          this->uvmaps_num++;
        }
        else {
          this->any_corner_attributes = true;
        }
      default:
        break;
    }
  });
  /* For those domains where we have attributes to propagage. we allocate
   * the arrays needed. */
  if (any_vert_attributes) {
    newvert_repverts_.reinitialize(newverts_num);
    newvert_repverts_.fill(-1);
  }
  if (any_edge_attributes) {
    newedge_repedges_.reinitialize(newedges_num);
    newedge_repedges_.fill(-1);
  }
  if (any_face_attributes) {
    newface_repfaces_.reinitialize(newfaces_num);
    newface_repfaces_.fill(-1);
  }
  if (any_corner_attributes) {
    newcorner_repcorners_.fill(-1);
  }
  /* When choosing face representatives, we need access
   * to uv map components and material indices, as tie breakers.
   */
  if (uvmaps_num > 0) {
    for (int i = 0; i < uvmaps_num; i++) {
      uv_map_infos_[i].find_components(mesh_info);
      uv_attributes_.append(Array<float2>(newcorners_num, float2(0.0f, 0.0f)));
    }
  }
  bke::AttributeReader<int> mat_reader = attrs.lookup<int>("material_index",
                                                           bke::AttrDomain::Face);
  if (mat_reader) {
    VArraySpan<int> vmat(mat_reader.varray);
    materials_ = Array<int>(vmat);
  }
}

/** Build the edge e from vertices v0 and v1, where those indices are in a "new element" space.
 * Negative input values are just copied as is (used to encode original elements).
 */
void BevelState::build_newedge(const int e, const int v0, const int v1)
{
  newedge_vertpairs_[e] = int2(v0, v1);
}

/** Build the face f from verts and edges, where those indices are in "new" element" space.
 * Negative input elements are just copied as is (used to encode original elements).
 */
void BevelState::build_newface(const int f, Span<int> verts, Span<int> edges)
{
  BLI_assert(verts.size() >= 3 && verts.size() == edges.size());
  IndexRange f_corners = newface_faces_face_[f];
  BLI_assert(f_corners.size() == verts.size());
  for (const int i : f_corners.index_range()) {
    const int v = verts[i];
    const int e = edges[i];
    const int c = f_corners[i];
    newcorner_verts_[c] = v;
    newcorner_edges_[c] = e;
  }
}

/** Set representative original elements to copy attributes from for new elements. */
static void set_vertex_mesh_reps(const int bv,
                                 MutableSpan<int> repverts,
                                 MutableSpan<int> repedges,
                                 MutableSpan<int> repfaces,
                                 const BevelState &bs)
{
  /* For vertices, just use the mesh vertex at bv as the representative.
   * Do this ieven if bs.any_vert_attributes is false, since we need it for positions.
   */
  const int mesh_v = bs.bevvert_mesh_verts()[bv];
  for (const int nv : bs.bevvert_newverts()[bv]) {
    repverts[nv] = mesh_v;
  }
  const MeshPattern &pat = bs.bevvert_meshpatterns()[bv];
  /* Placeholder logic for edges. */
  if (bs.any_edge_attributes && pat.kind == MeshKind::Adj && pat.num_segs > 1) {
    /* Use the beveled edge as rep for the "center lines" between anchors. */
    const int rings = adj::e_num_crossrings(pat.num_segs);
    const int ne_start = bs.bevvert_newedges()[bv][0];
    Span<int> edges = bs.bevvert_bevedges()[bv];
    for (const int a : IndexRange(pat.num_anchors)) {
      const int nv_a = pat.anchor_vert(a);
      const int be_pos = bs.last_attached_bevedge_pos(bv, nv_a);
      const int be = edges[be_pos];
      const int mesh_e = bs.bevedge_mesh_edges()[be];
      for (const int r : IndexRange(rings)) {
        const int start = adj::e_crossring_start(r, pat.num_anchors, pat.num_segs);
        const int len = adj::e_crossring_len(r, pat.num_anchors, pat.num_segs);
        const int side_len = len / pat.num_anchors;
        const int c_index = (pat.num_segs % 2) == 1 ? side_len / 2 - 1 : side_len / 2;
        const int e = ne_start + start + a * side_len + c_index;
        repedges[e] = mesh_e;
      }
    }
  }
  const int num_faces = pat.num_elements()[2];
  if (bs.any_face_attributes && num_faces > 0) {
    SmallIntArray anchor_face_reps(pat.num_anchors);
    SmallIntArray face_rep_tiebreaks(pat.num_anchors);
    for (const int a : IndexRange(pat.num_anchors)) {
      anchor_face_reps[a] = facerep::anchor_rep_face(bv, a, nullptr, bs);
    }
    const bool odd = (pat.num_segs % 2) == 1;
    int center_frep = -1;
    if (odd || pat.kind != MeshKind::Adj) {
      for (const int a : IndexRange(pat.num_anchors)) {
        const int anext = pat.next_anchor(a);
        face_rep_tiebreaks[a] = facerep::choose_face_rep(
            {anchor_face_reps[a], anchor_face_reps[anext]}, bs);
      }
      center_frep = facerep::find_center_face_rep(bv, false, bs);
    }
    switch (pat.kind) {
      case MeshKind::Adj: {

        if (odd) {
          /* The center face is always face 0 in the pattern. */
          repfaces[0] = center_frep;
        }
        for (const int a : IndexRange(pat.num_anchors)) {
          SmallIntArray afaces = pat.faces_for_anchor(a);
          for (const int f : afaces) {
            repfaces[f] = anchor_face_reps[a];
          }
          if (odd) {
            SmallIntArray clinefaces = pat.faces_for_centerline(a);
            for (const int f : clinefaces) {
              repfaces[f] = face_rep_tiebreaks[a];
            }
          }
        }
        break;
      }
      case MeshKind::TriFan:
      case MeshKind::TerminalPoly: {
        BLI_assert(num_faces == 1);
        repfaces[0] = center_frep;
        break;
      }
      default:
        BLI_assert_unreachable();
        break;
    }
  }
}

/** Set representative original elements to copy attributes from for new elements. */
static void set_edge_mesh_reps(const int be,
                               MutableSpan<int> repedges,
                               MutableSpan<int> repfaces,
                               const BevelState &bs)
{
  const int mesh_edge = bs.bevedge_mesh_edges()[be];
  const int nsegs = bs.bevedge_is_beveled(be) ? bs.params.segments : 0;
  const int ne_start = bs.bevedge_newedges()[be][0];
  for (const int i : IndexRange(nsegs + 1)) {
    repedges[ne_start + i] = mesh_edge;
  }
  if (nsegs > 0) {
    const int2 mesh_verts = bs.mesh_info.mesh.edges()[mesh_edge];
    /* Find a bevvert for one of the mesh_verts. */
    int bv = bs.vert_bevverts()[mesh_verts[0]];
    if (bv == -1) {
      bv = bs.vert_bevverts()[mesh_verts[1]];
    }
    BLI_assert(bv != -1);
    const int end = bs.bevedge_vert_end(bv, be);
    const int epos = bs.bevedge_pos(be, bv);
    BLI_assert(epos != -1);
    /* New faces are numered 0 to nsegs-1, left to right. */
    const int fa = bs.face_prev(bv, epos);
    const int fb = bs.face_next(bv, epos);
    const int fpre = end == 0 ? fa : fb;
    const int fpost = end == 0 ? fb : fa;
    const int nf_start = bs.bevedge_newfaces()[be][0];
    const int first_half_end = (nsegs % 2) == 1 ? nsegs / 2 : nsegs / 2 - 1;
    for (const int i : IndexRange(nsegs)) {
      repfaces[nf_start + i] = i <= first_half_end ? fpre : fpost;
    }
  }
}

/** Build the vertex meshes. */
void BevelState::build_vertex_meshes()
{
  bevedge_attach_verts_ = Array<int2>(this->bevedges_num, int2(-1, -1));

  threading::parallel_for(IndexRange(bevverts_num), 10'000, [&](IndexRange range) {
    for (const int bv : range) {
      topology::build_vmesh_skeleton(
          bv,
          newvert_positions_.as_mutable_span().slice(bevvert_newverts_[bv]),
          bevedge_attach_verts_.as_mutable_span(),
          *this);
      const MeshPattern &pat = bevvert_meshpatterns_[bv];
      const int anchors_num = pat.num_anchors;
      profile::AnchorProfiles profiles(anchors_num);
      profile::calculate_profiles(bv, profiles, *this);
      topology::build_internal_vmesh(
          bv, profiles, newvert_positions_.as_mutable_span().slice(bevvert_newverts_[bv]), *this);
      if (bevvert_newfaces_[bv].size() > 0) {
        const int first_v = bevvert_newverts_[bv][0];
        const int first_e = bevvert_newedges_[bv][0];
        const int first_f = bevvert_newfaces_[bv][0];
        for (const int f : IndexRange(bevvert_newfaces_[bv].size())) {
          auto [verts, edges] = pat.face_verts_and_edges(f, first_v, first_e);
          build_newface(f + first_f, verts, edges);
          for (const int i : edges.index_range()) {
            build_newedge(edges[i], verts[i], verts[(i + 1) % verts.size()]);
          }
        }
      }
      if (any_vert_attributes) {
        set_vertex_mesh_reps(bv,
                             newvert_repverts_.as_mutable_span(),
                             newedge_repedges_.as_mutable_span(),
                             newface_repfaces_.as_mutable_span(),
                             *this);
      }
      // DEBUG!!
      dump_bevvert(bv, *this);
      draw_bevvert(bv, *this);
      for (const int mapi : IndexRange(uvmaps_num)) {
        uv::calculate_vertex_mesh_face_uvs(bv, mapi, this->uv_attributes_, *this);
      }
    }
  });
}

/** Build the edge meshes. */
void BevelState::build_edge_meshes()
{
  threading::parallel_for(IndexRange(bevedges_num), 10'000, [&](IndexRange range) {
    for (const int be : range) {
      const int mesh_edge = bevedge_mesh_edges_[be];
      /* Get v0 and v1 to be in the same order as in the underlying mesh edge. */
      const int2 meshvs = mesh_info.mesh.edges()[mesh_edge];
      int bv0 = vert_bevverts_[meshvs[0]];
      int bv1 = vert_bevverts_[meshvs[1]];
      const int2 poses = bevedge_attach_verts_[be];
      /* It is possible that one or the other end of be does not have a bv (it will be -1).
       * In that case, we encode the original mesh vertex index as a "new vertex index" by
       * adding one and negating.
       */
      const int attach_nv0 = bv0 >= 0 ? bevvert_newverts_[bv0][poses[0]] : -(meshvs[0] + 1);
      const int attach_nv1 = bv1 >= 0 ? bevvert_newverts_[bv1][poses[1]] : -(meshvs[1] + 1);
      if (!bevedge_is_beveled(be)) {
        /* Just a single edge, but with one or both ends attached to new vertices. */
        const int ne = bevedge_newedges_[be][0];
        newedge_vertpairs_[ne] = int2(attach_nv0, attach_nv1);
      }
      else {
        /* Make params.segments quads.
         * #attach_nv0 is upper left, #attach_nv1 is lower right. */
        const int first_nv0 = bevvert_newverts_[bv0][0];
        const int first_nv1 = bevvert_newverts_[bv1][0];
        const int first_ne0 = bevvert_newedges_[bv0][0];
        const int first_ne1 = bevvert_newedges_[bv1][0];
        const int first_f = bevedge_newfaces_[be][0];
        const MeshPattern &pat0 = bevvert_meshpatterns_[bv0];
        const MeshPattern &pat1 = bevvert_meshpatterns_[bv1];
        /* Normally go clockwise (positively) from the attachment point, but for a Line
         * pattern we need to go the opposite way if the attachment point is at the end. */
        const int u_dir = (pat0.kind != MeshKind::Line || poses[0] == 0) ? 1 : -1;
        const int l_dir = (pat1.kind != MeshKind::Line || poses[1] == 0) ? 1 : -1;
        for (const int i : IndexRange(params.segments)) {
          /* Upper left and upper right vertices. */
          const int nv0 = pat0.next_boundary_vert(attach_nv0, i * u_dir, first_nv0);
          const int nv3 = pat0.next_boundary_vert(attach_nv0, (i + 1) * u_dir, first_nv0);
          /* Lower left and lower right vertices. */
          const int nv1 = pat1.next_boundary_vert(
              attach_nv1, (pat1.num_segs - i) * l_dir, first_nv1);
          const int nv2 = pat1.next_boundary_vert(
              attach_nv1, (pat1.num_segs - i - 1) * l_dir, first_nv1);
          const Array<int, 4> nverts = {nv0, nv1, nv2, nv3};
          const int ne_l = bevedge_newedges_[be][i];
          const int ne_r = bevedge_newedges_[be][i + 1];
          const int ne_t = pat0.boundary_edge_from_vert(
              u_dir == 1 ? nv0 : nv3, first_nv0, first_ne0);
          const int ne_b = pat1.boundary_edge_from_vert(
              l_dir == 1 ? nv2 : nv1, first_nv1, first_ne1);
          const Array<int, 4> nedges = {ne_l, ne_b, ne_r, ne_t};
          build_newface(first_f + i, nverts, nedges);
          build_newedge(ne_l, nv0, nv1);
          /* If bottom or top are Line patterns, build them here. */
          if (pat0.kind == MeshKind::Line) {
            build_newedge(ne_t, nv3, nv0);
          }
          if (pat1.kind == MeshKind::Line) {
            build_newedge(ne_b, nv1, nv2);
          }
          if (i == params.segments - 1) {
            build_newedge(ne_r, nv2, nv3);
          }
        }
      }
      if (any_edge_attributes) {
        set_edge_mesh_reps(
            be, newedge_repedges_.as_mutable_span(), newface_repfaces_.as_mutable_span(), *this);
      }
      for (const int mapi : IndexRange(uvmaps_num)) {
        uv::calculate_edge_mesh_uvs(be, mapi, this->uv_attributes_, *this);
      }
    }
  });
}

/** Build the face meshes. I.e., reconstruct faces that have bevel involvement. */
void BevelState::build_face_meshes()
{
  const Mesh &mesh = mesh_info.mesh;
  threading::parallel_for(IndexRange(bevfaces_num), 10'000, [&](IndexRange range) {
    for (const int bf : range) {
      const int newf = bevface_newfaces_[bf][0];
      const int meshf = bevface_mesh_faces_[bf];
      IndexRange newcorners = newface_faces_face_[newf];
      SmallIntArray nverts(newcorners.size());
      SmallIntArray nedges(newcorners.size());
      IndexRange origcorners = mesh.faces()[meshf];
      int newc = 0;
      int meshe_prev = mesh.corner_edges()[origcorners.last()];
      int be_prev = edge_bevedges_[meshe_prev];
      for (const int i : IndexRange(origcorners.size())) {
        const int origc = origcorners[i];
        const int meshv = mesh.corner_verts()[origc];
        const int meshe = mesh.corner_edges()[origc];
        const int bv = vert_bevverts_[meshv];
        const int be = edge_bevedges_[meshe];
        if (bv < 0) {
          /* Use original vert. Encode it as -(meshv+1). */
          nverts[newc] = -(meshv + 1);
          /* nedges[newc] will be set below. */
          newc++;
        }
        else {
          /* This is a bevel involved vert. There should also be a bevedge in and out. */
          BLI_assert(be_prev >= 0 && be >= 0);
          const MeshPattern &pat = bevvert_meshpatterns_[bv];
          int pos_prev = bevedge_attach_verts_[be_prev][bevedge_vert_end(bv, be_prev)];
          int pos = bevedge_attach_verts_[be][bevedge_vert_end(bv, be)];
          if (bevedge_is_beveled(be)) {
            /* Get to the other side of beveled edge. */
            pos = pat.next_boundary_vert(pos, params.segments, 0);
          }
          const int first_v = bevvert_newverts_[bv][0];
          const int first_e = bevvert_newedges_[bv][0];
          auto [verts, edges] = pat.boundary_vert_and_edges(pos_prev, pos, first_v, first_e);
          for (const int c : verts.index_range()) {
            nverts[newc] = verts[c];
            nedges[newc] = edges[c];
            newc++;
          }
        }
        /* The last edge actually needs to be the one for be. */
        if (be == -1) {
          nedges[newc - 1] = -(meshe + 1);
        }
        else {
          if (!bevedge_is_beveled(be) || bevedge_vert_end(bv, be) == 1) {
            nedges[newc - 1] = bevedge_newedges_[be][0];
          }
          else {
            nedges[newc - 1] = bevedge_newedges_[be][params.segments];
          }
        }
        be_prev = be;
      }
      build_newface(newf, nverts, nedges);
      if (any_face_attributes) {
        newface_repfaces_[newf] = meshf;
      }
      for (const int mapi : IndexRange(uvmaps_num)) {
        uv::calculate_face_mesh_uvs(bf, mapi, this->uv_attributes_, *this);
      }
    }
  });
}

static int count_bevface_corners(const BevelState &bs)
{
  const OffsetIndices faces = bs.mesh_info.mesh.faces();
  return threading::parallel_reduce(
      IndexRange(bs.bevfaces_num),
      10'000,
      0,
      [&](const IndexRange range, int sum) {
        for (const int i : range) {
          const int f = bs.bevface_mesh_faces()[i];
          sum += faces[f].size();
        }
        return sum;
      },
      [](const int a, const int b) { return a + b; });
}

static void remap_verts(const OffsetIndices<int> src_faces,
                        const OffsetIndices<int> dst_faces,
                        const IndexMask &vert_mask,
                        const IndexMask &edge_mask,
                        const IndexMask &face_mask,
                        const Span<int2> src_edges,
                        const Span<int> src_corner_verts,
                        MutableSpan<int2> dst_edges,
                        MutableSpan<int> dst_corner_verts,
                        Span<int> map)
{
  threading::parallel_invoke(
      vert_mask.size() > 1024,
      [&]() {
        face_mask.foreach_index(GrainSize(512), [&](const int64_t src_i, const int64_t dst_i) {
          const IndexRange src_face = src_faces[src_i];
          const IndexRange dst_face = dst_faces[dst_i];
          for (const int i : src_face.index_range()) {
            dst_corner_verts[dst_face[i]] = map[src_corner_verts[src_face[i]]];
          }
        });
      },
      [&]() {
        edge_mask.foreach_index(GrainSize(512), [&](const int64_t src_i, const int64_t dst_i) {
          dst_edges[dst_i][0] = map[src_edges[src_i][0]];
          dst_edges[dst_i][1] = map[src_edges[src_i][1]];
        });
      });
}

static void remap_edges(const OffsetIndices<int> src_faces,
                        const OffsetIndices<int> dst_faces,
                        const IndexMask &face_mask,
                        const Span<int> src_corner_edges,
                        MutableSpan<int> dst_corner_edges,
                        Span<int> map)
{
  face_mask.foreach_index(GrainSize(512), [&](const int64_t src_i, const int64_t dst_i) {
    const IndexRange src_face = src_faces[src_i];
    const IndexRange dst_face = dst_faces[dst_i];
    for (const int i : src_face.index_range()) {
      dst_corner_edges[dst_face[i]] = map[src_corner_edges[src_face[i]]];
    }
  });
}

/** Assemble the result Mesh. */
static std::optional<Mesh *> build_mesh(const BevelState &bs,
                                        const bke::AttributeFilter & /*attribute_filter*/)
{
  const Mesh &src_mesh = bs.mesh_info.mesh;
  const int src_survive_nverts = src_mesh.verts_num - bs.bevverts_num;
  const int src_survive_nedges = src_mesh.edges_num - bs.bevedges_num;
  const int src_survive_nfaces = src_mesh.faces_num - bs.bevfaces_num;
  const int src_survive_ncorners = src_mesh.corners_num - count_bevface_corners(bs);
  const int result_nverts = src_survive_nverts + bs.newverts_num;
  const int result_nedges = src_survive_nedges + bs.newedges_num;
  const int result_nfaces = src_survive_nfaces + bs.newfaces_num;
  const int result_ncorners = src_survive_ncorners + bs.newcorners_num;
  IndexMaskMemory memory;
  IndexMask src_survive_verts = bs.bevverts_mask().complement(IndexMask(src_mesh.verts_num),
                                                              memory);
  Array<int> src_vert_map(src_mesh.verts_num, -1);
  index_mask::build_reverse_map(src_survive_verts, src_vert_map.as_mutable_span());
  IndexMask src_survive_edges = bs.bevedges_mask().complement(IndexMask(src_mesh.edges_num),
                                                              memory);
  Array<int> src_edge_map(src_mesh.edges_num, -1);
  index_mask::build_reverse_map(src_survive_edges, src_edge_map.as_mutable_span());
  IndexMask src_survive_faces = bs.bevfaces_mask().complement(IndexMask(src_mesh.faces_num),
                                                              memory);
  Array<int> src_face_map(src_mesh.faces_num, -1);
  index_mask::build_reverse_map(src_survive_faces, src_face_map.as_mutable_span());

  IndexMask src_survive_corners;
  if (bs.any_corner_attributes || bs.uvmaps_num > 0) {
    /* Need src_survive_corners only if there are corner attributes to copy. */
    Array<bool> need_corner(src_mesh.corners_num, false);
    src_survive_faces.foreach_index([&](const int f) {
      for (const int c : src_mesh.faces()[f]) {
        need_corner[c] = true;
      }
    });
    src_survive_corners = IndexMask::from_bools(need_corner, memory);
  }

  Mesh *dst_mesh = BKE_mesh_new_nomain_from_template(
      &src_mesh, result_nverts, result_nedges, result_nfaces, result_ncorners);
  MutableSpan<int2> dst_edges = dst_mesh->edges_for_write();

  const Span<int2> src_edges = src_mesh.edges();
  const OffsetIndices src_faces = src_mesh.faces();
  const Span<int> src_corner_verts = src_mesh.corner_verts();
  const Span<int> src_corner_edges = src_mesh.corner_edges();
  const OffsetIndices<int> dst_faces = offset_indices::gather_selected_offsets(
      src_faces,
      src_survive_faces,
      dst_mesh->face_offsets_for_write().take_front(src_survive_faces.size() + 1));
  MutableSpan<int> dst_corner_verts = dst_mesh->corner_verts_for_write();
  MutableSpan<int> dst_corner_edges = dst_mesh->corner_edges_for_write();

  threading::parallel_invoke(
      src_survive_verts.size() > 1024,
      [&]() {
        remap_verts(src_faces,
                    dst_faces,
                    src_survive_verts,
                    src_survive_edges,
                    src_survive_faces,
                    src_edges,
                    src_corner_verts,
                    dst_edges,
                    dst_corner_verts,
                    src_vert_map);
      },
      [&]() {
        remap_edges(src_faces,
                    dst_faces,
                    src_survive_faces,
                    src_corner_edges,
                    dst_corner_edges,
                    src_edge_map);
      });

  /* We want to do something like bke::gather_attributes, but that assumes that
   * we have indices to map the entire destination space, but here we only
   * have indices for the surviving prefixes, so make similar code but
   * use array_utils::gather directly with a prefix of the destination.
   */
  const bke::AttributeAccessor src_attributes = src_mesh.attributes();
  bke::MutableAttributeAccessor dst_attributes = dst_mesh->attributes_for_write();
  src_attributes.foreach_attribute([&](const bke::AttributeIter &iter) {
    if (ELEM(iter.name, ".edge_verts", ".corner_vert", ".corner_edge")) {
      return;
    }
    const bke::GAttributeReader src = iter.get();
    bke::GSpanAttributeWriter dst = dst_attributes.lookup_or_add_for_write_span(
        iter.name, iter.domain, iter.data_type);
    IndexMask *survive_mask = nullptr;
    switch (iter.domain) {
      case bke::AttrDomain::Point:
        survive_mask = &src_survive_verts;
        break;
      case bke::AttrDomain::Edge:
        survive_mask = &src_survive_edges;
        break;
      case bke::AttrDomain::Face:
        survive_mask = &src_survive_faces;
        break;
      case bke::AttrDomain::Corner:
        survive_mask = &src_survive_corners;
        break;
      default:
        /* Not expecting any other domain for Mesh. */
        BLI_assert(false);
        return;
    }
    array_utils::gather(src.varray, *survive_mask, dst.span.take_front(survive_mask->size()));
    dst.finish();
  });

  auto mixed_vert_map = [&](const int v) {
    if (v < 0) {
      return src_vert_map[-(v + 1)];
    }
    else {
      return src_survive_nverts + v;
    }
  };
  auto mixed_edge_map = [&](const int e) {
    if (e < 0) {
      return src_edge_map[-(e + 1)];
    }
    else {
      return src_survive_nedges + e;
    }
  };
  auto new_vert_map = [&](const int nv) { return src_survive_nverts + nv; };
  auto new_edge_map = [&](const int ne) { return src_survive_nedges + ne; };
  auto new_face_map = [&](const int nf) { return src_survive_nfaces + nf; };
  auto new_corner_map = [&](const int nc) { return src_survive_ncorners + nc; };

  /* TODO: move the following out into functions and parallelize.  */
  MutableSpan<float3> dst_positions = dst_mesh->vert_positions_for_write();
  Span<float3> new_positions = bs.newvert_positions();
  for (const int nv : IndexRange(bs.newverts_num)) {
    dst_positions[new_vert_map(nv)] = new_positions[nv];
  }
  for (const int ne : IndexRange(bs.newedges_num)) {
    const int emapped = new_edge_map(ne);
    int2 vertpair = bs.newedge_vertpairs()[ne];
    dst_edges[emapped][0] = mixed_vert_map(vertpair[0]);
    dst_edges[emapped][1] = mixed_vert_map(vertpair[1]);
  }
  MutableSpan<int> dst_offset_indices = dst_mesh->face_offsets_for_write();
  const Span<int> new_face_offsets = bs.newface_faces_face_offsets();
  const int new_offset = dst_offset_indices[src_survive_nfaces];
  for (const int nf : IndexRange(bs.newfaces_num + 1)) {
    int fmapped = new_face_map(nf);
    dst_offset_indices[fmapped] = new_face_offsets[nf] + new_offset;
  }
  OffsetIndices<int> full_dst_faces(dst_mesh->face_offsets());
  for (const int nf : IndexRange(bs.newfaces_num)) {
    int fmapped = new_face_map(nf);
    const IndexRange new_face = bs.newface_faces_face()[nf];
    const IndexRange dst_face = full_dst_faces[fmapped];
    for (const int i : dst_face.index_range()) {
      dst_corner_verts[dst_face[i]] = mixed_vert_map(bs.newcorner_verts()[new_face[i]]);
      dst_corner_edges[dst_face[i]] = mixed_edge_map(bs.newcorner_edges()[new_face[i]]);
    }
  }

  /* Most attributes are copied from the representative src elements. */
  src_attributes.foreach_attribute([&](const bke::AttributeIter &iter) {
    if (ELEM(iter.name, "position", ".edge_verts", ".corner_vert", ".corner_edge")) {
      return;
    }
    const bke::GAttributeReader src = iter.get();
    bke::GSpanAttributeWriter dst = dst_attributes.lookup_or_add_for_write_span(
        iter.name, iter.domain, iter.data_type);
    GMutableSpan dst_span = dst.span;
    switch (iter.domain) {
      case bke::AttrDomain::Point: {
        Span<int> rep = bs.newvert_repverts();
        for (const int nv : bs.newvert_repverts().index_range()) {
          if (rep[nv] >= 0) {
            src.varray.get(rep[nv], dst_span[new_vert_map(nv)]);
          }
        }
        break;
      }
      case bke::AttrDomain::Edge: {
        Span<int> rep = bs.newedge_repedges();
        for (const int ne : bs.newedge_repedges().index_range()) {
          if (rep[ne] >= 0) {
            src.varray.get(rep[ne], dst_span[new_edge_map(ne)]);
          }
        }
        break;
      }
      case bke::AttrDomain::Face: {
        Span<int> rep = bs.newface_repfaces();
        for (const int nf : bs.newface_repfaces().index_range()) {
          if (rep[nf] >= 0) {
            src.varray.get(rep[nf], dst_span[new_face_map(nf)]);
          }
        }
        break;
      }
      case bke::AttrDomain::Corner: {
        const int uv_map_index = bs.find_uv_map_index(iter.name);
        if (uv_map_index != -1) {
          const Array<float2> &uv_values = bs.uv_attributes()[uv_map_index];
          GSpan uvspan(dst_span.type(), uv_values.data(), uv_values.size());
          dst_span.copy_from(GSpan(uvspan));
        }
        else {
          Span<int> rep = bs.newcorner_repcorners();
          for (const int nc : bs.newcorner_repcorners().index_range()) {
            if (rep[nc] >= 0) {
              src.varray.get(rep[nc], dst_span[new_corner_map(nc)]);
            }
          }
        }
        break;
      }
      default:
        /* Not expecting any other domain for Mesh. */
        BLI_assert(false);
        return;
    }
    dst.finish();
  });
  if (false) {
    fmt::println("dst mesh");
    print_float3_span(dst_mesh->vert_positions(), "vert_positions");
    print_int2_span(dst_mesh->edges(), "edges");
    print_offsetindices(dst_mesh->faces(), "faces");
    print_span(dst_mesh->corner_verts(), "corner_verts");
    print_span(dst_mesh->corner_edges(), "corner_edges");
  }
  /* TEMP: while developing, before we have attributes done properly. */
  bke::mesh_smooth_set(*dst_mesh, false, true);
  BLI_assert(bke::mesh_is_valid(*dst_mesh));

  return dst_mesh;
}

std::optional<Mesh *> mesh_bevel(const Mesh &src_mesh,
                                 const IndexMask &selection,
                                 const BevelParameters &params,
                                 const bke::AttributeFilter &attribute_filter)
{
  auto all_zero = [](const Array<float> &o) {
    return std::all_of(o.begin(), o.end(), [](float f) { return f == 0.0; });
  };
  if (all_zero(params.offsets[0]) && all_zero(params.offsets[1]) && all_zero(params.offsets[2]) &&
      all_zero(params.offsets[3]))
  {
    return std::nullopt;
  }
  fmt::println("\n\nBEVEL, segments={}\n", params.segments);

  BevelState state(src_mesh, selection, params);
  if (state.bevverts_num == 0 && state.bevedges_num == 0 && state.bevfaces_num == 0) {
    return std::nullopt;
  }
  state.initialize_profile_data();
  state.order_bevedges();
  state.determine_needed_new_elements();
  state.determine_needed_attribute_data();
  state.build_vertex_meshes();
  // dump_bevel_state(state, "before build_edge_meshes");
  state.build_face_meshes();
  state.build_edge_meshes();
  // dump_bevel_state(state, "before build_mesh");
  /* TODO: calculate output attributes, e.g. like create_cylinder_or_cone_mesh. */
  return build_mesh(state, attribute_filter);
}

}  // namespace blender::geometry
