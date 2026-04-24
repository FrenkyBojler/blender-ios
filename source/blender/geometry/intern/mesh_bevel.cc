/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <utility>

#include <fmt/format.h>

#include "BLI_array.hh"
#include "BLI_array_utils.hh"
#include "BLI_atomic_disjoint_set.hh"
#include "BLI_index_mask.hh"
#include "BLI_map.hh"
#include "BLI_math_base.h"
#include "BLI_math_base.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_multi_value_map.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"
#include "BLI_task.hh"
#include "BLI_time.h"
#include "BLI_vector.hh"

#include "BKE_attribute.hh"
#include "BKE_curveprofile.h"
#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.hh"

#include "atomic_ops.h"

#include "GEO_mesh_bevel.hh"

namespace blender::geometry {

enum class FKind {
  ORIG = 0,
  EDGE = 1,
  VERT = 2,
  PROFILE = 3,
};

enum class AngleKind {
  SMALLER = -1,
  STRAIGHT = 0,
  LARGER = 1,
};

struct ProfileSpacing {
  Array<double> xvals;
  Array<double> yvals;
  Array<double> xvals_2;
  Array<double> yvals_2;
  int seg_2;
  float fullness;
};

struct UVFace {
  int f;
  int attached_frep;
};

struct UVLayerInfo {
  Array<int> face_component;
  bool has_uv_layers;
};

using UVVertBucket = Set<int>;
using UVVertMap = Map<int, Vector<UVVertBucket>>;

enum class MeshKind {
  NONE = 0,
  POLY,
  ADJ,
  TRI_FAN,
  CUTOFF,
};

struct NewVert {
  int v;
  float3 co;
};

struct Profile {
  float super_r;
  float height;
  float3 start;
  float3 middle;
  float3 end;
  float3 plane_no;
  float3 plane_co;
  float3 proj_dir;
  Array<float3> prof_co;
  Array<float3> prof_co_2;
  bool special_params;
};

struct BoundVert;

struct EdgeHalf {
  EdgeHalf *next, *prev;

  int e;
  int fprev;
  int fnext;

  BoundVert *leftv;
  BoundVert *rightv;

  int profile_index;
  int seg;

  float offset_l;
  float offset_r;
  float offset_l_spec;
  float offset_r_spec;

  bool is_bev;
  bool is_rev;
  bool is_seam;
  bool visited_rpo;
};

struct BoundVert {
  BoundVert *next, *prev;

  NewVert nv;

  EdgeHalf *efirst;
  EdgeHalf *elast;
  EdgeHalf *eon;
  EdgeHalf *ebev;

  int index;
  float sinratio;

  BoundVert *adjchain;
  Profile profile;

  bool any_seam;
  bool visited;
  bool is_arc_start;
  bool is_patch_start;
  bool is_profile_start;

  int seam_len;
  int sharp_len;
};

struct VMesh {
  Array<NewVert> mesh;
  BoundVert *boundstart;
  int count;
  int seg;
  MeshKind mesh_kind;
};

struct BevVert {
  int v;
  int edgecount;
  int selcount;
  int wirecount;
  float offset;

  bool any_seam;
  bool visited;

  EdgeHalf *edges;

  Array<int> wire_edges;

  std::unique_ptr<VMesh> vmesh;

  Vector<std::unique_ptr<EdgeHalf>> owned_edges;
  Vector<std::unique_ptr<BoundVert>> owned_bound_verts;
};

class ExtendableMesh {
 public:
  const Mesh &mesh;

  ExtendableMesh(const Mesh &mesh);

  float3 vert_position(const int v) const;
  int2 edge_verts(const int e) const;
  IndexRange face_corners(const int f) const;
  int corner_vert(const int c) const;
  int corner_edge(const int c) const;

  int vert_create(const float3 &co);
  int edge_create(const int v1, const int v2);
  int face_create(Span<int> verts);

  void vert_kill(const int v);
  void edge_kill(const int e);
  void face_kill(const int f);
  void corner_kill(const int c);

  bool is_vert_killed(const int v) const;
  bool is_edge_killed(const int e) const;
  bool is_face_killed(const int f) const;
  bool is_corner_killed(const int c) const;

  GroupedSpan<int> vert_edges() const
  {
    return vert_edges_;
  }
  GroupedSpan<int> edge_faces() const
  {
    return edge_faces_;
  }
  GroupedSpan<int> vert_corners() const
  {
    return vert_corners_;
  }
  int corner_face(const int c) const
  {
    return corner_to_face_map_[c];
  }

 private:
  Vector<float3> new_vert_positions_;
  Vector<int2> new_edges_;
  Vector<int> new_face_offsets_;
  Vector<int> new_corner_verts_;
  Vector<int> new_corner_edges_;

  Array<bool> kill_verts_;
  Array<bool> kill_edges_;
  Array<bool> kill_faces_;
  Array<bool> kill_corners_;

  Map<int2, int> edge_lookup_;

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

ExtendableMesh::ExtendableMesh(const Mesh &mesh) : mesh(mesh)
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

  kill_verts_ = Array<bool>(mesh.verts_num, false);
  kill_edges_ = Array<bool>(mesh.edges_num, false);
  kill_faces_ = Array<bool>(mesh.faces_num, false);
  kill_corners_ = Array<bool>(mesh.corners_num, false);

  edge_lookup_.reserve(mesh.edges_num);
  for (const int e : mesh.edges().index_range()) {
    const int2 verts = mesh.edges()[e];
    const int v1 = std::min(verts[0], verts[1]);
    const int v2 = std::max(verts[0], verts[1]);
    edge_lookup_.add_new(int2(v1, v2), e);
  }

  new_face_offsets_.append(0);
}

float3 ExtendableMesh::vert_position(const int v) const
{
  if (v < mesh.verts_num) {
    return mesh.vert_positions()[v];
  }
  return new_vert_positions_[v - mesh.verts_num];
}

int2 ExtendableMesh::edge_verts(const int e) const
{
  if (e < mesh.edges_num) {
    return mesh.edges()[e];
  }
  return new_edges_[e - mesh.edges_num];
}

IndexRange ExtendableMesh::face_corners(const int f) const
{
  if (f < mesh.faces_num) {
    return mesh.faces()[f];
  }
  const int f_new = f - mesh.faces_num;
  const int start = new_face_offsets_[f_new];
  const int size = new_face_offsets_[f_new + 1] - start;
  return IndexRange(mesh.corners_num + start, size);
}

int ExtendableMesh::corner_vert(const int c) const
{
  if (c < mesh.corners_num) {
    return mesh.corner_verts()[c];
  }
  return new_corner_verts_[c - mesh.corners_num];
}

int ExtendableMesh::corner_edge(const int c) const
{
  if (c < mesh.corners_num) {
    return mesh.corner_edges()[c];
  }
  return new_corner_edges_[c - mesh.corners_num];
}

int ExtendableMesh::vert_create(const float3 &co)
{
  const int index = mesh.verts_num + new_vert_positions_.size();
  new_vert_positions_.append(co);
  return index;
}

int ExtendableMesh::edge_create(const int v1, const int v2)
{
  const int min_v = std::min(v1, v2);
  const int max_v = std::max(v1, v2);
  const int2 key(min_v, max_v);

  if (const int *existing_edge = edge_lookup_.lookup_ptr(key)) {
    return *existing_edge;
  }

  const int index = mesh.edges_num + new_edges_.size();
  new_edges_.append(int2(v1, v2));
  edge_lookup_.add_new(key, index);
  return index;
}

int ExtendableMesh::face_create(Span<int> verts)
{
  const int face_index = mesh.faces_num + new_face_offsets_.size() - 1;

  for (const int i : verts.index_range()) {
    const int v1 = verts[i];
    const int v2 = verts[(i + 1) % verts.size()];
    const int e = edge_create(v1, v2);

    new_corner_verts_.append(v1);
    new_corner_edges_.append(e);
  }

  new_face_offsets_.append(new_corner_verts_.size());
  return face_index;
}

void ExtendableMesh::vert_kill(const int v)
{
  if (v < mesh.verts_num) {
    kill_verts_[v] = true;
  }
}

void ExtendableMesh::edge_kill(const int e)
{
  if (e < mesh.edges_num) {
    kill_edges_[e] = true;
  }
}

void ExtendableMesh::face_kill(const int f)
{
  if (f < mesh.faces_num) {
    kill_faces_[f] = true;
  }
}

void ExtendableMesh::corner_kill(const int c)
{
  if (c < mesh.corners_num) {
    kill_corners_[c] = true;
  }
}

bool ExtendableMesh::is_vert_killed(const int v) const
{
  return v < mesh.verts_num ? kill_verts_[v] : false;
}

bool ExtendableMesh::is_edge_killed(const int e) const
{
  return e < mesh.edges_num ? kill_edges_[e] : false;
}

bool ExtendableMesh::is_face_killed(const int f) const
{
  return f < mesh.faces_num ? kill_faces_[f] : false;
}

bool ExtendableMesh::is_corner_killed(const int c) const
{
  return c < mesh.corners_num ? kill_corners_[c] : false;
}

struct BevelState {
  /* Input parameters. */
  BevelParameters params;

  /* Input selection. */
  IndexMask selection;

  /* The encapsulated extendable mesh. */
  ExtendableMesh emesh;

  /* Memory Ownership. */
  Map<int, std::unique_ptr<BevVert>> vert_hash;

  std::optional<Map<int, FKind>> face_hash;

  Map<int, std::unique_ptr<UVFace>> uv_face_hash;

  Vector<UVVertMap> uv_vert_maps;

  ProfileSpacing pro_spacing;
  ProfileSpacing pro_spacing_miter;
  UVLayerInfo uv_layer_info;

  /* Additional State mimicking bmesh_bevel that isn't fully contained in BevelParameters. */
  bool affect_vertices_odd;
  float pro_super_r;

  /* Feature flags and parameters that the node version might use or we keep to mimic bmesh_bevel.
   */
  bool loop_slide;
  bool limit_offset;
  bool offset_adjust;
  bool mark_seam;
  bool mark_sharp;
  bool harden_normals;

  /* Other data that might be needed depending on what attributes we are transferring. */
  int mat_nr;
  int face_strength_mode;
  int vmesh_method;

  BevelState(const Mesh &mesh, const BevelParameters &params, const IndexMask &selection);
};

BevelState::BevelState(const Mesh &mesh, const BevelParameters &params, const IndexMask &selection)
    : params(params), selection(selection), emesh(mesh)
{
  affect_vertices_odd = false;
  pro_super_r = -std::numbers::ln2_v<float> / logf(sqrtf(params.shape));
  loop_slide = false;
  limit_offset = false;
  offset_adjust = false;
  mark_seam = false;
  mark_sharp = false;
  harden_normals = false;
  mat_nr = -1;
  face_strength_mode = 0;
  vmesh_method = 0;
}

std::optional<Mesh *> mesh_bevel(
    const Mesh &src_mesh,
    const IndexMask &selection,
    const BevelParameters &params,
    const bke::AttributeFilter & /*attribute_filter*/)  // TODO: implement this
{
  BevelState state(src_mesh, params, selection);

  return std::nullopt;
}

}  // namespace blender::geometry
