/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#include "draw_external.hh"

#include "BLI_map.hh"
#include "BLI_math_geom_c.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"

#include "DNA_object_types.h"

#include "BKE_material.hh"
#include "BKE_object.hh"
#include "BKE_object_draw_provider.hh"
#include "BKE_object_types.hh"

#include "DEG_depsgraph_query.hh"

#include "GPU_batch.hh"
#include "GPU_vertex_buffer.hh"
#include "GPU_vertex_format.hh"

#include "draw_context_private.hh"
#include "draw_view.hh"

namespace blender::draw {

/* -------------------------------------------------------------------- */
/** \name Vertex Formats
 * \{ */

static const GPUVertFormat &position_format()
{
  static const GPUVertFormat format = GPU_vertformat_from_attribute(
      "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  return format;
}

static const GPUVertFormat &normal_format()
{
  static const GPUVertFormat format = GPU_vertformat_from_attribute(
      "nor", gpu::VertAttrType::SNORM_16_16_16_16);
  return format;
}

static short4 normal_float_to_short(const float3 &value)
{
  short3 result;
  normal_float_to_short_v3(result, value);
  return short4(result.x, result.y, result.z, 0);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Per-Object GPU Cache
 * \{ */

/* Per-drawable-node GPU buffers + batch, plus the vertex count they were built
 * for so the provider's TOPOLOGY flag can be corroborated. */
struct NodeCache {
  gpu::VertBufPtr pos;
  gpu::VertBufPtr nor;
  gpu::Batch *batch = nullptr;
  int verts_num = 0;

  ~NodeCache()
  {
    if (batch) {
      GPU_batch_discard(batch);
    }
  }
  NodeCache() = default;
  /* `batch` is a raw owning pointer, so a move must transfer it and null the
   * source — otherwise the moved-from destructor discards a batch the moved-to
   * copy (and its cached VBOs) still references (use-after-free at draw). */
  NodeCache(NodeCache &&other) noexcept
      : pos(std::move(other.pos)),
        nor(std::move(other.nor)),
        batch(other.batch),
        verts_num(other.verts_num)
  {
    other.batch = nullptr;
  }
  NodeCache &operator=(NodeCache &&other) noexcept
  {
    if (this != &other) {
      if (batch) {
        GPU_batch_discard(batch);
      }
      pos = std::move(other.pos);
      nor = std::move(other.nor);
      batch = other.batch;
      verts_num = other.verts_num;
      other.batch = nullptr;
    }
    return *this;
  }
  NodeCache(const NodeCache &) = delete;
  NodeCache &operator=(const NodeCache &) = delete;
};

struct ObjectCache {
  Vector<NodeCache> nodes;
};

/* Keyed by original object pointer (the draw object is a depsgraph copy). Freed
 * on mode exit / provider unregister (external_draw_cache_free) and GPU
 * teardown (external_draw_cache_free_all). */
static Map<const Object *, ObjectCache> &object_caches()
{
  static Map<const Object *, ObjectCache> caches;
  return caches;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Node Upload
 * \{ */

/* Upload one node's positions + normals into `cache`, (re)allocating the VBOs
 * and batch when the vertex count changed or nothing was cached yet. */
static void node_upload(NodeCache &cache, const ExternalDrawNode &node)
{
  const bool realloc = cache.batch == nullptr || cache.verts_num != node.verts_num ||
                       (node.update_flags & EXTERNAL_DRAW_UPDATE_TOPOLOGY) != 0;
  const bool upload = realloc || (node.update_flags & EXTERNAL_DRAW_UPDATE_DATA) != 0;
  if (!upload) {
    return;
  }

  if (realloc) {
    if (cache.batch) {
      GPU_batch_discard(cache.batch);
      cache.batch = nullptr;
    }
    /* Dynamic: the CPU-side data is kept so a stroke can re-upload positions
     * each frame (static usage frees it after the first GPU upload). */
    cache.pos = gpu::VertBufPtr(
        GPU_vertbuf_create_with_format_ex(position_format(), GPU_USAGE_DYNAMIC));
    cache.nor = gpu::VertBufPtr(
        GPU_vertbuf_create_with_format_ex(normal_format(), GPU_USAGE_DYNAMIC));
    GPU_vertbuf_data_alloc(*cache.pos, node.verts_num);
    GPU_vertbuf_data_alloc(*cache.nor, node.verts_num);
    cache.verts_num = node.verts_num;
  }

  MutableSpan<float3> positions = cache.pos->data<float3>();
  positions.copy_from(Span<float3>(reinterpret_cast<const float3 *>(node.positions),
                                   node.verts_num));

  MutableSpan<short4> normals = cache.nor->data<short4>();
  if (node.normals != nullptr) {
    const Span<float3> src(reinterpret_cast<const float3 *>(node.normals), node.verts_num);
    for (const int i : IndexRange(node.verts_num)) {
      normals[i] = normal_float_to_short(src[i]);
    }
  }
  else {
    /* Flat: one geometric normal per triangle (soup order, every 3 verts). */
    for (int tri = 0; tri * 3 < node.verts_num; tri++) {
      const float3 &a = positions[tri * 3 + 0];
      const float3 &b = positions[tri * 3 + 1];
      const float3 &c = positions[tri * 3 + 2];
      const short4 packed = normal_float_to_short(math::normalize(math::cross(b - a, c - a)));
      normals[tri * 3 + 0] = packed;
      normals[tri * 3 + 1] = packed;
      normals[tri * 3 + 2] = packed;
    }
  }

  /* Re-uploaded into a dynamic buffer: flag both for the next GPU use. */
  GPU_vertbuf_tag_dirty(cache.pos.get());
  GPU_vertbuf_tag_dirty(cache.nor.get());

  if (realloc) {
    cache.batch = GPU_batch_create(GPU_PRIM_TRIS, nullptr, nullptr);
    GPU_batch_vertbuf_add(cache.batch, cache.pos.get(), false);
    GPU_batch_vertbuf_add(cache.batch, cache.nor.get(), false);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public API
 * \{ */

Vector<SculptBatch> external_batches_get(const Object *ob, SculptBatchFeature /*features*/)
{
  const ExternalDrawProvider *provider = BKE_object_external_draw_provider_get(ob);
  if (provider == nullptr) {
    return {};
  }

  /* v1 milestone: positions + normals only (Workbench default). Generic
   * attribute requests (mask/face-set/color) land with the attribute stage. */
  const ExternalDrawAttrRequest request = {0, nullptr};

  const Object *ob_orig = DEG_get_original(ob);
  ExternalDrawNode *nodes = nullptr;
  const int nodes_num = provider->nodes_get(
      provider->user_data, const_cast<Object *>(ob), &request, &nodes);
  if (nodes_num == 0 || nodes == nullptr) {
    provider->nodes_release(provider->user_data, const_cast<Object *>(ob));
    return {};
  }

  ObjectCache &cache = object_caches().lookup_or_add_default(ob_orig);
  cache.nodes.resize(nodes_num);

  /* Frustum planes in object space (transform by inverse(obmat); the transpose
   * inverse of a plane cancels the obmat inverse), matching draw_sculpt.cc. */
  std::array<float4, 6> planes = View::default_get().frustum_planes_get();
  const float4x4 tmat = math::transpose(ob->object_to_world());
  for (const int i : IndexRange(planes.size())) {
    planes[i] = tmat * planes[i];
  }

  const int max_material = std::max(0, BKE_object_material_count_eval(ob) - 1);

  Vector<SculptBatch> result;
  for (const int i : IndexRange(nodes_num)) {
    const ExternalDrawNode &node = nodes[i];
    node_upload(cache.nodes[i], node);

    if (node.verts_num == 0) {
      continue;
    }
    /* Frustum cull: skip a node whose AABB is fully outside any plane. */
    bool outside = false;
    for (const float4 &plane : planes) {
      float3 vmin;
      for (int axis = 0; axis < 3; axis++) {
        vmin[axis] = plane[axis] < 0.0f ? node.bounds_min[axis] : node.bounds_max[axis];
      }
      if (math::dot(float3(plane.x, plane.y, plane.z), vmin) + plane.w < 0.0f) {
        outside = true;
        break;
      }
    }
    if (outside) {
      continue;
    }

    SculptBatch batch = {};
    batch.batch = cache.nodes[i].batch;
    batch.material_slot = std::clamp(node.material_index, 0, max_material);
    batch.debug_index = result.size();
    result.append(batch);
  }

  provider->nodes_release(provider->user_data, const_cast<Object *>(ob));
  return result;
}

void external_draw_cache_free(const Object *ob)
{
  if (ob == nullptr) {
    return;
  }
  object_caches().remove(DEG_get_original(ob));
}

void external_draw_cache_free_all()
{
  object_caches().clear();
}

/** \} */

}  // namespace blender::draw
