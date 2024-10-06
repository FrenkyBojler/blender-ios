/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <iostream>

#include "BLI_array.hh"
#include "BLI_hash.hh"
#include "BLI_map.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"
#include "BLI_task.hh"
#include "BLI_timeit.hh"
#include "BLI_vector.hh"

#include "BKE_attribute.hh"
#include "BKE_customdata.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.hh"

#include "mesh_boolean_manifold.hh"

#include "manifold.h"

using manifold::Manifold;
using manifold::MeshGL;

/* Using this for now for debug printing of materials. Can remove later. */
#include "DNA_material_types.h"

namespace blender::geometry::boolean {

/* Some debug output functions. */

static std::ostream &operator<<(std::ostream &os, const glm::vec3 &v)
{
  os << "(" << v[0] << "," << v[1] << "," << v[2] << ")";
  return os;
}

static std::ostream &operator<<(std::ostream &os, const glm::ivec3 &v)
{
  os << "(" << v[0] << "," << v[1] << "," << v[2] << ")";
  return os;
}

template<typename T>
static void dump_vector(const std::vector<T> &vec, int stride, const std::string &name)
{
  std::cout << name << ":";
  for (size_t i = 0; i < vec.size(); i++) {
    if (i % 10 == 0) {
      std::cout << "\n[" << i << "] ";
    }
    std::cout << vec[i] << " ";
    if (stride > 1 && (i % stride) == stride - 1) {
      std::cout << "/ ";
    }
  }
  std::cout << "\n";
}

static void dump_meshgl(const MeshGL &mgl, const std::string &name)
{
  std::cout << "\nMeshGL " << name << ":\n"
            << "num verts = " << mgl.NumVert() << "\nnum triangles = " << mgl.NumTri() << "\n"
            << "\n";
  dump_vector(mgl.vertProperties, mgl.numProp, "vertProperties");
  dump_vector(mgl.triVerts, 3, "triVerts");
  dump_vector(mgl.faceID, 1, "faceID");
  if (mgl.mergeFromVert.size() > 0) {
    dump_vector(mgl.mergeFromVert, 1, "mergeFromVert");
    dump_vector(mgl.mergeToVert, 1, "mergeToVert");
  }
  dump_vector(mgl.runIndex, 1, "runIndex");
  dump_vector(mgl.runOriginalID, 1, "runOrigiinalID");
}

static void dump_manmesh(const manifold::Mesh &mmesh, const std::string &name)
{
  std::cout << "\nmanifold::Mesh " << name << ":\n";
  dump_vector(mmesh.vertPos, 1, "vertPos");
  dump_vector(mmesh.triVerts, 1, "triVerts");
}

template<typename T> static void dump_span(Span<T> span, const std::string &name)
{
  std::cout << name << ":";
  for (const int i : span.index_range()) {
    if (i % 10 == 0) {
      std::cout << "\n[" << i << "] ";
    }
    std::cout << span[i] << " ";
  }
  std::cout << "\n";
}

static void dump_mesh(const Mesh *mesh, const std::string &name)
{
  std::cout << "\nMesh " << name << ":\n"
            << "verts_num = " << mesh->verts_num << "\nfaces_num = " << mesh->faces_num
            << "\nedges_num = " << mesh->edges_num << "\ncorners_num = " << mesh->corners_num
            << "\n";
  dump_span(mesh->vert_positions(), "verts");
  dump_span(mesh->edges(), "edges");
  dump_span(mesh->corner_verts(), "corner_verts");
  dump_span(mesh->corner_edges(), "corner_edges");
  dump_span(mesh->face_offsets(), "face_offsets");
  std::cout << "triangulation:\n";
  dump_span(mesh->corner_tris(), "corner_tris");
  dump_span(mesh->corner_tri_faces(), "corner_tri_faces");
  std::cout << "attributes:\n";
  bke::AttributeAccessor attrs = mesh->attributes();
  attrs.foreach_attribute([&](const bke::AttributeIter &iter) {
    if (ELEM(iter.name, "position", ".edge_verts", ".corner_vert", ".corner_edge")) {
      return true;
    }
    static const char *domain_names[] = {
        "point", "edge", "face", "corner", "curve", "instance", "layer"};
    const int di = static_cast<int8_t>(iter.domain);
    const char *domain = (di >= 0 && di < ATTR_DOMAIN_NUM) ? domain_names[di] : "?";
    std::string label = std::string(domain) + ": " + iter.name;
    switch (iter.data_type) {
      case CD_PROP_FLOAT: {
        VArraySpan<float> floatspan(*attrs.lookup<float>(iter.name));
        dump_span(floatspan, label);
      } break;
      case CD_PROP_INT32:
      case CD_PROP_BOOL: {
        VArraySpan<int> intspan(*attrs.lookup<int>(iter.name));
        dump_span(intspan, label);
      } break;
      case CD_PROP_FLOAT3: {
        VArraySpan<float3> float3span(*attrs.lookup<float3>(iter.name));
        dump_span(float3span, label);
      } break;
      case CD_PROP_FLOAT2: {
        VArraySpan<float2> float2span(*attrs.lookup<float2>(iter.name));
        dump_span(float2span, label);
      } break;
      default:
        std::cout << label << " attribute not dumped\n";
        break;
    }
    return true;
  });
  std::cout << "materials:\n";
  for (int i = 0; i < mesh->totcol; i++) {
    std::cout << "[" << i << "]: " << (mesh->mat[i] ? mesh->mat[i]->id.name + 2 : "none") << "\n";
  }
}

static Manifold manifold_from_mesh_via_meshgl(const Mesh *mesh, int mesh_index, int faceID_offset)
{
  constexpr int dbg_level = 1;
  if (dbg_level > 0) {
    std::cout << "\nMANIFOLD_FRON_MESH_VIA_MESHGL\n";
    dump_mesh(mesh, "mesh " + std::to_string(mesh_index));
  }
  timeit::ScopedTimer timer("manifold from mesh via meshgl");
  const int num_verts = mesh->verts_num;
  MeshGL meshgl;
  meshgl.numProp = 3;
  meshgl.vertProperties.resize(num_verts * meshgl.numProp);
  Span<float3> vpos = mesh->vert_positions();
  const int grain_size = 10000;
  threading::parallel_for(IndexRange(num_verts), grain_size, [&](const IndexRange range) {
    for (const int i : range) {
      const float3 &pos = vpos[i];
      meshgl.vertProperties[3 * i] = pos[0];
      meshgl.vertProperties[3 * i + 1] = pos[1];
      meshgl.vertProperties[3 * i + 2] = pos[2];
    }
  });

  Span<int3> corner_tris = mesh->corner_tris();
  Span<int> corner_verts = mesh->corner_verts();
  Span<int> corner_tri_faces = mesh->corner_tri_faces();
  const int num_tris = corner_tris.size();
  meshgl.triVerts.resize(3 * num_tris);
  meshgl.faceID.resize(num_tris);
  threading::parallel_for(corner_tris.index_range(), grain_size, [&](const IndexRange range) {
    for (const int i : range) {
      const int3 &ctri = corner_tris[i];
      meshgl.triVerts[3 * i] = corner_verts[ctri[0]];
      meshgl.triVerts[3 * i + 1] = corner_verts[ctri[1]];
      meshgl.triVerts[3 * i + 2] = corner_verts[ctri[2]];
      meshgl.faceID[i] = faceID_offset + corner_tri_faces[i];
    }
  });
  meshgl.runIndex.resize(2);
  meshgl.runOriginalID.resize(1);
  meshgl.runIndex[0] = 0;
  meshgl.runIndex[1] = 3 * num_tris;
  meshgl.runOriginalID[0] = mesh_index;
  if (dbg_level > 0) {
    dump_meshgl(meshgl, "converted result");
  }
  Manifold ans;
  {
    timeit::ScopedTimer mtimer("manifold constructor from meshgl");
    ans = Manifold(meshgl);
  }
  return ans;
}

/* Find the index in offset_indices that the first place
 * with a value >= x. Return -1 if there is no such index.
 * When the argument is a sorted array of range breakpoints, starting at 0,
 * this will return the index of the range that contains x, if there is one. */
template<typename T> static int which_offset_index(T x, Span<T> offset_indices)
{
  /* TODO: use binary search or std::lower if size of offset_indices is not small.
   * Maybe add this into the mathods for OffsetIndices. */
  int i = 0;
  for (; i != offset_indices.size() - 1; i++) {
    if (x < offset_indices[i + 1]) {
      return i;
    }
  }
  return -1;
}

/* Given an output triangle index \a output_tri in \a output_mesh_gl,
 * what is the corresponding input mesh index and face index? */
static std::pair<int, int> mesh_and_face(int output_tri,
                                         const MeshGL &output_meshgl,
                                         Span<int> mesh_face_offsets)

{
  /* First find the index for the original input_mesh that contains the output_tri. */
  int output_run_index = which_offset_index(uint32_t(3 * output_tri),
                                            Span<uint32_t>(output_meshgl.runIndex));
  BLI_assert(output_run_index != -1);
  int input_mesh_index = output_meshgl.runOriginalID[output_run_index];
  BLI_assert(input_mesh_index >= 0 && input_mesh_index < mesh_face_offsets.size());

  /* Now find the face index in the input mesh, given the triangle index in the output meshgl. */
  int tri_faceid = output_meshgl.faceID[output_tri];
  int face_in_input_mesh = tri_faceid - mesh_face_offsets[input_mesh_index];
  return {input_mesh_index, face_in_input_mesh};
}

class GAttributeReadWriteSpans {
 public:
  /* A set of attributes we want copied. */
  Vector<StringRef> attrs;
  /* Parallel array of data_type. */
  Vector<eCustomDataType> data_types;
  /* Destination attribute data, one span per attribute we want copied. */
  Vector<GMutableSpan> dest;
  /* Correpsonding AttributeWriters. */
  Vector<bke::GSpanAttributeWriter> dest_writers;
  /* For each input mesh, the source attribute data parallel to dest. */
  Array<Vector<std::optional<GVArraySpan>>> sources;

  GAttributeReadWriteSpans(Span<const Mesh *> input_meshes,
                           Mesh *output_mesh,
                           bke::AttrDomain domain);
  ~GAttributeReadWriteSpans();

  int find_attr_index(const char *name) const;
};

GAttributeReadWriteSpans::GAttributeReadWriteSpans(Span<const Mesh *> input_meshes,
                                                   Mesh *output_mesh,
                                                   bke::AttrDomain domain)
{
  const int num_mesh = input_meshes.size();
  Vector<bke::AttributeAccessor> input_accessors;
  this->sources.reinitialize(num_mesh);
  for (int i : IndexRange(num_mesh)) {
    input_accessors.append(input_meshes[i]->attributes());
  }
  bke::MutableAttributeAccessor output_accessor = output_mesh->attributes_for_write();
  output_accessor.foreach_attribute(
      [&](const bke::AttributeIter &iter) {
        if (iter.domain != domain) {
          return true;
        }
        this->attrs.append(iter.name);
        this->data_types.append(iter.data_type);
        this->dest_writers.append(output_accessor.lookup_or_add_for_write_only_span(
            iter.name, iter.domain, iter.data_type));
        this->dest.append(this->dest_writers.last().span);
        for (int i : IndexRange(num_mesh)) {
          this->sources[i].append(
              *input_accessors[i].lookup_or_default(iter.name, domain, iter.data_type));
        }
        return true;
      });
}

GAttributeReadWriteSpans::~GAttributeReadWriteSpans()
{
  for (bke::GSpanAttributeWriter &w : dest_writers) {
    w.finish();
  }
}

int GAttributeReadWriteSpans::find_attr_index(const char *name) const
{
  for (int i : this->attrs.index_range()) {
    if (this->attrs[i] == name) {
      return i;
    }
  }
  return -1;
}

static void copy_face_attrs(GAttributeReadWriteSpans &rw_spans,
                            int input_mesh_index,
                            int input_face,
                            int output_face,
                            int material_span_index,
                            Span<Array<short>> material_remaps)
{
  constexpr int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "copy_face_attrs, input mesh " << input_mesh_index << ", face " << input_face
              << " to  output face " << output_face << "\n";
  }
  for (const int i : rw_spans.attrs.index_range()) {
    std::optional<GVArraySpan> &src = rw_spans.sources[input_mesh_index][i];
    GMutableSpan &dst = rw_spans.dest[i];
    if (src.has_value()) {
      /* rw_spans.dest[output_face] = src[input_face] */
      dst.type().copy_assign(src.value()[input_face], dst[output_face]);
      /* Special additional handling for maetrial_index property. */
      if (i == material_span_index) {
        BLI_assert(dst.type().size() == sizeof(int32_t));
        int32_t src_mat;
        Span<short> remap = material_remaps[input_mesh_index];
        dst.type().copy_assign(src.value()[input_face], &src_mat);
        if (remap.index_range().contains(src_mat)) {
          int remapped_index = remap[src_mat];
          if (remapped_index >= 0) {
            dst.type().copy_assign(&remapped_index, dst[output_face]);
          }
        }
      }
    }
  }
}

/* Holds cumulative offsets for the given elements of a number
 * of concatenated Meshes. The sizes are one greater than the
 * number of meshes, so that the last value of each gives the
 * total number of elements. */
struct MeshOffsets {
  Array<int> vert_offsets;
  Array<int> face_offsets;

  MeshOffsets(Span<const Mesh *> meshes);
};

MeshOffsets::MeshOffsets(Span<const Mesh *> meshes)
{
  const int num_meshes = meshes.size();
  this->vert_offsets.reinitialize(num_meshes + 1);
  this->face_offsets.reinitialize(num_meshes + 1);
  for (int i = 0; i <= num_meshes; i++) {
    this->vert_offsets[i] = (i == 0) ? 0 : this->vert_offsets[i - 1] + meshes[i - 1]->verts_num;
    this->face_offsets[i] = (i == 0) ? 0 : this->face_offsets[i - 1] + meshes[i - 1]->faces_num;
  }
}

/* Return the mesh index and face index within that mesh corresponding to offset-face,
 * a face index in the concatenated index space of all mesh faces. */
static std::pair<int, int> offset_face_to_mesh_face(int offset_face, Span<int> mesh_face_offsets)
{
  for (int i = 1; i < mesh_face_offsets.size(); i++) {
    if (offset_face < mesh_face_offsets[i]) {
      return {i - 1, offset_face - mesh_face_offsets[i - 1]};
    }
  }
  return {mesh_face_offsets.size() - 1, offset_face - mesh_face_offsets.last()};
}

constexpr int inline_outface_size = 8;

struct OutFace {
  /* Vertex ids in meshgl indexing space. */
  Vector<int, inline_outface_size> verts;
  /* The faceID input to manifold, i.e. original face id in combined input mesh indexing space.
   */
  int face_id;
};

/* Data needed to build the final output Mesh. */
struct MeshAssembly {
  /* Vertex positions, linearized (use vertpos_stride to multiply index). */
  Span<float> vertpos;
  int vertpos_stride = 3;
  /* How many vertices were in the combined input meshes. */
  int num_input_verts;
  /* How many vertices are in the output (i.e., in vertpos). */
  int num_output_verts;
  /* Map from output vertex index to corresponding input vertex (-1 if none). */
  Array<int> out_to_in_vert_map;
  /* Offset face ids. i.e., offset by cumulative face count in input meshes) direct to output. */
  Vector<int> input_faces_to_output;
  /* New faces to output. */
  Vector<OutFace> new_faces;
};

/* Fill the MeshAssembly's out_to_in_vert_map.
 * Do this by fnding, for each output face, which verts of the corresponding
 * input face match.
 */
static void fill_vertex_map(MeshAssembly &ma,
                            const MeshGL &mgl,
                            Span<const Mesh *> meshes,
                            const MeshOffsets &mesh_offsets)
{
  constexpr int dbg_level = 1;
  if (dbg_level > 0) {
    std::cout << "fill_vertex_map\n";
  }
  ma.out_to_in_vert_map = Array<int>(ma.num_output_verts, -1);
  const int tris_num = mgl.NumTri();
  const int stride = mgl.numProp;
  for (const int t : IndexRange(tris_num)) {
    const int faceid = mgl.faceID[t];
    auto [mesh_index, face_in_mesh] = offset_face_to_mesh_face(faceid, mesh_offsets.face_offsets);
    const Mesh *mesh = meshes[mesh_index];
    const IndexRange orig_face = mesh->faces()[face_in_mesh];
    Span<int> orig_face_verts = mesh->corner_verts().slice(orig_face);
    for (const int i : IndexRange(3)) {
      int v = mgl.triVerts[3 * t + i];
      if (ma.out_to_in_vert_map[v] != -1) {
        continue;
      }
      int prop_offset = v * stride;
      float3 pos(mgl.vertProperties[prop_offset],
                 mgl.vertProperties[prop_offset + 1],
                 mgl.vertProperties[prop_offset + 2]);
      auto it = std::find_if(orig_face_verts.begin(), orig_face_verts.end(), [&](int orig_v) {
        return pos == mesh->vert_positions()[orig_v];
      });
      if (it != orig_face_verts.end()) {
        int orig_v = orig_face_verts[std::distance(orig_face_verts.begin(), it)];
        ma.out_to_in_vert_map[v] = orig_v + mesh_offsets.vert_offsets[mesh_index];
        if (dbg_level > 0) {
          std::cout << " m[" << v << "] = " << ma.out_to_in_vert_map[v] << "\n";
        }
      }
    }
  }
}

/* Most input faces should mape to face_group_inline or fewer output triangles. */
constexpr int face_group_inline = 4;

/* Return an array of length \a input_faces_num, where the ith entry
 * is a Vector of the \a mgl triangles that derive from the ith input
 * face (where i is an index in the concatenated input mesh face space.
 */
static Array<Vector<int, face_group_inline>> get_face_groups(const MeshGL &mgl,
                                                             int input_faces_num)
{
  constexpr int dbg_level = 0;
  Array<Vector<int, face_group_inline>> fg(input_faces_num);
  const int tris_num = mgl.NumTri();
  BLI_assert(mgl.faceID.size() == tris_num);
  for (const int t : IndexRange(tris_num)) {
    const int faceid = mgl.faceID[t];
    fg[faceid].append(t);
  }
  if (dbg_level > 0) {
    std::cout << "face_groups\n";
    for (const int i : fg.index_range()) {
      std::cout << "orig face " << i;
      dump_span(fg[i].as_span(), "");
    }
  }
  return fg;
}

#if 0
* TODO: later */
/* Return 1 if \a group is just the same oas the original face \a face_index
 * in \a mesh.
 * Return 2 if it is the same but with the noraal reversed.
 * Return 0 otherwise. */
static uchar check_original_face(const Vector<int, face_group_inline> &group,
                                 const MeshGL &mgl,
                                 const Mesh *mesh,
                                 int face_index)
{
  BLI_assert(0 <= face_index && face_index < mesh->faces_num);
  const IndexRange orig_face = mesh->faces()[face_index];
  /* The face can't be original if the number of triangles isn't equal
   * to the original face size minus 2. */
  int orig_face_size = orig_face.size();
  if (orig_face_size != group.size() + 2) {
    return 0;
  }
  Span<int> orig_face_verts = mesh->corner_verts().slice(mesh->faces()[face_index]);
  /* edge_value[i] will be 1 if that edge is identical to an output edge,
   * and -1 if it is the reverse of an output edge. */
  Array<uchar, 20> edge_value(orig_face_size, 0);
  int stride = mgl.numProp;
  for (const int t : group) {
    /* face_vert_index[i] will hold the position in input face face_index
     * where the ith vertex of triangle t is (assuming that no position
     * is exactly repeated in an output face). -1 if there is no such. */
    Array<int, 3> face_vert_index(3);
    for (const int i : IndexRange(3)) {
      int v = mgl.triVerts[3 * t + i];
      int prop_offset = v * stride;
      float3 pos(mgl.vertProperties[prop_offset],
                 mgl.vertProperties[prop_offset + 1],
                 mgl.vertProperties[prop_offset + 2]);
      auto it = std::find_if(orig_face_verts.begin(), orig_face_verts.end(), [&](int orig_v) {
        return pos == mesh->vert_positions()[orig_v];
      });
      face_vert_index[i] = it == orig_face_verts.end() ?
                               -1 :
                               std::distance(orig_face_verts.begin(), it);
    }
    /* Now we can tell which original edges are covered by t. */
    for (const int i : IndexRange(3)) {
      const int a = face_vert_index[i];
      const int b = face_vert_index[(i + 1) % 3];
      if (a != -1 && b != -1) {
        if ((a + 1) % orig_face_size == b) {
          edge_value[a] = 1;
        }
        else if ((b + 1) % orig_face_size == a) {
          edge_value[b] = -1;
        }
      }
    }
  }
  if (std::all_of(edge_value.begin(), edge_value.end(), [](int x) { return x == 1; })) {
    return 1;
  }
  else if (std::all_of(edge_value.begin(), edge_value.end(), [](int x) { return x == -1; })) {
    return 2;
  }
  return 0;
}
#endif

static OutFace make_out_face(const MeshGL &mgl, int tri_index, int orig_face)
{
  OutFace ans;
  ans.verts = Vector<int, inline_outface_size>(3);
  const int k = 3 * tri_index;
  ans.verts[0] = mgl.triVerts[k];
  ans.verts[1] = mgl.triVerts[k + 1];
  ans.verts[2] = mgl.triVerts[k + 2];
  ans.face_id = orig_face;
  return ans;
}

/* For face merging, there is this indexing spaces:
 * "group edge" index:  linearized indices of edges in the
 * triangles in the group.
 * A SharedEdge has two such indices, with the assertion that
 * they are the have the same vertices (but in opposite order).
 */
struct SharedEdge {
  /* First shared edge ("group edge" indexing). */
  int e1;
  /* Second shared edge. */
  int e2;
  /* First vertex for e1 (second for e2). */
  int v1;
  /* Second vertex for e1 (first for e2). */
  int v2;

  SharedEdge(int e1, int e2, int v1, int v2) : e1(e1), e2(e2), v1(v1), v2(v2) {}
};

/* Canonical SharedEdge has v1 < v2. */
static inline SharedEdge canon_shared_edge(int e1, int e2, int v1, int v2)
{
  if (v1 < v2) {
    return SharedEdge(e1, e2, v1, v2);
  }
  return SharedEdge(e2, e1, v2, v1);
}

/* A pair of vertices in MeshGL output space. */
struct VertPair {
  int v1;
  int v2;

  VertPair(int v1, int v2) : v1(v1), v2(v2) {}

  uint64_t hash() const
  {
    return this->v1 ^ this->v2;
  }
};

static bool operator==(const VertPair &a, const VertPair &b)
{
  return a.v1 == b.v1 && a.v2 == b.v2;
}

struct FaceNode {
  Vector<SharedEdge, 4> shared_edges;
  int node_id;
};

static Vector<SharedEdge> get_shared_edges(Span<OutFace> faces)
{
  Vector<SharedEdge> ans;
  /* Map from two verts making an edge to where that edge appears
   * in list of group edges. */
  Map<VertPair, int> edge_verts_to_tri;
  for (const int face_index : faces.index_range()) {
    const OutFace &f = faces[face_index];
    for (const int i : IndexRange(3)) {
      int v1 = f.verts[i];
      int v2 = f.verts[(i + 1) % 3];
      int this_e = face_index * 3 + i;
      edge_verts_to_tri.add_new(VertPair(v1, v2), this_e);
      int other_e = edge_verts_to_tri.lookup_default(VertPair(v2, v1), -1);
      if (other_e != -1) {
        std::cout << "found shared pair between verts " << v1 << " and " << v2 << "\n";
        ans.append(canon_shared_edge(this_e, other_e, v1, v2));
      }
    }
  }
  return ans;
}

static void merge_out_faces(Vector<OutFace> &faces, Span<int> group, const MeshGL &mgl)
{
  constexpr int dbg_level = 1;
  if (group.size() <= 1) {
    return;
  }
  if (dbg_level > 0) {
    std::cout << "\nmerge_out_faces for faceid " << faces[0].face_id << "\n";
    for (const int i : faces.index_range()) {
      const OutFace &f = faces[i];
      dump_span(f.verts.as_span(), std::to_string(i));
    }
  }
  Vector<SharedEdge> shared_edges = get_shared_edges(faces);
  if (dbg_level > 0) {
    std::cout << "shared edges:\n";
    for (const SharedEdge &se : shared_edges) {
      std::cout << "(e" << se.e1 << ",e" << se.e2
      << ";v" << se.v1 << ",v" << se.v2 << ")";
    }
    std::cout << "\n";
    //dump_span(shared_edges.as_span(), "shared edges");
  }
}

static MeshAssembly assemble_mesh_from_meshgl(const MeshGL &mgl,
                                              Span<const Mesh *> meshes,
                                              const MeshOffsets &mesh_offsets)
{
  constexpr int dbg_level = 2;
  if (dbg_level > 0) {
    std::cout << "assemble_mesh_from_meshgl\n";
  }
  MeshAssembly ma;
  ma.vertpos = Span<float>(&*mgl.vertProperties.begin(), mgl.vertProperties.size());
  ma.vertpos_stride = mgl.numProp;
  ma.num_input_verts = mesh_offsets.vert_offsets.last();
  ma.num_output_verts = ma.vertpos.size() / ma.vertpos_stride;
  const int input_faces_num = mesh_offsets.face_offsets.last();
  fill_vertex_map(ma, mgl, meshes, mesh_offsets);
  /* For each offset input mesh face, what mgl triangles have it as id? */
  Array<Vector<int, face_group_inline>> face_groups = get_face_groups(mgl, input_faces_num);
  if (dbg_level > 1) {
    std::cout << "groups:\n";
    for (const int i : face_groups.index_range()) {
      std::cout << "orig (offset) face " << i << ": ";
      dump_span(face_groups[i].as_span(), "");
    }
  }
  for (const int gid : face_groups.index_range()) {
    Span<int> group = face_groups[gid].as_span();
    Vector<OutFace> group_faces(group.size());
    for (const int i : group_faces.index_range()) {
      int tri_index = group[i];
      group_faces[i] = make_out_face(mgl, tri_index, gid);
    }
    merge_out_faces(group_faces, group, mgl);
    ma.new_faces.extend(group_faces.as_span());
  }
  return ma;
}

/* Convert the meshgl that is the result of the boolean back into a
 * Blender Mesh.
 * Note: the caller of mesh_boolean_manifold will fix the returned
 * mesh's mat[] array to hold materials approprite for the material_remaps.
 */
static Mesh *meshgl_to_mesh(const MeshGL &mgl,
                            Span<const Mesh *> meshes,
                            Span<Array<short>> material_remaps,
                            const MeshOffsets &mesh_offsets)
{
  constexpr int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "\nMESHGL_TO_MESH\n";
    dump_meshgl(mgl, "meshgl_to_mesh argument");
    std::cout << "material_remaps:\n";
    for (int i : material_remaps.index_range()) {
      dump_span(material_remaps[i].as_span(), std::to_string(i));
    }
  }
  timeit::ScopedTimer timer("meshgl to mesh");
  /* TODO: dissolve unnecessary triangle faces. */
  MeshAssembly ma = assemble_mesh_from_meshgl(mgl, meshes, mesh_offsets);
  int tot_positions = mgl.NumVert();
  int tot_faces = mgl.NumTri();
  int tot_corners = tot_faces * 3;
  if (mgl.mergeFromVert.size() > 0) {
    /* TODO: handle vertex merging */
    std::cout << "IMPLEMENT ME: handle vertex merging\n";
  }
  /* We will use Blender's parallelized function to calculate edges later. */
  Mesh *mesh = BKE_mesh_new_nomain_from_template(
      meshes[0], tot_positions, 0, tot_faces, tot_corners);
  int num_props = mgl.numProp;
  MutableSpan<float3> positions = mesh->vert_positions_for_write();
  int grain_size = 100000;
  threading::parallel_for(IndexRange(tot_positions), grain_size, [&](const IndexRange range) {
    for (const int i : range) {
      int offset = num_props * i;
      float3 pos(mgl.vertProperties[offset],
                 mgl.vertProperties[offset + 1],
                 mgl.vertProperties[offset + 2]);
      positions[i] = pos;
    }
  });
  GAttributeReadWriteSpans face_attrs(meshes, mesh, bke::AttrDomain::Face);
  int material_span_index = face_attrs.find_attr_index("material_index");
#if 0
  /* TODO: following is very specific to all-triangle output, */
  MutableSpan<int> face_offsets = mesh->face_offsets_for_write();
  MutableSpan<int> corner_verts = mesh->corner_verts_for_write();
  grain_size = 50000;
  threading::parallel_for(IndexRange(tot_faces), grain_size, [&](const IndexRange range) {
    for (const int face_index : range) {
      int corner_index = 3 * face_index;
      face_offsets[face_index] = corner_index;
      corner_verts[corner_index] = mgl.triVerts[corner_index];
      corner_verts[corner_index + 1] = mgl.triVerts[corner_index + 1];
      corner_verts[corner_index + 2] = mgl.triVerts[corner_index + 2];
      std::pair<int, int> m_and_f = mesh_and_face(face_index, mgl, mesh_offsets.face_offsets);
      int input_mesh_index = m_and_f.first;
      int input_face_index = m_and_f.second;
      copy_face_attrs(face_attrs,
                      input_mesh_index,
                      input_face_index,
                      face_index,
                      material_span_index,
                      material_remaps);
    }
  });
  face_offsets[tot_faces] = 3 * tot_faces;
#endif
  // bke::mesh_smooth_set(*mesh, false);
  {
    timeit::ScopedTimer("calculating edges");
    bke::mesh_calc_edges(*mesh, false, false);
  }
  if (dbg_level > 0) {
    dump_mesh(mesh, "output mesh");
  }
  if (dbg_level > 1) {
    BKE_mesh_validate(mesh, true, true);
  }
  return mesh;
}

Mesh *mesh_boolean_manifold(Span<const Mesh *> meshes,
                            Span<float4x4> transforms,
                            const float4x4 &target_transform,
                            Span<Array<short>> material_remaps,
                            BooleanOpParameters op_params)
{
  constexpr int dbg_level = 1;
  if (dbg_level > 0) {
    std::cout << "\nMESH_BOOLEAN_MANIFOLD with " << meshes.size() << " args\n";
  }
  try {
    timeit::ScopedTimer timer("manifold boolean");
    const int num_meshes = meshes.size();
    std::vector<Manifold> manifolds(num_meshes);
    Array<bool> manifold_ok(num_meshes);
    bool no_transforms = math::is_identity(target_transform);
    no_transforms &= std::all_of(transforms.begin(), transforms.end(), [](const float4x4 &t) {
      return math::is_identity(t);
    });
    if (!no_transforms) {
      std::cout << "IMPLEMENT ME: mesh_boolean_manifold with transforms\n";
      return nullptr;
    }
    MeshOffsets mesh_offsets(meshes);
    if (dbg_level > 0) {
      for (const int i : IndexRange(num_meshes)) {
        manifolds[i] = manifold_from_mesh_via_meshgl(meshes[i], i, mesh_offsets.face_offsets[i]);
        manifold_ok[i] = manifolds[i].Status() == Manifold::Error::NoError;
      }
    }
    else {
      threading::parallel_for_each(IndexRange(num_meshes), [&](int i) {
        manifolds[i] = manifold_from_mesh_via_meshgl(meshes[i], i, mesh_offsets.face_offsets[i]);
        manifold_ok[i] = manifolds[i].Status() == Manifold::Error::NoError;
      });
    }
    if (std::any_of(manifold_ok.begin(), manifold_ok.end(), [](bool v) { return !v; })) {
      std::cout << "Cannot convert Mesh to Manifold, so manifold solver fails\n";
      return nullptr;
    }
    Operation op = op_params.boolean_mode;
    manifold::OpType mop = op == Operation::Intersect ?
                               manifold::OpType::Intersect :
                               (op == Operation::Union ? manifold::OpType::Add :
                                                         manifold::OpType::Subtract);
    MeshGL meshgl_result;
    {
      timeit::ScopedTimer("doing boolean and getting meshgl result");
      Manifold man_result = Manifold::BatchBoolean(manifolds, mop);
      meshgl_result = man_result.GetMeshGL();
      if (dbg_level > 0) {
        std::cout << "boolean result has " << meshgl_result.NumTri() << " tris\n";
        dump_meshgl(meshgl_result, "boolean result meshgl");
      }
    }
    Mesh *mesh_result = meshgl_to_mesh(meshgl_result, meshes, material_remaps, mesh_offsets);
    /* TODO: if (unlikely) target_transform is not identity, trasform the mesh. */
    UNUSED_VARS(target_transform);
    return mesh_result;
  }
  catch (const std::exception &e) {
    std::cout << "mesh_boolean_manifold: exception: " << e.what() << "\n";
  }
  catch (...) {
    std::cout << "mesh_boolean_manifold: unknown exception\n";
  }
  return nullptr;
}

}  // namespace blender::geometry::boolean
