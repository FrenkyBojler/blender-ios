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
  };
  Map<StringRefNull, Spec> attr_map;

  NeededAttributes(Span<const Mesh *> meshes);

  int num_attrs_for_domain(bke::AttrDomain domain) const;
};

/* Get the union of the needed attributes from all the meshes,
 * in a deterministic order, and omitting the structure attributes. */
NeededAttributes::NeededAttributes(Span<const Mesh *> meshes)
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

/* Ensure that \a mesh has all the needed attributes. */
static void add_needed_attributes_to_mesh(Mesh *mesh, const NeededAttributes &needed_attributes)
{
  /* Sort the attributes by name to get deterministic order. */
  Vector<NeededAttributes::Spec> attr_specs;
  attr_specs.reserve(needed_attributes.attr_map.size());
  for (const NeededAttributes::Spec &val : needed_attributes.attr_map.values()) {
    attr_specs.append(val);
  }
  std::sort(attr_specs.begin(),
            attr_specs.end(),
            [](const NeededAttributes::Spec &a, const NeededAttributes::Spec &b) {
              return a.name < b.name;
            });
  bke::MutableAttributeAccessor accessor = mesh->attributes_for_write();
  bke::AttributeInitDefaultValue attr_init;
  for (const NeededAttributes::Spec &spec : attr_specs) {
    // DEBUG!!
    std::cout << "adding attribute " << spec.name << "\n";
    accessor.add(spec.name, spec.domain, spec.data_type, attr_init);
  }
  // DEBUG!!
  // dump_mesh(mesh, "AFTER ADD_NEEDED_ATTRIBUTES");
}

/* Given an \a input_face index, along with its \a input_mesh_index, copy the attributes
 * in the #GAttributeReadWriteSpans \a rw_spans to attributes in the destination mesh
 * as recorded in \a rw_spans.
 * The "material_index" attribute, which should have index \a material_span_index,
 * gets special treatment: apply the material remap from `matrial_remaps[input_face]`
 * to the value of that attribute.
 */
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
      if (dbg_level > 0) {
        std::cout << "attribute index " << i << ", name = " << rw_spans.attrs[i]
                  << ", value = " << src->type().to_string(src.value()[input_face]) << "\n";
      }
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

/* Like previous, but generic for a given \a domain. */
static void copy_attrs_for_domain(bke::AttrDomain domain,
                                  GAttributeReadWriteSpans &rw_spans,
                                  int input_mesh_index,
                                  int input_element,
                                  int output_element)
{
  constexpr int dbg_level = 1;
  if (dbg_level > 0) {
    std::cout << "copy attrs for domain "
              << (domain == bke::AttrDomain::Point ?
                      "Point" :
                      (domain == bke::AttrDomain::Edge ?
                           "Edge" :
                           (domain == bke::AttrDomain::Corner ? "Corner" : "?")))
              << ", input mesh " << input_mesh_index << ", element " << input_element
              << " to  output element " << output_element << "\n";
  }
  for (const int i : rw_spans.attrs.index_range()) {
    const StringRef attr_name = rw_spans.attrs[i];
    if ((domain == bke::AttrDomain::Point and attr_name == "position") or
        (domain == bke::AttrDomain::Edge and attr_name == ".edge_verts") or
        (domain == bke::AttrDomain::Corner and ELEM(attr_name, ".corner_vert", ".corner_edge")))
    {
      continue;
    }
    if (dbg_level > 0) {
      std::cout << "  attribute index " << i << ", name = " << attr_name << "\n";
    }
    std::optional<GVArraySpan> &src = rw_spans.sources[input_mesh_index][i];
    GMutableSpan &dst = rw_spans.dest[i];
    if (src.has_value()) {
      if (dbg_level > 0) {
        std::cout << "value gets " << src->type().to_string(src.value()[input_element]) << "\n";
      }
      /* rw_spans.dest[output_element] = src[input_element] */
      dst.type().copy_assign(src.value()[input_element], dst[output_element]);
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
  /* Offset face ids. i.e., offset by cumulative face count in input meshes) direct to output. */
  Vector<int> input_faces_to_output;
  /* New faces to output. */
  Vector<OutFace> new_faces;
};

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
    dump_span(ma.input_faces_to_output.as_span(), "input_faces_to_output");
    std::cout << "new_faces:\n";
    for (const int i : ma.new_faces.index_range()) {
      std::cout << i << ": face_id = " << ma.new_faces[i].face_id << "\nverts ";
      dump_span(ma.new_faces[i].verts.as_span(), "");
    }
  }
  return ma;
}

/* Return true if we need a "material_index" face attribute.
 * We need it if any of the material_reamps maps a slot to non-zero
 * (because mapping to zero means just use the result mesh's default slot).
 */
static bool need_material_attribute(Span<Array<short>> material_remaps)
{
  for (const Array<short> &remap : material_remaps) {
    for (const int remap_val : remap) {
      if (remap_val > 0) {
        return true;
      }
    }
  }
  return false;
}

/* Return true if direction vectors \a a and \a b are approximately parallel. */
static inline bool approximately_parallel(const float3 &a, const float3 &b)
{
  float ab = math::length(a) * math::length(b);
  if (ab == 0.0f) {
    return true;
  }
  float abs_cos_ab = math::abs(math::dot(a, b) / ab);
  return (math::abs(abs_cos_ab - 1.0f) <= 1e-5f);
}

/* Look for an edge in face \a face_index of \a mesh can be
 * used as an attribute representative for an edge between
 * vertices \a vert_index and \a vert_index_next in the same
 * face, if any.
 * Also look for a corner in that face that is for vertex \a vert_index.
 *
 * It is possible that either of vert_index or vert_index_next is -1.
 * If only one of them is -1, look for an edge attached to the other
 * and in the same direction as \a edge_dir.
 *
 * Return a pair where the first element is the edge index in \a mesh
 * that is a good representative, or -1 if none, and the second element
 * is the corner in \a mesh that is a corner of our face that is for
 * \a vert+index, or -1 if none.
 *
 * Note there are some cases that this logic won't find a representative
 * edge when one exists: (a) if there are vertex aliases such that the same
 * output vertex maps to multiple input vertices; (b) if the original edge
 * only exists as middle subset in the output face. A TODO to handle these.
 */
static int2 get_rep_edge_and_corner(const int vert_index,
                                    const int vert_index_next,
                                    const float3 &edge_dir,
                                    const Mesh *mesh,
                                    const int face_index)
{
  const IndexRange face = mesh->faces()[face_index];
  Span<int> corner_verts = mesh->corner_verts();
  Span<int> corner_edges = mesh->corner_edges();
  Span<float3> vert_positions = mesh->vert_positions();
  int rep_edge = -1;
  int rep_corner = -1;
  if (vert_index != -1) {
    const int corner = bke::mesh::face_find_corner_from_vert(face, corner_verts, vert_index);
    if (corner != -1) {
      rep_corner = corner;
      const int next_corner = bke::mesh::face_corner_next(face, corner);
      const int face_v_next = corner_verts[next_corner];
      if (face_v_next == vert_index_next) {
        rep_edge = corner_edges[corner];
      }
      else {
        /* Does the direciton match at least? */
        const float3 mesh_edge_dir = vert_positions[face_v_next] - vert_positions[vert_index];
        if (approximately_parallel(edge_dir, mesh_edge_dir)) {
          rep_edge = corner_edges[corner];
        }
      }
    }
  }
  else if (vert_index_next != -1) {
    /* Maybe there is an edge that ends at vert_index_next and is in the right direction. */
    const int corner = bke::mesh::face_find_corner_from_vert(face, corner_verts, vert_index_next);
    if (corner != -1) {
      const int prev_corner = bke::mesh::face_corner_prev(face, corner);
      const int face_v_prev = corner_verts[prev_corner];
      const float3 mesh_edge_dir = vert_positions[vert_index_next] - vert_positions[face_v_prev];
      if (approximately_parallel(edge_dir, mesh_edge_dir)) {
        rep_edge = corner_edges[prev_corner];
      }
    }
  }
  return int2(rep_edge, rep_corner);
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
  constexpr int dbg_level = 1;
  if (dbg_level > 0) {
    std::cout << "\nMESHGL_TO_MESH\n";
    dump_meshgl(mgl, "meshgl_to_mesh argument");
    std::cout << "material_remaps:\n";
    for (int i : material_remaps.index_range()) {
      dump_span(material_remaps[i].as_span(), std::to_string(i));
    }
  }
  timeit::ScopedTimer timer("meshgl to mesh");
  if (mgl.mergeFromVert.size() > 0) {
    /* TODO: handle vertex merging */
    std::cout << "IMPLEMENT ME: handle vertex merging\n";
  }
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
   */
  Mesh *mesh = BKE_mesh_new_nomain_from_template(
      meshes[0], tot_positions, 0, tot_faces, tot_corners);

  /* Ensure that mesh has all needed attributes. */
  NeededAttributes needed_attributes(meshes);
  add_needed_attributes_to_mesh(mesh, needed_attributes);

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
  MutableSpan<int> face_offsets = mesh->face_offsets_for_write();
  MutableSpan<int> corner_verts = mesh->corner_verts_for_write();
  GAttributeReadWriteSpans face_attrs(meshes, mesh, bke::AttrDomain::Face);
  int material_span_index = face_attrs.find_attr_index("material_index");
  if (material_span_index == -1 && need_material_attribute(material_remaps)) {
    material_span_index = face_attrs.add_attribute(
        "material_index", bke::AttrDomain::Face, CD_PROP_INT32);
  }
  {
    timeit::ScopedTimer timer_c("calculate faces");
    int grain_size = 50000;
    threading::parallel_for(IndexRange(tot_faces), grain_size, [&](const IndexRange range) {
      for (const int face_index : range) {
        const int corner_index = face_corner_start_index[face_index];
        face_offsets[face_index] = corner_index;
        const OutFace &face = ma.new_faces[face_index];
        for (const int i : face.verts.index_range()) {
          corner_verts[corner_index + i] = face.verts[i];
        }
        const int input_mesh_index = which_offset_index<int>(face.face_id,
                                                             mesh_offsets.face_offsets);
        BLI_assert(input_mesh_index >= 0);
        const int input_face_index = face.face_id - mesh_offsets.face_offsets[input_mesh_index];
        copy_face_attrs(face_attrs,
                        input_mesh_index,
                        input_face_index,
                        face_index,
                        material_span_index,
                        material_remaps);
      }
    });
    face_offsets[tot_faces] = tot_corners;
  }

  // bke::mesh_smooth_set(*mesh, false);
  {
    timeit::ScopedTimer timer_e("calculating edges");
    bke::mesh_calc_edges(*mesh, false, false);
  }

  if (needed_attributes.num_attrs_for_domain(bke::AttrDomain::Edge) > 0 ||
      needed_attributes.num_attrs_for_domain(bke::AttrDomain::Corner) > 0)
  {
    timeit::ScopedTimer timer_eattr("calculating edge and corner attrs");
    Span<int> corner_edges = mesh->corner_edges();
    Span<int> corner_verts = mesh->corner_verts();
    Span<float3> positions = mesh->vert_positions();
    GAttributeReadWriteSpans edge_attrs(meshes, mesh, bke::AttrDomain::Edge);
    GAttributeReadWriteSpans corner_attrs(meshes, mesh, bke::AttrDomain::Corner);
    int grain_size = 25000;
    threading::parallel_for(IndexRange(tot_faces), grain_size, [&](const IndexRange range) {
      for (const int face_index : IndexRange(range)) {
        const int corner_index = face_corner_start_index[face_index];
        const OutFace &face = ma.new_faces[face_index];
        const int input_mesh_index = which_offset_index<int>(face.face_id,
                                                             mesh_offsets.face_offsets);
        BLI_assert(input_mesh_index >= 0);
        const Mesh *input_mesh = meshes[input_mesh_index];
        const int flen = face.verts.size();
        const int input_face_index = face.face_id - mesh_offsets.face_offsets[input_mesh_index];
        const int input_face_vert_offset = mesh_offsets.vert_offsets[input_mesh_index];
        const int input_face_vert_offset_end = mesh_offsets.vert_offsets[input_mesh_index + 1];
        auto to_mesh_vert_index = [&](int v) {
          int in_v = ma.out_to_in_vert_map[v];
          if (in_v == -1 || in_v < input_face_vert_offset || in_v >= input_face_vert_offset_end) {
            return -1;
          }
          return in_v - input_face_vert_offset;
        };
        for (const int i : IndexRange(flen)) {
          const int output_corner = corner_index + i;
          const int output_e = corner_edges[output_corner];
          const int output_v = corner_verts[output_corner];
          const int output_v_next = corner_verts[corner_index + (i + 1) % flen];
          const int input_v = to_mesh_vert_index(output_v);
          const int input_v_next = to_mesh_vert_index(output_v_next);
          float3 edge_dir = positions[output_v_next] - positions[output_v];
          int2 edge_and_corner = get_rep_edge_and_corner(
              input_v, input_v_next, edge_dir, input_mesh, input_face_index);
          /* Just handle edges in forward direction. Assuming mesh is manifold
           * at this time, there can be at most one face with the edge in a forward
           * direction, so parallel loops won't race for same edge. */
          if (true) {
            const int edge_rep = edge_and_corner[0];
            const int corner_rep = edge_and_corner[1];
            /* TODO: figure out how to only do this for one instance
             * of the edge, deterministically. */
            if (edge_rep != -1) {
              copy_attrs_for_domain(
                  bke::AttrDomain::Edge, edge_attrs, input_mesh_index, edge_rep, output_e);
            }
#if 0
            if (corner_rep != -1) {
              copy_attrs_for_domain(bke::AttrDomain::Corner,
                                    corner_attrs,
                                    input_mesh_index,
                                    corner_rep,
                                    output_corner);
            }
            else {
              /* TODO: interpolate output corner attributes in face. */
            }
#endif
          }
        }
      }
    });
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
    if (dbg_level > 0) {
      for (const int i : IndexRange(num_meshes)) {
        manifolds[i] = manifold_from_mesh_via_meshgl(meshes[i], i, mesh_offsets.face_offsets[i]);
        manifold_ok[i] = manifolds[i].Status() == Manifold::Error::NoError;
      }
    }
    else {
      timeit::ScopedTimer timer_in("INPUT MESHES TO MANIFOLD");
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
      mesh_result = meshgl_to_mesh(meshgl_result, meshes, material_remaps, mesh_offsets);
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
