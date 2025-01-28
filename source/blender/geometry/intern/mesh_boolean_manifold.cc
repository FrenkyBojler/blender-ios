/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <iostream>

#include "BLI_alloca.h"
#include "BLI_array.hh"
#include "BLI_hash.hh"
#include "BLI_map.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
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
#include "BKE_attribute_math.hh"
#include "BKE_customdata.hh"
#include "BKE_geometry_set.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.hh"

#include "GEO_join_geometries.hh"

#include "mesh_boolean_manifold.hh"

#include "manifold.h"

using manifold::Manifold;
using manifold::MeshGL;

/* Using this for now for debug printing of materials. Can remove later. */
#include "DNA_material_types.h"

namespace blender::geometry::boolean {

/* Some debug output functions. */

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

template<typename T>
static void dump_span_with_stride(Span<T> span, int stride, const std::string &name)
{
  std::cout << name << ":";
  for (const int i : span.index_range()) {
    if (i % 10 == 0) {
      std::cout << "\n[" << i << "] ";
    }
    std::cout << span[i] << " ";
    if (stride > 1 && (i % stride) == stride - 1) {
      std::cout << "/ ";
    }
  }
  std::cout << "\n";
}

template<typename T>
static void dump_vector(std::vector<T> vec, int stride, const std::string &name)
{
  std::cout << name << ":";
  for (int i = 0; i < vec.size(); i++) {
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

static const char *domain_names[] = {
    "point", "edge", "face", "corner", "curve", "instance", "layer"};

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
      return;
    }
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
  });
  std::cout << "materials:\n";
  for (int i = 0; i < mesh->totcol; i++) {
    std::cout << "[" << i << "]: " << (mesh->mat[i] ? mesh->mat[i]->id.name + 2 : "none") << "\n";
  }
}

/* Holds cumulative offsets for the given elements of a number
 * of concatenated Meshes. The sizes are one greater than the
 * number of meshes, so that the last value of each gives the
 * total number of elements. */
struct MeshOffsets {
  Array<int> vert_start;
  Array<int> face_start;
  Array<int> edge_start;
  Array<int> corner_start;
  OffsetIndices<int> vert_offsets;
  OffsetIndices<int> face_offsets;
  OffsetIndices<int> edge_offsets;
  OffsetIndices<int> corner_offsets;

  MeshOffsets(Span<const Mesh *> meshes);
};

MeshOffsets::MeshOffsets(Span<const Mesh *> meshes)
{
  const int num_meshes = meshes.size();
  this->vert_start.reinitialize(num_meshes + 1);
  this->face_start.reinitialize(num_meshes + 1);
  this->edge_start.reinitialize(num_meshes + 1);
  this->corner_start.reinitialize(num_meshes + 1);
  for (int i = 0; i <= num_meshes; i++) {
    this->vert_start[i] = (i == 0) ? 0 : this->vert_start[i - 1] + meshes[i - 1]->verts_num;
    this->face_start[i] = (i == 0) ? 0 : this->face_start[i - 1] + meshes[i - 1]->faces_num;
    this->edge_start[i] = (i == 0) ? 0 : this->edge_start[i - 1] + meshes[i - 1]->edges_num;
    this->corner_start[i] = (i == 0) ? 0 : this->corner_start[i - 1] + meshes[i - 1]->corners_num;
  }
  this->vert_offsets = OffsetIndices<int>(this->vert_start);
  this->face_offsets = OffsetIndices<int>(this->face_start);
  this->edge_offsets = OffsetIndices<int>(this->edge_start);
  this->corner_offsets = OffsetIndices<int>(this->corner_start);
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


/* Return the mesh index and face index within that mesh corresponding to offset-face,
 * a face index in the concatenated index space of all mesh faces. */
static std::pair<int, int> offset_face_to_mesh_face(int offset_face, Span<int> mesh_face_start)
{
  for (int i = 1; i < mesh_face_start.size(); i++) {
    if (offset_face < mesh_face_start[i]) {
      return {i - 1, offset_face - mesh_face_start[i - 1]};
    }
  }
  return {mesh_face_start.size() - 1, offset_face - mesh_face_start.last()};
}

/* Create and return the Manifold library's internal #Manifold class instance
 * to represent the subset \a joined_mesh which came from the input
 * mesh with index \a mesh_index.  We can tell which elements are in the
 * subset using \a mesh_offsets.
 * This is done using Manifold's #MeshGL struct, which has linearized
 * vector of x, y, z coordinates in its #vertProperties,
 * where the index divided by 3 is the input "vertex index".
 * It also has a linearized list of the triples of vertex indices that
 * give the triangulation of the mesh faces, where the index divided
 * by 3 is the "triangle index".
 * The #faceID vector is indexed by triangle index, and gives the
 * original mesh face index in the joined mesh..
 * It also sets up #runIndex and #runOriginalID so that when we
 * access OriginalId's in the output, they will be \a mesh_index.
 */
static void get_manifold(Manifold &manifold, const Mesh *joined_mesh, int mesh_index, const MeshOffsets &mesh_offsets)
{
  constexpr int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "get_manifold for mesh " << mesh_index << "\n";
  }
  MeshGL meshgl;
  constexpr int props_num = 3;
  meshgl.numProp = props_num;
  const int verts_num = mesh_offsets.vert_offsets[mesh_index].size();
  const int vert_start = mesh_offsets.vert_start[mesh_index];
  meshgl.vertProperties.resize(verts_num * props_num);
  Span<float3> vpos = joined_mesh->vert_positions();
  const int grain_size = 20000;
  threading::parallel_for(IndexRange(verts_num), grain_size, [&](const IndexRange range) {
    for (const int i : range) {
      int offset_i = i + vert_start;
      const float3 &pos = vpos[offset_i];
      meshgl.vertProperties[props_num * i] = pos[0];
      meshgl.vertProperties[props_num * i + 1] = pos[1];
      meshgl.vertProperties[props_num * i + 2] = pos[2];
    }
  });
  /* Calling joined_mesh->corner_tris() may cause triangulation to happen,
   * to populate a triangulation cache for the mesh. */
  Span<int3> corner_tris = joined_mesh->corner_tris();
  Span<int> corner_verts = joined_mesh->corner_verts();
  Span<int> corner_tri_faces = joined_mesh->corner_tri_faces();
  const int tris_start = poly_to_tri_count(mesh_offsets.face_start[mesh_index], mesh_offsets.corner_start[mesh_index]);
  const int tris_end = poly_to_tri_count(mesh_offsets.face_start[mesh_index + 1], mesh_offsets.corner_start[mesh_index + 1]);
  const int tris_num = tris_end - tris_start;
  meshgl.triVerts.resize(3 * tris_num);
  meshgl.faceID.resize(tris_num);
  threading::parallel_for(IndexRange(tris_start, tris_num), grain_size, [&](const IndexRange range) {
    for (const int i : range) {
      const int3 &ctri = corner_tris[i];
      const int meshgl_i = i - tris_start;
      const int tv_start = 3 * meshgl_i;
      meshgl.triVerts[tv_start] = corner_verts[ctri[0]] - vert_start;
      meshgl.triVerts[tv_start + 1] = corner_verts[ctri[1]] - vert_start;
      meshgl.triVerts[tv_start + 2] = corner_verts[ctri[2]] - vert_start;
      meshgl.faceID[meshgl_i] = corner_tri_faces[i];
    }
  });
  meshgl.runIndex.resize(2);
  meshgl.runOriginalID.resize(1);
  meshgl.runIndex[0] = 0;
  meshgl.runIndex[1] = 3 * tris_num;
  meshgl.runOriginalID[0] = mesh_index;
  if (dbg_level > 0) {
    dump_meshgl(meshgl, "converted result for mesh " + std::to_string(mesh_index));
  }
  {
    timeit::ScopedTimer mtimer("manifold constructor from meshgl");
    manifold = Manifold(meshgl);
  }
}

static void get_manifolds(MutableSpan<Manifold> manifolds, const Mesh *joined_mesh, const MeshOffsets &mesh_offsets)
{
  constexpr int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "GET_MANIFOLDS\n";
    dump_mesh(joined_mesh, "joined_mesh");
    std::cout << "\nMesh Offset (starts):\n";
    dump_span(mesh_offsets.vert_start.as_span(), "vert");
    dump_span(mesh_offsets.face_start.as_span(), "face");
    dump_span(mesh_offsets.edge_start.as_span(), "edge");
    dump_span(mesh_offsets.corner_start.as_span(), "corner");
  }
  const int meshes_num = manifolds.size();
  if (dbg_level > 0) {
    for (const int mesh_index : IndexRange(meshes_num)) {
      get_manifold(manifolds[mesh_index], joined_mesh, mesh_index, mesh_offsets);
    }
  }
  else {
    threading::parallel_for_each(IndexRange(meshes_num), [&](int mesh_index) {
      get_manifold(manifolds[mesh_index], joined_mesh, mesh_index, mesh_offsets);
    });
  }
}

/* Holds data needed to readn and write attributes of a number of input #Meshes
 * and write it to a destination #Mesh. */
class GAttributeReadWriteSpans {
 public:
  /* The underlying output Mesh. */
  Mesh *output_mesh;
  /* The corresponding write attribute accessor. */
  bke::MutableAttributeAccessor output_accessor;
  /* The underlying input Meshes. */
  Span<const Mesh *> input_meshes;
  /* Read attribute accessor for each input Mesh. */
  Vector<bke::AttributeAccessor> input_accessors;
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

  int add_attribute(StringRefNull name, bke::AttrDomain domain, eCustomDataType data_type);

  int find_attr_index(const char *name) const;
};

int GAttributeReadWriteSpans::add_attribute(StringRefNull name,
                                            bke::AttrDomain domain,
                                            eCustomDataType data_type)
{
  this->attrs.append(name);
  this->data_types.append(data_type);
  this->dest_writers.append(
      this->output_accessor.lookup_or_add_for_write_only_span(name, domain, data_type));
  this->dest.append(this->dest_writers.last().span);
  for (int i : this->input_meshes.index_range()) {
    this->sources[i].append(*this->input_accessors[i].lookup_or_default(name, domain, data_type));
  }
  return this->dest_writers.size() - 1;
}

/* Construct the #GAttributeReadWriteSpans to read from attribuytes of
 * \a input_meshes and write to the attributes of \a output_mesh.
 * Restrict attribtes to those of the given \a domain. */
GAttributeReadWriteSpans::GAttributeReadWriteSpans(Span<const Mesh *> input_meshes,
                                                   Mesh *output_mesh,
                                                   bke::AttrDomain domain)
    : output_mesh(output_mesh),
      output_accessor(output_mesh->attributes_for_write()),
      input_meshes(input_meshes)
{
  const int num_mesh = input_meshes.size();
  this->sources.reinitialize(num_mesh);
  for (int i : IndexRange(num_mesh)) {
    this->input_accessors.append(input_meshes[i]->attributes());
  }
  this->output_accessor.foreach_attribute([&](const bke::AttributeIter &iter) {
    if (iter.domain != domain) {
      return;
    }
    this->add_attribute(iter.name, iter.domain, iter.data_type);
    return;
  });
}

/* Destruct a #GAttributeReadWriteSpans : finsish off the writers. */
GAttributeReadWriteSpans::~GAttributeReadWriteSpans()
{
  for (bke::GSpanAttributeWriter &w : dest_writers) {
    w.finish();
  }
}

/* Find the attribute index of the given named attribute in the #GAttributeReadWriteSpans. */
int GAttributeReadWriteSpans::find_attr_index(const char *name) const
{
  for (int i : this->attrs.index_range()) {
    if (this->attrs[i] == name) {
      return i;
    }
  }
  return -1;
}

/* Class to hold the attribute names for attributes we need on each of the domains.
 * We'll omit the attributes "position", ".edge_verts", ".corner_vert", ".corner_edge",
 * which are all used for structure that we set directly in the mesh.
 * This are identfied by the function #BKE_mesh_attribute_required.
 */
class NeededAttributes {
 public:
  struct Spec {
    StringRefNull name;
    bke::AttrDomain domain;
    eCustomDataType data_type;

    Spec(bke::AttributeIter iter) : name(iter.name), domain(iter.domain), data_type(iter.data_type)
    {
    }

    Spec(StringRefNull name, bke::AttrDomain domain, eCustomDataType type)
        : name(name), domain(domain), data_type(type)
    {
    }
  };
  Map<StringRefNull, Spec> attr_map;

  NeededAttributes(Span<const Mesh *> meshes, bool need_material_index);

  int num_attrs_for_domain(bke::AttrDomain domain) const;
};

/* Get the union of the needed attributes from all the meshes,
 * in a deterministic order, and omitting the structure attributes.
 * If \a need_material_index is true, we need a "material_index" face attribute.
 */
NeededAttributes::NeededAttributes(Span<const Mesh *> meshes, bool need_material_index)
{
  for (const Mesh *mesh : meshes) {
    bke::AttributeAccessor attrs = mesh->attributes();
    attrs.foreach_attribute([&](const bke::AttributeIter &iter) {
      if (BKE_mesh_attribute_required(iter.name.c_str())) {
        return;
      }
      this->attr_map.add(iter.name, Spec(iter));
    });
  }
  if (need_material_index) {
    if (!this->attr_map.lookup_try("material_index")) {
      this->attr_map.add(
          "material_index",
          NeededAttributes::Spec("material_index", bke::AttrDomain::Face, CD_PROP_INT32));
    }
  }
}

/* Return the number of attributes in the #NeededAttributes that are for \a domain. */
int NeededAttributes::num_attrs_for_domain(bke::AttrDomain domain) const
{
  int sum = 0;
  this->attr_map.foreach_item([&](const StringRefNull &, const NeededAttributes::Spec &spec) {
    if (spec.domain == domain) {
      sum++;
    }
  });
  return sum;
}

constexpr int inline_outface_size = 8;

struct OutFace {
  /* Vertex ids in meshgl indexing space. */
  Vector<int, inline_outface_size> verts;
  /* The faceID input to manifold, i.e. original face id in combined input mesh indexing space.
   */
  int face_id;

  /* Find the first index (should be only one) of verts that contains v, else -1. */
  int find_vert_index(int v) const
  {
    return verts.first_index_of_try(v);
  }
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
  /* New faces to output. */
  Vector<OutFace> new_faces;
};

/* Arrays to find, for each index of a given type in the output mesh,
 * what is the corresponding index of a representative element in the joined mesh.
 * if there is no representative, a -1 is used.
 * These are created lazily - if their current length is zero, then need to be
 * created. */
class OutToInMaps {
public:
  Array<int> vertex_map;
  Array<int> face_map;
  Array<int> edge_map;
  Array<int> corner_map;

  OutToInMaps(const MeshAssembly *ma, const Mesh *jm, const Mesh *om) : mesh_assembly_(ma), joined_mesh_(jm), output_mesh_(om)
  {
  }

  void ensure_vertex_map();
  void ensure_face_map();
  void ensure_edge_map();
  void ensure_corner_map();

private:
  const MeshAssembly *mesh_assembly_;
  const Mesh *joined_mesh_;
  const Mesh *output_mesh_;
};

void OutToInMaps::ensure_face_map()
{
  if (this->face_map.size() > 0) {
    return;
  }
  /* The MeshAssembly's new_faces should map one to one with output faces. */
  timeit::ScopedTimer timer("filling face map");
  this->face_map.reinitialize(output_mesh_->faces_num);
  BLI_assert(mesh_assembly_->new_faces.size() == this->face_map.size());
  constexpr int grain_size = 50000;
  threading::parallel_for(mesh_assembly_->new_faces.index_range(), grain_size, [&](const IndexRange range) {
    for (const int i : range) {
      this->face_map[i] = mesh_assembly_->new_faces[i].face_id;
    }
  });
}

void OutToInMaps::ensure_vertex_map()
{
  if (this->vertex_map.size() > 0) {
    return;
  }
  /* There may be better ways, but for now we discover the output to input
   * vertex mapping by going through the output faces, and for each, looking
   * through the vertices of the corresponding input face for matches.
   */
  this->ensure_face_map();
  timeit::ScopedTimer timer("filling vertex map");
  this->vertex_map = Array<int>(output_mesh_->verts_num, -1);
  /* To parallelize this, need to deal with the fact that this will
   * have different threads wanting to write vertex_map, and also want
   * determinism of which one wins if there is more than one possibility.
   */
  OffsetIndices<int> in_faces = joined_mesh_->faces();
  OffsetIndices<int> out_faces = output_mesh_->faces();
  Span<int> in_corner_verts = joined_mesh_->corner_verts();
  Span<int> out_corner_verts = output_mesh_->corner_verts();
  Span<float3> out_vert_positions = output_mesh_->vert_positions();
  Span<float3> in_vert_positions = joined_mesh_->vert_positions();
  for (const int out_face_index : IndexRange(output_mesh_->faces_num)) {
    const int in_face_index = this->face_map[out_face_index];
    const IndexRange in_face = in_faces[in_face_index];
    const IndexRange out_face = out_faces[out_face_index];
    Span<int> in_face_verts = in_corner_verts.slice(in_face);
    for (const int out_v : out_corner_verts.slice(out_face)) {
      if (this->vertex_map[out_v] != -1) {
        continue;
      }
      float3 out_pos = out_vert_positions[out_v];
      auto it = std::find_if(in_face_verts.begin(), in_face_verts.end(), [&](int in_v) {
        return out_pos == in_vert_positions[in_v];
      });
      if (it != in_face_verts.end()) {
        int in_v = in_face_verts[std::distance(in_face_verts.begin(), it)];
        this->vertex_map[out_v] = in_v;
      }
    }
  }
}

void OutToInMaps::ensure_corner_map()
{
  if (this->corner_map.size() > 0) {
    return;
  }
  /* There may be better ways, but for now we discover the output to input
   * corner mapping by going through the output faces, and for each, looking
   * through the corners of the corresponding input face for matches of the
   * vertex involved.
   */
  this->ensure_face_map();
  this->ensure_vertex_map();
  timeit::ScopedTimer timer("filling corner map");
  this->corner_map = Array<int>(output_mesh_->corners_num, -1);
  OffsetIndices<int> in_faces = joined_mesh_->faces();
  OffsetIndices<int> out_faces = output_mesh_->faces();
  Span<int> in_corner_verts = joined_mesh_->corner_verts();
  Span<int> out_corner_verts = output_mesh_->corner_verts();
  constexpr int grain_size = 10000;
  threading::parallel_for(IndexRange(output_mesh_->faces_num), grain_size, [&](const IndexRange range) {
    for (const int out_face_index : range) {
      const int in_face_index = this->face_map[out_face_index];
      const IndexRange in_face = in_faces[in_face_index];
      for (const int out_c : out_faces[out_face_index]) {
        BLI_assert(this->corner_map[out_c] == -1);
        const int out_v = out_corner_verts[out_c];
        const int in_v = this->vertex_map[out_v];
        if (in_v == -1) {
          continue;
        }
        const int in_face_i = in_corner_verts.slice(in_face).first_index_try(in_v);
        if (in_face_i != -1) {
          const int in_c = in_face[in_face_i];
          this->corner_map[out_c] = in_c;
        }
      }
    }
  });
}

static bool same_dir(const float3 &p1, const float3 &p2, const float3 &q1, const float3 &q2)
{
  float3 p = p1 - p2;
  float3 q = q1 - q2;
  float pq = math::length(p) * math::length(q);
  if (pq == 0.0f) {
    return true;
  }
  float abs_cos_pq = math::abs(math::dot(p, q) / pq);
  return (math::abs(abs_cos_pq - 1.0f) <= 1e-5f);
}

void OutToInMaps::ensure_edge_map()
{
  constexpr int dbg_level = 0;
  if (this->edge_map.size() > 0) {
    return;
  }
  if (dbg_level > 0) {
    std::cout << "\nensure_edge_map\n";
    if (dbg_level > 1) {
      dump_mesh(joined_mesh_, "joined_mesh");
      dump_mesh(output_mesh_, "output_mesh");
    }
  }
  /* There may be better ways to get the edge map, but for now
   * we go through the output faces, and for each edge, see if
   * there is an input edge in the corresponding input face that
   * has one or the other end in common, and if only one end is
   * in common, is in approximately the same direction.
   * We can assume that the output and input are manifold.
   * So if there is an edge that starts or ends at a corner in
   * the corresponding input face, then we need only look for the
   * "starts at" case, because if it is "ends at" in this face, it
   * should be "starts at" in the matching face.
   */
  this->ensure_face_map();
  this->ensure_vertex_map();
  this->ensure_corner_map();
  /* To parallelize this, would need a way to figure out that
   * this is the "canonical" edge representative so that only
   * one thread tries to write this. Or could use atomic operations.
   */
  timeit::ScopedTimer timer("filling edge map");
  this->edge_map = Array<int>(output_mesh_->edges_num, -1);
  Span<int> out_corner_edges = output_mesh_->corner_edges();
  Span<int> out_corner_verts = output_mesh_->corner_verts();
  Span<int2> out_edges = output_mesh_->edges();
  Span<float3> out_positions = output_mesh_->vert_positions();
  Span<int> in_corner_edges = joined_mesh_->corner_edges();
  Span<int> in_corner_verts = joined_mesh_->corner_verts();
  Span<int2> in_edges = joined_mesh_->edges();
  Span<float3> in_positions = joined_mesh_->vert_positions();
  OffsetIndices<int> in_faces = joined_mesh_->faces();
  OffsetIndices<int> out_faces = output_mesh_->faces();
  Array<bool> done_edge(output_mesh_->edges_num, false);
  for (const int out_face_index : IndexRange(output_mesh_->faces_num)) {
    const int in_face_index = this->face_map[out_face_index];
    const IndexRange in_face = in_faces[in_face_index];
    if (dbg_level > 0) {
      std::cout << "process out_face = " << out_face_index << ", in_face = " << in_face_index << "\n";
    }
    for (const int out_c : out_faces[out_face_index]) {
      const int in_c = this->corner_map[out_c];
      if (dbg_level > 0) {
        std::cout << "  out_c = " << out_c << ", in_c = " << in_c << "\n";
      }
      if (in_c == -1) {
        /* No possible "starts at" match here. */
        continue;
      }
      const int out_e = out_corner_edges[out_c];
      if (dbg_level > 0) {
        std::cout << "  out_e = " << out_e << ", done = " << done_edge[out_e] << "\n";
      }
      if (done_edge[out_e]) {
        continue;
      }
      const int out_v = out_corner_verts[out_c];
      const int in_e = in_corner_edges[in_c];
      const int in_v = in_corner_verts[in_c];
      /* Because of corner mapping, the output vertex should map to the input one. */
      BLI_assert(this->vertex_map[out_v] == in_v);
      int2 out_e_v = out_edges[out_e];
      if (out_e_v[0] != out_v) {
        out_e_v = {out_e_v[1], out_e_v[0]};
      }
      int2 in_e_v = in_edges[in_e];
      if (in_e_v[0] != in_v) {
        in_e_v = {in_e_v[1], in_e_v[0]};
      }
      if (dbg_level > 0) {
        std::cout << "  out_v = " << out_v << ", in_e = " << in_e << ", in_v = " << in_v << "\n";
        std::cout << "  out_e_v = " << out_e_v << ", in_e_v = " << in_e_v << "\n";
        std::cout << "  vertex_map(out_e_v) = " << int2(this->vertex_map[out_e_v[0]], this->vertex_map[out_e_v[1]]) << "\n";
      }
      /* Here out_e_v should hold the output vertices in out_e, with the first
       * one being out_v, the vertex at corner out_c.
       * Similarly for in_e_v, with the first one being in_v.
       */
      BLI_assert(this->vertex_map[out_e_v[0]] == in_e_v[0]);
      int edge_rep = -1;
      if (this->vertex_map[out_e_v[1]] == in_e_v[1]) {
        /* Here both ends of the edges match. */
        if (dbg_level > 0) {
          std::cout << "  case 1, edge_rep = in_e = " << in_e << "\n";
        }
        edge_rep = in_e;
      }
      else if (this->vertex_map[out_e_v[1]] == -1) {
        /* Here the "ends at" vertex of the output edge is a new vertex.
         * Does the edge at least go in the same direction as in_e?
         */
        if (same_dir(out_positions[out_e_v[0]],
                     out_positions[out_e_v[1]],
                     in_positions[in_e_v[0]],
                     in_positions[in_e_v[1]])) {
          if (dbg_level > 0) {
            std::cout << "  case 2, edge_rep = in_e = " << in_e << "\n";
          }
          edge_rep = in_e;
        }
      }
      /* It is possible that the output face and corresponding
       * input face have opposite windings. So do all of the previous
       * again with the previous edge of input face but same edge of
       * output face.
       */
      if (edge_rep == -1) {
        const int in_c_prev = bke::mesh::face_corner_prev(in_face, in_c);
        const int in_e_prev = in_corner_edges[in_c_prev];
        const int in_v_prev = in_corner_verts[in_c_prev];
        int2 in_e_v_prev = in_edges[in_e_prev];
        if (in_e_v_prev[0] != in_v_prev) {
          in_e_v_prev = {in_e_v_prev[1], in_e_v_prev[0]};
        }
        if (dbg_level > 0) {
          std::cout << "  in_c_prev = " << in_c_prev << ", in_e_prev = " << in_e_prev
          << ", in_v_prev = " << in_v_prev << "\n";
          std::cout << "  in_e_v_prev = " << in_e_v_prev << "\n";
        }
        if (this->vertex_map[out_e_v[0]] == in_e_v_prev[1]) {
          if (this->vertex_map[out_e_v[1]] == in_e_v_prev[0]) {
            if (dbg_level > 0) {
              std::cout << "  case 3, edge_rep = in_e_prev = " << in_e_prev << "\n";
            }
            edge_rep = in_e_prev;
          }
          else if (this->vertex_map[out_e_v[1]] == -1) {
            if (same_dir(out_positions[out_e_v[0]],
                         out_positions[out_e_v[1]],
                         in_positions[in_e_v_prev[0]],
                         in_positions[in_e_v_prev[1]])) {
              if (dbg_level > 0) {
                std::cout << "  case 4, edge_rep = in_e_prev = " << in_e_prev << "\n";
              }
              edge_rep = in_e_prev;
            }
          }
        }
      }
      if (edge_rep != -1) {
        if (dbg_level > 0) {
          std::cout << "  found: set edge_map[" << out_e << "] = " << edge_rep << "\n";
        }
        this->edge_map[out_e] = edge_rep;
        done_edge[out_e] = true;
      }
    }
  }
}

/* Fill the MeshAssembly's out_to_in_vert_map.
 * Do this by finding, for each output face, which verts of the corresponding
 * input face match.
 */
static void fill_vertex_map(MeshAssembly &ma,
                            const MeshGL &mgl,
                            Span<const Mesh *> meshes,
                            const MeshOffsets &mesh_offsets)
{
  timeit::ScopedTimer timer("fill_vertex_map");
  constexpr int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "fill_vertex_map\n";
  }
  ma.out_to_in_vert_map = Array<int>(ma.num_output_verts, -1);
  const int tris_num = mgl.NumTri();
  const int stride = mgl.numProp;
  for (const int t : IndexRange(tris_num)) {
    const int faceid = mgl.faceID[t];
    auto [mesh_index, face_in_mesh] = offset_face_to_mesh_face(faceid, mesh_offsets.face_start);
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
        ma.out_to_in_vert_map[v] = orig_v + mesh_offsets.vert_start[mesh_index];
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
  timeit::ScopedTimer timer("get_face_groups");
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

/* For face merging, there is this indexing space:
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

  /* Return the indices (in the linearized triangle space of an OutFace group)
   * corresponding to e1 and e2. */
  int2 outface_group_face_indices() const
  {
    return int2(e1 / 3, e2 / 3);
  }
};

/* Canonical SharedEdge has v1 < v2. */
static inline SharedEdge canon_shared_edge(int e1, int e2, int v1, int v2)
{
  if (v1 < v2) {
    return SharedEdge(e1, e2, v1, v2);
  }
  return SharedEdge(e2, e1, v2, v1);
}

/* Given a span of OutFaces, all triangles, find as many SharedEdge's as possible.
 * A SharedEdge is one where it is in two triangles but with the vertices in opposite order.
 * The edge ids are given as indexes into all the edges of \a faces in order.
 */
static Vector<SharedEdge> get_shared_edges(Span<OutFace> faces)
{
  Vector<SharedEdge> ans;
  /* Map from two vert indices making an edge to where that edge appears
   * in list of group edges. */
  Map<int2, int> edge_verts_to_tri;
  for (const int face_index : faces.index_range()) {
    const OutFace &f = faces[face_index];
    for (const int i : IndexRange(3)) {
      int v1 = f.verts[i];
      int v2 = f.verts[(i + 1) % 3];
      int this_e = face_index * 3 + i;
      edge_verts_to_tri.add_new(int2(v1, v2), this_e);
      int other_e = edge_verts_to_tri.lookup_default(int2(v2, v1), -1);
      if (other_e != -1) {
        ans.append(canon_shared_edge(this_e, other_e, v1, v2));
      }
    }
  }
  return ans;
}

/* Return true if the splice of faces \a f1 and \a f2 forms a legal face (no repeated verts).
 * The splice will be between vertices \a v1 and \a v2, which are assumed to not be
 * repeated in the other face (since incoming faces are assumed legal).
 */
static bool is_legal_merge(const OutFace &f1, const OutFace &f2, int v1, int v2)
{
  /* For now, just look for each non-splice-involved vertex of each face to see if
   * it is in the other face.
   * TODO: if the faces are big, sort both together and look for repeats after sorting.
   */
  for (const int v : f1.verts) {
    if (v != v1 && v != v2) {
      if (f2.find_vert_index(v) != -1) {
        return false;
      }
    }
  }
  for (const int v : f2.verts) {
    if (v != v1 && v != v2) {
      if (f1.find_vert_index(v) != -1) {
        return false;
      }
    }
  }
  return true;
}

/* Try merging OutFaces \a f1 and \a f2, which should have a \a se as a shared edge.
 * Assume the shared edge has v1,v2 in CCW order in f1, and in the opposite order in f2.
 * This involves splicing the two faces together and checking that there
 * is no repeated vertex if this is done.
 * If the merge is successful, update f1 to be the merged face and return true,
 * else leave the faces alone and return false.
 */
static bool try_merge_out_face_pair(OutFace &f1, const OutFace &f2, const SharedEdge &se)
{

  constexpr int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "try_merge_out_face_pair\n";
    dump_span(f1.verts.as_span(), "f1");
    dump_span(f2.verts.as_span(), "f2");
    std::cout << "shared edge: "
              << "(e" << se.e1 << ",e" << se.e2 << ";v" << se.v1 << ",v" << se.v2 << ")\n";
  }
  const int f1_len = f1.verts.size();
  const int f2_len = f2.verts.size();
  const int v1 = se.v1;
  const int v2 = se.v2;
  /* Find i1, the index of the earlier of v1 and v2 in f1,
   * and i2, the index of the earlier of v1 and v2 in f2. */
  const int i1 = f1.find_vert_index(v1);
  BLI_assert(i1 != -1);
  const int i1_next = (i1 + 1) % f1_len;
  const int i2 = f2.find_vert_index(v2);
  BLI_assert(i2 != -1);
  const int i2_next = (i2 + 1) % f2_len;
  BLI_assert(f1.verts[i1] == v1 && f1.verts[i1_next] == v2);
  BLI_assert(f2.verts[i2] == v2 && f2.verts[i2_next] == v1);
  const bool can_merge = is_legal_merge(f1, f2, v1, v2);
  if (dbg_level > 0) {
    std::cout << "i1 = " << i1 << ", i2 = " << i2 << ", can_merge = " << can_merge << "\n";
  }
  if (!can_merge) {
    return false;
  }
  /* The merged face is the concatenation of these slices
   * (giving inclusive indices, with implied wrap-around at end of faces):
   * f1 : [0, i1]
   * f2 : [i2_next+1, i2_prev]
   * f1 : [i1_next, f1_len-1]
   */
  const int i2_prev = (i2 + f2_len - 1) % f2_len;
  const int i2_next_next = (i2_next + 1) % f2_len;
  auto f2_start_it = f2.verts.begin() + i2_next_next;
  auto f2_end_it = f2.verts.begin() + i2_prev + 1;
  if (f2_end_it > f2_start_it) {
    f1.verts.insert(i1_next, f2_start_it, f2_end_it);
  }
  else {
    const int n1 = std::distance(f2_start_it, f2.verts.end());
    if (n1 > 0) {
      f1.verts.insert(i1_next, f2_start_it, f2.verts.end());
    }
    if (n1 < f2_len - 2) {
      f1.verts.insert(i1_next + n1, f2.verts.begin(), f2_end_it);
    }
  }
  if (dbg_level > 0) {
    dump_span(f1.verts.as_span(), "merge result");
  }
  return true;
}

/* Give a group of #OutFace's that are all from a same original mesh face,
 * remove as many dissolvable edges as possible while still keeping the faces legal.
 * A face is legal if it has no repeated vertices and has size at least 3.
 */
static void merge_out_faces(Vector<OutFace> &faces)
{
  constexpr int dbg_level = 0;
  if (faces.size() <= 1) {
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
      std::cout << "(e" << se.e1 << ",e" << se.e2 << ";v" << se.v1 << ",v" << se.v2 << ")";
    }
    std::cout << "\n";
    // dump_span(shared_edges.as_span(), "shared edges");
  }
  if (shared_edges.is_empty()) {
    return;
  }
  /* `shared_edge_valid[i]` is true if both edges in shared_edges[i] are still alive. */
  Array<bool> shared_edge_valid(shared_edges.size(), true);
  /* If `merged_to_faces[i]` is not -1, then argument faces[i] has been merged to that other face.
   */
  Array<int> merged_to(faces.size(), -1);
  /* Local function to follow merged_to mappings as far as possible. */
  auto final_merged_to = [&](int f_orig) {
    BLI_assert(f_orig != -1);
    int f_mapped = f_orig;
    do {
      if (merged_to[f_mapped] != -1) {
        f_mapped = merged_to[f_mapped];
      }
    } while (merged_to[f_mapped] != -1);
    return f_mapped;
  };
  /* TODO: sort shared_edges by decreasing length. */
  for (const int i : shared_edges.index_range()) {
    if (!shared_edge_valid[i]) {
      continue;
    }
    const SharedEdge se = shared_edges[i];
    const int2 orig_faces = se.outface_group_face_indices();
    const int2 cur_faces = int2(final_merged_to(orig_faces[0]), final_merged_to(orig_faces[1]));
    const int f1 = cur_faces[0];
    const int f2 = cur_faces[1];
    if (f1 == -1 || f2 == -2) {
      continue;
    }
    if (dbg_level > 0) {
      std::cout << "try merge of faces " << f1 << " and " << f2 << "\n";
    }
    if (try_merge_out_face_pair(faces[f1], faces[f2], se)) {
      if (dbg_level > 0) {
        std::cout << "successful merge\n";
        dump_span(faces[f1].verts.as_span(), "new f1");
      }
      merged_to[f2] = f1;
    }
  }
  /* Now compress the surviving faces. */
  int move_from = 0;
  int move_to = 0;
  const int orig_num_faces = faces.size();
  while (move_from < orig_num_faces) {
    /* Don't move faces that have been merged elsewhere. */
    while (move_from < orig_num_faces && merged_to[move_from] != -1) {
      move_from++;
    }
    if (move_from >= orig_num_faces) {
      break;
    }
    if (move_to < move_from) {
      faces[move_to] = faces[move_from];
    }
    move_to++;
    move_from++;
  }
  if (move_to < orig_num_faces) {
    faces.resize(move_to);
  }
  if (dbg_level > 0) {
    std::cout << "final faces:\n";
    for (const int i : faces.index_range()) {
      dump_span(faces[i].verts.as_span(), std::to_string(i));
    }
  }
}

/* Build the MeshAssembly corresponding to \a mgl.
 * This involves:
 *  (1) Pointing at output vertices.
 *  (2) Making a map from output vertices to input vertices (using -1 if no match).
 *  (3) Making initial face_groups, where each face group is all the output triangles that
 *      were part of the same input face.
 *  (4) For each face group, remove as many shared edges as possible.
 */
static MeshAssembly assemble_mesh_from_meshgl(const MeshGL &mgl,
                                              Span<const Mesh *> meshes,
                                              const MeshOffsets &mesh_offsets)
{
  timeit::ScopedTimer timer("calculating assemble_mesh_from_meshgl");
  constexpr int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "assemble_mesh_from_meshgl\n";
  }
  MeshAssembly ma;
  ma.vertpos = Span<float>(&*mgl.vertProperties.begin(), mgl.vertProperties.size());
  ma.vertpos_stride = mgl.numProp;
  ma.num_input_verts = mesh_offsets.vert_start.last();
  ma.num_output_verts = ma.vertpos.size() / ma.vertpos_stride;
  const int input_faces_num = mesh_offsets.face_start.last();
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
  {
    timeit::ScopedTimer timer("face merging");
    for (const int gid : face_groups.index_range()) {
      Span<int> group = face_groups[gid].as_span();
      Vector<OutFace> group_faces(group.size());
      for (const int i : group_faces.index_range()) {
        int tri_index = group[i];
        group_faces[i] = make_out_face(mgl, tri_index, gid);
      }
      merge_out_faces(group_faces);
      ma.new_faces.extend(group_faces.as_span());
    }
  }
  if (dbg_level > 0) {
    std::cout << "mesh_assembly result:\n";
    std::cout << "num_input_verts = " << ma.num_input_verts
              << ", num_output_verts = " << ma.num_output_verts << "\n";
    dump_span_with_stride(ma.vertpos, ma.vertpos_stride, "vertpos");
    dump_span(ma.out_to_in_vert_map.as_span(), "out_to_in_vert_map");
    std::cout << "new_faces:\n";
    for (const int i : ma.new_faces.index_range()) {
      std::cout << i << ": face_id = " << ma.new_faces[i].face_id << "\nverts ";
      dump_span(ma.new_faces[i].verts.as_span(), "");
    }
  }
  return ma;
}

static void copy_attribute_using_map(const bke::AttributeIter iter,
                                     bke::MutableAttributeAccessor &output_attrs,
                                     bke::AttributeAccessor &input_attrs,
                                     Span<int> out_to_in_map)
{
  constexpr int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "copy_attribute_using_map, name = " << iter.name << "\n";
  }
  /* If the attribute isn't already in the output mesh, then join_geometries
   * chose not to copy it for some reason, so respect that. */
  if (!output_attrs.lookup(iter.name, iter.domain, iter.data_type)) {
    return;
  }
  bke::GSpanAttributeWriter dst_writer = output_attrs.lookup_or_add_for_write_only_span(iter.name, iter.domain, iter.data_type);
  bke::GAttributeReader src_reader = input_attrs.lookup_or_default(iter.name, iter.domain, iter.data_type);
  GMutableSpan dst = dst_writer.span;
  std::optional<GVArraySpan> src = *src_reader;
  if (!src.has_value()) {
    return;
  }
  const CPPType &ty = dst_writer.span.type();
  const int grain_size = 20000;
  threading::parallel_for(out_to_in_map.index_range(), grain_size, [&](const IndexRange range) {
    for (const int out_elem : range) {
      const int in_elem = out_to_in_map[out_elem];
      if (in_elem != -1) {
          ty.copy_assign(src.value()[in_elem], dst[out_elem]);
      }
    }
  });
  dst_writer.finish();
}

static void interpolate_corner_attributes(bke::MutableAttributeAccessor &output_attrs,
                                          bke::AttributeAccessor &input_attrs,
                                          Mesh *output_mesh,
                                          const Mesh *input_mesh,
                                          Span<int> out_to_in_corner_map,
                                          Span<int> out_to_in_face_map)
{
  timeit::ScopedTimer timer("interpolate corner attributes");
  /* Make parallel arrays of things needed access and write all corner attributes to interpolate. */
  Vector<bke::AttributeIter> attribute_iters;
  Vector<bke::GSpanAttributeWriter> writers;
  Vector<bke::GAttributeReader> readers;
  Vector<std::optional<GVArraySpan>> srcs;
  Vector<GMutableSpan> dsts;
  output_attrs.foreach_attribute([&](const bke::AttributeIter &iter) {
    if (iter.domain != bke::AttrDomain::Corner ||
        (iter.name == ".corner_vert" || iter.name == ".corner_edge")) {
      return;
    }
    bke::GAttributeReader reader = input_attrs.lookup_or_default(iter.name, iter.domain, iter.data_type);
    std::optional<GVArraySpan> src = *reader;
    if (!src.has_value()) {
      return;
    }
    attribute_iters.append(iter);
    writers.append(output_attrs.lookup_or_add_for_write_only_span(iter.name, iter.domain, iter.data_type));
    readers.append(input_attrs.lookup_or_default(iter.name, iter.domain, iter.data_type));
    srcs.append(*readers.last());
    dsts.append(writers.last().span);
  });
  /* Loop per source face, as there is an expensive weight calculation that needs to be done per face. */
  const OffsetIndices<int> output_faces = output_mesh->faces();
  const OffsetIndices<int> input_faces = input_mesh->faces();
  Span<int> input_corner_verts = input_mesh->corner_verts();
  Span<float3> input_vert_positions = input_mesh->vert_positions();
  Span<int> output_corner_verts = output_mesh->corner_verts();
  Span<float3> output_vert_positions = output_mesh->vert_positions();
  const int grain_size = 5000;
  threading::parallel_for(out_to_in_face_map.index_range(), grain_size, [&](const IndexRange range) {
    Vector<float, 20> weights;
    Vector<float2, 20> cos_2d;
    float axis_mat[3][3];
    for (const int out_face_index : range) {
      /* Are there any corners needing interpolation in this face?
       * The corners needing interpolation are those whose out_to_in_corner_map entry is -1.
       */
      IndexRange out_face = output_faces[out_face_index];
      if (!std::any_of(out_face.begin(), out_face.end(), [&](int c) {
        return out_to_in_corner_map[c] == -1;
      })) {
        continue;
      }
      /* At least one output corner did not map to an input corner. */

      /* First get coordinates of input face projected onto 2d, and make sure that
       * weights has the right size. */
      const int in_face_index = out_to_in_face_map[out_face_index];
      const IndexRange in_face = input_faces[in_face_index];
      Span<int> in_face_verts = input_corner_verts.slice(in_face);
      const int in_face_size = in_face.size();
      weights.resize(in_face_size);
      cos_2d.resize(in_face_size);
      float (*cos_2d_p)[2] = reinterpret_cast<float (*)[2]>(cos_2d.data());
      const float3 axis_dominant = bke::mesh::face_normal_calc(input_vert_positions, in_face_verts);
      axis_dominant_v3_to_m3(axis_mat, axis_dominant);
      for (const int i : in_face_verts.index_range()) {
        float3 co = input_vert_positions[in_face_verts[i]];
        cos_2d[i] = (float3x3(axis_mat) * co).xy();
      }
      /* Now the loop to actually interpolate attributes of the new-vertex corners of the output face. */
      for (const int out_c : output_faces[out_face_index]) {
        const int in_c = out_to_in_corner_map[out_c];
        if (in_c != -1) {
          continue;
        }
        const int out_v = output_corner_verts[out_c];
        float co[2];
        mul_v2_m3v3(co, axis_mat, output_vert_positions[out_v]);
        interp_weights_poly_v2(weights.data(), cos_2d_p, in_face_size, co);

        for (const int attr_index : dsts.index_range()) {
          std::optional<GVArraySpan> &src_opt = srcs[attr_index];
          GMutableSpan dst = dsts[attr_index];
          if (!src_opt.has_value()) {
            continue;
          }
          GVArraySpan &src = src_opt.value();
          const CPPType &ty = dst.type();
          BUFFER_FOR_CPP_TYPE_VALUE(ty, buffer);
          BLI_SCOPED_DEFER([&]() { ty.destruct(buffer); });
          bke::attribute_math::convert_to_static_type(ty, [&](auto dummy) {
            using T = decltype(dummy);
            const Span<T> src_typed = src.typed<T>();
            Array<T,20> in_values(in_face.size());
            for (const int i : in_values.index_range()) {
              in_values[i] = src_typed[in_face[i]];
            }
            bke::attribute_math::DefaultMixer<T> mixer{MutableSpan(static_cast<T *>(buffer), 1)};
            for (const int i : in_values.index_range()) {
              mixer.mix_in(0, in_values[i], weights[i]);
            }
            mixer.finalize();
            ty.copy_assign(buffer, dst[out_c]);
          });
        }
      }
    }
  });
  for (bke::GSpanAttributeWriter &writer : writers) {
    writer.finish();
  }
}

/* Add all the edge attributes that are in \a from_mesh to \a to_mesh. */
static void add_edge_attributes_from_mesh(Mesh *to_mesh, const Mesh *from_mesh)
{
  bke::MutableAttributeAccessor to_attrs = to_mesh->attributes_for_write();
  bke::AttributeAccessor from_attrs = from_mesh->attributes();
  from_attrs.foreach_attribute([&](const bke::AttributeIter &iter) {
    if (iter.domain == bke::AttrDomain::Edge) {
      if (iter.name == ".edge_verts") {
        return;
      }
      bke::GAttributeWriter writer = to_attrs.lookup_or_add_for_write(iter.name, iter.domain, iter.data_type);
      if (writer) {
        writer.finish();
      }
    }
  });
}

/* Convert the meshgl that is the result of the boolean back into a
 * Blender Mesh.
 */
static Mesh *meshgl_to_mesh_new(const MeshGL &mgl,
                                Span<const Mesh *> meshes,
                                const Mesh *joined_mesh,
                                const MeshOffsets &mesh_offsets)
{
  constexpr int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "MESHGL_TO_MESH (NEW)\n";
  }
  timeit::ScopedTimer timer("meshgl to mesh from joined_mesh");
  BLI_assert(mgl.mergeFromVert.size() == 0);
  MeshAssembly ma = assemble_mesh_from_meshgl(mgl, meshes, mesh_offsets);
  const int tot_positions = ma.num_output_verts;
  const int tot_faces = ma.new_faces.size();

  /* Get total number of corners, and index of the start
   * corner for each new face. */
  int tot_corners = 0;
  /* TODO: maybe parallelize corner counting and offset calculation. */
  Array<int> face_corner_start_index;
  {
    timeit::ScopedTimer timer_c("calculate corner_start_index");
    face_corner_start_index.reinitialize(tot_faces + 1);
    for (const int i : ma.new_faces.index_range()) {
      face_corner_start_index[i] = tot_corners;
      tot_corners += ma.new_faces[i].verts.size();
    }
    face_corner_start_index[tot_faces] = tot_corners;
  }

  /* Make a new Mesh, now that we know the number of positions, faces, and corners.
   * We will use Blender's parallelized function to calculate edges later.
   * By using joined_mesh as the template, all the needed attributes should have
   * been created, as well as other "parameters" such as vertex group names
   * and materials.
   */
  Mesh *mesh = BKE_mesh_new_nomain_from_template(
      joined_mesh, tot_positions, 0, tot_faces, tot_corners);

  /* Set the vertex positions. */
  MutableSpan<float3> positions = mesh->vert_positions_for_write();
  {
    timeit::ScopedTimer timer_c("set positions");
    int grain_size = 100000;
    threading::parallel_for(IndexRange(tot_positions), grain_size, [&](const IndexRange range) {
      for (const int i : range) {
        int offset = ma.vertpos_stride * i;
        float3 pos(ma.vertpos[offset], ma.vertpos[offset + 1], ma.vertpos[offset + 2]);
        positions[i] = pos;
      }
    });
  }

  /* Make the faces. */
  MutableSpan<int> face_start = mesh->face_offsets_for_write();
  MutableSpan<int> corner_verts = mesh->corner_verts_for_write();
  {
    timeit::ScopedTimer timer_c("calculate faces");
    int grain_size = 50000;
    threading::parallel_for(IndexRange(tot_faces), grain_size, [&](const IndexRange range) {
      for (const int face_index : range) {
        const int corner_index = face_corner_start_index[face_index];
        face_start[face_index] = corner_index;
        const OutFace &face = ma.new_faces[face_index];
        for (const int i : face.verts.index_range()) {
          corner_verts[corner_index + i] = face.verts[i];
        }
      }
    });
    face_start[tot_faces] = tot_corners;
  }

  {
    timeit::ScopedTimer timer_e("calculating edges");
    bke::mesh_calc_edges(*mesh, false, false);
    /* That function killed the edge attributes that were copied from joined_mesh.
     * Add them back. */
    add_edge_attributes_from_mesh(mesh, joined_mesh);
  }

  {
    timeit::ScopedTimer timer_a("copying and interpolating attributes");

    /* Copy attributes from joined_mesh to elements they are mapped to
     * in the new mesh. For most attributes, if there is no input element
     * mapping to it, the attribute value is left at default.
     * But for coerner attributes (most importantly, UV maps), missing
     * values are interpolated in their containing face.
     * We'll do corner interpolation in a separate pass so as to do
     * such attributes at once for a given face.
     */
    bke::AttributeAccessor join_attrs = joined_mesh->attributes();
    bke::MutableAttributeAccessor output_attrs = mesh->attributes_for_write();

    OutToInMaps out_to_in(&ma, joined_mesh, mesh);
    bool need_corner_interpolation = false;
  
    output_attrs.foreach_attribute([&](const bke::AttributeIter &iter) {
      if (ELEM(iter.name, "position", ".edge_verts", ".corner_vert", ".corner_edge")) {
        return;
      }
      Span<int> out_to_in_map;
      bool do_copy = true;
      switch(iter.domain) {
        case bke::AttrDomain::Point: {
          out_to_in.ensure_vertex_map();
          out_to_in_map = out_to_in.vertex_map.as_span();
        } break;
        case bke::AttrDomain::Face: {
          out_to_in.ensure_face_map();
          out_to_in_map = out_to_in.face_map.as_span();
        } break;
        case bke::AttrDomain::Edge: {
          out_to_in.ensure_edge_map();
          out_to_in_map = out_to_in.edge_map.as_span();
        } break;
        case bke::AttrDomain::Corner: {
          out_to_in.ensure_corner_map();
          out_to_in_map = out_to_in.corner_map.as_span();
          need_corner_interpolation = true;
        } break;
        default:
          do_copy = false;
          break;
      }
      if (do_copy) {
        copy_attribute_using_map(iter, output_attrs, join_attrs, out_to_in_map);
      }
    });
    if (need_corner_interpolation) {
      interpolate_corner_attributes(output_attrs, join_attrs, mesh, joined_mesh, out_to_in.corner_map, out_to_in.face_map);
    }
  }
  return mesh;
}

static bke::GeometrySet join_meshes(Span<const Mesh *> meshes)
{
  timeit::ScopedTimer jtimer("join meshes");
  const int meshes_num = meshes.size();
  Array<bke::GeometrySet> geometries(meshes_num);
  for (const int i : geometries.index_range()) {
    geometries[i] = bke::GeometrySet::from_mesh(const_cast<Mesh *>(meshes[i]), bke::GeometryOwnershipType::ReadOnly);
  }
  return geometry::join_geometries(geometries, {});
}

Mesh *mesh_boolean_manifold(Span<const Mesh *> meshes,
                            Span<float4x4> transforms,
                            const float4x4 &target_transform,
                            Span<Array<short>>,
                            BooleanOpParameters op_params)
{
  constexpr int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "\nMESH_BOOLEAN_MANIFOLD with " << meshes.size() << " args\n";
  }
  try {
    timeit::ScopedTimer timer("MANIFOLD BOOLEAN");
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
    bke::GeometrySet joined_meshes_set = join_meshes(meshes);
    const Mesh *joined_mesh = joined_meshes_set.get_mesh();
    BLI_assert(joined_mesh != nullptr);
    get_manifolds(manifolds, joined_mesh, mesh_offsets);
    if (std::any_of(manifolds.begin(), manifolds.end(), [](const Manifold &m) {
        return m.Status() != Manifold::Error::NoError; }))
    {
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
      timeit::ScopedTimer timer_bool("DOING BOOLEAN, GETTING MANIFOLD RESULT");
      Manifold man_result = Manifold::BatchBoolean(manifolds, mop);
      meshgl_result = man_result.GetMeshGL();
      if (dbg_level > 0) {
        std::cout << "boolean result has " << meshgl_result.NumTri() << " tris\n";
        dump_meshgl(meshgl_result, "boolean result meshgl");
      }
    }
    Mesh *mesh_result;
    {
      timeit::ScopedTimer timer_out("MESHGL RESULT TO MESH");
      mesh_result = meshgl_to_mesh_new(meshgl_result, meshes, joined_mesh, mesh_offsets);
    }
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
