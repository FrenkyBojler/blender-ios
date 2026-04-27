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
#include "BKE_attribute_filters.hh"
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
  float3 face_normal(const int f) const;
  float3 face_center(const int f) const;
  int corner_vert(const int c) const;
  int corner_edge(const int c) const;

  /* Creates a new vertex at `co`.  `example_vert` is the index of an original vertex
   * whose non-positional attributes will be copied to this new vertex at output time;
   * pass -1 when no representative is known. */
  int vert_create(const float3 &co, int example_vert = -1);
  /* Creates (or looks up) the edge between v1 and v2.  `example_edge` is the original
   * edge index to use for attribute copying; pass -1 when unknown. */
  int edge_create(const int v1, const int v2, int example_edge = -1);
  /* Creates a new face from the given vertex ring.  `example_face` is the original face
   * whose attributes will be copied; pass -1 when unknown (deferred). */
  int face_create(Span<int> verts, int example_face = -1);

  /* Returns the index of any existing edge (original or newly created) between v1 and v2,
   * or -1 if no such edge exists yet. */
  int find_edge(const int v1, const int v2) const
  {
    const int min_v = std::min(v1, v2);
    const int max_v = std::max(v1, v2);
    return edge_lookup_.lookup_default(int2(min_v, max_v), -1);
  }

  /* Allocates per-UV-layer float2 storage for new corners.  Must be called after
   * UVLayerInfo is initialized and before any face_create call. */
  void init_uv_storage(int uv_layers_num);

  /* Per-UV-layer float2 values for new corners (one per layer, parallel to new_corner_verts_). */
  MutableSpan<float2> new_corner_uvs(int layer_index)
  {
    return new_corner_uvs_[layer_index].as_mutable_span();
  }
  Span<float2> new_corner_uvs(int layer_index) const
  {
    return new_corner_uvs_[layer_index].as_span();
  }

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
  /** Returns the number of newly created faces (not counting the original mesh faces). */
  int new_faces_num() const
  {
    /* new_face_offsets_ starts with a sentinel 0 at index 0 and gains one entry per face_create
     * call, so the number of new faces is new_face_offsets_.size() - 1. */
    return int(new_face_offsets_.size()) - 1;
  }

  Span<float3> new_vert_positions() const
  {
    return new_vert_positions_;
  }
  Span<int2> new_edges() const
  {
    return new_edges_;
  }
  /** Face offset array for new faces: size is new_faces_num()+1, sentinel 0 at index 0. */
  Span<int> new_face_offsets() const
  {
    return new_face_offsets_;
  }
  Span<int> new_corner_verts() const
  {
    return new_corner_verts_;
  }
  Span<int> new_corner_edges() const
  {
    return new_corner_edges_;
  }

  /* Per-new-element representative (example) original indices for attribute propagation.
   * -1 means no representative is known. */
  Span<int> new_vert_examples() const
  {
    return new_vert_examples_;
  }
  Span<int> new_edge_examples() const
  {
    return new_edge_examples_;
  }
  Span<int> new_face_examples() const
  {
    return new_face_examples_;
  }
  Span<int> new_corner_examples() const
  {
    return new_corner_examples_;
  }
  const Array<bool> &kill_verts_array() const
  {
    return kill_verts_;
  }
  const Array<bool> &kill_edges_array() const
  {
    return kill_edges_;
  }
  const Array<bool> &kill_faces_array() const
  {
    return kill_faces_;
  }
  const Array<bool> &kill_corners_array() const
  {
    return kill_corners_;
  }

 private:
  Vector<float3> new_vert_positions_;
  Vector<int2> new_edges_;
  Vector<int> new_face_offsets_;
  Vector<int> new_corner_verts_;
  Vector<int> new_corner_edges_;

  /* Representative original element indices for attribute propagation (-1 = unknown). */
  Vector<int> new_vert_examples_;
  Vector<int> new_edge_examples_;
  Vector<int> new_face_examples_;
  Vector<int> new_corner_examples_;

  /* Per-UV-layer float2 values for new corners, indexed [layer][new_corner]. */
  Vector<Vector<float2>> new_corner_uvs_;

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

float3 ExtendableMesh::face_normal(const int f) const
{
  /* Only valid for original mesh faces; new faces are not used in tri_corner_test. */
  BLI_assert(f < mesh.faces_num);
  return mesh.face_normals()[f];
}

float3 ExtendableMesh::face_center(const int f) const
{
  /* Average the corner vertex positions. */
  const IndexRange corners = face_corners(f);
  float3 center(0.0f);
  for (const int c : corners) {
    center += vert_position(corner_vert(c));
  }
  return center / float(corners.size());
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

int ExtendableMesh::vert_create(const float3 &co, const int example_vert)
{
  const int index = mesh.verts_num + new_vert_positions_.size();
  new_vert_positions_.append(co);
  new_vert_examples_.append(example_vert);
  return index;
}

int ExtendableMesh::edge_create(const int v1, const int v2, const int example_edge)
{
  const int min_v = std::min(v1, v2);
  const int max_v = std::max(v1, v2);
  const int2 key(min_v, max_v);

  if (const int *existing_edge = edge_lookup_.lookup_ptr(key)) {
    /* Edge already exists (original or previously created); skip example. */
    return *existing_edge;
  }

  const int index = mesh.edges_num + new_edges_.size();
  new_edges_.append(int2(v1, v2));
  new_edge_examples_.append(example_edge);
  edge_lookup_.add_new(key, index);
  return index;
}

int ExtendableMesh::face_create(Span<int> verts, const int example_face)
{
  const int face_index = mesh.faces_num + new_face_offsets_.size() - 1;

  for (const int i : verts.index_range()) {
    const int v1 = verts[i];
    const int v2 = verts[(i + 1) % verts.size()];
    /* Edges created inside face_create have no single representative edge; use -1. */
    const int e = edge_create(v1, v2, -1);

    new_corner_verts_.append(v1);
    new_corner_edges_.append(e);
    /* Corner examples are deferred; -1 for now. */
    new_corner_examples_.append(-1);
    /* Grow UV storage to match (values initialized to zero). */
    for (Vector<float2> &layer_uvs : new_corner_uvs_) {
      layer_uvs.append(float2(0.0f));
    }
  }

  new_face_offsets_.append(int(new_corner_verts_.size()));
  new_face_examples_.append(example_face);

  return face_index;
}

void ExtendableMesh::init_uv_storage(const int uv_layers_num)
{
  new_corner_uvs_.resize(uv_layers_num);
}

void ExtendableMesh::vert_kill(const int v)
{
  if (v < mesh.verts_num) {
    kill_verts_[v] = true;
    /* Also kill all edges incident to this vertex, matching BMesh's BM_vert_kill semantics. */
    for (const int e : vert_edges()[v]) {
      kill_edges_[e] = true;
    }
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
    /* Also kill all corners of this face, matching BMesh's BM_face_kill semantics. */
    for (const int c : face_corners(f)) {
      kill_corners_[c] = true;
    }
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

  /** Returns true when UV data is contiguous across edge `e` between faces `f1` and `f2`. */
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

namespace geom {

constexpr float BEVEL_EPSILON_D = 1e-6f;
constexpr float BEVEL_EPSILON_SQ = 1e-12f;
constexpr float BEVEL_EPSILON_BIG = 1e-4f;
constexpr float BEVEL_EPSILON_ANG = DEG2RADF(2.0f);
constexpr float BEVEL_SMALL_ANG = DEG2RADF(10.0f);
const float BEVEL_SMALL_ANG_DOT = (1.0f - std::cos(BEVEL_SMALL_ANG));
const float BEVEL_EPSILON_ANG_DOT = (1.0f - std::cos(BEVEL_EPSILON_ANG));

static int edge_other_vert(const ExtendableMesh &emesh, int e, int v)
{
  int2 verts = emesh.edge_verts(e);
  return verts[0] == v ? verts[1] : verts[0];
}

/* Calculate coordinates of a point a distance d from v on e and return it in r_slideco. */
static void slide_dist(const ExtendableMesh &emesh, int e, int v, float d, float r_slideco[3])
{
  float3 v_co = emesh.vert_position(v);
  float3 other_co = emesh.vert_position(geom::edge_other_vert(emesh, e, v));
  float3 dir = other_co - v_co;
  float len = math::length(dir);
  dir /= len;

  if (d > len) {
    d = len - 50.0f * BEVEL_EPSILON_D;
  }
  float3 res = v_co + dir * d;
  copy_v3_v3(r_slideco, res);
}

static bool is_outside_edge(const ExtendableMesh &emesh,
                            EdgeHalf *eh,
                            const float co[3],
                            int *ret_closer_v)
{
  // Actually, BMesh's is_outside_edge uses e->v1 and e->v2.
  int v1 = emesh.edge_verts(eh->e)[0];
  int v2 = emesh.edge_verts(eh->e)[1];
  float3 l1 = emesh.vert_position(v1);
  float3 u_dir = emesh.vert_position(v2) - l1;
  float3 h = float3(co[0], co[1], co[2]) - l1;
  float lenu = math::length(u_dir);
  u_dir /= lenu;
  float lambda = math::dot(u_dir, h);
  if (lambda <= -BEVEL_EPSILON_BIG * lenu) {
    *ret_closer_v = v1;
    return true;
  }
  if (lambda >= (1.0f + BEVEL_EPSILON_BIG) * lenu) {
    *ret_closer_v = v2;
    return true;
  }
  return false;
}

static bool point_between_edges(
    const ExtendableMesh &emesh, const float co[3], int v, int f, EdgeHalf *e1, EdgeHalf *e2)
{
  int v1 = geom::edge_other_vert(emesh, e1->e, v);
  int v2 = geom::edge_other_vert(emesh, e2->e, v);
  float3 dir1 = emesh.vert_position(v) - emesh.vert_position(v1);
  float3 dir2 = emesh.vert_position(v) - emesh.vert_position(v2);
  float3 dirco = emesh.vert_position(v) - float3(co[0], co[1], co[2]);
  dir1 = math::normalize(dir1);
  dir2 = math::normalize(dir2);
  dirco = math::normalize(dirco);
  float ang11 = angle_normalized_v3v3(dir1, dir2);
  float ang1co = angle_normalized_v3v3(dir1, dirco);
  float3 no;
  no = math::cross(dir1, dir2);
  if (math::dot(no, emesh.mesh.face_normals()[f]) < 0.0f) {
    ang11 = float(M_PI * 2.0) - ang11;
  }
  no = math::cross(dir1, dirco);
  if (math::dot(no, emesh.mesh.face_normals()[f]) < 0.0f) {
    ang1co = float(M_PI * 2.0) - ang1co;
  }
  return (ang11 - ang1co > -BEVEL_EPSILON_ANG);
}

/* Is the angle swept from e1 to e2, CCW when viewed from the normal side of f,
 * not a reflex angle or a straight angle? Assume e1 and e2 share a vert. */
static bool edge_edge_angle_less_than_180(const ExtendableMesh &emesh,
                                          const int e1,
                                          const int e2,
                                          const int f)
{
  BLI_assert(f != -1);
  int v = -1, v1 = -1, v2 = -1;
  const int2 ev1 = emesh.edge_verts(e1);
  const int2 ev2 = emesh.edge_verts(e2);
  if (ev1[0] == ev2[0]) {
    v = ev1[0];
    v1 = ev1[1];
    v2 = ev2[1];
  }
  else if (ev1[0] == ev2[1]) {
    v = ev1[0];
    v1 = ev1[1];
    v2 = ev2[0];
  }
  else if (ev1[1] == ev2[0]) {
    v = ev1[1];
    v1 = ev1[0];
    v2 = ev2[1];
  }
  else if (ev1[1] == ev2[1]) {
    v = ev1[1];
    v1 = ev1[0];
    v2 = ev2[0];
  }
  if (v == -1) {
    return false;
  }
  float3 dir1 = emesh.vert_position(v1) - emesh.vert_position(v);
  float3 dir2 = emesh.vert_position(v2) - emesh.vert_position(v);
  float3 cross = math::cross(dir1, dir2);
  return math::dot(cross, emesh.mesh.face_normals()[f]) > 0.0f;
}

static int get_edge_starting_at(const ExtendableMesh &emesh, int f, int vert)
{
  const IndexRange corners = emesh.face_corners(f);
  for (int c : corners) {
    if (emesh.corner_vert(c) == vert) {
      return emesh.corner_edge(c);
    }
  }
  return -1;
}

static int get_edge_ending_at(const ExtendableMesh &emesh, int f, int vert)
{
  const IndexRange corners = emesh.face_corners(f);
  for (int i = 0; i < corners.size(); i++) {
    int c = corners[i];
    int next_c = corners.start() + (i + 1) % corners.size();
    if (emesh.corner_vert(next_c) == vert) {
      return emesh.corner_edge(c);
    }
  }
  return -1;
}

static void offset_meet(const ExtendableMesh &emesh,
                        EdgeHalf *e1,
                        EdgeHalf *e2,
                        int v,
                        int f,
                        bool edges_between,
                        float meetco[3],
                        const EdgeHalf *e_in_plane)
{
  float3 v_co = emesh.vert_position(v);
  /* `dir1` points from e1's far end toward `v`; `dir2` points from `v` away along e2.
   * This asymmetry matches the BMesh convention and ensures that `cross(dir1, norm_v1)`
   * gives a perpendicular that points into the face (i.e. toward the bevel offset). */
  float3 dir1 = v_co - emesh.vert_position(geom::edge_other_vert(emesh, e1->e, v));
  float3 dir2 = emesh.vert_position(geom::edge_other_vert(emesh, e2->e, v)) - v_co;

  float3 dir1n = float3(0.0f);
  float3 dir2p = float3(0.0f);
  if (edges_between) {
    EdgeHalf *e1next = e1->next;
    EdgeHalf *e2prev = e2->prev;
    dir1n = emesh.vert_position(geom::edge_other_vert(emesh, e1next->e, v)) - v_co;
    dir2p = v_co - emesh.vert_position(geom::edge_other_vert(emesh, e2prev->e, v));
  }

  float ang = angle_v3v3(dir1, dir2);
  float3 norm_perp1;
  if (ang < BEVEL_EPSILON_ANG) {
    float3 norm_v = float3(0.0f);
    if (f != -1) {
      norm_v = emesh.mesh.face_normals()[f];
    }
    else {
      int fcount = 0;
      for (EdgeHalf *eloop = e1; eloop != e2; eloop = eloop->next) {
        if (eloop->fnext != -1) {
          norm_v += emesh.mesh.face_normals()[eloop->fnext];
          fcount++;
        }
      }
      if (fcount == 0) {
        norm_v = emesh.mesh.vert_normals()[v];
      }
      else {
        norm_v /= float(fcount);
      }
    }
    float3 dir_sum = dir1 + dir2;
    norm_perp1 = math::normalize(math::cross(dir_sum, norm_v));
    float d = math::max(e1->offset_r, e2->offset_l);
    d = d / math::cos(ang / 2.0f);
    float3 off1a = v_co + norm_perp1 * d;
    copy_v3_v3(meetco, off1a);
  }
  else if (math::abs(ang - float(M_PI)) < BEVEL_EPSILON_ANG) {
    float d = math::max(e1->offset_r, e2->offset_l);
    slide_dist(emesh, e2->e, v, d, meetco);
  }
  else {
    float3 norm_v1, norm_v2;
    if (f != -1 && ang < BEVEL_SMALL_ANG) {
      norm_v1 = norm_v2 = emesh.mesh.face_normals()[f];
    }
    else if (!edges_between) {
      norm_v1 = math::normalize(math::cross(dir2, dir1));
      if (math::dot(norm_v1,
                    f != -1 ? emesh.mesh.face_normals()[f] : emesh.mesh.vert_normals()[v]) < 0.0f)
      {
        norm_v1 = -norm_v1;
      }
      norm_v2 = norm_v1;
    }
    else {
      norm_v1 = math::normalize(math::cross(dir1n, dir1));
      int f_curr = e1->fnext;
      if (math::dot(norm_v1,
                    f_curr != -1 ? emesh.mesh.face_normals()[f_curr] :
                                   emesh.mesh.vert_normals()[v]) < 0.0f)
      {
        norm_v1 = -norm_v1;
      }
      norm_v2 = math::normalize(math::cross(dir2, dir2p));
      f_curr = e2->fprev;
      if (math::dot(norm_v2,
                    f_curr != -1 ? emesh.mesh.face_normals()[f_curr] :
                                   emesh.mesh.vert_normals()[v]) < 0.0f)
      {
        norm_v2 = -norm_v2;
      }
    }

    float3 norm_perp2;
    norm_perp1 = math::normalize(math::cross(dir1, norm_v1));
    norm_perp2 = math::normalize(math::cross(dir2, norm_v2));

    float off1a[3], off1b[3], off2a[3], off2b[3];
    copy_v3_v3(off1a, v_co + norm_perp1 * e1->offset_r);
    copy_v3_v3(off1b, float3(off1a[0], off1a[1], off1a[2]) + dir1);
    copy_v3_v3(off2a, v_co + norm_perp2 * e2->offset_l);
    copy_v3_v3(off2b, float3(off2a[0], off2a[1], off2a[2]) + dir2);

    float isect2[3];
    int isect_kind = isect_line_line_v3(off1a, off1b, off2a, off2b, meetco, isect2);
    if (isect_kind == 0) {
      copy_v3_v3(meetco, off1a);
    }
    else {
      int closer_v;
      if (e1->offset_r == 0.0f && is_outside_edge(emesh, e1, meetco, &closer_v)) {
        copy_v3_v3(meetco, emesh.vert_position(closer_v));
      }
      if (e2->offset_l == 0.0f && is_outside_edge(emesh, e2, meetco, &closer_v)) {
        copy_v3_v3(meetco, emesh.vert_position(closer_v));
      }
      if (edges_between && e1->offset_r > 0.0f && e2->offset_l > 0.0f) {
        if (isect_kind == 2) {
          mid_v3_v3v3(meetco, meetco, isect2);
        }
        for (EdgeHalf *e_loop = e1; e_loop != e2; e_loop = e_loop->next) {
          int fnext = e_loop->fnext;
          if (fnext == -1) {
            continue;
          }
          float plane[4];
          float3 no = emesh.mesh.face_normals()[fnext];
          plane_from_point_normal_v3(plane, v_co, no);
          float dropco[3];
          closest_to_plane_normalized_v3(dropco, plane, meetco);
          if (e_in_plane) {
            float ang = angle_v3v3(no, emesh.mesh.face_normals()[e_in_plane->fnext]);
            if ((math::abs(ang) < BEVEL_SMALL_ANG) ||
                (math::abs(ang - float(M_PI)) < BEVEL_SMALL_ANG))
            {
              continue;
            }
          }
          if (point_between_edges(emesh, dropco, v, fnext, e_loop, e_loop->next)) {
            copy_v3_v3(meetco, dropco);
            break;
          }
        }
      }
    }
  }
}

static bool offset_meet_edge(const ExtendableMesh &emesh,
                             EdgeHalf *e1,
                             EdgeHalf *e2,
                             int v,
                             float meetco[3],
                             float *r_angle)
{
  float3 v_co = emesh.vert_position(v);
  float3 dir1 = emesh.vert_position(geom::edge_other_vert(emesh, e1->e, v)) - v_co;
  float3 dir2 = emesh.vert_position(geom::edge_other_vert(emesh, e2->e, v)) - v_co;
  dir1 = math::normalize(dir1);
  dir2 = math::normalize(dir2);

  float ang = angle_normalized_v3v3(dir1, dir2);
  if (math::abs(ang) < BEVEL_EPSILON_ANG) {
    if (r_angle) {
      *r_angle = 0.0f;
    }
    return false;
  }
  float3 fno = math::cross(dir1, dir2);
  if (math::dot(fno, emesh.mesh.vert_normals()[v]) < 0.0f) {
    ang = 2.0f * float(M_PI) - ang;
    if (r_angle) {
      *r_angle = ang;
    }
    return false;
  }
  if (r_angle) {
    *r_angle = ang;
  }

  if (math::abs(ang - float(M_PI)) < BEVEL_EPSILON_ANG) {
    return false;
  }

  float sinang = math::sin(ang);

  float3 meet_res = v_co;
  if (e1->offset_r == 0.0f) {
    meet_res += dir1 * (e2->offset_l / sinang);
  }
  else {
    meet_res += dir2 * (e1->offset_r / sinang);
  }
  copy_v3_v3(meetco, meet_res);
  return true;
}

static bool good_offset_on_edge_between(
    const ExtendableMesh &emesh, EdgeHalf *e1, EdgeHalf *e2, EdgeHalf *emid, int v)
{
  float ang;
  float meet[3];

  return offset_meet_edge(emesh, e1, emid, v, meet, &ang) &&
         offset_meet_edge(emesh, emid, e2, v, meet, &ang);
}

static bool offset_on_edge_between(const ExtendableMesh &emesh,
                                   EdgeHalf *e1,
                                   EdgeHalf *e2,
                                   EdgeHalf *emid,
                                   int v,
                                   float meetco[3],
                                   float *r_sinratio)
{
  bool retval = false;

  BLI_assert(e1->is_bev && e2->is_bev && !emid->is_bev);

  float ang1, ang2;
  float meet1[3], meet2[3];
  bool ok1 = offset_meet_edge(emesh, e1, emid, v, meet1, &ang1);
  bool ok2 = offset_meet_edge(emesh, emid, e2, v, meet2, &ang2);
  if (ok1 && ok2) {
    mid_v3_v3v3(meetco, meet1, meet2);
    if (r_sinratio) {
      *r_sinratio = (ang1 == 0.0f) ? 1.0f : math::sin(ang2) / math::sin(ang1);
    }
    retval = true;
  }
  else if (ok1 && !ok2) {
    copy_v3_v3(meetco, meet1);
  }
  else if (!ok1 && ok2) {
    copy_v3_v3(meetco, meet2);
  }
  else {
    slide_dist(emesh, emid->e, v, e1->offset_r, meetco);
  }

  return retval;
}

/* -------------------------------------------------------------------- */
/** \name VMesh grid helpers
 * \{ */

/**
 * Return a pointer to the #NewVert at grid position (i, j, k) in `vm`.
 * The grid layout is `mesh[i * nj * nk + j * nk + k]`
 * where `nj = seg/2 + 1` and `nk = seg + 1`.
 */
static NewVert *mesh_vert(VMesh *vm, int i, int j, int k)
{
  const int nk = vm->seg + 1;
  const int nj = vm->seg / 2 + 1;
  return &vm->mesh[i * nj * nk + j * nk + k];
}

/**
 * Return the canonical representative for vmesh position (i, j, k).
 * Due to the rotational symmetry of the vmesh grid, many positions are
 * equivalent; this function maps any (i, j, k) to the one canonical
 * representative in the stored range.
 */
static NewVert *mesh_vert_canon(VMesh *vm, int i, int j, int k)
{
  const int n = vm->count;
  const int ns = vm->seg;
  const int ns2 = ns / 2;
  const int odd = ns % 2;
  BLI_assert(0 <= i && i <= n && 0 <= j && j <= ns && 0 <= k && k <= ns);

  if (!odd && j == ns2 && k == ns2) {
    return mesh_vert(vm, 0, j, k);
  }
  if (j <= ns2 - 1 + odd && k <= ns2) {
    return mesh_vert(vm, i, j, k);
  }
  if (k <= ns2) {
    return mesh_vert(vm, (i + n - 1) % n, k, ns - j);
  }
  return mesh_vert(vm, (i + 1) % n, ns - k, j);
}

/* Returns true when (i, j, k) is the canonical representative of its equivalence class. */
static bool is_canon(const VMesh *vm, int i, int j, int k)
{
  const int ns2 = vm->seg / 2;
  if (vm->seg % 2 == 1) {
    return (j <= ns2 && k <= ns2);
  }
  return ((j < ns2 && k <= ns2) || (j == ns2 && k == ns2 && i == 0));
}

/* Copies coordinates and vertex indices from canonical grid positions to all equivalent ones. */
static void vmesh_copy_equiv_verts(VMesh *vm)
{
  const int n = vm->count;
  const int ns = vm->seg;
  const int ns2 = ns / 2;
  for (int i = 0; i < n; i++) {
    for (int j = 0; j <= ns2; j++) {
      for (int k = 0; k <= ns; k++) {
        if (is_canon(vm, i, j, k)) {
          continue;
        }
        NewVert *v1 = mesh_vert(vm, i, j, k);
        NewVert *v0 = mesh_vert_canon(vm, i, j, k);
        v1->co = v0->co;
        v1->v = v0->v;
      }
    }
  }
}

/* Computes the centroid of the center polygon into `r_cent`. */
static void vmesh_center(VMesh *vm, float r_cent[3])
{
  const int n = vm->count;
  const int ns2 = vm->seg / 2;
  if (vm->seg % 2) {
    zero_v3(r_cent);
    for (int i = 0; i < n; i++) {
      add_v3_v3(r_cent, mesh_vert(vm, i, ns2, ns2)->co);
    }
    mul_v3_fl(r_cent, 1.0f / float(n));
  }
  else {
    copy_v3_v3(r_cent, mesh_vert(vm, 0, ns2, ns2)->co);
  }
}

/* Sets co to the average of four NewVert positions. */
static void avg4(
    float co[3], const NewVert *v0, const NewVert *v1, const NewVert *v2, const NewVert *v3)
{
  add_v3_v3v3(co, v0->co, v1->co);
  add_v3_v3(co, v2->co);
  add_v3_v3(co, v3->co);
  mul_v3_fl(co, 0.25f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Profile geometry helpers
 * \{ */

/** Returns true when `d1` and `d2` are parallel or anti-parallel. */
static bool nearly_parallel(const float d1[3], const float d2[3])
{
  const float ang = angle_v3v3(d1, d2);
  return (fabsf(ang) < BEVEL_EPSILON_ANG) || (fabsf(ang - float(M_PI)) < BEVEL_EPSILON_ANG);
}

/**
 * Builds a 4x4 matrix that maps the unit square to the triangle `(va, vmid, vb)`.
 * Returns false when the three points are collinear or degenerate.
 */
static bool make_unit_square_map(const float va[3],
                                 const float vmid[3],
                                 const float vb[3],
                                 float r_mat[4][4])
{
  float va_vmid[3], vb_vmid[3];
  sub_v3_v3v3(va_vmid, vmid, va);
  sub_v3_v3v3(vb_vmid, vmid, vb);

  if (is_zero_v3(va_vmid) || is_zero_v3(vb_vmid)) {
    return false;
  }
  if (fabsf(angle_v3v3(va_vmid, vb_vmid) - float(M_PI)) <= BEVEL_EPSILON_ANG) {
    return false;
  }

  float vo[3], vd[3], vddir[3];
  sub_v3_v3v3(vo, va, vb_vmid);
  cross_v3_v3v3(vddir, vb_vmid, va_vmid);
  normalize_v3(vddir);
  add_v3_v3v3(vd, vo, vddir);

  sub_v3_v3v3(&r_mat[0][0], vmid, va);
  r_mat[0][3] = 0.0f;
  sub_v3_v3v3(&r_mat[1][0], vmid, vb);
  r_mat[1][3] = 0.0f;
  add_v3_v3v3(&r_mat[2][0], vmid, vd);
  sub_v3_v3(&r_mat[2][0], va);
  sub_v3_v3(&r_mat[2][0], vb);
  r_mat[2][3] = 0.0f;
  add_v3_v3v3(&r_mat[3][0], va, vb);
  sub_v3_v3(&r_mat[3][0], vmid);
  r_mat[3][3] = 1.0f;

  return true;
}

/**
 * Returns the Sabin-modified Catmull-Clark gamma value for an n-sided corner.
 * This controls the smoothness of the center vertex during subdivision.
 */
static float sabin_gamma(int n)
{
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
  const double k = cos(M_PI / double(n));
  const double k2 = k * k;
  const double k4 = k2 * k2;
  const double k6 = k4 * k2;
  const double y = pow(M_SQRT3 * sqrt(64.0 * k6 - 144.0 * k4 + 135.0 * k2 - 27.0) + 9.0 * k,
                       1.0 / 3.0);
  const double x = 0.480749856769136 * y - (0.231120424783545 * (12.0 * k2 - 9.0)) / y;
  return float((k * x + 2.0 * k2 - 1.0) / (x * x * (k * x + 1.0)));
}

/** \} */

}  // namespace geom

/* -------------------------------------------------------------------- */
/** \name Debug printing utilities
 * \{ */

namespace debug {

/* Prints a Span of a printable type, 10 items per line.
 * Each line is prefixed with the starting index in brackets.
 * A label line is printed before the span. */
template<typename T> [[maybe_unused]] static void print_span(Span<T> span, const char *label)
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

/* Prints a single float3 as "(x,y,z)" with no trailing newline. */
[[maybe_unused]] static void print_float3(const float3 &v)
{
  fmt::print("({},{},{})", v[0], v[1], v[2]);
}

/* Prints a Span<float3>, 10 items per line, preceded by a label. */
[[maybe_unused]] static void print_float3_span(Span<float3> span, const char *label)
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

/* Prints a single int2 pair as "(a,b)" with no trailing newline. */
[[maybe_unused]] static void print_int2(const int2 pair)
{
  fmt::print("({},{})", pair[0], pair[1]);
}

/* Prints a Span<int2>, 10 items per line, preceded by a label. */
[[maybe_unused]] static void print_int2_span(Span<int2> span, const char *label)
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

/* Prints a single IndexRange as "[first..last]" or "[]" if empty, with no trailing newline. */
[[maybe_unused]] static void print_indexrange(const IndexRange &range)
{
  if (range.size() == 0) {
    fmt::print("[]");
  }
  else {
    fmt::print("[{}..{}]", range.first(), range.last());
  }
}

/* Prints a GroupedSpan<int>, one group per line, preceded by a label. */
[[maybe_unused]] static void print_groupedspan(const GroupedSpan<int> &groupedspan,
                                               const char *label)
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

/* Returns a human-readable name for a #MeshKind value. */
[[maybe_unused]] static const char *mesh_kind_name(MeshKind kind)
{
  switch (kind) {
    case MeshKind::NONE:
      return "NONE";
    case MeshKind::POLY:
      return "POLY";
    case MeshKind::ADJ:
      return "ADJ";
    case MeshKind::TRI_FAN:
      return "TRI_FAN";
    case MeshKind::CUTOFF:
      return "CUTOFF";
    default:
      return "?";
  }
}

/* Prints a single #Profile's key parameters. */
[[maybe_unused]] static void dump_profile(const Profile &prof)
{
  fmt::print("  Profile: super_r={} height={} special_params={}\n",
             prof.super_r,
             prof.height,
             prof.special_params);
  fmt::print("    start=");
  print_float3(prof.start);
  fmt::print(" middle=");
  print_float3(prof.middle);
  fmt::print(" end=");
  print_float3(prof.end);
  fmt::println("");
  fmt::print("    plane_no=");
  print_float3(prof.plane_no);
  fmt::print(" plane_co=");
  print_float3(prof.plane_co);
  fmt::print(" proj_dir=");
  print_float3(prof.proj_dir);
  fmt::println("");
  if (!prof.prof_co.is_empty()) {
    print_float3_span(prof.prof_co, "    prof_co");
  }
}

/* Prints a single #EdgeHalf's fields. */
[[maybe_unused]] static void dump_edge_half(const EdgeHalf &eh, const int index)
{
  fmt::println("  EdgeHalf[{}]: e={} fprev={} fnext={}", index, eh.e, eh.fprev, eh.fnext);
  fmt::println("    offset_l={} offset_r={} offset_l_spec={} offset_r_spec={}",
               eh.offset_l,
               eh.offset_r,
               eh.offset_l_spec,
               eh.offset_r_spec);
  fmt::println("    is_bev={} is_rev={} is_seam={} visited_rpo={}",
               eh.is_bev,
               eh.is_rev,
               eh.is_seam,
               eh.visited_rpo);
  fmt::println("    leftv={} rightv={}",
               eh.leftv ? eh.leftv->index : -1,
               eh.rightv ? eh.rightv->index : -1);
}

/* Prints a single #BoundVert's fields. */
[[maybe_unused]] static void dump_bound_vert(const BoundVert &bndv)
{
  fmt::print("  BoundVert[{}]: co=", bndv.index);
  print_float3(bndv.nv.co);
  fmt::println("");
  fmt::println("    efirst={} elast={} eon={} ebev={}",
               bndv.efirst ? bndv.efirst->e : -1,
               bndv.elast ? bndv.elast->e : -1,
               bndv.eon ? bndv.eon->e : -1,
               bndv.ebev ? bndv.ebev->e : -1);
  fmt::println(
      "    sinratio={} any_seam={} visited={}", bndv.sinratio, bndv.any_seam, bndv.visited);
  fmt::println("    is_arc_start={} is_patch_start={} is_profile_start={}",
               bndv.is_arc_start,
               bndv.is_patch_start,
               bndv.is_profile_start);
  fmt::println("    seam_len={} sharp_len={}", bndv.seam_len, bndv.sharp_len);
  dump_profile(bndv.profile);
}

/* Prints a #VMesh and all its #BoundVert chain, plus the full #NewVert grid. */
[[maybe_unused]] static void dump_vmesh(const VMesh &vm)
{
  fmt::println(
      "  VMesh: count={} seg={} mesh_kind={}", vm.count, vm.seg, mesh_kind_name(vm.mesh_kind));
  if (vm.boundstart == nullptr) {
    fmt::println("  (no boundverts)");
    return;
  }
  /* Walk the circular linked list of BoundVerts. */
  const BoundVert *bndv = vm.boundstart;
  do {
    dump_bound_vert(*bndv);
    bndv = bndv->next;
  } while (bndv != vm.boundstart);

  /* Print the NewVert grid if it has been allocated. */
  if (!vm.mesh.is_empty()) {
    const int n = vm.count;
    const int ns = vm.seg;
    const int ns2 = ns / 2;
    /* Non-const pointer needed by mesh_vert (accessor is not const-qualified). */
    VMesh *vmp = const_cast<VMesh *>(&vm);
    fmt::println("  NewVerts (i, j, k) for 0<=i<{} 0<=j<={} 0<=k<{}:", n, ns2, ns);
    for (int i = 0; i < n; i++) {
      for (int j = 0; j <= ns2; j++) {
        fmt::print("    ({},{}): ", i, j);
        for (int k = 0; k < ns; k++) {
          const NewVert *nv = geom::mesh_vert(vmp, i, j, k);
          fmt::print("({},({:.3f},{:.3f},{:.3f})) ", nv->v, nv->co[0], nv->co[1], nv->co[2]);
        }
        fmt::println("");
      }
    }
  }
}

/* Dumps a full #BevVert, including its #EdgeHalf array, wire edges, and #VMesh. */
[[maybe_unused]] static void dump_bev_vert(const BevVert &bv)
{
  fmt::println("BevVert: v={} edgecount={} selcount={} wirecount={}",
               bv.v,
               bv.edgecount,
               bv.selcount,
               bv.wirecount);
  fmt::println("  offset={} any_seam={} visited={}", bv.offset, bv.any_seam, bv.visited);

  /* Print the EdgeHalf array. */
  fmt::println("  edges ({}):", bv.edges.size());
  for (const int i : bv.edges.index_range()) {
    dump_edge_half(bv.edges[i], i);
  }

  /* Print wire edges. */
  if (!bv.wire_edges.is_empty()) {
    print_span<int>(bv.wire_edges, "  wire_edges");
  }

  /* Print the VMesh if present. */
  if (bv.vmesh) {
    dump_vmesh(*bv.vmesh);
  }
  else {
    fmt::println("  (no vmesh)");
  }
}

/**
 * Dumps the edge-strip quads associated with `bv` that were created by
 * #construct::bevel_build_edge_polygons.
 *
 * For each beveled #EdgeHalf of `bv`, uses `leftv->index` as the boundary row `i`
 * in the #VMesh, then for each segment k walks the ring to find successive boundary
 * vertex pairs (i,0,k) and (i,0,k+1).  It searches the new faces of `emesh` for
 * a quad whose vertex list contains both those vertices in the opposite cyclic
 * order (since bevel_build_edge_polygons builds the quad with bv2's verts first).
 * Each matching quad is dumped as a list of corners with their vertex and edge indices.
 */
[[maybe_unused]] static void dump_edge_polygons(const BevelState &state, const BevVert &bv)
{
  const ExtendableMesh &emesh = state.emesh;
  VMesh *vm = bv.vmesh.get();
  if (!vm) {
    return;
  }
  const int ns = vm->seg;

  fmt::println("  Edge polygons for bv={}:", bv.v);

  for (int ei = 0; ei < bv.edgecount; ei++) {
    const EdgeHalf &eh = bv.edges[ei];
    if (!eh.is_bev || !eh.leftv) {
      continue;
    }
    const int i = eh.leftv->index;
    fmt::println("    EdgeHalf[{}] e={} i={}:", ei, eh.e, i);

    for (int k = 0; k < ns; k++) {
      /* The two boundary verts on this endpoint's ring. */
      const int va = geom::mesh_vert(vm, i, 0, k)->v;
      const int vb = geom::mesh_vert(vm, i, 0, k + 1)->v;
      if (va < 0 || vb < 0) {
        continue;
      }
      /* Search new faces for a quad containing va and vb in reverse order (vb before va).
       * new_faces_num() gives the count of faces added by face_create. */
      bool found = false;
      const int first_new_face = emesh.mesh.faces_num;
      const int past_new_face = first_new_face + emesh.new_faces_num();

      for (int fi = first_new_face; fi < past_new_face && !found; fi++) {
        const IndexRange corners = emesh.face_corners(fi);

        /* Check if va and vb appear in this face in the order (vb, va). */
        const int sz = int(corners.size());
        int pos_va = -1, pos_vb = -1;
        for (int ci = 0; ci < sz; ci++) {
          const int cv = emesh.corner_vert(corners[ci]);
          if (cv == va) {
            pos_va = ci;
          }
          if (cv == vb) {
            pos_vb = ci;
          }
        }
        if (pos_va >= 0 && pos_vb >= 0 && (pos_vb + 1) % sz == pos_va) {
          found = true;
          fmt::print("      k={} face={} corners:", k, fi);
          for (int ci = 0; ci < sz; ci++) {
            const int c = corners[ci];
            const int cv = emesh.corner_vert(c);
            const int ce = emesh.corner_edge(c);
            fmt::print(" [c={} v={} e={}]", c, cv, ce);
          }
          fmt::println("");
        }
      }
      if (!found) {
        fmt::println("      k={} va={} vb={}: no matching face found", k, va, vb);
      }
    }
  }
}

/**
 * Dumps the rebuilt face `face_idx` of `emesh`.
 * Prints the face index and each corner's vertex index, edge index, and vertex coordinates.
 */
[[maybe_unused]] static void dump_rebuilt_face(const BevelState &state, const int face_idx)
{
  const ExtendableMesh &emesh = state.emesh;
  const IndexRange corners = emesh.face_corners(face_idx);
  fmt::print("  Rebuilt face={} ({} verts):", face_idx, corners.size());
  for (const int c : corners) {
    const int v = emesh.corner_vert(c);
    const int e = emesh.corner_edge(c);
    const float3 co = emesh.vert_position(v);
    fmt::print(" [v={} e={} co=({:.3f},{:.3f},{:.3f})]", v, e, co[0], co[1], co[2]);
  }
  fmt::println("");
}

/**
 * Dumps the representative (example) face index recorded for every newly created face
 * in `emesh`.  New faces have indices `[mesh.faces_num, mesh.faces_num + new_faces_num)`.
 * Prints one line per new face: "  new_face=<idx> example=<orig_face_idx>".
 */
[[maybe_unused]] static void dump_new_face_examples(const BevelState &state)
{
  const ExtendableMesh &emesh = state.emesh;
  const int n_new = emesh.new_faces_num();
  fmt::println("MESH new face examples ({} new faces):", n_new);
  const Span<int> exs = emesh.new_face_examples();
  const uv::UVLayerInfo &uvi = state.uv_layer_info;
  for (int i = 0; i < n_new; i++) {
    const int face_idx = emesh.mesh.faces_num + i;
    const int ex = (i < int(exs.size())) ? exs[i] : -1;
    if (ex >= 0 && ex < emesh.mesh.faces_num) {
      const int comp = (!uvi.face_component.is_empty()) ? uvi.face_component[ex] : -1;
      const float3 cent = emesh.face_center(ex);
      fmt::println("  new_face={} example={} comp={} center=({:.3f},{:.3f},{:.3f})",
                   face_idx,
                   ex,
                   comp,
                   cent[0],
                   cent[1],
                   cent[2]);
    }
    else {
      fmt::println("  new_face={} example={}", face_idx, ex);
    }
  }
}

/**
 * Dumps the representative (example) edge index recorded for every newly created edge
 * in `emesh`.  New edges have indices `[mesh.edges_num, mesh.edges_num + new_edges_num)`.
 * Prints one line per new edge: "  new_edge=<idx> example=<orig_edge_idx>".
 */
[[maybe_unused]] static void dump_new_edge_examples(const BevelState &state)
{
  const ExtendableMesh &emesh = state.emesh;
  const Span<int2> new_edges = emesh.new_edges();
  const int n_new = int(new_edges.size());
  fmt::println("MESH new edge examples ({} new edges):", n_new);
  const Span<int> exs = emesh.new_edge_examples();
  for (int i = 0; i < n_new; i++) {
    const int edge_idx = emesh.mesh.edges_num + i;
    const int ex = (i < int(exs.size())) ? exs[i] : -1;
    fmt::println("  new_edge={} (v{}--v{}) example={}", edge_idx, new_edges[i][0], new_edges[i][1], ex);
  }
}

}  // namespace debug

/** \} */

/* Forward declarations for profile:: helpers needed in the first construct:: block. */
namespace profile {
static void set_profile_params(const BevelState &state, const BevVert *bv, BoundVert *bndv);
static void move_profile_plane(BoundVert *bndv, float3 bmvert_co);
}  // namespace profile

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
static BoundVert *add_new_bound_vert(BevVert *bv, const float co[3])
{
  auto new_bv = std::make_unique<BoundVert>();
  BoundVert *v = new_bv.get();
  bv->owned_bound_verts.append(std::move(new_bv));
  copy_v3_v3(v->nv.co, co);
  if (!bv->vmesh) {
    bv->vmesh = std::make_unique<VMesh>();
    bv->vmesh->count = 0;
    bv->vmesh->boundstart = nullptr;
    bv->vmesh->mesh_kind = MeshKind::NONE;
  }
  VMesh *vm = bv->vmesh.get();
  if (vm->boundstart == nullptr) {
    vm->boundstart = v;
    v->next = v->prev = v;
  }
  else {
    v->prev = vm->boundstart->prev;
    v->next = vm->boundstart;
    v->prev->next = v;
    vm->boundstart->prev = v;
  }
  v->index = vm->count++;
  /* Set the same defaults that the BMesh path uses in #add_new_bound_vert.
   * `sinratio` of 1.0 means no angular correction; `profile.super_r` of 1.0
   * is the PRO_LINE_R value (straight-line profile). */
  v->sinratio = 1.0f;
  v->profile.super_r = 1.0f;
  return v;
}

static void adjust_bound_vert(BoundVert *bndv, const float co[3])
{
  copy_v3_v3(bndv->nv.co, co);
}

/* If a beveled edge has a seam (check_seam == true) or a sharp (check_sharp == true),
 * then we may need to correct for discontinuities in those edge flags after beveling.
 * The code will automatically make the outer edges of a multi-segment beveled edge have
 * the same flags. So beveled edges next to each other will not lead to discontinuities.
 * But if there are beveled edges that do NOT have a seam (or sharp), then we need to mark
 * all the edge segments of such beveled edges with seam (or sharp) until we hit the next
 * beveled edge that has such a mark. This routine sets, for each rightv of a beveled edge
 * that has seam (or sharp), how many edges follow without the corresponding property.
 * The count is put in the seam_len field for seams and the sharp_len field for sharps.
 *
 * TODO: This approach doesn't work for terminal edges or miters. */
static void check_edge_data_seam_sharp_edges(BevVert *bv, bool check_seam, bool /*check_sharp*/)
{
  /* Returns true when the edge half lacks the flag being checked.
   * For seams: the edge is NOT a seam.
   * For sharps: EdgeHalf has no is_sharp field yet, so no edge is ever treated as sharp. */
  auto hasnot = [&](const EdgeHalf *e) -> bool {
    if (check_seam) {
      return !e->is_seam;
    }
    /* check_sharp: no is_sharp field exists yet; treat all edges as not-sharp. */
    return false;
  };

  EdgeHalf *e = &bv->edges[0];
  EdgeHalf *efirst = &bv->edges[0];

  /* Get to first edge with the seam or sharp property. */
  while (hasnot(e)) {
    e = e->next;
    if (e == efirst) {
      break;
    }
  }

  /* If no such edge found, return. */
  if (hasnot(e)) {
    return;
  }

  /* Set efirst to this first encountered edge. */
  efirst = e;

  do {
    int flag_count = 0;
    EdgeHalf *ne = e->next;

    while (hasnot(ne) && ne != efirst) {
      if (ne->is_bev) {
        flag_count++;
      }
      ne = ne->next;
    }
    if (ne == e || (ne == efirst && hasnot(efirst))) {
      break;
    }
    /* Set seam_len / sharp_len of starting edge's rightv. */
    if (check_seam) {
      e->rightv->seam_len = flag_count;
    }
    else {
      e->rightv->sharp_len = flag_count;
    }
    e = ne;
  } while (e != efirst);
}

/* Sets the #any_seam property for a #BevVert and all its #BoundVert's. */
static void set_bound_vert_seams(BevVert *bv, bool mark_seam, bool mark_sharp)
{
  bv->any_seam = false;
  BoundVert *v = bv->vmesh->boundstart;
  do {
    v->any_seam = false;
    for (EdgeHalf *e = v->efirst; e; e = e->next) {
      v->any_seam |= e->is_seam;
      if (e == v->elast) {
        break;
      }
    }
    bv->any_seam |= v->any_seam;
  } while ((v = v->next) != bv->vmesh->boundstart);

  if (mark_seam) {
    check_edge_data_seam_sharp_edges(bv, true, false);
  }
  if (mark_sharp) {
    check_edge_data_seam_sharp_edges(bv, false, true);
  }
}

static void offset_in_plane(
    const ExtendableMesh &emesh, EdgeHalf *e, const float3 *plane_no, bool left, float r_co[3])
{
  int v = e->is_rev ? emesh.edge_verts(e->e)[1] : emesh.edge_verts(e->e)[0];
  float3 v_co = emesh.vert_position(v);
  float3 other_co = emesh.vert_position(geom::edge_other_vert(emesh, e->e, v));
  float3 dir = math::normalize(other_co - v_co);
  float3 no;
  if (plane_no) {
    no = *plane_no;
  }
  else {
    no = float3(0.0f);
    if (math::abs(dir[0]) < math::abs(dir[1])) {
      no[0] = 1.0f;
    }
    else {
      no[1] = 1.0f;
    }
  }

  float3 fdir;
  if (left) {
    fdir = math::normalize(math::cross(dir, no));
  }
  else {
    fdir = math::normalize(math::cross(no, dir));
  }
  float3 res = v_co + fdir * (left ? e->offset_l : e->offset_r);
  copy_v3_v3(r_co, res);
}

static void build_boundary_vertex_only(const ExtendableMesh &emesh,
                                       const BevelState &state,
                                       BevVert *bv,
                                       bool construct)
{
  BLI_assert(state.params.affect_type == BevelAffect::Vertices);

  EdgeHalf *efirst = &bv->edges[0];
  EdgeHalf *e = efirst;
  do {
    float co[3];
    geom::slide_dist(emesh, e->e, bv->v, e->offset_l, co);
    if (construct) {
      BoundVert *v = add_new_bound_vert(bv, co);
      v->efirst = v->elast = e;
      e->leftv = e->rightv = v;
    }
    else {
      adjust_bound_vert(e->leftv, co);
    }
  } while ((e = e->next) != efirst);

  if (construct) {
    set_bound_vert_seams(bv, state.mark_seam, state.mark_sharp);
    VMesh *vm = bv->vmesh.get();
    if (vm->count == 2) {
      vm->mesh_kind = MeshKind::NONE;
    }
    else if (state.params.segments == 1) {
      vm->mesh_kind = MeshKind::POLY;
    }
    else {
      vm->mesh_kind = MeshKind::ADJ;
    }
  }
}

static void build_boundary_terminal_edge(const ExtendableMesh &emesh,
                                         const BevelState &state,
                                         BevVert *bv,
                                         EdgeHalf *efirst,
                                         const bool construct)
{
  EdgeHalf *e = efirst;
  float co[3];
  if (bv->edgecount == 2) {
    const float3 *no = e->fprev != -1 ?
                           &emesh.mesh.face_normals()[e->fprev] :
                           (e->fnext != -1 ? &emesh.mesh.face_normals()[e->fnext] : nullptr);
    offset_in_plane(emesh, e, no, true, co);
    if (construct) {
      BoundVert *bndv = add_new_bound_vert(bv, co);
      bndv->efirst = bndv->elast = bndv->ebev = e;
      e->leftv = bndv;
    }
    else {
      adjust_bound_vert(e->leftv, co);
    }
    no = e->fnext != -1 ? &emesh.mesh.face_normals()[e->fnext] :
                          (e->fprev != -1 ? &emesh.mesh.face_normals()[e->fprev] : nullptr);
    offset_in_plane(emesh, e, no, false, co);
    if (construct) {
      BoundVert *bndv = add_new_bound_vert(bv, co);
      bndv->efirst = bndv->elast = e;
      e->rightv = bndv;
    }
    else {
      adjust_bound_vert(e->rightv, co);
    }
    geom::slide_dist(emesh, e->next->e, bv->v, e->offset_l, co);
    if (construct) {
      BoundVert *bndv = add_new_bound_vert(bv, co);
      bndv->efirst = bndv->elast = e->next;
      e->next->leftv = e->next->rightv = bndv;
      set_bound_vert_seams(bv, state.mark_seam, state.mark_sharp);
    }
    else {
      adjust_bound_vert(e->next->leftv, co);
    }
  }
  else {
    geom::offset_meet(emesh, e->prev, e, bv->v, e->fprev, false, co, nullptr);
    if (construct) {
      BoundVert *bndv = add_new_bound_vert(bv, co);
      bndv->efirst = e->prev;
      bndv->elast = bndv->ebev = e;
      e->leftv = bndv;
      e->prev->leftv = e->prev->rightv = bndv;
    }
    else {
      adjust_bound_vert(e->leftv, co);
    }
    e = e->next;
    geom::offset_meet(emesh, e->prev, e, bv->v, e->fprev, false, co, nullptr);
    if (construct) {
      BoundVert *bndv = add_new_bound_vert(bv, co);
      bndv->efirst = e->prev;
      bndv->elast = e;
      e->leftv = e->rightv = bndv;
      e->prev->rightv = bndv;
    }
    else {
      adjust_bound_vert(e->leftv, co);
    }
    float d = efirst->offset_l_spec;
    if (state.params.custom_profile != nullptr || state.params.shape < 0.25f) {
      d *= math::sqrt(2.0f);
    }
    for (e = e->next; e->next != efirst; e = e->next) {
      geom::slide_dist(emesh, e->e, bv->v, d, co);
      if (construct) {
        BoundVert *bndv = add_new_bound_vert(bv, co);
        bndv->efirst = bndv->elast = e;
        e->leftv = e->rightv = bndv;
      }
      else {
        adjust_bound_vert(e->leftv, co);
      }
    }
    if (construct) {
      /* Special case: snap profile to the plane of the adjacent two edges.
       * Mirrors the BMesh #build_boundary_terminal_edge logic (BMesh lines 3394-3399). */
      if (bv->edgecount >= 3) {
        BoundVert *bndv = bv->vmesh->boundstart;
        BLI_assert(bndv->ebev != nullptr);
        profile::set_profile_params(state, bv, bndv);
        profile::move_profile_plane(bndv, state.emesh.vert_position(bv->v));
      }
      set_bound_vert_seams(bv, state.mark_seam, state.mark_sharp);
    }
  }
}

/* Return the next EdgeHalf after from_e that is beveled.
 * If from_e is nullptr, find the first beveled edge. */
static EdgeHalf *next_bev(BevVert *bv, EdgeHalf *from_e)
{
  if (from_e == nullptr) {
    from_e = &bv->edges.last();
  }
  EdgeHalf *e = from_e;
  do {
    if (e->is_bev) {
      return e;
    }
  } while ((e = e->next) != from_e);
  return nullptr;
}

static bool eh_on_plane(const ExtendableMesh &emesh, EdgeHalf *e)
{
  if (e->fprev == -1 || e->fnext == -1) {
    return false;
  }
  return angle_v3v3(emesh.mesh.face_normals()[e->fprev], emesh.mesh.face_normals()[e->fnext]) <
         geom::BEVEL_SMALL_ANG;
}

enum AngleKind { ANGLE_SMALLER, ANGLE_STRAIGHT, ANGLE_LARGER };

static AngleKind edges_angle_kind(const ExtendableMesh &emesh, EdgeHalf *e1, EdgeHalf *e2, int v)
{
  int v1 = geom::edge_other_vert(emesh, e1->e, v);
  int v2 = geom::edge_other_vert(emesh, e2->e, v);
  float3 dir1 = emesh.vert_position(v) - emesh.vert_position(v1);
  float3 dir2 = emesh.vert_position(v) - emesh.vert_position(v2);
  dir1 = math::normalize(dir1);
  dir2 = math::normalize(dir2);

  if (math::abs(math::dot(dir1, dir2)) > geom::BEVEL_EPSILON_ANG_DOT) {
    return ANGLE_STRAIGHT;
  }

  float3 cross = math::normalize(math::cross(dir1, dir2));
  float3 no;
  if (e1->fnext != -1) {
    no = emesh.mesh.face_normals()[e1->fnext];
  }
  else if (e2->fprev != -1) {
    no = emesh.mesh.face_normals()[e2->fprev];
  }
  else {
    no = emesh.mesh.vert_normals()[v];
  }

  if (math::dot(cross, no) < 0.0f) {
    return ANGLE_LARGER;
  }
  return ANGLE_SMALLER;
}

static void build_boundary(const ExtendableMesh &emesh,
                           const BevelState &state,
                           BevVert *bv,
                           bool construct)
{
  if (bv->edgecount <= 1) {
    return;
  }

  if (state.params.affect_type == BevelAffect::Vertices) {
    build_boundary_vertex_only(emesh, state, bv, construct);
    return;
  }

  VMesh *vm = bv->vmesh.get();

  EdgeHalf *efirst = next_bev(bv, nullptr);
  BLI_assert(efirst->is_bev);

  if (bv->selcount == 1) {
    build_boundary_terminal_edge(emesh, state, bv, efirst, construct);
    return;
  }

  int miter_outer = (bv->selcount >= 3) ? 0 /*bp->miter_outer*/ : 0 /*BEVEL_MITER_SHARP*/;
  int miter_inner = 0 /*bp->miter_inner*/;

  EdgeHalf *emiter = nullptr;
  EdgeHalf *e = efirst;
  EdgeHalf *e2;
  do {
    BLI_assert(e->is_bev);
    EdgeHalf *eon = nullptr;
    int in_plane = 0;
    int not_in_plane = 0;
    EdgeHalf *enip = nullptr;
    EdgeHalf *eip = nullptr;
    for (e2 = e->next; !e2->is_bev; e2 = e2->next) {
      if (eh_on_plane(emesh, e2)) {
        in_plane++;
        eip = e2;
      }
      else {
        not_in_plane++;
        enip = e2;
      }
    }

    float r, co[3];
    if (in_plane == 0 && not_in_plane == 0) {
      geom::offset_meet(emesh, e, e2, bv->v, e->fnext, false, co, nullptr);
    }
    else if (not_in_plane > 0) {
      if (/*bp->loop_slide &&*/ not_in_plane == 1 &&
          geom::good_offset_on_edge_between(emesh, e, e2, enip, bv->v))
      {
        if (geom::offset_on_edge_between(emesh, e, e2, enip, bv->v, co, &r)) {
          eon = enip;
        }
      }
      else {
        geom::offset_meet(emesh, e, e2, bv->v, -1, true, co, eip);
      }
    }
    else {
      if (/*bp->loop_slide &&*/ in_plane == 1 &&
          geom::good_offset_on_edge_between(emesh, e, e2, eip, bv->v))
      {
        if (geom::offset_on_edge_between(emesh, e, e2, eip, bv->v, co, &r)) {
          eon = eip;
        }
      }
      else {
        geom::offset_meet(emesh, e, e2, bv->v, e->fnext, false, co, nullptr);
      }
    }

    if (construct) {
      BoundVert *v = add_new_bound_vert(bv, co);
      v->efirst = e;
      v->elast = e2;
      v->ebev = e2;
      v->eon = eon;
      if (eon) {
        v->sinratio = r;
      }
      e->rightv = v;
      e2->leftv = v;
      for (EdgeHalf *e3 = e->next; e3 != e2; e3 = e3->next) {
        e3->leftv = e3->rightv = v;
      }
      AngleKind ang_kind = edges_angle_kind(emesh, e, e2, bv->v);

      if ((miter_outer != 0 && !emiter && ang_kind == ANGLE_LARGER) ||
          (miter_inner != 0 && ang_kind == ANGLE_SMALLER))
      {
        if (ang_kind == ANGLE_LARGER) {
          emiter = e;
        }
        BoundVert *v1 = v;
        v1->ebev = nullptr;
        BoundVert *v2 = nullptr;
        if (ang_kind == ANGLE_LARGER && miter_outer == 1 /*BEVEL_MITER_PATCH*/) {
          v2 = add_new_bound_vert(bv, co);
        }
        (void)v2;  // TODO: properly use v2 when mitering is fully supported
        BoundVert *v3 = add_new_bound_vert(bv, co);
        v3->ebev = e2;
        v3->efirst = nullptr;
        v3->elast = e2;
        v3->eon = eon;
        e2->leftv = v3;
        if (eon) {
          v3->sinratio = r;
          v1->sinratio = r;
        }
        if (ang_kind == ANGLE_LARGER) {
          v1->is_patch_start = (miter_outer == 1 /*BEVEL_MITER_PATCH*/);
          v1->is_arc_start = (miter_outer == 2 /*BEVEL_MITER_ARC*/);
          v1->is_profile_start = false;
        }
        else {
          v1->is_arc_start = (miter_inner == 2 /*BEVEL_MITER_ARC*/);
        }
      }
    }
    else {
      adjust_bound_vert(e->rightv, co);
    }
  } while ((e = e2) != efirst);

  if (construct) {
    set_bound_vert_seams(bv, state.mark_seam, state.mark_sharp);

    if (vm->count == 2) {
      vm->mesh_kind = MeshKind::NONE;
    }
    else if (efirst->seg == 1) {
      vm->mesh_kind = MeshKind::POLY;
    }
    else {
      switch (state.vmesh_method) {
        case VMeshMethod::BEVEL_VMESH_ADJ:
          vm->mesh_kind = MeshKind::ADJ;
          break;
        case VMeshMethod::BEVEL_VMESH_CUTOFF:
          vm->mesh_kind = MeshKind::CUTOFF;
          break;
      }
    }
  }
}

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

/* -------------------------------------------------------------------- */
/** \name Profile parameter setup and evaluation
 * \{ */

/**
 * Sets `profile.start/middle/end/plane_co/plane_no/proj_dir/super_r` for a #BoundVert.
 * Ported from BMesh's #set_profile_params; edge access uses #ExtendableMesh instead of BMesh.
 */
static void set_profile_params(const BevelState &state, const BevVert *bv, BoundVert *bndv)
{
  bool do_linear_interp = true;
  const EdgeHalf *e = bndv->ebev;
  Profile &pro = bndv->profile;
  const ExtendableMesh &emesh = state.emesh;

  float start[3], end[3];
  copy_v3_v3(start, bndv->nv.co);
  copy_v3_v3(end, bndv->next->nv.co);

  if (e) {
    do_linear_interp = false;
    pro.super_r = state.pro_super_r;
    /* Projection direction is along the beveled edge. */
    const int2 everts = emesh.edge_verts(e->e);
    sub_v3_v3v3(pro.proj_dir, emesh.vert_position(everts[0]), emesh.vert_position(everts[1]));
    if (e->is_rev) {
      negate_v3(pro.proj_dir);
    }
    normalize_v3(pro.proj_dir);

    /* Middle = closest point on the edge line to the segment start-end. */
    float otherco[3];
    if (!isect_line_line_v3(emesh.vert_position(everts[0]),
                            emesh.vert_position(everts[1]),
                            start,
                            end,
                            pro.middle,
                            otherco))
    {
      copy_v3_v3(pro.middle, emesh.vert_position(everts[0]));
    }

    copy_v3_v3(pro.start, start);
    copy_v3_v3(pro.end, end);

    float d1[3], d2[3];
    sub_v3_v3v3(d1, pro.middle, start);
    sub_v3_v3v3(d2, pro.middle, end);
    normalize_v3(d1);
    normalize_v3(d2);
    cross_v3_v3v3(pro.plane_no, d1, d2);
    normalize_v3(pro.plane_no);

    if (geom::nearly_parallel(d1, d2)) {
      /* Start, middle, end are collinear. */
      const float3 v_co = emesh.vert_position(bv->v);
      copy_v3_v3(pro.middle, v_co);

      if (e->prev->is_bev && e->next->is_bev && bv->selcount >= 3) {
        float d3[3], d4[3], co3[3], co4[3], meetco[3], isect2[3];
        const int2 eprev_verts = emesh.edge_verts(e->prev->e);
        const int2 enext_verts = emesh.edge_verts(e->next->e);
        sub_v3_v3v3(d3, emesh.vert_position(eprev_verts[0]), emesh.vert_position(eprev_verts[1]));
        sub_v3_v3v3(d4, emesh.vert_position(enext_verts[0]), emesh.vert_position(enext_verts[1]));
        normalize_v3(d3);
        normalize_v3(d4);
        if (geom::nearly_parallel(d3, d4)) {
          mid_v3_v3v3(pro.middle, start, end);
          do_linear_interp = true;
        }
        else {
          add_v3_v3v3(co3, start, d3);
          add_v3_v3v3(co4, end, d4);
          if (isect_line_line_v3(start, co3, end, co4, meetco, isect2) != 0) {
            copy_v3_v3(pro.middle, meetco);
          }
          else {
            mid_v3_v3v3(pro.middle, start, end);
            do_linear_interp = true;
          }
        }
      }
      copy_v3_v3(pro.end, end);
      sub_v3_v3v3(d1, pro.middle, start);
      normalize_v3(d1);
      sub_v3_v3v3(d2, pro.middle, end);
      normalize_v3(d2);
      cross_v3_v3v3(pro.plane_no, d1, d2);
      normalize_v3(pro.plane_no);
      if (geom::nearly_parallel(d1, d2)) {
        do_linear_interp = true;
      }
      else {
        copy_v3_v3(pro.plane_co, v_co);
        copy_v3_v3(pro.proj_dir, pro.plane_no);
      }
    }
    copy_v3_v3(pro.plane_co, start);
  }
  else if (bndv->is_arc_start) {
    copy_v3_v3(pro.start, start);
    copy_v3_v3(pro.end, end);
    pro.super_r = PRO_CIRCLE_R;
    zero_v3(pro.plane_co);
    zero_v3(pro.plane_no);
    zero_v3(pro.proj_dir);
    do_linear_interp = false;
  }
  else if (state.params.affect_type == BevelAffect::Vertices) {
    copy_v3_v3(pro.start, start);
    copy_v3_v3(pro.middle, emesh.vert_position(bv->v));
    copy_v3_v3(pro.end, end);
    pro.super_r = state.pro_super_r;
    zero_v3(pro.plane_co);
    zero_v3(pro.plane_no);
    zero_v3(pro.proj_dir);
    do_linear_interp = false;
  }

  if (do_linear_interp) {
    pro.super_r = PRO_LINE_R;
    copy_v3_v3(pro.start, start);
    copy_v3_v3(pro.end, end);
    mid_v3_v3v3(pro.middle, start, end);
    zero_v3(pro.plane_co);
    zero_v3(pro.plane_no);
    zero_v3(pro.proj_dir);
  }
}

/**
 * Adjusts the profile plane of `bndv->profile` so that it contains the
 * plane through `bndv->profile.start`, `bndv->profile.end`, and `bmvert_co`.
 * Mirrors BMesh's #move_profile_plane. Sets `special_params = true` to prevent
 * #calculate_vm_profiles from resetting the parameters.
 * Currently used only in #build_boundary_terminal_edge.
 */
static void move_profile_plane(BoundVert *bndv, const float3 bmvert_co)
{
  Profile &pro = bndv->profile;

  /* Only do this if projecting, and start, end, and proj_dir are not coplanar. */
  if (is_zero_v3(pro.proj_dir)) {
    return;
  }

  float d1[3], d2[3];
  sub_v3_v3v3(d1, bmvert_co, pro.start);
  normalize_v3(d1);
  sub_v3_v3v3(d2, bmvert_co, pro.end);
  normalize_v3(d2);
  float no[3], no2[3], no3[3];
  cross_v3_v3v3(no, d1, d2);
  cross_v3_v3v3(no2, d1, pro.proj_dir);
  cross_v3_v3v3(no3, d2, pro.proj_dir);

  if (normalize_v3(no) > geom::BEVEL_EPSILON_BIG && normalize_v3(no2) > geom::BEVEL_EPSILON_BIG &&
      normalize_v3(no3) > geom::BEVEL_EPSILON_BIG)
  {
    const float dot2 = dot_v3v3(no, no2);
    const float dot3 = dot_v3v3(no, no3);
    if (fabsf(dot2) < (1.0f - geom::BEVEL_EPSILON_BIG) &&
        fabsf(dot3) < (1.0f - geom::BEVEL_EPSILON_BIG))
    {
      copy_v3_v3(pro.plane_no, no);
    }
  }

  /* Parameters are now non-default; prevent recalculation later. */
  pro.special_params = true;
}

/**
 * Adjusts the profile planes for the two #BoundVert instances involved in a weld.
 * Moves the plane to the one most likely to contain the profile projection intersections.
 * Sets `special_params = true` on both to prevent recalculation.
 * Mirrors BMesh's #move_weld_profile_planes.
 */
static void move_weld_profile_planes(BoundVert *bndv1,
                                     BoundVert *bndv2,
                                     const float3 v_co)
{
  /* Only do this if projecting. */
  if (is_zero_v3(bndv1->profile.proj_dir) || is_zero_v3(bndv2->profile.proj_dir)) {
    return;
  }
  float d1[3], d2[3], no[3];
  sub_v3_v3v3(d1, v_co, bndv1->nv.co);
  sub_v3_v3v3(d2, v_co, bndv2->nv.co);
  cross_v3_v3v3(no, d1, d2);
  const float l1 = normalize_v3(no);

  float no2[3], no3[3];
  cross_v3_v3v3(no2, d1, bndv1->profile.proj_dir);
  const float l2 = normalize_v3(no2);
  cross_v3_v3v3(no3, d2, bndv2->profile.proj_dir);
  const float l3 = normalize_v3(no3);

  if (l1 != 0.0f && (l2 != 0.0f || l3 != 0.0f)) {
    const float dot1 = fabsf(dot_v3v3(no, no2));
    const float dot2 = fabsf(dot_v3v3(no, no3));
    if (fabsf(dot1 - 1.0f) > geom::BEVEL_EPSILON_D) {
      copy_v3_v3(bndv1->profile.plane_no, no);
    }
    if (fabsf(dot2 - 1.0f) > geom::BEVEL_EPSILON_D) {
      copy_v3_v3(bndv2->profile.plane_no, no);
    }
  }

  bndv1->profile.special_params = true;
  bndv2->profile.special_params = true;
}

/**
 * Fills `r_prof_co` with 3D positions for each segment point of the profile,
 * mapped from the 2D superellipse using the `map` matrix and projected along `proj_dir`.
 */
static void calculate_profile_segments(const Profile &pro,
                                       const float map[4][4],
                                       const bool use_map,
                                       const bool reversed,
                                       const int ns,
                                       const double *xvals,
                                       const double *yvals,
                                       MutableSpan<float3> r_prof_co)
{
  for (int k = 0; k <= ns; k++) {
    float co[3];
    if (k == 0) {
      copy_v3_v3(co, pro.start);
    }
    else if (k == ns) {
      copy_v3_v3(co, pro.end);
    }
    else {
      if (use_map) {
        const float p[3] = {
            reversed ? float(yvals[ns - k]) : float(xvals[k]),
            reversed ? float(xvals[ns - k]) : float(yvals[k]),
            0.0f,
        };
        mul_v3_m4v3(co, map, p);
      }
      else {
        interp_v3_v3v3(co, pro.start, pro.end, float(k) / float(ns));
      }
    }
    /* Project onto the profile plane along proj_dir. */
    if (!is_zero_v3(pro.proj_dir)) {
      float co2[3];
      add_v3_v3v3(co2, co, pro.proj_dir);
      if (!isect_line_plane_v3(r_prof_co[k], co, co2, pro.plane_co, pro.plane_no)) {
        copy_v3_v3(r_prof_co[k], co);
      }
    }
    else {
      copy_v3_v3(r_prof_co[k], co);
    }
  }
}

/**
 * Computes the `prof_co` (and optionally `prof_co_2`) arrays for `bndv->profile`,
 * applying the superellipse 2D-to-3D mapping and projection.
 * No-op when `params.segments == 1`.
 */
static void calculate_profile(BevelState &state, BoundVert *bndv, bool reversed, bool /*miter*/)
{
  Profile &pro = bndv->profile;
  /* TODO: handle custom profile (BEVEL_PROFILE_CUSTOM). */
  const ProfileSpacing &pro_spacing = state.pro_spacing;

  if (state.params.segments <= 1) {
    return;
  }

  const bool need_2 = (state.params.segments != pro_spacing.seg_2);

  if (pro.prof_co.is_empty()) {
    pro.prof_co = Array<float3>(state.params.segments + 1);
    if (need_2) {
      pro.prof_co_2 = Array<float3>(pro_spacing.seg_2 + 1);
    }
    else {
      /* prof_co_2 points to the same data. */
      pro.prof_co_2 = pro.prof_co;
    }
  }

  bool use_map;
  float map[4][4];
  if (pro.super_r == PRO_LINE_R) {
    use_map = false;
  }
  else {
    use_map = geom::make_unit_square_map(pro.start, pro.middle, pro.end, map);
  }

  calculate_profile_segments(pro,
                             map,
                             use_map,
                             reversed,
                             state.params.segments,
                             pro_spacing.xvals.data(),
                             pro_spacing.yvals.data(),
                             pro.prof_co.as_mutable_span());
  if (need_2) {
    calculate_profile_segments(pro,
                               map,
                               use_map,
                               reversed,
                               pro_spacing.seg_2,
                               pro_spacing.xvals_2.data(),
                               pro_spacing.yvals_2.data(),
                               pro.prof_co_2.as_mutable_span());
  }
}

/**
 * Returns the 3D coordinate of profile point `i` out of `nseg` for `pro`.
 * When `nseg == params.segments`, indexes into `prof_co`;
 * otherwise uses the higher-resolution `prof_co_2` with sub-sampling.
 */
static void get_profile_point(
    const BevelState &state, const Profile *pro, int i, int nseg, float r_co[3])
{
  if (state.params.segments == 1) {
    copy_v3_v3(r_co, i == 0 ? pro->start : pro->end);
    return;
  }
  if (nseg == state.params.segments) {
    BLI_assert(!pro->prof_co.is_empty());
    copy_v3_v3(r_co, pro->prof_co[i]);
  }
  else {
    BLI_assert(is_power_of_2_i(nseg) && nseg <= state.pro_spacing.seg_2);
    const int subsample_spacing = state.pro_spacing.seg_2 / nseg;
    copy_v3_v3(r_co, pro->prof_co_2[i * subsample_spacing]);
  }
}

/**
 * Sets profile parameters for all #BoundVert entries of `vm` and computes
 * their profile coordinate arrays. This is the last step before vertex creation.
 */
static void calculate_vm_profiles(BevelState &state, BevVert *bv, VMesh *vm)
{
  BoundVert *bndv = vm->boundstart;
  do {
    if (!bndv->profile.special_params) {
      set_profile_params(state, bv, bndv);
    }
    /* TODO: handle miter/reversed flags for BEVEL_PROFILE_CUSTOM. */
    calculate_profile(state, bndv, false, false);
  } while ((bndv = bndv->next) != vm->boundstart);
}

/** \} */

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

  /* Allocate per-UV-layer storage for new corner UV values. */
  emesh.init_uv_storage(int(uv_layer_info.maps.size()));
}

namespace construct {

/* Forward declaration -- defined in the ADJ vmesh subdivision section below. */
static VMesh adj_vmesh(BevelState &state, BevVert *bv);

/* -------------------------------------------------------------------- */
/** \name Representative face / edge selection
 *
 * Mirrors the BMesh `choose_rep_face`, `boundvert_rep_face`, and `frep_for_center_poly`
 * functions used to select which original face should be the "example" for new bevel faces.
 * The same 6-criterion lexicographic tie-breaking rule is used.
 * \{ */

/**
 * Choose the best representative face index from `faces` (original face indices, -1 = skip).
 *
 * Tie-breaking criteria (lower value wins at each priority):
 *   0. UV-connected-component id (`uv_layer_info.face_component`; 0 when no UV layers).
 *   1. Reserved for a future "face selected" attribute; always 0.0f for now.
 *   2. Material index (`material_index` face attribute; 0 when absent).
 *   3. Higher z-coordinate of face center (stored negated so "higher wins" = lower value).
 *   4. Lower x-coordinate of face center.
 *   5. Lower y-coordinate of face center.
 *
 * Returns -1 when every candidate is -1.
 */
static int choose_rep_face(const BevelState &state, Span<int> faces)
{
  constexpr int VEC_VALUE_LEN = 6;

  const int nfaces = int(faces.size());
  if (nfaces == 0) {
    return -1;
  }

  /* Read optional per-face attributes once. */
  const uv::UVLayerInfo &uvi = state.uv_layer_info;
  const Mesh &mesh = state.emesh.mesh;
  const bke::AttributeAccessor attrs = mesh.attributes();
  VArraySpan<int> mat_span;
  {
    bke::AttributeReader<int> mat_reader = attrs.lookup<int>("material_index",
                                                             bke::AttrDomain::Face);
    if (mat_reader) {
      mat_span = VArraySpan<int>(mat_reader.varray);
    }
  }

  /* Build score vectors for each non-null candidate. */
  Array<float[VEC_VALUE_LEN]> value_vecs(nfaces);
  Array<bool> still_viable(nfaces, false);
  int num_viable = 0;

  for (int fi = 0; fi < nfaces; fi++) {
    const int f = faces[fi];
    if (f < 0 || f >= mesh.faces_num) {
      continue;
    }
    still_viable[fi] = true;
    num_viable++;

    int vi = 0;
    /* 0: UV-island component. */
    value_vecs[fi][vi++] = (!uvi.face_component.is_empty()) ? float(uvi.face_component[f]) :
                                                              0.0f;
    /* 1: Selected-face placeholder (always 0; no face-selection concept in mesh path). */
    value_vecs[fi][vi++] = 0.0f;
    /* 2: Material index. */
    value_vecs[fi][vi++] = mat_span.is_empty() ? 0.0f : float(mat_span[f]);
    /* 3–5: Face center coordinates.  Lower z wins (matches BMesh's un-negated cent[2]). */
    const float3 cent = state.emesh.face_center(f);
    value_vecs[fi][vi++] = cent.z;
    value_vecs[fi][vi++] = cent.x;
    value_vecs[fi][vi++] = cent.y;
    BLI_assert(vi == VEC_VALUE_LEN);
  }

  if (num_viable == 0) {
    return -1;
  }

  /* Lexicographic elimination: find unique minimum at each criterion. */
  int best_fi = -1;
  for (int vi = 0; num_viable > 1 && vi < VEC_VALUE_LEN; vi++) {
    for (int fi = 0; fi < nfaces; fi++) {
      if (!still_viable[fi] || fi == best_fi) {
        continue;
      }
      if (best_fi == -1) {
        best_fi = fi;
        continue;
      }
      if (value_vecs[fi][vi] < value_vecs[best_fi][vi]) {
        best_fi = fi;
        /* Eliminate all previous viable candidates. */
        for (int j = fi - 1; j >= 0; j--) {
          if (still_viable[j]) {
            still_viable[j] = false;
            num_viable--;
          }
        }
      }
      else if (value_vecs[fi][vi] > value_vecs[best_fi][vi]) {
        still_viable[fi] = false;
        num_viable--;
      }
    }
  }
  if (best_fi == -1) {
    best_fi = 0;
  }
  return faces[best_fi];
}

/**
 * Return a good representative face for faces created around/near BoundVert `v`.
 * Mirrors BMesh's #boundvert_rep_face.
 * Face indices are original mesh indices; -1 means none.
 * If `r_fother` is non-null, a secondary candidate (or -1) is stored there.
 */
static int boundvert_rep_face(const BoundVert *v, int *r_fother)
{
  int frep;
  int frep2 = -1;

  if (v->ebev) {
    frep = v->ebev->fprev;
    if (v->efirst->fprev != frep) {
      frep2 = v->efirst->fprev;
    }
  }
  else if (v->efirst) {
    frep = v->efirst->fprev;
    if (frep >= 0) {
      if (v->elast->fnext != frep) {
        frep2 = v->elast->fnext;
      }
      else if (v->efirst->fnext != frep) {
        frep2 = v->efirst->fnext;
      }
      else if (v->elast->fprev != frep) {
        frep2 = v->elast->fprev;
      }
    }
    else if (v->efirst->fnext >= 0) {
      frep = v->efirst->fnext;
      if (v->elast->fnext != frep) {
        frep2 = v->elast->fnext;
      }
    }
    else if (v->elast->fprev >= 0) {
      frep = v->elast->fprev;
    }
  }
  else if (v->prev->elast) {
    frep = v->prev->elast->fnext;
    if (v->next->efirst) {
      if (frep >= 0) {
        frep2 = v->next->efirst->fprev;
      }
      else {
        frep = v->next->efirst->fprev;
      }
    }
  }
  else {
    frep = -1;
  }

  if (r_fother) {
    *r_fother = frep2;
  }
  return frep;
}

/**
 * Pick a good representative face for the center polygon of `bv`.
 * Collects one candidate per beveled edge (choosing from fprev/fnext), eliminates
 * duplicates, then calls #choose_rep_face on the shortlist.
 * Mirrors BMesh's #frep_for_center_poly.
 */
static int frep_for_center_poly(const BevelState &state, const BevVert *bv)
{
  const bool consider_all_faces = (bv->selcount == 1 || state.affect_vertices_odd);

  /* Collect candidates (at most edgecount, one per edge). */
  Array<int> fchoices(bv->edgecount, -1);
  int fcount = 0;
  int any_f = -1;

  for (int i = 0; i < bv->edgecount; i++) {
    const EdgeHalf &e = bv->edges[i];
    if (!e.is_bev && !consider_all_faces) {
      continue;
    }
    const int candidates[2] = {e.fprev, e.fnext};
    const int bmf = choose_rep_face(state, Span<int>(candidates, 2));
    if (bmf < 0) {
      continue;
    }
    if (any_f < 0) {
      any_f = bmf;
    }
    /* Skip duplicates. */
    bool already_there = false;
    for (int j = fcount - 1; j >= 0; j--) {
      if (fchoices[j] == bmf) {
        already_there = true;
        break;
      }
    }
    if (!already_there) {
      /* is_bad_uv_poly check is deferred (future task). */
      fchoices[fcount++] = bmf;
    }
  }

  if (fcount == 0) {
    return any_f;
  }
  return choose_rep_face(state, fchoices.as_span().take_front(fcount));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name VMesh face builders
 * \{ */

/**
 * Creates the center polygon (or ngon) face for `bv` from the first boundary vertex of each
 * arc (and intermediate arc verts for multi-segment bevels). Returns the new face index,
 * or -1 when degenerate (fewer than 3 verts).
 */
static int bevel_build_poly(BevelState &state, BevVert *bv)
{
  VMesh *vm = bv->vmesh.get();
  const int ns = vm->seg;

  Vector<int, 32> verts;
  BoundVert *bndv = vm->boundstart;
  do {
    verts.append(geom::mesh_vert(vm, bndv->index, 0, 0)->v);
    if (bndv->ebev && ns > 1) {
      for (int k = 1; k < ns; k++) {
        verts.append(geom::mesh_vert(vm, bndv->index, 0, k)->v);
      }
    }
  } while ((bndv = bndv->next) != vm->boundstart);

  if (verts.size() < 3) {
    return -1;
  }
  const int face_rep = frep_for_center_poly(state, bv);
  return state.emesh.face_create(verts.as_span(), face_rep);
}

/**
 * Builds a triangle-fan for `bv` (M_TRI_FAN) by first creating a center ngon with
 * #bevel_build_poly, then splitting it into triangles radiating from the first vertex.
 */
static void bevel_build_trifan(BevelState &state, BevVert *bv)
{
  /* For M_TRI_FAN the ngon must already have vertices; just split it into tris. */
  VMesh *vm = bv->vmesh.get();
  const int ns = vm->seg;
  BLI_assert(ns == 1 || bv->selcount == 1);

  /* Collect all verts as for poly but build triangles directly. */
  Vector<int, 32> ring;
  BoundVert *bndv = vm->boundstart;
  do {
    ring.append(geom::mesh_vert(vm, bndv->index, 0, 0)->v);
    if (bndv->ebev && ns > 1) {
      for (int k = 1; k < ns; k++) {
        ring.append(geom::mesh_vert(vm, bndv->index, 0, k)->v);
      }
    }
  } while ((bndv = bndv->next) != vm->boundstart);

  if (ring.size() < 3) {
    return;
  }

  const int face_rep = frep_for_center_poly(state, bv);
  const int v_fan = ring[0];
  for (int i = 1; i + 1 < int(ring.size()); i++) {
    const int tri[3] = {v_fan, ring[i], ring[i + 1]};
    state.emesh.face_create(Span<int>(tri, 3), face_rep);
  }
}

/**
 * M_NONE with two boundary verts and vertex bevel: places intermediate profile
 * verts and, if the original vertex had no adjacent faces, creates the connecting edges.
 */
static void bevel_vert_two_edges(BevelState &state, BevVert *bv)
{
  VMesh *vm = bv->vmesh.get();
  BLI_assert(vm->count == 2 && state.params.affect_type == BevelAffect::Vertices);

  const int ns = vm->seg;
  const int v1 = geom::mesh_vert(vm, 0, 0, 0)->v;
  const int v2 = geom::mesh_vert(vm, 1, 0, 0)->v;

  if (ns > 1) {
    /* Set up a linear profile from v1 to v2 through the original vert. */
    BoundVert *bndv0 = vm->boundstart;
    Profile &pro = bndv0->profile;
    pro.super_r = state.pro_super_r;
    copy_v3_v3(pro.start, geom::mesh_vert(vm, 0, 0, 0)->co);
    copy_v3_v3(pro.end, geom::mesh_vert(vm, 1, 0, 0)->co);
    copy_v3_v3(pro.middle, state.emesh.vert_position(bv->v));
    zero_v3(pro.plane_co);
    zero_v3(pro.plane_no);
    zero_v3(pro.proj_dir);
    profile::calculate_profile(state, bndv0, false, false);

    for (int k = 1; k < ns; k++) {
      float co[3];
      profile::get_profile_point(state, &pro, k, ns, co);
      const int nv = state.emesh.vert_create(float3(co), bv->v);
      geom::mesh_vert(vm, 0, 0, k)->co = float3(co);
      geom::mesh_vert(vm, 0, 0, k)->v = nv;
    }
    copy_v3_v3(geom::mesh_vert(vm, 0, 0, ns)->co, geom::mesh_vert(vm, 1, 0, 0)->co);
    geom::mesh_vert(vm, 0, 0, ns)->v = v2;

    /* Mirror to the second BoundVert arc (reversed). */
    for (int k = 1; k < ns; k++) {
      geom::mesh_vert(vm, 1, 0, ns - k)->co = geom::mesh_vert(vm, 0, 0, k)->co;
      geom::mesh_vert(vm, 1, 0, ns - k)->v = geom::mesh_vert(vm, 0, 0, k)->v;
    }
  }

  /* Create edges between successive arc verts if the original vertex had no faces. */
  for (int k = 0; k < ns; k++) {
    const int va = geom::mesh_vert(vm, 0, 0, k)->v;
    const int vb = geom::mesh_vert(vm, 0, 0, k + 1)->v;
    if (va >= 0 && vb >= 0) {
      state.emesh.edge_create(va, vb);
    }
  }
  (void)v1;
}

/**
 * Builds the ADJ (grid-fill) face mesh for `bv` (M_ADJ).
 * Vertex coordinates must already be set in `vm->mesh`; this function creates the
 * quad faces (and center ngon for odd segment counts).
 * Each face receives a representative original face example chosen by the same
 * lexicographic tie-breaking rule as BMesh's #bevel_build_rings.
 */
static void bevel_build_rings(BevelState &state, BevVert *bv)
{
  VMesh *vm = bv->vmesh.get();
  const int n_bndv = vm->count;
  const int ns = vm->seg;
  const int ns2 = ns / 2;
  const int odd = ns % 2;
  BLI_assert(n_bndv >= 3 && ns > 1);

  /* Compute the representative face for each BoundVert sector. */
  Array<int> bndv_rep_faces(n_bndv, -1);
  {
    BoundVert *bv_iter = vm->boundstart;
    do {
      bndv_rep_faces[bv_iter->index] = boundvert_rep_face(bv_iter, nullptr);
    } while ((bv_iter = bv_iter->next) != vm->boundstart);
  }

  /* For odd segment counts, pre-compute which rep wins the center-line tie-break
   * and the center polygon representative, mirroring BMesh's frep_beats_next logic. */
  Array<bool> frep_beats_next;
  int center_frep = -1;
  if (odd && state.params.affect_type != BevelAffect::Vertices) {
    frep_beats_next = Array<bool>(n_bndv, false);
    center_frep = frep_for_center_poly(state, bv);
    for (int i = 0; i < n_bndv; i++) {
      const int inext = (i + 1) % n_bndv;
      const int candidates[2] = {bndv_rep_faces[i], bndv_rep_faces[inext]};
      const int winner = choose_rep_face(state, Span<int>(candidates, 2));
      frep_beats_next[i] = (winner == bndv_rep_faces[i]);
    }
  }

  BoundVert *bndv = vm->boundstart;
  do {
    const int i = bndv->index;
    const int inext = bndv->next->index;

    for (int j = 0; j < ns2; j++) {
      for (int k = 0; k < ns2 + odd; k++) {
        /* Quad with lower-left corner at (i, j, k). */
        const int va = geom::mesh_vert(vm, i, j, k)->v;
        const int vb = geom::mesh_vert(vm, i, j, k + 1)->v;
        const int vc = geom::mesh_vert(vm, i, j + 1, k + 1)->v;
        const int vd = geom::mesh_vert(vm, i, j + 1, k)->v;
        if (va < 0 || vb < 0 || vc < 0 || vd < 0) {
          continue;
        }

        /* Choose face rep for this quad, mirroring BMesh's bevel_build_rings:
         * - All quads in sector i default to bndv_rep_faces[i].
         * - For odd ns, the center-line (k == ns2) is a special case:
         *   when the sector's beveled edge is a UV seam use the frep_beats_next
         *   tie-break; otherwise BMesh keeps fr[0] = f = bndv_rep_faces[i]. */
        int face_rep = bndv_rep_faces[i];
        if (odd && state.params.affect_type != BevelAffect::Vertices) {
          if (k == ns2) {
            const EdgeHalf *e = bndv->ebev;
            if (!e || e->is_seam) {
              face_rep = frep_beats_next[i] ? bndv_rep_faces[i] : bndv_rep_faces[inext];
            }
            /* Non-seam center-line: keep bndv_rep_faces[i] (matches BMesh fr[0]=f). */
          }
        }

        const int quad[4] = {va, vb, vc, vd};
        state.emesh.face_create(Span<int>(quad, 4), face_rep);
      }
    }
    (void)inext;
  } while ((bndv = bndv->next) != vm->boundstart);

  /* Center ngon for odd segment count. */
  if (odd) {
    Vector<int, 16> center_verts;
    bndv = vm->boundstart;
    do {
      center_verts.append(geom::mesh_vert(vm, bndv->index, ns2, ns2)->v);
    } while ((bndv = bndv->next) != vm->boundstart);
    if (center_verts.size() >= 3) {
      state.emesh.face_create(center_verts.as_span(), center_frep);
    }
  }
}


/* Forward declaration — defined in the Edge polygon construction section below. */
static EdgeHalf *find_edge_half_for_edge(BevVert *bv, int edge_index);

/* -------------------------------------------------------------------- */
/** \name Face rebuild
 * \{ */

/**
 * Returns the number of EdgeHalf steps CCW from `e1` to `e2` around the same BevVert ring.
 * Mirrors BMesh's #count_ccw_edges_between.
 */
static int count_ccw_edges_between(const EdgeHalf *e1, const EdgeHalf *e2)
{
  int count = 0;
  const EdgeHalf *e = e1;
  do {
    if (e == e2) {
      break;
    }
    e = e->next;
    count++;
  } while (e != e1);
  return count;
}

/**
 * Rebuilds face `f_idx` of `emesh` by replacing beveled vertex corners with
 * the corresponding boundary ring arcs from each #BevVert's #VMesh.
 * Returns the new face index if rebuilt, or -1 otherwise.
 * Mirrors BMesh's #bev_rebuild_polygon.
 */
static int bev_rebuild_polygon(BevelState &state, const int f_idx)
{
  const ExtendableMesh &emesh = state.emesh;
  const IndexRange corners = emesh.face_corners(f_idx);
  if (corners.size() < 3) {
    return false;
  }

  bool do_rebuild = false;
  Vector<int, 32> vv;       /* New vertex indices for the rebuilt face. */
  Vector<int, 32> orig_v;   /* Representative original vertex for each vv entry.
                              * For non-beveled vertices this equals vv[i] (the original vertex).
                              * For VMesh arc vertices this equals bv->v (the original beveled
                              * vertex whose VMesh produced the arc). Used to identify the
                              * best-matching original edge example for each rebuilt edge. */

  const int sz = int(corners.size());
  for (int ci = 0; ci < sz; ci++) {
    const int c = corners[ci];
    const int v_idx = emesh.corner_vert(c);
    const int e_idx = emesh.corner_edge(c);
    const int c_prev = corners[(ci + sz - 1) % sz];
    const int e_prev_idx = emesh.corner_edge(c_prev);

    BevVert *bv = state.vert_hash.lookup_default(v_idx, nullptr);
    if (bv && bv->vmesh) {
      /* This corner's vertex is a beveled vertex.
       * Replace it with the arc of new boundary ring vertices. */
      EdgeHalf *e = find_edge_half_for_edge(bv, e_idx);
      EdgeHalf *eprev = find_edge_half_for_edge(bv, e_prev_idx);
      BLI_assert(e && eprev);

      /* Determine CCW vs CW traversal (matching BMesh go_ccw logic). */
      bool go_ccw;
      if (e->prev == eprev) {
        if (eprev->prev == e) {
          /* Valence-2 vertex: break tie using face membership. */
          go_ccw = (e->fnext != f_idx);
        }
        else {
          go_ccw = true;
        }
      }
      else if (eprev->prev == e) {
        go_ccw = false;
      }
      else {
        /* Non-contiguous ordering: pick the shorter arc. */
        go_ccw = count_ccw_edges_between(eprev, e) < count_ccw_edges_between(e, eprev);
      }

      VMesh *vm = bv->vmesh.get();
      BoundVert *vstart;
      BoundVert *vend;
      if (go_ccw) {
        vstart = eprev->rightv;
        vend = e->leftv;
        /* profile_index > 0 handling is a TODO (miters not yet implemented). */
      }
      else {
        vstart = eprev->leftv;
        vend = e->rightv;
      }
      BLI_assert(vstart && vend);

      /* Emit the starting corner vertex. */
      vv.append(geom::mesh_vert(vm, vstart->index, 0, 0)->v);
      orig_v.append(bv->v);

      BoundVert *v = vstart;
      while (v != vend) {
        if (go_ccw) {
          const int i = v->index;
          for (int k = 1; k <= vm->seg; k++) {
            const int nv = geom::mesh_vert(vm, i, 0, k)->v;
            if (nv >= 0) {
              vv.append(nv);
              orig_v.append(bv->v);
            }
          }
          v = v->next;
        }
        else {
          const int i = v->prev->index;
          for (int k = vm->seg - 1; k >= 0; k--) {
            const int nv = geom::mesh_vert(vm, i, 0, k)->v;
            if (nv >= 0) {
              vv.append(nv);
              orig_v.append(bv->v);
            }
          }
          v = v->prev;
        }
      }
      do_rebuild = true;
    }
    else {
      /* Non-beveled vertex: keep as-is. */
      vv.append(v_idx);
      orig_v.append(v_idx);
    }
  }

  if (do_rebuild && vv.size() >= 3) {
    /* Pre-create edges with representative original edge examples so that face_create's
     * internal edge_create calls inherit the correct example via the dedup lookup.
     * - "Complete" case: both orig_v entries are original vertices → find the original edge.
     * - "Partial" case: one or both are VMesh arc endpoints → find the original edge between
     *   the two representative original vertices (bv->v or the original vertex). */
    const int n = int(vv.size());
    for (int i = 0; i < n; i++) {
      const int ov_a = orig_v[i];
      const int ov_b = orig_v[(i + 1) % n];
      if (ov_a == ov_b) {
        /* Both vv entries came from the same beveled vertex's VMesh arc; no original
         * edge spans this arc segment — leave example as -1. */
        continue;
      }
      /* Look for an original edge between ov_a and ov_b. */
      const int example_edge = emesh.find_edge(ov_a, ov_b);
      if (example_edge >= 0 && example_edge < emesh.mesh.edges_num) {
        state.emesh.edge_create(vv[i], vv[(i + 1) % n], example_edge);
      }
    }

    const int new_face_idx = state.emesh.face_create(vv.as_span(), f_idx);
    /* TODO: copy seam/sharp edge attributes to the new face's edges. */
    return new_face_idx;
  }
  return -1;
}

/**
 * For each original face that touches at least one beveled vertex, rebuild it.
 * Mirrors #bevel_rebuild_existing_polygons from the BMesh system.
 */
static void bevel_rebuild_existing_polygons(BevelState &state,
                                            Vector<int> &r_rebuilt_orig_faces,
                                            int &r_rebuilt_face_0)
{
  const ExtendableMesh &emesh = state.emesh;
  const int orig_faces_num = emesh.mesh.faces_num;

  /* Track which original faces have already been rebuilt to avoid processing them twice
   * when multiple beveled vertices share a face. */
  Array<bool> rebuilt(orig_faces_num, false);
  r_rebuilt_face_0 = -1;

  state.bevel_affected_vertices.foreach_index([&](const int v) {
    for (const int c : emesh.vert_corners()[v]) {
      const int f_idx = emesh.corner_face(c);
      if (f_idx < orig_faces_num && !rebuilt[f_idx]) {
        const int new_f = bev_rebuild_polygon(state, f_idx);
        if (new_f >= 0) {
          rebuilt[f_idx] = true;
          r_rebuilt_orig_faces.append(f_idx);
          if (f_idx == 0) {
            r_rebuilt_face_0 = new_f;
          }
        }
      }
    }
  });
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Edge polygon construction
 * \{ */

/**
 * Returns the #EdgeHalf in `bv` whose beveled edge index equals `edge_index`,
 * or `nullptr` if not found.
 */
static EdgeHalf *find_edge_half_for_edge(BevVert *bv, int edge_index)
{
  for (int i = 0; i < bv->edgecount; i++) {
    if (bv->edges[i].e == edge_index) {
      return &bv->edges[i];
    }
  }
  return nullptr;
}

/**
 * Returns true when `bv` is a "weld cross": 4 edges, 2 beveled, opposite each other.
 * Matches BMesh's #bevvert_is_weld_cross.
 */
static bool bevvert_is_weld_cross(const BevVert *bv)
{
  return (bv->edgecount == 4 && bv->selcount == 2 &&
          ((bv->edges[0].is_bev && bv->edges[2].is_bev) ||
           (bv->edges[1].is_bev && bv->edges[3].is_bev)));
}

/**
 * Builds the `nseg` quad strips of F_EDGE faces along a single beveled edge `edge_index`.
 * Mirrors BMesh's #bevel_build_edge_polygons.
 *
 * Diagram (bme = the original beveled edge):
 * \code
 *      bme->v1
 *     / | \
 *   v1--|--v4
 *   |   |   |
 *   v2--|--v3
 *     \ | /
 *      bme->v2
 * \endcode
 * `v1`/`v4` are the leftv/rightv BoundVerts on bv1 (the e1-\>leftv side),
 * `v2`/`v3` are the rightv/leftv BoundVerts on bv2.
 */
static void bevel_build_edge_polygons(BevelState &state, int edge_index)
{
  const ExtendableMesh &emesh = state.emesh;
  const int2 edge_verts = emesh.edge_verts(edge_index);
  const int v1_idx = edge_verts[0];
  const int v2_idx = edge_verts[1];

  /* Skip non-manifold edges (those with != 2 adjacent faces). */
  const GroupedSpan<int> edge_faces = emesh.edge_faces();
  if (edge_index >= emesh.mesh.edges_num || edge_faces[edge_index].size() != 2) {
    return;
  }

  BevVert *bv1 = state.vert_hash.lookup(v1_idx);
  BevVert *bv2 = state.vert_hash.lookup(v2_idx);
  if (!bv1 || !bv2) {
    return;
  }

  EdgeHalf *e1 = find_edge_half_for_edge(bv1, edge_index);
  EdgeHalf *e2 = find_edge_half_for_edge(bv2, edge_index);
  if (!e1 || !e2) {
    return;
  }

  const int nseg = e1->seg;
  BLI_assert(nseg > 0 && nseg == e2->seg);

  /* Corner boundary-ring vertex indices at both endpoints. */
  const int i1 = e1->leftv->index;
  const int i2 = e2->leftv->index;
  VMesh *vm1 = bv1->vmesh.get();
  VMesh *vm2 = bv2->vmesh.get();

  /* The two adjacent original faces for this edge.
   * Strips on the left half  (k <= mid) use f1; strips on the right half use f2.
   * When nseg is odd, the center strip (k == mid+1) is the tie-break case. */
  const int f1 = e1->fprev; /* -1 on a boundary edge. */
  const int f2 = e1->fnext; /* -1 on a boundary edge. */
  const int mid = nseg / 2;
  const bool odd_nseg = (nseg % 2) != 0;

  /* For odd nseg with a seam at e1, choose the winner once via choose_rep_face. */
  int f_choice = f1;
  if (odd_nseg && e1->is_seam) {
    const int candidates[2] = {f1, f2};
    f_choice = choose_rep_face(state, Span<int>(candidates, 2));
  }

  /* Starting vertices at k=0 on bv1's end and k=nseg on bv2's end.
   * bv2's ring is traversed in reverse (nseg→0), matching BMesh's use of e2->rightv as the
   * starting corner (e2->rightv corresponds to mesh_vert(vm2, i2, 0, nseg)). */
  int v_prev_1 = geom::mesh_vert(vm1, i1, 0, 0)->v;    /* v1 in BMesh diagram. */
  int v_prev_2 = geom::mesh_vert(vm2, i2, 0, nseg)->v; /* v2 in BMesh diagram. */

  for (int k = 1; k <= nseg; k++) {
    /* Next boundary verts along the ring: vm1 goes forward (k), vm2 goes backward (nseg-k). */
    const int v_next_1 = geom::mesh_vert(vm1, i1, 0, k)->v;        /* v4 in BMesh diagram. */
    const int v_next_2 = geom::mesh_vert(vm2, i2, 0, nseg - k)->v; /* v3 in BMesh diagram. */

    /* Pre-create the two "long" edges of the quad — those that run between the two VMeshes
     * parallel to the original beveled edge — so that face_create's internal edge_create
     * finds them already registered and inherits the correct example index.
     * The two "short" edges (within each VMesh's boundary arc) are handled by face_create
     * with -1 examples, which is acceptable since they are within a single VMesh. */
    state.emesh.edge_create(v_prev_1, v_prev_2, edge_index);
    state.emesh.edge_create(v_next_2, v_next_1, edge_index);

    /* Choose face rep for this strip, mirroring BMesh's bevel_build_edge_polygons:
     * - k <= mid    → f1 (= e1->fprev, the "left" original face)
     * - k >  mid+1  → f2 (= e1->fnext, the "right" original face)
     * - k == mid+1  → center strip: f_choice (won via choose_rep_face when seam,
     *                  else falls through to f1). */
    int face_rep;
    if (odd_nseg && k == mid + 1) {
      face_rep = f_choice;
    }
    else {
      face_rep = (k <= mid) ? f1 : f2;
    }

    /* The quad winds as: v_prev_1 -> v_prev_2 -> v_next_2 -> v_next_1. */
    const int quad[4] = {v_prev_1, v_prev_2, v_next_2, v_next_1};
    state.emesh.face_create(Span<int>(quad, 4), face_rep);

    /* TODO: record F_EDGE face kind and copy edge attributes (seam/sharp),
     * matching bev_create_ngon / record_face_kind / BM_elem_attrs_copy. */

    v_prev_1 = v_next_1;
    v_prev_2 = v_next_2;
  }

  /* TODO: implement weld-cross edge attribute continuity (weld_cross_attrs_copy). */
  (void)bevvert_is_weld_cross;
}


/** \} */

/**
 * Main vmesh builder for a single bevelled vertex.
 * Allocates the #NewVert grid, creates boundary vertices in #ExtendableMesh,
 * computes profile coordinates, then dispatches to the appropriate per-kind builder.
 */
static void build_vmesh(BevelState &state, BevVert *bv)
{
  VMesh *vm = bv->vmesh.get();
  const int n = vm->count;
  const int ns = vm->seg;
  const int ns2 = ns / 2;

  /* Allocate the grid. */
  vm->mesh = Array<NewVert>(n * (ns2 + 1) * (ns + 1), NewVert{-1, float3(0.0f)});

  /* Detect the weld case: exactly two beveled edges welding together. */
  const bool weld = (bv->selcount == 2) && (vm->count == 2);
  BoundVert *weld1 = nullptr;
  BoundVert *weld2 = nullptr;

  /* Create mesh vertices for each BoundVert's (i, 0, 0) position. */
  BoundVert *bndv = vm->boundstart;
  do {
    const int i = bndv->index;
    copy_v3_v3(geom::mesh_vert(vm, i, 0, 0)->co, bndv->nv.co);
    geom::mesh_vert(vm, i, 0, 0)->v = state.emesh.vert_create(float3(bndv->nv.co), bv->v);
    bndv->nv.v = geom::mesh_vert(vm, i, 0, 0)->v;

    if (weld && bndv->ebev) {
      if (!weld1) {
        weld1 = bndv;
      }
      else {
        weld2 = bndv;
      }
    }
  } while ((bndv = bndv->next) != vm->boundstart);

  /* For the weld case, set profile parameters and move the profile planes before
   * calculate_vm_profiles runs (which skips BoundVerts with special_params == true).
   * Mirrors BMesh's build_vmesh (BMesh lines 6484-6486). */
  if (weld && weld1 && weld2) {
    profile::set_profile_params(state, bv, weld1);
    profile::set_profile_params(state, bv, weld2);
    profile::move_weld_profile_planes(weld1, weld2, state.emesh.vert_position(bv->v));
  }

  /* Calculate profiles for non-ADJ kinds; ADJ computes its own via adj_vmesh. */
  profile::calculate_vm_profiles(state, bv, vm);

  /* Fill boundary arc verts (j=0, k=1..ns-1) for non-ADJ. */
  bndv = vm->boundstart;
  do {
    const int i = bndv->index;
    /* Last arc vert shares with the next BoundVert's first. */
    copy_v3_v3(geom::mesh_vert(vm, i, 0, ns)->co, bndv->next->nv.co);
    geom::mesh_vert(vm, i, 0, ns)->v = bndv->next->nv.v;

    if (vm->mesh_kind != MeshKind::ADJ) {
      for (int k = 1; k < ns; k++) {
        if (bndv->ebev) {
          float co[3];
          profile::get_profile_point(state, &bndv->profile, k, ns, co);
          copy_v3_v3(geom::mesh_vert(vm, i, 0, k)->co, co);
          if (!weld) {
            geom::mesh_vert(vm, i, 0, k)->v = state.emesh.vert_create(float3(co), bv->v);
          }
        }
        else if (n == 2 && !bndv->ebev) {
          /* Non-beveled side of a weld: mirror from the other BoundVert. */
          geom::mesh_vert(vm, i, 0, k)->co = geom::mesh_vert(vm, 1 - i, 0, ns - k)->co;
          geom::mesh_vert(vm, i, 0, k)->v = geom::mesh_vert(vm, 1 - i, 0, ns - k)->v;
        }
      }
    }
  } while ((bndv = bndv->next) != vm->boundstart);

  /* Weld case: build a blended profile between the two weld BoundVerts. */
  if (weld) {
    vm->mesh_kind = MeshKind::NONE;
    for (int k = 1; k < ns; k++) {
      const float3 &v_w1 = geom::mesh_vert(vm, weld1->index, 0, k)->co;
      const float3 &v_w2 = geom::mesh_vert(vm, weld2->index, 0, ns - k)->co;
      float3 co;
      /* TODO: handle BEVEL_PROFILE_CUSTOM weld blending. */
      if (weld1->profile.super_r == profile::PRO_LINE_R &&
          weld2->profile.super_r != profile::PRO_LINE_R)
      {
        co = v_w2;
      }
      else if (weld2->profile.super_r == profile::PRO_LINE_R &&
               weld1->profile.super_r != profile::PRO_LINE_R)
      {
        co = v_w1;
      }
      else {
        co = (v_w1 + v_w2) * 0.5f;
      }
      const int nv = state.emesh.vert_create(co, bv->v);
      geom::mesh_vert(vm, weld1->index, 0, k)->co = co;
      geom::mesh_vert(vm, weld1->index, 0, k)->v = nv;
    }
    for (int k = 1; k < ns; k++) {
      geom::mesh_vert(vm, weld2->index, 0, ns - k)->co =
          geom::mesh_vert(vm, weld1->index, 0, k)->co;
      geom::mesh_vert(vm, weld2->index, 0, ns - k)->v = geom::mesh_vert(vm, weld1->index, 0, k)->v;
    }
  }

  /* Check for pipe test for ADJ (3- or 4-boundary, seg > 1). */
  /* TODO: pipe_adj_vmesh, tri_corner_adj_vmesh, square_out_adj_vmesh special cases. */

  switch (vm->mesh_kind) {
    case MeshKind::NONE:
      if (n == 2 && state.params.affect_type == BevelAffect::Vertices) {
        bevel_vert_two_edges(state, bv);
      }
      break;
    case MeshKind::POLY:
      bevel_build_poly(state, bv);
      break;
    case MeshKind::TRI_FAN:
      bevel_build_trifan(state, bv);
      break;
    case MeshKind::ADJ: {
      /* Compute the ADJ interior coordinates via cubic subdivision. */
      VMesh vm_adj = adj_vmesh(state, bv);
      /* Copy final positions into vm->mesh and create ExtendableMesh verts. */
      for (int i = 0; i < n; i++) {
        for (int j = 0; j <= ns2; j++) {
          for (int k = 0; k <= ns; k++) {
            if (j == 0 && (k == 0 || k == ns)) {
              continue; /* Boundary corners already created. */
            }
            if (!geom::is_canon(vm, i, j, k)) {
              continue;
            }
            const float3 co = geom::mesh_vert(&vm_adj, i, j, k)->co;
            const int nv = state.emesh.vert_create(co, bv->v);
            geom::mesh_vert(vm, i, j, k)->co = co;
            geom::mesh_vert(vm, i, j, k)->v = nv;
          }
        }
      }
      geom::vmesh_copy_equiv_verts(vm);
      bevel_build_rings(state, bv);
      break;
    }
    case MeshKind::CUTOFF:
      /* TODO: implement M_CUTOFF. */
      break;
  }
}

/** \} */

/**
 * For each UV map, group the corners of vertex `v` into buckets of corners whose UV
 * coordinates coincide (within #STD_UV_CONNECT_LIMIT). Store the result in
 * `state.uv_vert_maps[i][v]`, one entry per UV layer.
 *
 * This is the Mesh equivalent of the BMesh #determine_uv_vert_connectivity function.
 * Corners play the role of BMesh loops; #uv::UVLayerInfo::maps supplies the UV values.
 */
static void determine_uv_vert_connectivity(BevelState &state, int v)
{
  const int num_uv_layers = int(state.uv_layer_info.maps.size());
  BLI_assert(int(state.uv_vert_maps.size()) == num_uv_layers);

  for (int i = 0; i < num_uv_layers; i++) {
    const Span<float2> uv_vals = state.uv_layer_info.maps[i].values.as_span();
    Vector<UVVertBucket> uv_vert_buckets;

    for (const int c : state.emesh.vert_corners()[v]) {
      const float2 &luv = uv_vals[c];
      bool is_overlap_found = false;
      for (UVVertBucket &bucket : uv_vert_buckets) {
        for (const int c2 : bucket) {
          if (compare_v2v2(luv, uv_vals[c2], STD_UV_CONNECT_LIMIT)) {
            bucket.add(c);
            is_overlap_found = true;
            break;
          }
        }
        if (is_overlap_found) {
          break;
        }
      }
      if (!is_overlap_found) {
        uv_vert_buckets.append(UVVertBucket{c});
      }
    }

    BLI_assert(state.uv_vert_maps[i].contains(v) == false);
    state.uv_vert_maps[i].add_new(v, uv_vert_buckets);
  }
}

/* -------------------------------------------------------------------- */
/** \name ADJ vmesh subdivision helpers
 * \{ */

/* Allocates a VMesh with a zeroed NewVert grid of size count*(seg/2+1)*(seg+1). */
static VMesh new_adj_vmesh(int count, int seg, BoundVert *bounds)
{
  VMesh vm;
  vm.count = count;
  vm.seg = seg;
  vm.boundstart = bounds;
  vm.mesh = Array<NewVert>(count * (seg / 2 + 1) * (seg + 1), NewVert{-1, float3(0.0f)});
  vm.mesh_kind = MeshKind::ADJ;
  return vm;
}

/* Fills frac[0..ns] with cumulative arc-length fractions along ring 0 of vmesh row i. */
static void fill_vmesh_fracs(VMesh *vm, Array<float> &frac, int i)
{
  const int ns = vm->seg;
  frac[0] = 0.0f;
  float total = 0.0f;
  for (int k = 0; k < ns; k++) {
    total += math::distance(geom::mesh_vert(vm, i, 0, k)->co,
                            geom::mesh_vert(vm, i, 0, k + 1)->co);
    frac[k + 1] = total;
  }
  if (total > 0.0f) {
    for (int k = 1; k <= ns; k++) {
      frac[k] /= total;
    }
  }
  else {
    frac[ns] = 1.0f;
  }
}

/* Fills frac[0..ns] with cumulative arc-length fractions along bndv's profile. */
static void fill_profile_fracs(const BevelState &state,
                               BoundVert *bndv,
                               Array<float> &frac,
                               int ns)
{
  float co[3], nextco[3];
  frac[0] = 0.0f;
  float total = 0.0f;
  copy_v3_v3(co, bndv->nv.co);
  for (int k = 0; k < ns; k++) {
    profile::get_profile_point(state, &bndv->profile, k + 1, ns, nextco);
    total += len_v3v3(co, nextco);
    frac[k + 1] = total;
    copy_v3_v3(co, nextco);
  }
  if (total > 0.0f) {
    for (int k = 1; k <= ns; k++) {
      frac[k] /= total;
    }
  }
  else {
    frac[ns] = 1.0f;
  }
}

/* Returns index i such that frac[i] <= f <= frac[i+1], and sets r_rest to the remainder. */
static int interp_range(const Array<float> &frac, int n, float f, float *r_rest)
{
  for (int i = 0; i < n; i++) {
    if (f <= frac[i + 1]) {
      float rest = f - frac[i];
      *r_rest = (rest == 0.0f) ? 0.0f : rest / (frac[i + 1] - frac[i]);
      if (i == n - 1 && *r_rest == 1.0f) {
        i = n;
        *r_rest = 0.0f;
      }
      return i;
    }
  }
  *r_rest = 0.0f;
  return n;
}

/* Re-samples vm_in to produce a VMesh with nseg boundary segments. */
static VMesh interp_vmesh(const BevelState &state, VMesh &vm_in, int nseg)
{
  const int n_bndv = vm_in.count;
  const int ns_in = vm_in.seg;
  const int nseg2 = nseg / 2;
  const int odd = nseg % 2;
  VMesh vm_out = new_adj_vmesh(n_bndv, nseg, vm_in.boundstart);

  Array<float> prev_frac(ns_in + 1), frac(ns_in + 1);
  Array<float> new_frac(nseg + 1), prev_new_frac(nseg + 1);

  fill_vmesh_fracs(&vm_in, prev_frac, n_bndv - 1);
  BoundVert *bndv = vm_in.boundstart;
  fill_profile_fracs(state, bndv->prev, prev_new_frac, nseg);

  for (int i = 0; i < n_bndv; i++) {
    fill_vmesh_fracs(&vm_in, frac, i);
    fill_profile_fracs(state, bndv, new_frac, nseg);
    for (int j = 0; j <= nseg2 - 1 + odd; j++) {
      for (int k = 0; k <= nseg2; k++) {
        float restk, restkprev;
        int k_in = interp_range(frac, ns_in, new_frac[k], &restk);
        int k_in_prev = interp_range(prev_frac, ns_in, prev_new_frac[nseg - j], &restkprev);
        int j_in = ns_in - k_in_prev;
        float restj = -restkprev;
        if (restj > -geom::BEVEL_EPSILON_D) {
          restj = 0.0f;
        }
        else {
          j_in--;
          restj = 1.0f + restj;
        }
        float co[3];
        if (restj < geom::BEVEL_EPSILON_D && restk < geom::BEVEL_EPSILON_D) {
          copy_v3_v3(co, geom::mesh_vert_canon(&vm_in, i, j_in, k_in)->co);
        }
        else {
          const int j0inc = (restj < geom::BEVEL_EPSILON_D || j_in == ns_in) ? 0 : 1;
          const int k0inc = (restk < geom::BEVEL_EPSILON_D || k_in == ns_in) ? 0 : 1;
          float quad[4][3];
          copy_v3_v3(quad[0], geom::mesh_vert_canon(&vm_in, i, j_in, k_in)->co);
          copy_v3_v3(quad[1], geom::mesh_vert_canon(&vm_in, i, j_in, k_in + k0inc)->co);
          copy_v3_v3(quad[2], geom::mesh_vert_canon(&vm_in, i, j_in + j0inc, k_in + k0inc)->co);
          copy_v3_v3(quad[3], geom::mesh_vert_canon(&vm_in, i, j_in + j0inc, k_in)->co);
          interp_bilinear_quad_v3(quad, restk, restj, co);
        }
        copy_v3_v3(geom::mesh_vert(&vm_out, i, j, k)->co, co);
      }
    }
    bndv = bndv->next;
    prev_frac = frac;
    prev_new_frac = new_frac;
  }
  if (!odd) {
    float center[3];
    geom::vmesh_center(&vm_in, center);
    copy_v3_v3(geom::mesh_vert(&vm_out, 0, nseg2, nseg2)->co, center);
  }
  geom::vmesh_copy_equiv_verts(&vm_out);
  return vm_out;
}

/**
 * One step of Catmull-Clark-like cubic subdivision (Levin 1999).
 * `vm_in.seg` must be even and >= 2. Returns a new VMesh with doubled resolution.
 */
static VMesh cubic_subdiv(const BevelState &state, VMesh &vm_in)
{
  const int n_boundary = vm_in.count;
  const int ns_in = vm_in.seg;
  const int ns_in2 = ns_in / 2;
  BLI_assert(ns_in % 2 == 0);
  const int ns_out = 2 * ns_in;
  VMesh vm_out = new_adj_vmesh(n_boundary, ns_out, vm_in.boundstart);

  /* Adjust even boundary vertices. */
  for (int i = 0; i < n_boundary; i++) {
    copy_v3_v3(geom::mesh_vert(&vm_out, i, 0, 0)->co, geom::mesh_vert(&vm_in, i, 0, 0)->co);
    for (int k = 1; k < ns_in; k++) {
      float co[3];
      copy_v3_v3(co, geom::mesh_vert(&vm_in, i, 0, k)->co);
      /* Smooth boundary (not for custom profile). */
      if (state.params.custom_profile == nullptr) {
        float acc[3];
        add_v3_v3v3(acc,
                    geom::mesh_vert(&vm_in, i, 0, k - 1)->co,
                    geom::mesh_vert(&vm_in, i, 0, k + 1)->co);
        madd_v3_v3fl(acc, co, -2.0f);
        madd_v3_v3fl(co, acc, -1.0f / 6.0f);
      }
      copy_v3_v3(geom::mesh_vert_canon(&vm_out, i, 0, 2 * k)->co, co);
    }
  }

  /* Adjust odd boundary vertices from profile. */
  BoundVert *bndv = vm_out.boundstart;
  for (int i = 0; i < n_boundary; i++) {
    for (int k = 1; k < ns_out; k += 2) {
      float co[3];
      profile::get_profile_point(state, &bndv->profile, k, ns_out, co);
      if (state.params.custom_profile == nullptr) {
        float acc[3];
        add_v3_v3v3(acc,
                    geom::mesh_vert_canon(&vm_out, i, 0, k - 1)->co,
                    geom::mesh_vert_canon(&vm_out, i, 0, k + 1)->co);
        madd_v3_v3fl(acc, co, -2.0f);
        madd_v3_v3fl(co, acc, -1.0f / 6.0f);
      }
      copy_v3_v3(geom::mesh_vert_canon(&vm_out, i, 0, k)->co, co);
    }
    bndv = bndv->next;
  }
  geom::vmesh_copy_equiv_verts(&vm_out);

  /* Copy adjusted boundary back into vm_in. */
  for (int i = 0; i < n_boundary; i++) {
    for (int k = 0; k < ns_in; k++) {
      copy_v3_v3(geom::mesh_vert(&vm_in, i, 0, k)->co, geom::mesh_vert(&vm_out, i, 0, 2 * k)->co);
    }
  }
  geom::vmesh_copy_equiv_verts(&vm_in);

  /* New face vertices. */
  for (int i = 0; i < n_boundary; i++) {
    for (int j = 0; j < ns_in2; j++) {
      for (int k = 0; k < ns_in2; k++) {
        float co[3];
        geom::avg4(co,
                   geom::mesh_vert(&vm_in, i, j, k),
                   geom::mesh_vert(&vm_in, i, j, k + 1),
                   geom::mesh_vert(&vm_in, i, j + 1, k),
                   geom::mesh_vert(&vm_in, i, j + 1, k + 1));
        copy_v3_v3(geom::mesh_vert(&vm_out, i, 2 * j + 1, 2 * k + 1)->co, co);
      }
    }
  }

  /* New vertical edge vertices. */
  for (int i = 0; i < n_boundary; i++) {
    for (int j = 0; j < ns_in2; j++) {
      for (int k = 1; k <= ns_in2; k++) {
        float co[3];
        geom::avg4(co,
                   geom::mesh_vert(&vm_in, i, j, k),
                   geom::mesh_vert(&vm_in, i, j + 1, k),
                   geom::mesh_vert_canon(&vm_out, i, 2 * j + 1, 2 * k - 1),
                   geom::mesh_vert_canon(&vm_out, i, 2 * j + 1, 2 * k + 1));
        copy_v3_v3(geom::mesh_vert(&vm_out, i, 2 * j + 1, 2 * k)->co, co);
      }
    }
  }

  /* New horizontal edge vertices. */
  for (int i = 0; i < n_boundary; i++) {
    for (int j = 1; j < ns_in2; j++) {
      for (int k = 0; k < ns_in2; k++) {
        float co[3];
        geom::avg4(co,
                   geom::mesh_vert(&vm_in, i, j, k),
                   geom::mesh_vert(&vm_in, i, j, k + 1),
                   geom::mesh_vert_canon(&vm_out, i, 2 * j - 1, 2 * k + 1),
                   geom::mesh_vert_canon(&vm_out, i, 2 * j + 1, 2 * k + 1));
        copy_v3_v3(geom::mesh_vert(&vm_out, i, 2 * j, 2 * k + 1)->co, co);
      }
    }
  }

  /* New interior vertices (not on boundary). */
  constexpr float gamma_interior = 0.25f;
  constexpr float beta_interior = -gamma_interior;
  for (int i = 0; i < n_boundary; i++) {
    for (int j = 1; j < ns_in2; j++) {
      for (int k = 1; k <= ns_in2; k++) {
        float co1[3], co2[3], co[3];
        geom::avg4(co1,
                   geom::mesh_vert_canon(&vm_out, i, 2 * j, 2 * k - 1),
                   geom::mesh_vert_canon(&vm_out, i, 2 * j, 2 * k + 1),
                   geom::mesh_vert_canon(&vm_out, i, 2 * j - 1, 2 * k),
                   geom::mesh_vert_canon(&vm_out, i, 2 * j + 1, 2 * k));
        geom::avg4(co2,
                   geom::mesh_vert_canon(&vm_out, i, 2 * j - 1, 2 * k - 1),
                   geom::mesh_vert_canon(&vm_out, i, 2 * j + 1, 2 * k - 1),
                   geom::mesh_vert_canon(&vm_out, i, 2 * j - 1, 2 * k + 1),
                   geom::mesh_vert_canon(&vm_out, i, 2 * j + 1, 2 * k + 1));
        copy_v3_v3(co, co1);
        madd_v3_v3fl(co, co2, beta_interior);
        madd_v3_v3fl(co, geom::mesh_vert(&vm_in, i, j, k)->co, gamma_interior);
        copy_v3_v3(geom::mesh_vert(&vm_out, i, 2 * j, 2 * k)->co, co);
      }
    }
  }

  geom::vmesh_copy_equiv_verts(&vm_out);

  /* Special center vertex (Sabin modification). */
  const float gamma_c = geom::sabin_gamma(n_boundary);
  const float beta_c = -gamma_c;
  float co1[3], co2[3], co[3];
  zero_v3(co1);
  zero_v3(co2);
  for (int i = 0; i < n_boundary; i++) {
    add_v3_v3(co1, geom::mesh_vert(&vm_out, i, ns_in, ns_in - 1)->co);
    add_v3_v3(co2, geom::mesh_vert(&vm_out, i, ns_in - 1, ns_in - 1)->co);
    add_v3_v3(co2, geom::mesh_vert(&vm_out, i, ns_in - 1, ns_in + 1)->co);
  }
  copy_v3_v3(co, co1);
  mul_v3_fl(co, 1.0f / float(n_boundary));
  madd_v3_v3fl(co, co2, beta_c / (2.0f * float(n_boundary)));
  madd_v3_v3fl(co, geom::mesh_vert(&vm_in, 0, ns_in2, ns_in2)->co, gamma_c);
  for (int i = 0; i < n_boundary; i++) {
    copy_v3_v3(geom::mesh_vert(&vm_out, i, ns_in, ns_in)->co, co);
  }

  /* Restore final profile boundary. */
  bndv = vm_out.boundstart;
  for (int i = 0; i < n_boundary; i++) {
    const int inext = (i + 1) % n_boundary;
    for (int k = 0; k <= ns_out; k++) {
      float pco[3];
      profile::get_profile_point(state, &bndv->profile, k, ns_out, pco);
      copy_v3_v3(geom::mesh_vert(&vm_out, i, 0, k)->co, pco);
      if (k >= ns_in && k < ns_out) {
        copy_v3_v3(geom::mesh_vert(&vm_out, inext, ns_out - k, 0)->co, pco);
      }
    }
    bndv = bndv->next;
  }

  return vm_out;
}

/**
 * Snaps `co` to lie on the superellipsoid `x^r + y^r + z^r = 1`.
 * When `r == PRO_CIRCLE_R` normalizes the vector; for the square cases snaps
 * to the nearest axis-aligned face. Only used for cube corner special cases.
 */
static void snap_to_superellipsoid(float co[3], const float super_r, bool midline)
{
  const float r = super_r;
  if (r == profile::PRO_CIRCLE_R) {
    normalize_v3(co);
    return;
  }

  float a = max_ff(0.0f, co[0]);
  float b = max_ff(0.0f, co[1]);
  float c = max_ff(0.0f, co[2]);
  float x = a, y = b, z = c;
  if (ELEM(r, profile::PRO_SQUARE_R, profile::PRO_SQUARE_IN_R)) {
    BLI_assert(fabsf(z) < geom::BEVEL_EPSILON_D);
    z = 0.0f;
    x = min_ff(1.0f, x);
    y = min_ff(1.0f, y);
    if (r == profile::PRO_SQUARE_R) {
      const float dx = 1.0f - x;
      const float dy = 1.0f - y;
      if (dx < dy) {
        x = 1.0f;
        y = midline ? 1.0f : y;
      }
      else {
        y = 1.0f;
        x = midline ? 1.0f : x;
      }
    }
    else {
      if (x < y) {
        x = 0.0f;
        y = midline ? 0.0f : y;
      }
      else {
        y = 0.0f;
        x = midline ? 0.0f : x;
      }
    }
  }
  else {
    const float rinv = 1.0f / r;
    if (a == 0.0f) {
      if (b == 0.0f) {
        x = 0.0f;
        y = 0.0f;
        z = powf(c, rinv);
      }
      else {
        x = 0.0f;
        y = powf(1.0f / (1.0f + powf(c / b, r)), rinv);
        z = c * y / b;
      }
    }
    else {
      x = powf(1.0f / (1.0f + powf(b / a, r) + powf(c / a, r)), rinv);
      y = b * x / a;
      z = c * x / a;
    }
  }
  co[0] = x;
  co[1] = y;
  co[2] = z;
}

/**
 * Builds a 4x4 matrix that maps the unit cube (with vertices at ±1) to the
 * tetrahedron formed by `va`, `vb`, `vc` (the three boundary verts) and `vd`
 * (the original beveled vertex). Same as BMesh's #make_unit_cube_map.
 */
static void make_unit_cube_map(
    const float va[3], const float vb[3], const float vc[3], const float vd[3], float r_mat[4][4])
{
  copy_v3_v3(r_mat[0], va);
  sub_v3_v3(r_mat[0], vb);
  sub_v3_v3(r_mat[0], vc);
  add_v3_v3(r_mat[0], vd);
  mul_v3_fl(r_mat[0], 0.5f);
  r_mat[0][3] = 0.0f;
  copy_v3_v3(r_mat[1], vb);
  sub_v3_v3(r_mat[1], va);
  sub_v3_v3(r_mat[1], vc);
  add_v3_v3(r_mat[1], vd);
  mul_v3_fl(r_mat[1], 0.5f);
  r_mat[1][3] = 0.0f;
  copy_v3_v3(r_mat[2], vc);
  sub_v3_v3(r_mat[2], va);
  sub_v3_v3(r_mat[2], vb);
  add_v3_v3(r_mat[2], vd);
  mul_v3_fl(r_mat[2], 0.5f);
  r_mat[2][3] = 0.0f;
  copy_v3_v3(r_mat[3], va);
  add_v3_v3(r_mat[3], vb);
  add_v3_v3(r_mat[3], vc);
  sub_v3_v3(r_mat[3], vd);
  mul_v3_fl(r_mat[3], 0.5f);
  r_mat[3][3] = 1.0f;
}

/**
 * Builds the canonical unit-simplex vmesh for the cube corner case by:
 *   1. constructing a seg=2 seed with profile-parameterized boundary midpoints,
 *   2. iteratively doubling via #cubic_subdiv until seg >= nseg,
 *   3. resampling to nseg via #interp_vmesh,
 *   4. snapping every grid point to the superellipsoid `x^r + y^r + z^r = 1`.
 * Equivalent to BMesh's #make_cube_corner_adj_vmesh.
 */
static VMesh make_cube_corner_adj_vmesh(BevelState &state)
{
  const float r = state.pro_super_r;
  const int nseg = state.params.segments;

  /* Create 3 BoundVerts for the unit simplex corners (1,0,0), (0,1,0), (0,0,1).
   * They are stack-allocated here and remain live for the entire subdivision.
   * cubic_subdiv and interp_vmesh access bndv->profile via vm->boundstart, so
   * these must not go out of scope until vm1 is returned. */
  BoundVert unit_bv[3] = {};

  for (int i = 0; i < 3; i++) {
    float co_start[3] = {0.0f, 0.0f, 0.0f};
    float co_end[3] = {0.0f, 0.0f, 0.0f};
    float co_mid[3] = {0.0f, 0.0f, 0.0f};
    co_start[i] = 1.0f;
    co_end[(i + 1) % 3] = 1.0f;
    co_mid[i] = 1.0f;
    co_mid[(i + 1) % 3] = 1.0f;

    /* Circular-list links. */
    unit_bv[i].next = &unit_bv[(i + 1) % 3];
    unit_bv[i].prev = &unit_bv[(i + 2) % 3];
    unit_bv[i].index = i;
    copy_v3_v3(unit_bv[i].nv.co, co_start);

    /* Set up the profile for the arc from corner i to corner i+1. */
    Profile &pro = unit_bv[i].profile;
    copy_v3_v3(pro.start, co_start);
    copy_v3_v3(pro.end, co_end);
    copy_v3_v3(pro.middle, co_mid);
    copy_v3_v3(pro.plane_co, co_start);
    cross_v3_v3v3(pro.plane_no, co_start, co_end);
    copy_v3_v3(pro.proj_dir, pro.plane_no);
    pro.super_r = r;
    pro.height = 0.0f;
    pro.special_params = false;

    /* Build the 2D→3D map and fill prof_co / prof_co_2. */
    float map[4][4];
    const bool use_map = (r != profile::PRO_LINE_R) &&
                         geom::make_unit_square_map(pro.start, pro.middle, pro.end, map);

    const ProfileSpacing &ps = state.pro_spacing;
    pro.prof_co = Array<float3>(nseg + 1);
    profile::calculate_profile_segments(pro,
                                        map,
                                        use_map,
                                        false,
                                        nseg,
                                        ps.xvals.data(),
                                        ps.yvals.data(),
                                        pro.prof_co.as_mutable_span());

    const bool need_2 = (nseg != ps.seg_2);
    if (need_2) {
      pro.prof_co_2 = Array<float3>(ps.seg_2 + 1);
      profile::calculate_profile_segments(pro,
                                          map,
                                          use_map,
                                          false,
                                          ps.seg_2,
                                          ps.xvals_2.data(),
                                          ps.yvals_2.data(),
                                          pro.prof_co_2.as_mutable_span());
    }
    else {
      pro.prof_co_2 = pro.prof_co;
    }
  }

  /* Build the seg=2 seed vmesh using the unit-simplex BoundVerts as the boundary ring. */
  VMesh vm0 = new_adj_vmesh(3, 2, &unit_bv[0]);

  for (int i = 0; i < 3; i++) {
    copy_v3_v3(geom::mesh_vert(&vm0, i, 0, 0)->co, unit_bv[i].nv.co);
    /* Sample the profile midpoint at k=1 out of seg=2. */
    float pt[3];
    profile::get_profile_point(state, &unit_bv[i].profile, 1, 2, pt);
    copy_v3_v3(geom::mesh_vert(&vm0, i, 0, 1)->co, pt);
  }

  /* Center vertex: place it on the (1,1,1) diagonal scaled by 1/sqrt(3),
   * then adjust slightly based on super_r to match BMesh. */
  float cen[3];
  copy_v3_fl(cen, float(M_SQRT1_3));
  if (nseg > 2) {
    if (r > 1.5f) {
      mul_v3_fl(cen, 1.4f);
    }
    else if (r < 0.75f) {
      mul_v3_fl(cen, 0.6f);
    }
  }
  copy_v3_v3(geom::mesh_vert(&vm0, 0, 1, 1)->co, cen);
  geom::vmesh_copy_equiv_verts(&vm0);

  /* Subdivide until seg >= nseg, then resample. */
  VMesh vm1 = std::move(vm0);
  while (vm1.seg < nseg) {
    VMesh next = cubic_subdiv(state, vm1);
    vm1 = std::move(next);
  }
  if (vm1.seg != nseg) {
    VMesh resampled = interp_vmesh(state, vm1, nseg);
    vm1 = std::move(resampled);
  }

  /* Snap every grid point onto the superellipsoid `x^r + y^r + z^r = 1`. */
  const int ns2 = nseg / 2;
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j <= ns2; j++) {
      for (int k = 0; k <= nseg; k++) {
        snap_to_superellipsoid(geom::mesh_vert(&vm1, i, j, k)->co, r, false);
      }
    }
  }
  return vm1;
}

/**
 * Tests whether `bv` is a good candidate for the tri-corner cube-corner special case.
 * Returns 1 when it qualifies (3-vert, equal offsets, ~90° corner angles),
 * 0 when the count is 3 but other conditions are not met,
 * and -1 when it definitely should not use this path.
 */
static int tri_corner_test(const BevelState &state, const BevVert *bv)
{
  /* Custom profiles and vertex-only mode skip this path. */
  if (state.params.affect_type == BevelAffect::Vertices || state.params.custom_profile != nullptr)
  {
    return -1;
  }
  if (bv->vmesh->count != 3) {
    return 0;
  }

  const float offset = bv->edges[0].offset_l;
  int in_plane_e = 0;
  float totang = 0.0f;
  const ExtendableMesh &emesh = state.emesh;

  for (int i = 0; i < bv->edgecount; i++) {
    const EdgeHalf &e = bv->edges[i];
    /* Compute the signed dihedral angle of this edge from its two adjacent face normals. */
    float ang = 0.0f;
    if (e.fprev >= 0 && e.fnext >= 0) {
      const float3 no_prev = emesh.face_normal(e.fprev);
      const float3 no_next = emesh.face_normal(e.fnext);
      ang = angle_signed_on_axis_v3v3_v3(no_prev, no_next, float3(0.0f) /* unused */);
      /* Use the dot-product sign to distinguish concave from convex. */
      const float dot = math::dot(no_prev, no_next);
      ang = acosf(math::clamp(dot, -1.0f, 1.0f));
      /* Negate for concave (the dihedral is > π). */
      if (math::dot(math::cross(no_prev, no_next),
                    emesh.vert_position(bv->v) - emesh.face_center(e.fprev)) < 0.0f)
      {
        ang = -ang;
      }
    }

    const float absang = fabsf(ang);
    if (absang <= float(M_PI_4)) {
      in_plane_e++;
    }
    else if (absang >= 3.0f * float(M_PI_4)) {
      return -1;
    }

    if (e.is_bev && !compare_ff(e.offset_l, offset, geom::BEVEL_EPSILON_D)) {
      return -1;
    }
    totang += ang;
  }

  if (in_plane_e != bv->edgecount - 3) {
    return -1;
  }
  const float angdiff = fabsf(fabsf(totang) - 3.0f * float(M_PI_2));
  if ((state.pro_super_r == profile::PRO_SQUARE_R && angdiff > float(M_PI) / 16.0f) ||
      (angdiff > float(M_PI_4)))
  {
    return -1;
  }
  if (bv->edgecount != 3 || bv->selcount != 3) {
    return 0;
  }
  return 1;
}

/**
 * Builds the ADJ vmesh for a tri-corner bevel using the cube-corner superellipsoid snap approach.
 * Equivalent to BMesh's #tri_corner_adj_vmesh.
 */
static VMesh tri_corner_adj_vmesh(BevelState &state, BevVert *bv)
{
  BoundVert *bndv = bv->vmesh->boundstart;
  float co0[3], co1[3], co2[3];
  copy_v3_v3(co0, bndv->nv.co);
  bndv = bndv->next;
  copy_v3_v3(co1, bndv->nv.co);
  bndv = bndv->next;
  copy_v3_v3(co2, bndv->nv.co);

  float mat[4][4];
  const float3 v_co = state.emesh.vert_position(bv->v);
  make_unit_cube_map(co0, co1, co2, v_co, mat);

  VMesh vm = make_cube_corner_adj_vmesh(state);
  /* Set the correct BoundVert ring (the canonical helper builds with nullptr). */
  vm.boundstart = bv->vmesh->boundstart;

  const int ns = vm.seg;
  const int ns2 = ns / 2;
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j <= ns2; j++) {
      for (int k = 0; k <= ns; k++) {
        float v[4];
        copy_v3_v3(v, geom::mesh_vert(&vm, i, j, k)->co);
        v[3] = 1.0f;
        mul_m4_v4(mat, v);
        copy_v3_v3(geom::mesh_vert(&vm, i, j, k)->co, v);
      }
    }
  }
  return vm;
}

/**
 * Builds the general ADJ vmesh by starting from a seed mesh with seg=2,
 * then iteratively doubling resolution via #cubic_subdiv and
 * resampling to the target with #interp_vmesh.
 * Dispatches to #tri_corner_adj_vmesh for the cube-corner special case.
 */
static VMesh adj_vmesh(BevelState &state, BevVert *bv)
{
  const int n_bndv = bv->vmesh->count;
  const int nseg = bv->vmesh->seg;

  /* Same as the bevel of 3 edges of a vertex in a cube: use the superellipsoid snap path. */
  if (n_bndv == 3 && tri_corner_test(state, bv) != -1 &&
      state.pro_super_r != profile::PRO_SQUARE_IN_R)
  {
    return tri_corner_adj_vmesh(state, bv);
  }

  VMesh vm0 = new_adj_vmesh(n_bndv, 2, bv->vmesh->boundstart);

  /* Seed mesh: boundary from BoundVert coords, mid-arc from profile at k=1. */
  float3 center(0.0f);
  BoundVert *bndv = vm0.boundstart;
  for (int i = 0; i < n_bndv; i++) {
    copy_v3_v3(geom::mesh_vert(&vm0, i, 0, 0)->co, bndv->nv.co);
    float pt[3];
    profile::get_profile_point(state, &bndv->profile, 1, 2, pt);
    copy_v3_v3(geom::mesh_vert(&vm0, i, 0, 1)->co, pt);
    center += float3(bndv->nv.co);
    bndv = bndv->next;
  }
  center /= float(n_bndv);

  /* Center vertex position using fullness. */
  const float3 v_co = state.emesh.vert_position(bv->v);
  const float3 center_dir = v_co - center;
  if (math::length_squared(center_dir) > geom::BEVEL_EPSILON_SQ) {
    const float fullness = state.pro_spacing.fullness;
    float3 cen_co = center + center_dir * fullness;
    copy_v3_v3(geom::mesh_vert(&vm0, 0, 1, 1)->co, cen_co);
  }
  else {
    copy_v3_v3(geom::mesh_vert(&vm0, 0, 1, 1)->co, center);
  }
  geom::vmesh_copy_equiv_verts(&vm0);

  /* Subdivide until seg >= nseg. */
  VMesh vm1 = std::move(vm0);
  while (vm1.seg < nseg) {
    VMesh next = cubic_subdiv(state, vm1);
    vm1 = std::move(next);
  }
  if (vm1.seg != nseg) {
    VMesh resampled = interp_vmesh(state, vm1, nseg);
    vm1 = std::move(resampled);
  }
  return vm1;
}

/** \} */

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
  /* Use the first offset component of the first edge as an approximation.
   * This is exact when all edges share a uniform offset, which is the common case.
   * TODO: handle vertex groups and bevel weights properly. */
  bv->offset = (first_e != -1) ? state.params.offsets[0][first_e] : 1.0f;

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

    /* An edge half is a seam when its two adjacent faces have discontinuous UV data,
     * or when one of those faces is absent (boundary edge). Mirrors the BMesh logic
     * in #bev_vert_construct. */
    if (eh->fprev != -1 && eh->fnext != -1) {
      eh->is_seam = !state.uv_layer_info.contig_ldata_across_edge(
          emesh.mesh, eh->e, eh->fprev, eh->fnext);
    }
    else {
      eh->is_seam = true;
    }
  }

  state.vert_hash.add_new(v, bv);
}

/* -------------------------------------------------------------------- */
/** \name Output mesh assembly
 * \{ */

/**
 * Assembles a new output #Mesh from the #ExtendableMesh, which holds both the
 * original mesh elements (some marked as killed) and the newly created elements.
 *
 * Index convention in #ExtendableMesh:
 *   - original elements:  indices 0 .. mesh.Xnum - 1
 *   - new elements:       indices mesh.Xnum .. mesh.Xnum + newX - 1
 *
 * Mirrors the `build_mesh` function from the TRY1 reference implementation,
 * adapted for the new index convention (no negated indices).
 * Attribute copying is a TODO for a later pass.
 */
static std::optional<Mesh *> build_output_mesh(const BevelState &state)
{
  const ExtendableMesh &emesh = state.emesh;
  const Mesh &src_mesh = emesh.mesh;

  /* Collect surviving original element masks from the kill arrays. */
  index_mask::IndexMaskMemory memory;
  const IndexMask src_survive_verts = IndexMask::from_bools(emesh.kill_verts_array(), memory)
                                          .complement(IndexMask(src_mesh.verts_num), memory);
  const IndexMask src_survive_edges = IndexMask::from_bools(emesh.kill_edges_array(), memory)
                                          .complement(IndexMask(src_mesh.edges_num), memory);
  const IndexMask src_survive_faces = IndexMask::from_bools(emesh.kill_faces_array(), memory)
                                          .complement(IndexMask(src_mesh.faces_num), memory);

  /* Build old→new index maps for surviving original elements; -1 for killed entries. */
  Array<int> src_vert_map(src_mesh.verts_num, -1);
  index_mask::build_reverse_map(src_survive_verts, src_vert_map.as_mutable_span());
  Array<int> src_edge_map(src_mesh.edges_num, -1);
  index_mask::build_reverse_map(src_survive_edges, src_edge_map.as_mutable_span());

  const int n_surv_verts = src_survive_verts.size();
  const int n_surv_edges = src_survive_edges.size();
  const int n_surv_faces = src_survive_faces.size();

  /* Count surviving corners: only corners in surviving original faces are kept. */
  int n_surv_corners = 0;
  const OffsetIndices<int> src_faces = src_mesh.faces();
  src_survive_faces.foreach_index(
      [&](const int f) { n_surv_corners += int(src_faces[f].size()); });

  const Span<float3> new_positions = emesh.new_vert_positions();
  const Span<int2> new_edge_data = emesh.new_edges();
  const Span<int> new_face_offs = emesh.new_face_offsets(); /* size = new_faces_num + 1 */
  const Span<int> new_cv = emesh.new_corner_verts();
  const Span<int> new_ce = emesh.new_corner_edges();
  const int n_new_verts = int(new_positions.size());
  const int n_new_edges = int(new_edge_data.size());
  const int n_new_faces = emesh.new_faces_num();
  const int n_new_corners = int(new_cv.size());

  Mesh *dst = BKE_mesh_new_nomain_from_template(&src_mesh,
                                                n_surv_verts + n_new_verts,
                                                n_surv_edges + n_new_edges,
                                                n_surv_faces + n_new_faces,
                                                n_surv_corners + n_new_corners);

  MutableSpan<float3> dst_positions = dst->vert_positions_for_write();
  MutableSpan<int2> dst_edges = dst->edges_for_write();
  MutableSpan<int> dst_face_offsets = dst->face_offsets_for_write();
  MutableSpan<int> dst_corner_verts = dst->corner_verts_for_write();
  MutableSpan<int> dst_corner_edges = dst->corner_edges_for_write();

  const Span<float3> src_positions = src_mesh.vert_positions();
  const Span<int2> src_edges = src_mesh.edges();
  const Span<int> src_corner_verts = src_mesh.corner_verts();
  const Span<int> src_corner_edges = src_mesh.corner_edges();

  /* Maps any combined (original + new) vertex index to the destination index. */
  auto mixed_vert_map = [&](const int v) -> int {
    if (v < src_mesh.verts_num) {
      return src_vert_map[v];
    }
    return n_surv_verts + (v - src_mesh.verts_num);
  };
  /* Maps any combined (original + new) edge index to the destination index. */
  auto mixed_edge_map = [&](const int e) -> int {
    if (e < src_mesh.edges_num) {
      return src_edge_map[e];
    }
    return n_surv_edges + (e - src_mesh.edges_num);
  };

  /* 1. Surviving original vert positions. */
  src_survive_verts.foreach_index([&](const int64_t src_v, const int64_t dst_v) {
    dst_positions[dst_v] = src_positions[src_v];
  });

  /* 2. New vert positions. */
  for (const int nv : IndexRange(n_new_verts)) {
    dst_positions[n_surv_verts + nv] = new_positions[nv];
  }

  /* 3. Surviving original edges (vertex pairs remapped to destination indices). */
  src_survive_edges.foreach_index([&](const int64_t src_e, const int64_t dst_e) {
    dst_edges[dst_e][0] = src_vert_map[src_edges[src_e][0]];
    dst_edges[dst_e][1] = src_vert_map[src_edges[src_e][1]];
  });

  /* 4. New edges. */
  for (const int ne : IndexRange(n_new_edges)) {
    dst_edges[n_surv_edges + ne][0] = mixed_vert_map(new_edge_data[ne][0]);
    dst_edges[n_surv_edges + ne][1] = mixed_vert_map(new_edge_data[ne][1]);
  }

  /* 5. Surviving original faces: offsets and corner data (remapped). */
  {
    int dst_corner = 0;
    src_survive_faces.foreach_index([&](const int64_t src_f, const int64_t dst_f) {
      const IndexRange src_face = src_faces[src_f];
      dst_face_offsets[dst_f] = dst_corner;
      for (const int i : src_face.index_range()) {
        dst_corner_verts[dst_corner] = src_vert_map[src_corner_verts[src_face[i]]];
        dst_corner_edges[dst_corner] = src_edge_map[src_corner_edges[src_face[i]]];
        dst_corner++;
      }
    });
    BLI_assert(dst_corner == n_surv_corners);
  }

  /* 6. New faces: offsets and corner data. */
  for (const int nf : IndexRange(n_new_faces)) {
    dst_face_offsets[n_surv_faces + nf] = n_surv_corners + new_face_offs[nf];
  }
  /* Sentinel at the end (Blender stores offsets as face_offsets[face_num] = corners_num). */
  dst_face_offsets[n_surv_faces + n_new_faces] = n_surv_corners + new_face_offs[n_new_faces];
  for (const int nc : IndexRange(n_new_corners)) {
    dst_corner_verts[n_surv_corners + nc] = mixed_vert_map(new_cv[nc]);
    dst_corner_edges[n_surv_corners + nc] = mixed_edge_map(new_ce[nc]);
  }

  /* 7. Attribute propagation via the modern Attribute API.
   *
   * For each element domain, build a flat src-index array of size dst_count where entry i
   * holds the source element index whose attributes should be copied to dst element i.
   * - Surviving original elements map directly (src_v → dst_v from the survival mask).
   * - New elements use their recorded representative (example) index; -1 falls back to 0.
   * Then a single gather_attributes() call handles all attribute types for that domain.
   * Built-in geometry attributes (position, edge verts, corner_vert/edge) are excluded by the
   * attribute filter because they were already written explicitly in steps 1-6 above. */

  const bke::AttributeAccessor src_attrs = src_mesh.attributes();
  bke::MutableAttributeAccessor dst_attrs = dst->attributes_for_write();

  /* Skip attributes that encode topology already written with correct remapped indices in
   * steps 1-6 above.  Letting gather_attributes overwrite these with un-remapped original
   * indices produces invalid geometry:
   *   position     – re-applied explicitly for new verts after the Point domain gather.
   *   .edge_verts  – written with remapped vertex indices in steps 3-4.
   *   .corner_vert – written with remapped vertex indices in steps 5-6.
   *   .corner_edge – written with remapped edge indices in steps 5-6. */
  const StringRef skip_names[] = {
      "position", ".edge_verts", ".corner_vert", ".corner_edge"};
  const auto geom_filter = bke::attribute_filter_from_skip_ref(
      Span<StringRef>{skip_names, ARRAY_SIZE(skip_names)});

  /* 7a. Point domain (verts): surviving originals then new verts. */
  {
    Array<int> src_for_dst(n_surv_verts + n_new_verts, 0);
    src_survive_verts.foreach_index([&](const int64_t src_v, const int64_t dst_v) {
      src_for_dst[dst_v] = int(src_v);
    });
    const Span<int> new_vert_exs = emesh.new_vert_examples();
    for (const int ni : IndexRange(n_new_verts)) {
      const int ex = new_vert_exs[ni];
      src_for_dst[n_surv_verts + ni] = (ex >= 0) ? ex : 0;
    }
    bke::gather_attributes(
        src_attrs, bke::AttrDomain::Point, bke::AttrDomain::Point, geom_filter, src_for_dst, dst_attrs);
    /* Re-apply new vert positions: gather_attributes above copies position from the example vert,
     * but new bevel verts must keep their computed profile positions. */
    for (const int ni : IndexRange(n_new_verts)) {
      dst_positions[n_surv_verts + ni] = new_positions[ni];
    }
  }

  /* 7b. Edge domain: surviving original edges then new edges. */
  {
    Array<int> src_for_dst(n_surv_edges + n_new_edges, 0);
    src_survive_edges.foreach_index([&](const int64_t src_e, const int64_t dst_e) {
      src_for_dst[dst_e] = int(src_e);
    });
    const Span<int> new_edge_exs = emesh.new_edge_examples();
    for (const int ni : IndexRange(n_new_edges)) {
      const int ex = new_edge_exs[ni];
      src_for_dst[n_surv_edges + ni] = (ex >= 0) ? ex : 0;
    }
    bke::gather_attributes(
        src_attrs, bke::AttrDomain::Edge, bke::AttrDomain::Edge, geom_filter, src_for_dst, dst_attrs);
  }

  /* 7c. Face domain: surviving original faces then new faces. */
  {
    Array<int> src_for_dst(n_surv_faces + n_new_faces, 0);
    src_survive_faces.foreach_index([&](const int64_t src_f, const int64_t dst_f) {
      src_for_dst[dst_f] = int(src_f);
    });
    const Span<int> new_face_exs = emesh.new_face_examples();
    for (const int nf : IndexRange(n_new_faces)) {
      const int ex = new_face_exs[nf];
      src_for_dst[n_surv_faces + nf] = (ex >= 0) ? ex : 0;
    }
    bke::gather_attributes(
        src_attrs, bke::AttrDomain::Face, bke::AttrDomain::Face, geom_filter, src_for_dst, dst_attrs);
  }

  /* 7d. Corner domain: surviving original corners (from surviving faces), new corners deferred.
   *     New corner attributes (UV, normals) require interpolation and are a future task. */
  {
    /* Build the src index array for surviving corners only (new corners keep defaults). */
    Array<int> src_for_dst(n_surv_corners + n_new_corners, 0);
    int dst_c = 0;
    src_survive_faces.foreach_index([&](const int64_t src_f, const int64_t /*dst_f*/) {
      for (const int sc : src_faces[src_f]) {
        src_for_dst[dst_c++] = sc;
      }
    });
    BLI_assert(dst_c == n_surv_corners);
    /* New corners: no example yet — leave src index 0 (default values). */
    bke::gather_attributes(
        src_attrs, bke::AttrDomain::Corner, bke::AttrDomain::Corner, geom_filter, src_for_dst, dst_attrs);
  }

  BLI_assert(bke::mesh_is_valid(*dst));
  return dst;
}

/** \} */

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
    BevVert *bv = state.vert_hash.lookup(v);
    construct::build_boundary(state.emesh, state, bv, true);
    construct::determine_uv_vert_connectivity(state, v);
    construct::build_vmesh(state, bv);
    if (v == 1) {
      fmt::println("\nMESH code dump bv for vert 1");
      debug::dump_bev_vert(*bv);
      /* Diagnostic: print boundary ring ->v at i=count-1 including wrap k=seg. */
      if (bv->vmesh && bv->vmesh->count > 2) {
        VMesh *vm_dbg = bv->vmesh.get();
        const int ns_dbg = vm_dbg->seg;
        fmt::print("  DEBUG boundary ring i=2: ");
        for (int kk = 0; kk <= ns_dbg; kk++) {
          fmt::print("k={} v={} | ", kk, geom::mesh_vert(vm_dbg, 2, 0, kk)->v);
        }
        fmt::println("");
      }
    }
  });

  /* Build edge-strip polygons along each beveled edge. */
  if (params.affect_type != BevelAffect::Vertices) {
    state.selection.foreach_index(
        [&](const int e) { construct::bevel_build_edge_polygons(state, e); });

    /* Debug: dump edge polygons for vertex 1. */
    {
      BevVert *bv1 = state.vert_hash.lookup_default(1, nullptr);
      if (bv1) {
        debug::dump_edge_polygons(state, *bv1);
      }
    }
  }

  /* Rebuild original faces that touch beveled vertices. */
  Vector<int> rebuilt_orig_faces;
  int rebuilt_face_0 = -1;
  construct::bevel_rebuild_existing_polygons(state, rebuilt_orig_faces, rebuilt_face_0);

  /* Debug: dump the rebuilt face that replaced original face 0. */
  fmt::println("\nMESH rebuilt face for orig face 0:");
  debug::dump_rebuilt_face(state, rebuilt_face_0);

  /* Debug: dump the example face recorded for each newly created face. */
  fmt::println("\n");
  debug::dump_new_face_examples(state);

  /* Kill the original faces that were rebuilt, mirroring BMesh's deferred kill pattern. */
  for (const int f : rebuilt_orig_faces) {
    state.emesh.face_kill(f);
  }

  /* Kill original beveled vertices. */
  state.bevel_affected_vertices.foreach_index([&](const int v) { state.emesh.vert_kill(v); });

  /* TODO: bevel_extend_edge_data (sharp/seam propagation). */

  return construct::build_output_mesh(state);
}

}  // namespace blender::geometry
