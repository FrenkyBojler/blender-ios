/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#include "BLI_array_utils.hh"
#include "BLI_string.h"

#include "BKE_attribute.h"
#include "BKE_attribute.hh"
#include "BKE_attribute_math.hh"
#include "BKE_mesh.hh"

#include "attribute_convert.hh"
#include "draw_cache_inline.hh"
#include "draw_subdivision.hh"
#include "extract_mesh.hh"

#include "GPU_vertex_buffer.hh"
#include <optional>

namespace blender::draw {

/* ---------------------------------------------------------------------- */
/** \name Extract Attributes
 * \{ */

static void init_vbo_for_attribute(const MeshRenderData &mr,
                                   gpu::VertBuf &vbo,
                                   const StringRef name,
                                   const eCustomDataType data_type,
                                   bool build_on_device,
                                   uint32_t len)
{
  char attr_name[32], attr_safe_name[GPU_MAX_SAFE_ATTR_NAME];
  GPU_vertformat_safe_attr_name(name, attr_safe_name, GPU_MAX_SAFE_ATTR_NAME);
  /* Attributes use auto-name. */
  SNPRINTF(attr_name, "a%s", attr_safe_name);

  GPUVertFormat format = init_format_for_attribute(data_type, attr_name);
  GPU_vertformat_deinterleave(&format);

  if (mr.active_color_name && name == mr.active_color_name) {
    GPU_vertformat_alias_add(&format, "ac");
  }
  if (mr.default_color_name && name == mr.default_color_name) {
    GPU_vertformat_alias_add(&format, "c");
  }

  if (build_on_device) {
    GPU_vertbuf_init_build_on_device(vbo, format, len);
  }
  else {
    GPU_vertbuf_init_with_format(vbo, format);
    GPU_vertbuf_data_alloc(vbo, len);
  }
}

template<typename T>
static void extract_data_mesh_mapped_corner(const Span<T> attribute,
                                            const Span<int> indices,
                                            gpu::VertBuf &vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;
  MutableSpan data = vbo.data<VBOType>();

  if constexpr (std::is_same_v<T, VBOType>) {
    array_utils::gather(attribute, indices, data);
  }
  else {
    threading::parallel_for(indices.index_range(), 8192, [&](const IndexRange range) {
      for (const int i : range) {
        data[i] = Converter::convert(attribute[indices[i]]);
      }
    });
  }
}

template<typename T>
static void extract_data_mesh_face(const OffsetIndices<int> faces,
                                   const Span<T> attribute,
                                   gpu::VertBuf &vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;
  MutableSpan data = vbo.data<VBOType>();

  threading::parallel_for(faces.index_range(), 2048, [&](const IndexRange range) {
    for (const int i : range) {
      data.slice(faces[i]).fill(Converter::convert(attribute[i]));
    }
  });
}

template<typename T>
static void extract_data_bmesh_vert(const BMesh &bm, const int cd_offset, gpu::VertBuf &vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;
  VBOType *data = vbo.data<VBOType>().data();

  const BMFace *face;
  BMIter f_iter;
  BM_ITER_MESH (face, &f_iter, &const_cast<BMesh &>(bm), BM_FACES_OF_MESH) {
    const BMLoop *loop = BM_FACE_FIRST_LOOP(face);
    for ([[maybe_unused]] const int i : IndexRange(face->len)) {
      const T *src = static_cast<const T *>(POINTER_OFFSET(loop->v->head.data, cd_offset));
      *data = Converter::convert(*src);
      loop = loop->next;
      data++;
    }
  }
}

template<typename T>
static void extract_data_bmesh_edge(const BMesh &bm, const int cd_offset, gpu::VertBuf &vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;
  VBOType *data = vbo.data<VBOType>().data();

  const BMFace *face;
  BMIter f_iter;
  BM_ITER_MESH (face, &f_iter, &const_cast<BMesh &>(bm), BM_FACES_OF_MESH) {
    const BMLoop *loop = BM_FACE_FIRST_LOOP(face);
    for ([[maybe_unused]] const int i : IndexRange(face->len)) {
      const T &src = *static_cast<const T *>(POINTER_OFFSET(loop->e->head.data, cd_offset));
      *data = Converter::convert(src);
      loop = loop->next;
      data++;
    }
  }
}

template<typename T>
static void extract_data_bmesh_face(const BMesh &bm, const int cd_offset, gpu::VertBuf &vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;
  VBOType *data = vbo.data<VBOType>().data();

  const BMFace *face;
  BMIter f_iter;
  BM_ITER_MESH (face, &f_iter, &const_cast<BMesh &>(bm), BM_FACES_OF_MESH) {
    const T &src = *static_cast<const T *>(POINTER_OFFSET(face->head.data, cd_offset));
    std::fill_n(data, face->len, Converter::convert(src));
    data += face->len;
  }
}

template<typename T>
static void extract_data_bmesh_loop(const BMesh &bm, const int cd_offset, gpu::VertBuf &vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;
  VBOType *data = vbo.data<VBOType>().data();

  const BMFace *face;
  BMIter f_iter;
  BM_ITER_MESH (face, &f_iter, &const_cast<BMesh &>(bm), BM_FACES_OF_MESH) {
    const BMLoop *loop = BM_FACE_FIRST_LOOP(face);
    for ([[maybe_unused]] const int i : IndexRange(face->len)) {
      const T &src = *static_cast<const T *>(POINTER_OFFSET(loop->head.data, cd_offset));
      *data = Converter::convert(src);
      loop = loop->next;
      data++;
    }
  }
}

const CustomDataLayer *lookup_layer_by_name(const CustomData &data, const StringRef name)
{
  const int index = CustomData_get_named_layer_index_notype(&data, name);
  if (index == -1) {
    return nullptr;
  }
  return &data.layers[index];
}

static std::pair<const CustomDataLayer *, bke::AttrDomain> bmesh_attribute_lookup(
    const BMesh &bm, const StringRef name)
{
  if (const CustomDataLayer *layer = lookup_layer_by_name(bm.vdata, name)) {
    return {layer, bke::AttrDomain::Point};
  }
  if (const CustomDataLayer *layer = lookup_layer_by_name(bm.pdata, name)) {
    return {layer, bke::AttrDomain::Face};
  }
  if (const CustomDataLayer *layer = lookup_layer_by_name(bm.ldata, name)) {
    return {layer, bke::AttrDomain::Corner};
  }
  return {nullptr, bke::AttrDomain::Point};
}

static void extract_attribute(const MeshRenderData &mr, const StringRef name, gpu::VertBuf &vbo)
{
  if (mr.extract_type == MeshExtractType::BMesh) {
    const auto &[layer, domain] = bmesh_attribute_lookup(*mr.bm, name);
    init_vbo_for_attribute(mr, vbo, name, eCustomDataType(layer->type), false, mr.corners_num);
    const int cd_offset = layer->offset;

    bke::attribute_math::convert_to_static_type(eCustomDataType(layer->type), [&](auto dummy) {
      using T = decltype(dummy);
      if constexpr (!std::is_void_v<typename AttributeConverter<T>::VBOType>) {
        switch (domain) {
          case bke::AttrDomain::Point:
            extract_data_bmesh_vert<T>(*mr.bm, cd_offset, vbo);
            break;
          case bke::AttrDomain::Edge:
            extract_data_bmesh_edge<T>(*mr.bm, cd_offset, vbo);
            break;
          case bke::AttrDomain::Face:
            extract_data_bmesh_face<T>(*mr.bm, cd_offset, vbo);
            break;
          case bke::AttrDomain::Corner:
            extract_data_bmesh_loop<T>(*mr.bm, cd_offset, vbo);
            break;
          default:
            BLI_assert_unreachable();
        }
      }
    });
  }
  else {
    const bke::AttributeAccessor attributes = mr.mesh->attributes();
    const bke::GAttributeReader attribute = attributes.lookup(name);
    init_vbo_for_attribute(mr,
                           vbo,
                           name,
                           bke::cpp_type_to_custom_data_type(attribute.varray.type()),
                           false,
                           uint32_t(mr.corners_num));
    bke::attribute_math::convert_to_static_type(attribute.varray.type(), [&](auto dummy) {
      using T = decltype(dummy);
      if constexpr (!std::is_void_v<typename AttributeConverter<T>::VBOType>) {
        const VArraySpan<T> data = attribute.varray.typed<T>();
        switch (attribute.domain) {
          case bke::AttrDomain::Point:
            extract_data_mesh_mapped_corner(data, mr.corner_verts, vbo);
            break;
          case bke::AttrDomain::Edge:
            extract_data_mesh_mapped_corner(data, mr.corner_edges, vbo);
            break;
          case bke::AttrDomain::Face:
            extract_data_mesh_face(mr.faces, data, vbo);
            break;
          case bke::AttrDomain::Corner:
            vertbuf_data_extract_direct(data, vbo);
            break;
          default:
            BLI_assert_unreachable();
        }
      }
    });
  }
}

void extract_attributes(const MeshRenderData &mr,
                        const Span<StringRef> requests,
                        const Span<gpu::VertBuf *> vbos)
{
  for (const int i : vbos.index_range()) {
    if (DRW_vbo_requested(vbos[i])) {
      extract_attribute(mr, requests[i], *vbos[i]);
    }
  }
}

static eCustomDataType lookup_attribute_data_type(const MeshRenderData &mr, const StringRef name)
{
  if (mr.extract_type == MeshExtractType::BMesh) {
    const auto &[layer, domain] = bmesh_attribute_lookup(*mr.bm, name);
    return eCustomDataType(layer->type);
  }
  const bke::AttributeAccessor attributes = mr.mesh->attributes();
  const std::optional<bke::AttributeMetaData> attribute = attributes.lookup_meta_data(name);
  return attribute->data_type;
}

void extract_attributes_subdiv(const MeshRenderData &mr,
                               const DRWSubdivCache &subdiv_cache,
                               const Span<StringRef> requests,
                               const Span<gpu::VertBuf *> vbos)
{
  for (const int i : vbos.index_range()) {
    if (DRW_vbo_requested(vbos[i])) {
      const StringRef request = requests[i];
      const eCustomDataType data_type = lookup_attribute_data_type(mr, request);

      const Mesh *coarse_mesh = subdiv_cache.mesh;

      /* Prepare VBO for coarse data. The compute shader only expects floats. */
      gpu::VertBuf *src_data = GPU_vertbuf_calloc();
      GPUVertFormat coarse_format = init_format_for_attribute(data_type, "data");
      GPU_vertbuf_init_with_format_ex(*src_data, coarse_format, GPU_USAGE_STATIC);
      GPU_vertbuf_data_alloc(*src_data, uint32_t(coarse_mesh->corners_num));
      init_vbo_for_attribute(mr, *src_data, request, data_type, false, coarse_mesh->corners_num);

      extract_attribute(mr, request, *src_data);

      gpu::VertBuf &dst_buffer = *vbos[i];
      init_vbo_for_attribute(
          mr, dst_buffer, request, data_type, true, subdiv_cache.num_subdiv_loops);

      /* Ensure data is uploaded properly. */
      GPU_vertbuf_tag_dirty(src_data);
      bke::attribute_math::convert_to_static_type(data_type, [&](auto dummy) {
        using T = decltype(dummy);
        using Converter = AttributeConverter<T>;
        if constexpr (!std::is_void_v<typename Converter::VBOType>) {
          draw_subdiv_interp_custom_data(subdiv_cache,
                                         *src_data,
                                         dst_buffer,
                                         Converter::gpu_component_type,
                                         Converter::gpu_component_len,
                                         0);
        }
      });

      GPU_vertbuf_discard(src_data);
    }
  }
}

void extract_attr_viewer(const MeshRenderData &mr, gpu::VertBuf &vbo)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "attribute_value", GPU_COMP_F32, 4, GPU_FETCH_FLOAT);
  }

  GPU_vertbuf_init_with_format(vbo, format);
  GPU_vertbuf_data_alloc(vbo, mr.corners_num);
  MutableSpan vbo_data = vbo.data<ColorGeometry4f>();

  const StringRefNull attr_name = ".viewer";
  const bke::AttributeAccessor attributes = mr.mesh->attributes();
  const bke::AttributeReader attribute = attributes.lookup_or_default<ColorGeometry4f>(
      attr_name, bke::AttrDomain::Corner, {1.0f, 0.0f, 1.0f, 1.0f});
  attribute.varray.materialize(vbo_data);
}

/** \} */

}  // namespace blender::draw
