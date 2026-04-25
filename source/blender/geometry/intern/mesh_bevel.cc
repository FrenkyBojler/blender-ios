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
  float3 dir1 = emesh.vert_position(geom::edge_other_vert(emesh, e1->e, v)) - v_co;
  float3 dir2 = emesh.vert_position(geom::edge_other_vert(emesh, e2->e, v)) - v_co;

  float3 dir1n = float3(0.0f);
  float3 dir2p = float3(0.0f);
  if (edges_between) {
    EdgeHalf *e1next = e1->next;
    EdgeHalf *e2prev = e2->prev;
    dir1n = emesh.vert_position(geom::edge_other_vert(emesh, e1next->e, v)) - v_co;
    dir2p = emesh.vert_position(geom::edge_other_vert(emesh, e2prev->e, v)) - v_co;
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

/* Prints a #VMesh and all its #BoundVert chain. */
[[maybe_unused]] static void dump_vmesh(const VMesh &vm)
{
  fmt::println(
      "  VMesh: count={} seg={} mesh_kind={}", vm.count, vm.seg, mesh_kind_name(vm.mesh_kind));
  if (vm.boundstart == nullptr) {
    fmt::println("  (no boundverts)");
    return;
  }
  /* Walk the circular linked list. */
  const BoundVert *bndv = vm.boundstart;
  do {
    dump_bound_vert(*bndv);
    bndv = bndv->next;
  } while (bndv != vm.boundstart);
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

}  // namespace debug

/** \} */

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
    BevVert *bv = state.vert_hash.lookup(v);
    construct::build_boundary(state.emesh, state, bv, true);
    if (v == 0) {
      fmt::println("\nMESH code dump bv for vert 0");
      debug::dump_bev_vert(*bv);
    }

    // TODO: determine_uv_vert_connectivity
  });

  return std::nullopt;
}

}  // namespace blender::geometry
