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

enum class VMeshMethod {
  BEVEL_VMESH_ADJ,
  BEVEL_VMESH_CUTOFF,
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
  EdgeHalf *next = nullptr;
  EdgeHalf *prev = nullptr;

  int e = -1;
  int fprev = -1;
  int fnext = -1;

  BoundVert *leftv = nullptr;
  BoundVert *rightv = nullptr;

  int profile_index = 0;
  int seg = 0;

  float offset_l = 0.0f;
  float offset_r = 0.0f;
  float offset_l_spec = 0.0f;
  float offset_r_spec = 0.0f;

  bool is_bev = false;
  bool is_rev = false;
  bool is_seam = false;
  bool visited_rpo = false;
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

  Array<EdgeHalf> edges;

  Array<int> wire_edges;

  std::unique_ptr<VMesh> vmesh;

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

namespace uv {

class UVLayerInfo {
 public:
  bool has_math_layers = false;
  Array<int> face_component;

  struct Map {
    std::string name;
    Array<float2> values;
  };
  Vector<Map> maps;

  void init(const Mesh &mesh);

  /**
   * Determine connected components of faces, where faces in the same
   * component have contiguous UV coordinates across shared edges for ALL UV maps.
   */
  void find_components(const ExtendableMesh &emesh, int seg);

 private:
  bool contig_ldata_across_edge(const Mesh &mesh, int e, int f1, int f2) const;
};

void UVLayerInfo::init(const Mesh &mesh)
{
  has_math_layers = false;
  const bke::AttributeAccessor attrs = mesh.attributes();
  attrs.foreach_attribute([&](const bke::AttributeIter &iter) {
    if (iter.domain == bke::AttrDomain::Corner && iter.data_type == bke::AttrType::Float2) {
      bke::AttributeReader<float2> uv_reader = iter.get<float2>();
      if (uv_reader) {
        Map map;
        map.name = iter.name;
        map.values = Array<float2>(mesh.corners_num);
        uv_reader.varray.materialize(map.values.as_mutable_span());
        this->maps.append(std::move(map));
        this->has_math_layers = true;
      }
    }
  });
}

bool UVLayerInfo::contig_ldata_across_edge(const Mesh &mesh, int e, int f1, int f2) const
{
  if (!has_math_layers) {
    return true;
  }

  const int2 edge_verts = mesh.edges()[e];
  const int v1 = edge_verts[0];
  const int v2 = edge_verts[1];

  Span<int> corner_verts = mesh.corner_verts();
  IndexRange f1_corners = mesh.faces()[f1];
  IndexRange f2_corners = mesh.faces()[f2];

  int c1_v1 = bke::mesh::face_find_corner_from_vert(f1_corners, corner_verts, v1);
  int c1_v2 = bke::mesh::face_find_corner_from_vert(f1_corners, corner_verts, v2);
  int c2_v1 = bke::mesh::face_find_corner_from_vert(f2_corners, corner_verts, v1);
  int c2_v2 = bke::mesh::face_find_corner_from_vert(f2_corners, corner_verts, v2);

  if (c1_v1 == -1 || c1_v2 == -1 || c2_v1 == -1 || c2_v2 == -1) {
    return false;
  }

  for (const Map &map : maps) {
    if (map.values[c1_v1] != map.values[c2_v1]) {
      return false;
    }
    if (map.values[c1_v2] != map.values[c2_v2]) {
      return false;
    }
  }

  return true;
}

void UVLayerInfo::find_components(const ExtendableMesh &emesh, int seg)
{
  if (!has_math_layers || (seg % 2) == 0) {
    return;
  }

  const Mesh &mesh = emesh.mesh;
  const int totface = mesh.faces_num;
  face_component = Array<int>(totface, -1);
  if (totface == 0) {
    return;
  }

  GroupedSpan<int> edge_faces = emesh.edge_faces();

  Array<bool> in_stack(totface, false);
  Vector<int> stack;
  stack.reserve(totface);

  int current_component = -1;
  for (int f = 0; f < totface; f++) {
    if (face_component[f] == -1 && !in_stack[f]) {
      current_component++;
      stack.append(f);
      in_stack[f] = true;

      while (!stack.is_empty()) {
        int f_curr = stack.pop_last();
        in_stack[f_curr] = false;

        if (face_component[f_curr] != -1) {
          continue;
        }
        face_component[f_curr] = current_component;

        /* Find neighbors via edges. */
        const Span<int> f_edges = mesh.corner_edges().slice(mesh.faces()[f_curr]);
        for (const int e_index : f_edges) {
          const Span<int> adj_faces = edge_faces[e_index];
          for (const int f_other : adj_faces) {
            if (f_other != f_curr) {
              if (face_component[f_other] != -1 || in_stack[f_other]) {
                continue;
              }
              if (contig_ldata_across_edge(mesh, e_index, f_curr, f_other)) {
                stack.append(f_other);
                in_stack[f_other] = true;
              }
            }
          }
        }
      }
    }
  }

  /* We can usually get more pleasing result if components 0 and 1
   * are the topmost and bottom-most (in z-coordinate) components,
   * so adjust component indices to make that so. */
  if (current_component <= 0) {
    return; /* Only one component, so no need to do this. */
  }

  float top_face_z = -1e30f;
  int top_face_component = -1;
  float bot_face_z = 1e30f;
  int bot_face_component = -1;

  const Span<int> corner_verts = mesh.corner_verts();
  const Span<float3> positions = mesh.vert_positions();

  for (int f = 0; f < totface; f++) {
    float min_z = 1e30f;
    float max_z = -1e30f;
    for (const int corner : mesh.faces()[f]) {
      const float fz = positions[corner_verts[corner]].z;
      min_z = std::min(min_z, fz);
      max_z = std::max(max_z, fz);
    }
    const float fz = (min_z + max_z) * 0.5f;

    if (fz > top_face_z) {
      top_face_z = fz;
      top_face_component = face_component[f];
    }
    if (fz < bot_face_z) {
      bot_face_z = fz;
      bot_face_component = face_component[f];
    }
  }

  auto swap_face_components = [&](int c1, int c2) {
    if (c1 == c2) {
      return;
    }
    for (int &c : face_component) {
      if (c == c1) {
        c = c2;
      }
      else if (c == c2) {
        c = c1;
      }
    }
  };

  swap_face_components(face_component[0], top_face_component);
  if (bot_face_component != top_face_component) {
    if (bot_face_component == 0) {
      /* It was swapped with old top_face_component. */
      bot_face_component = top_face_component;
    }
    swap_face_components(face_component[1], bot_face_component);
  }
}

}  // namespace uv

namespace construct {

/* Assume e1 and e2 both share some vert. Do they share a face?
 * If they share a face then there is some corner around e1 that is in a face
 * where the next or previous edge in the face must be e2. */
static bool edges_face_connected_at_vert(const ExtendableMesh &emesh, const int e1, const int e2)
{
  const GroupedSpan<int> edge_faces = emesh.edge_faces();
  const Span<int> e1_faces = edge_faces[e1];
  const Span<int> e2_faces = edge_faces[e2];
  for (const int f1 : e1_faces) {
    if (e2_faces.contains(f1)) {
      return true;
    }
  }
  return false;
}

/* Return 1 if a and b are in CCW order on the normal side of f,
 * and -1 if they are reversed, and 0 if there is no shared face f. */
static int bev_ccw_test(const ExtendableMesh &emesh, const int a, const int b, const int f)
{
  if (f == -1) {
    return 0;
  }
  const IndexRange corners = emesh.face_corners(f);
  int ca = -1;
  int cb = -1;
  for (const int c : corners) {
    if (emesh.corner_edge(c) == a) {
      ca = c - corners.start();
    }
    if (emesh.corner_edge(c) == b) {
      cb = c - corners.start();
    }
  }
  if (ca == -1 || cb == -1) {
    return 0;
  }
  return ((cb + 1) % corners.size() == ca) ? 1 : -1;
}

/* See if we have usual case for bevel edge order:
 * there is an ordering such that all the faces are between
 * successive edges and form a manifold "cap" at bv.
 * If this is the case, set bv->edges to such an order
 * and return true; else unmark any partial path and return false.
 * Assume the first edge is already in bv->edges[0].e.
 *
 * Add edges to bv->edges in order that keeps adjacent edges sharing
 * a unique face, if possible. */
static bool fast_bevel_edge_order(const ExtendableMesh &emesh, BevVert *bv)
{
  int ntot = bv->edgecount;

  EdgeHalf *eh = &bv->edges[0];
  int e = eh->e;
  if (emesh.edge_faces()[e].is_empty()) {
    return false;
  }

  for (int i = 1; i < ntot; i++) {
    int num_shared_face = 0;
    int first_suc = -1;
    for (const int e2 : emesh.vert_edges()[bv->v]) {
      bool used = false;
      for (int k = 0; k < i; k++) {
        if (bv->edges[k].e == e2) {
          used = true;
          break;
        }
      }
      if (used || bv->wire_edges.as_span().contains(e2)) {
        continue;
      }

      for (const int f : emesh.edge_faces()[e2]) {
        if (emesh.edge_faces()[e].contains(f)) {
          num_shared_face++;
          if (first_suc == -1) {
            first_suc = e2;
          }
        }
      }
      if (num_shared_face >= 3) {
        break;
      }
    }
    if (num_shared_face == 1 || (i == 1 && num_shared_face == 2)) {
      eh = &bv->edges[i];
      eh->e = e = first_suc;
    }
    else {
      for (int k = 1; k < i; k++) {
        bv->edges[k].e = -1;
      }
      return false;
    }
  }
  return true;
}

/* Do a depth first search to try to find a path that orders the rest of the edges
 * (after i) around a vertex bv, such that successive edges share a face.
 * Also prefer paths where the last edge shares a face with the first edge (bv->edges[0].e),
 * but will accept a path that doesn't close if it is the longest one found.
 * This is needed to handle cases where there are multiple faces between edges, or "shells"
 * of "internal faces" at a vertex -- i.e., faces that bridge between the edges that naturally
 * form a manifold cap around bv. It is rare to have more than one of these, so unlikely
 * that the exponential time case will be hit in practice.
 * Returns the new index i' where bv->edges[i'] ends the best path found.
 * The path will be recorded in bv->edges and used edges will be marked.
 */
static int bevel_edge_order_extend(const ExtendableMesh &emesh, BevVert *bv, int i)
{
  Vector<int, 4> sucs;
  Vector<int, 16> save_path;

  int e = bv->edges[i].e;

  for (const int e2 : emesh.vert_edges()[bv->v]) {
    bool used = false;
    for (int k = 0; k <= i; k++) {
      if (bv->edges[k].e == e2) {
        used = true;
        break;
      }
    }
    if (!used && !bv->wire_edges.as_span().contains(e2)) {
      if (edges_face_connected_at_vert(emesh, e, e2)) {
        sucs.append(e2);
      }
    }
  }

  const int nsucs = sucs.size();

  int bestj = i;
  int j = i;
  for (int sucindex = 0; sucindex < nsucs; sucindex++) {
    int nexte = sucs[sucindex];
    bv->edges[j + 1].e = nexte;
    int tryj = bevel_edge_order_extend(emesh, bv, j + 1);
    if (tryj > bestj ||
        (tryj == bestj && edges_face_connected_at_vert(emesh, bv->edges[tryj].e, bv->edges[0].e)))
    {
      bestj = tryj;
      save_path.clear();
      for (int k = j + 1; k <= bestj; k++) {
        save_path.append(bv->edges[k].e);
      }
    }
    for (int k = j + 1; k <= tryj; k++) {
      bv->edges[k].e = -1;
    }
  }

  if (bestj > j) {
    for (int k = j + 1; k <= bestj; k++) {
      bv->edges[k].e = save_path[k - (j + 1)];
    }
  }
  return bestj;
}

/* Fill in bv->edges with a good ordering of non-wire edges around bv->v.
 * Use only edges where wire_edges is not set (if edge beveling, others are wire).
 * first_e is a good edge to start with. */
static void find_bevel_edge_order(const ExtendableMesh &emesh, BevVert *bv, int first_e)
{
  int ntot = bv->edgecount;
  for (int i = 0;;) {
    bv->edges[i].e = first_e;
    if (i == 0 && fast_bevel_edge_order(emesh, bv)) {
      break;
    }
    i = bevel_edge_order_extend(emesh, bv, i);
    i++;
    if (i >= bv->edgecount) {
      break;
    }
    first_e = -1;
    for (const int e : emesh.vert_edges()[bv->v]) {
      bool used = false;
      for (int k = 0; k < i; k++) {
        if (bv->edges[k].e == e) {
          used = true;
          break;
        }
      }
      if (used || bv->wire_edges.as_span().contains(e)) {
        continue;
      }
      if (first_e == -1) {
        first_e = e;
      }
      if (emesh.edge_faces()[e].size() == 1) {
        first_e = e;
        break;
      }
    }
  }
  for (int i = 0; i < ntot; i++) {
    EdgeHalf *eh = &bv->edges[i];
    EdgeHalf *eh2 = (i == bv->edgecount - 1) ? &bv->edges[0] : &bv->edges[i + 1];
    int e = eh->e;
    int e2 = eh2->e;
    if (eh->fnext != -1 || eh2->fprev != -1) {
      continue;
    }
    int bestf = -1;
    for (const int f : emesh.edge_faces()[e]) {
      if (emesh.edge_faces()[e2].contains(f)) {
        const IndexRange corners = emesh.face_corners(f);
        for (const int c : corners) {
          if (emesh.corner_vert(c) == bv->v) {
            bestf = f;
            break;
          }
        }
      }
    }
    if (bestf != -1) {
      eh->fnext = eh2->fprev = bestf;
    }
  }
}

}  // namespace construct

struct BevelState;

namespace construct {

static void bevel_vert_construct(BevelState &state, int v);

}  // namespace construct

struct BevelState {
  /* Input parameters. */
  BevelParameters params;

  /* Input selection. */
  IndexMask selection;

  /* Bevel affected vertices mask and its memory. */
  index_mask::IndexMaskMemory memory;
  IndexMask bevel_affected_vertices;

  /* The encapsulated extendable mesh. */
  ExtendableMesh emesh;

  /* Memory Ownership. */
  Vector<BevVert> bev_verts;
  Map<int, BevVert *> vert_hash;

  std::optional<Map<int, FKind>> face_hash;

  Map<int, std::unique_ptr<UVFace>> uv_face_hash;

  Vector<UVVertMap> uv_vert_maps;

  ProfileSpacing pro_spacing;
  ProfileSpacing pro_spacing_miter;
  uv::UVLayerInfo uv_layer_info;

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
  VMeshMethod vmesh_method;

  BevelState(const Mesh &mesh, const BevelParameters &params, const IndexMask &selection);
  void initialize_profile_data();
  void uv_init();
};

BevelState::BevelState(const Mesh &mesh, const BevelParameters &params, const IndexMask &selection)
    : params(params), selection(selection), emesh(mesh)
{
  if (params.affect_type == BevelAffect::Vertices) {
    bevel_affected_vertices = selection;
  }
  else {
    Array<bool> is_affected(mesh.verts_num, false);
    selection.foreach_index([&](const int e) {
      const int2 edge_verts = mesh.edges()[e];
      is_affected[edge_verts[0]] = true;
      is_affected[edge_verts[1]] = true;
    });
    bevel_affected_vertices = IndexMask::from_bools(is_affected, memory);
  }

  affect_vertices_odd = false;
  loop_slide = false;
  limit_offset = false;
  offset_adjust = false;
  mark_seam = false;
  mark_sharp = false;
  harden_normals = false;
  mat_nr = -1;
  face_strength_mode = 0;
  vmesh_method = VMeshMethod::BEVEL_VMESH_ADJ;
  if (vmesh_method == VMeshMethod::BEVEL_VMESH_CUTOFF) {
    /* ignoring miters */
    this->params.miter.fill(false);
  }
}

namespace profile {

constexpr float PRO_SQUARE_R = 1e4f;
constexpr float PRO_CIRCLE_R = 2.0f;
constexpr float PRO_LINE_R = 1.0f;
constexpr float PRO_SQUARE_IN_R = 0.0f;

/**
 * Get the coordinate on the superellipse (x^r + y^r = 1), at parameter value x
 * (or, if !rbig, mirrored (y=x)-line).
 * rbig should be true if r > 1.0 and false if <= 1.0.
 * Assume r > 0.0.
 */
static double superellipse_co(double x, float r, bool rbig)
{
  BLI_assert(r > 0.0f);
  double dr = r;
  if (rbig) {
    return math::pow((1.0 - math::pow(x, dr)), (1.0 / dr));
  }
  return 1.0 - math::pow((1.0 - math::pow(1.0 - x, dr)), (1.0 / dr));
}

/* Find xnew > x0 so that distance((x0,y0), (xnew, ynew)) = dtarget.
 * False position Illinois method used because the function is somewhat linear
 * -> linear interpolation converges fast.
 * Assumes that the gradient is always between 1 and -1 for x in [x0, x0+dtarget]. */
static double find_superellipse_chord_endpoint(double x0, double dtarget, float r, bool rbig)
{
  double y0 = superellipse_co(x0, r, rbig);
  const double tol = 1e-13;
  const int maxiter = 10;

  double xmin = x0 + std::numbers::sqrt2 / 2.0 * dtarget;
  xmin = std::min(xmin, 1.0);
  double xmax = x0 + dtarget;
  xmax = std::min(xmax, 1.0);
  double ymin = superellipse_co(xmin, r, rbig);
  double ymax = superellipse_co(xmax, r, rbig);

  double dmaxerr = math::sqrt(math::pow((xmax - x0), 2.0) + math::pow((ymax - y0), 2.0)) - dtarget;
  double dminerr = math::sqrt(math::pow((xmin - x0), 2.0) + math::pow((ymin - y0), 2.0)) - dtarget;

  double xnew = xmax - dmaxerr * (xmax - xmin) / (dmaxerr - dminerr);
  bool lastupdated_upper = true;

  for (int iter = 0; iter < maxiter; iter++) {
    double ynew = superellipse_co(xnew, r, rbig);
    double dnewerr = math::sqrt(math::pow((xnew - x0), 2.0) + math::pow((ynew - y0), 2.0)) -
                     dtarget;
    if (abs(dnewerr) < tol) {
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
  int imax = (seg + 1) / 2 - 1;

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

  for (int i = 0; i <= imax; i++) {
    xvals[i] = i * mx / seg * 2;
    yvals[i] = superellipse_co(xvals[i], r, rbig);
  }
  yvals[0] = 1;

  for (int iter = 0; iter < smoothitermax; iter++) {
    double sum = 0.0;
    double dmin = 2.0;
    double dmax = 0.0;
    for (int i = 0; i < imax; i++) {
      double d = math::sqrt(math::pow((xvals[i + 1] - xvals[i]), 2.0) +
                            math::pow((yvals[i + 1] - yvals[i]), 2.0));
      sum += d;
      dmax = std::max(d, dmax);
      dmin = std::min(d, dmin);
    }
    double davg;
    if (seg_odd) {
      sum += std::numbers::sqrt2 / 2 * (yvals[imax] - xvals[imax]);
      davg = sum / (imax + 0.5);
    }
    else {
      sum += math::sqrt(math::pow((xvals[imax] - mx), 2.0) + math::pow((yvals[imax] - mx), 2.0));
      davg = sum / (imax + 1.0);
    }
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

    for (int i = 1; i <= imax; i++) {
      xvals[i] = find_superellipse_chord_endpoint(xvals[i - 1], davg, r, rbig);
      yvals[i] = superellipse_co(xvals[i], r, rbig);
    }
  }

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

  if (r == PRO_LINE_R) {
    for (int i = 0; i <= n; i++) {
      xvals[i] = double(i) / n;
      yvals[i] = 1.0 - double(i) / n;
    }
    return;
  }
  if (r == PRO_CIRCLE_R) {
    double temp = M_PI_2 / n;
    for (int i = 0; i <= n; i++) {
      xvals[i] = math::sin(i * temp);
      yvals[i] = math::cos(i * temp);
    }
    return;
  }
  if (r == PRO_SQUARE_IN_R) {
    if (!seg_odd) {
      for (int i = 0; i <= n2; i++) {
        xvals[i] = 0.0;
        yvals[i] = 1.0 - double(i) / n2;
        xvals[n - i] = yvals[i];
        yvals[n - i] = xvals[i];
      }
    }
    else {
      double temp = 1.0 / (n2 + std::numbers::sqrt2 / 2.0);
      for (int i = 0; i <= n2; i++) {
        xvals[i] = 0.0;
        yvals[i] = 1.0 - double(i) * temp;
        xvals[n - i] = yvals[i];
        yvals[n - i] = xvals[i];
      }
    }
    return;
  }
  if (r == PRO_SQUARE_R) {
    if (!seg_odd) {
      for (int i = 0; i <= n2; i++) {
        xvals[i] = double(i) / n2;
        yvals[i] = 1.0;
        xvals[n - i] = yvals[i];
        yvals[n - i] = xvals[i];
      }
    }
    else {
      double temp = 1.0 / (n2 + std::numbers::sqrt2 / 2.0);
      for (int i = 0; i <= n2; i++) {
        xvals[i] = double(i) * temp;
        yvals[i] = 1.0;
        xvals[n - i] = yvals[i];
        yvals[n - i] = xvals[i];
      }
    }
    return;
  }
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
  constexpr int circle_fullness_segs = 11;
  static const float circle_fullness[circle_fullness_segs] = {
      0.0f,
      0.559f,
      0.642f,
      0.551f,
      0.646f,
      0.624f,
      0.646f,
      0.619f,
      0.647f,
      0.639f,
      0.647f,
  };

  float fullness;
  if (bs->params.custom_profile) {
    fullness = 0.0f;
    for (int i = 0; i < nseg; i++) {
      fullness += float(bs->pro_spacing.xvals[i] + bs->pro_spacing.yvals[i]) / (2.0f * nseg);
    }
  }
  else {
    if (bs->pro_super_r == PRO_LINE_R) {
      fullness = 0.0f;
    }
    else if (bs->pro_super_r == PRO_CIRCLE_R && nseg > 0 && nseg <= circle_fullness_segs) {
      fullness = circle_fullness[nseg - 1];
    }
    else {
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
 * Fills the ProfileSpacing struct with the 2D coordinates for the profile's vertices.
 * The superellipse used for multi-segment profiles does not have a closed-form way
 * to generate evenly spaced points along an arc. We use an expensive search procedure
 * to find the parameter values that lead to bp->seg even chords.
 * We also want spacing for a number of segments that is a power of 2 >= bp->seg (but at least 4).
 * Use doubles because otherwise we cannot come close to float precision for final results.
 *
 * \param pro_spacing: The struct to fill. Changes depending on whether there needs
 * to be a separate miter profile.
 */
static void set_profile_spacing(BevelState *bs, ProfileSpacing *pro_spacing, bool custom)
{
  int segments = bs->params.segments;

  if (segments <= 1) {
    pro_spacing->seg_2 = 0;
    return;
  }

  int seg_2 = std::max(power_of_2_max_i(bs->params.segments), 4);
  bs->pro_spacing.seg_2 = seg_2;

  /* Sample the seg_2 segments used during vertex mesh subdivision. */
  pro_spacing->xvals_2 = Array<double>(seg_2 + 1);
  pro_spacing->yvals_2 = Array<double>(seg_2 + 1);
  if (seg_2 != segments) {
    if (custom) {
      /* Make sure the curve profile widget's sample table is full of the seg_2 samples. */
      BKE_curveprofile_init(bs->params.custom_profile, short(seg_2));
      for (const int i : IndexRange(seg_2 + 1)) {
        pro_spacing->xvals_2[i] = double(bs->params.custom_profile->segments[i].y);
        pro_spacing->yvals_2[i] = double(bs->params.custom_profile->segments[i].x);
      }
    }
    else {
      find_even_superellipse_chords(
          seg_2, bs->pro_super_r, pro_spacing->xvals_2, pro_spacing->yvals_2);
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
      BKE_curveprofile_init(bs->params.custom_profile, short(segments));
    }
    for (const int i : IndexRange(segments + 1)) {
      pro_spacing->xvals[i] = double(bs->params.custom_profile->segments[i].y);
      pro_spacing->yvals[i] = double(bs->params.custom_profile->segments[i].x);
    }
  }
  else {
    find_even_superellipse_chords(
        segments, bs->pro_super_r, pro_spacing->xvals, pro_spacing->yvals);
  }

  if (seg_2 == segments) {
    std::copy(pro_spacing->xvals.begin(), pro_spacing->xvals.end(), pro_spacing->xvals_2.begin());
    std::copy(pro_spacing->yvals.begin(), pro_spacing->yvals.end(), pro_spacing->yvals_2.begin());
  }
}

}  // namespace profile

void BevelState::initialize_profile_data()
{
  const float psr = -std::numbers::ln2_v<float> /
                    std::log(math::sqrt(this->params.shape > 0 ? this->params.shape : 1e-20f));
  this->pro_super_r = psr;

  if (this->params.shape >= 0.950f) {
    this->pro_super_r = profile::PRO_SQUARE_R;
  }
  else if (abs(psr - profile::PRO_CIRCLE_R) < 1e-4f) {
    this->pro_super_r = profile::PRO_CIRCLE_R;
  }
  else if (abs(psr - profile::PRO_LINE_R) < 1e-4f) {
    this->pro_super_r = profile::PRO_LINE_R;
  }
  else if (abs(psr) < 1e-4f) {
    this->pro_super_r = profile::PRO_SQUARE_IN_R;
  }

  profile::set_profile_spacing(this, &this->pro_spacing, this->params.custom_profile != nullptr);

  if (this->params.segments > 1) {
    this->pro_spacing.fullness = profile::find_profile_fullness(this);
  }
}

void BevelState::uv_init()
{
  face_hash.emplace();

  uv_layer_info.init(emesh.mesh);
  uv_layer_info.find_components(emesh, params.segments);

  uv_vert_maps.clear();
  uv_vert_maps.resize(uv_layer_info.maps.size());
}

namespace construct {

/* Construction around the vertex. */
static void bevel_vert_construct(BevelState &state, int v)
{
  int nsel = 0;
  int tot_edges = 0;
  int tot_wire = 0;
  int first_e = -1;

  const ExtendableMesh &emesh = state.emesh;

  /* Gather input selected edges.
   * Only bevel selected edges that have exactly two incident faces.
   * Want edges to be ordered so that they share faces.
   * There may be one or more chains of shared faces broken by
   * gaps where there are no faces.
   * Want to ignore wire edges completely for edge beveling.
   * TODO: make following work when more than one gap. */

  for (const int e : emesh.vert_edges()[v]) {
    int face_count = emesh.edge_faces()[e].size();

    bool is_selected = (state.params.affect_type != BevelAffect::Vertices &&
                        state.selection.contains(e));
    if (is_selected) {
      BLI_assert(face_count == 2);
      nsel++;
      if (first_e == -1) {
        first_e = e;
      }
    }
    if (face_count == 1) {
      first_e = e;
    }
    if (face_count > 0 || state.params.affect_type == BevelAffect::Vertices) {
      tot_edges++;
    }
    if (face_count == 0) {
      tot_wire++;
    }
  }

  if (first_e == -1 && !emesh.vert_edges()[v].is_empty()) {
    first_e = emesh.vert_edges()[v].first();
  }

  if ((nsel == 0 && state.params.affect_type != BevelAffect::Vertices) ||
      (tot_edges < 2 && state.params.affect_type == BevelAffect::Vertices))
  {
    return;
  }

  state.bev_verts.append({});
  BevVert *bv = &state.bev_verts.last();
  bv->v = v;
  bv->edgecount = tot_edges;
  bv->selcount = nsel;
  bv->wirecount = tot_wire;
  bv->offset = 1.0f;  // TODO: handle vertex group or bevel weights

  bv->edges = Array<EdgeHalf>(tot_edges);

  if (tot_wire > 0) {
    bv->wire_edges = Array<int>(tot_wire);
    int i = 0;
    for (const int e : emesh.vert_edges()[v]) {
      if (emesh.edge_faces()[e].is_empty()) {
        bv->wire_edges[i++] = e;
      }
    }
  }

  bv->vmesh = std::make_unique<VMesh>();
  bv->vmesh->seg = state.params.segments;

  find_bevel_edge_order(emesh, bv, first_e);

  for (int i = 0; i < tot_edges; i++) {
    EdgeHalf *eh = &bv->edges[i];
    int e = eh->e;
    bool is_selected = (state.params.affect_type != BevelAffect::Vertices &&
                        state.selection.contains(e));
    if (is_selected) {
      eh->is_bev = true;
      eh->seg = state.params.segments;
    }
    else {
      eh->is_bev = false;
      eh->seg = 0;
    }

    const int2 edge_verts = emesh.edge_verts(e);
    eh->is_rev = (edge_verts[1] == v);
    eh->leftv = eh->rightv = nullptr;
    eh->profile_index = 0;
  }

  if (tot_edges > 1) {
    int ccw_test_sum = 0;
    for (int i = 0; i < tot_edges; i++) {
      ccw_test_sum += bev_ccw_test(
          emesh, bv->edges[i].e, bv->edges[(i + 1) % tot_edges].e, bv->edges[i].fnext);
    }
    if (ccw_test_sum < 0) {
      for (int i = 0; i <= (tot_edges / 2) - 1; i++) {
        std::swap(bv->edges[i], bv->edges[tot_edges - i - 1]);
        std::swap(bv->edges[i].fprev, bv->edges[i].fnext);
        std::swap(bv->edges[tot_edges - i - 1].fprev, bv->edges[tot_edges - i - 1].fnext);
      }
      if (tot_edges % 2 == 1) {
        int i = tot_edges / 2;
        std::swap(bv->edges[i].fprev, bv->edges[i].fnext);
      }
    }
  }

  for (int i = 0; i < tot_edges; i++) {
    EdgeHalf *eh = &bv->edges[i];
    eh->next = &bv->edges[(i + 1) % tot_edges];
    eh->prev = &bv->edges[(i + tot_edges - 1) % tot_edges];

    if (eh->is_bev) {
      const float offset_src_l = state.params.offsets[0][eh->e];
      const float offset_src_r = state.params.offsets[1][eh->e];
      const float offset_dst_l = state.params.offsets[2][eh->e];
      const float offset_dst_r = state.params.offsets[3][eh->e];

      eh->offset_l_spec = (offset_src_l + offset_dst_l) * 0.5f;
      eh->offset_r_spec = (offset_src_r + offset_dst_r) * 0.5f;
      eh->offset_l = eh->offset_l_spec;
      eh->offset_r = eh->offset_r_spec;
    }
    else {
      eh->offset_l = eh->offset_l_spec = 0.0f;
      eh->offset_r = eh->offset_r_spec = 0.0f;
    }
  }

  state.vert_hash.add_new(v, bv);
}

}  // namespace construct

std::optional<Mesh *> mesh_bevel(
    const Mesh &src_mesh,
    const IndexMask &selection,
    const BevelParameters &params,
    const bke::AttributeFilter & /*attribute_filter*/)  // TODO: implement this
{
  auto all_non_positive = [](const Array<float> &o) {
    return std::ranges::all_of(o, [](float f) { return f <= 0.0f; });
  };
  if (all_non_positive(params.offsets[0]) && all_non_positive(params.offsets[1]) &&
      all_non_positive(params.offsets[2]) && all_non_positive(params.offsets[3]))
  {
    return std::nullopt;
  }

  BevelState state(src_mesh, params, selection);
  state.initialize_profile_data();
  state.uv_init();

  state.bev_verts.reserve(state.bevel_affected_vertices.size());
  state.bevel_affected_vertices.foreach_index([&](const int v) {
    construct::bevel_vert_construct(state, v);
    // TODO: build_boundary and determine_uv_vert_connectivity
  });

  return std::nullopt;
}

}  // namespace blender::geometry
