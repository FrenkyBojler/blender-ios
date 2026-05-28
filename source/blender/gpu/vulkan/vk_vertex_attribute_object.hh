/* SPDX-FileCopyrightText: 2023 Blender Authors All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "BLI_mutex.hh"

#include "render_graph/vk_render_graph.hh"
#include "vk_buffer.hh"
#include "vk_vertex_input_description.hh"

namespace blender::gpu {

class VKVertexBuffer;
class VKContext;
class VKBatch;
class VKShaderInterface;
class VKImmediate;
class VKShader;
class VKDevice;

using AttributeMask = uint16_t;

/* TODO: VKVertexAttributeObject should not contain any reference to VBO's. This should make the
 * API be compatible with both #VKBatch and #VKImmediate. */
/* TODO: In steam of storing the bindings/attributes we should add a data structure that can store
 * them. Building the bindings/attributes should be done inside #VKPipelinePool. */

/**
 * \brief Cache key for vertex attribute objects.
 *
 * The key is computed from the vertex buffer formats.
 */
struct VKVertexAttributeObjectCacheKey {
  uint32_t vbo_count = 0;
  uint16_t enabled_attr_mask = 0;
  uint64_t formats_hash = 0;

  bool operator==(const VKVertexAttributeObjectCacheKey &other) const
  {
    return vbo_count == other.vbo_count &&
           enabled_attr_mask == other.enabled_attr_mask &&
           formats_hash == other.formats_hash;
  }

  uint64_t hash() const
  {
    return formats_hash ^ (enabled_attr_mask * 31);
  }
};

class VKVertexAttributeObject;

class VKVertexAttributeObjectCache {
 public:
  mutable Mutex mutex_;

  VKVertexAttributeObjectCache() = default;

  void insert(VKVertexAttributeObjectCacheKey key, const VKVertexAttributeObject &value);
  const VKVertexAttributeObject *lookup(VKVertexAttributeObjectCacheKey key) const;
  void clear();

 private:
  Map<VKVertexAttributeObjectCacheKey, std::unique_ptr<VKVertexAttributeObject>> cache_;
};

class VKVertexAttributeObject {
 public:
  VKVertexInputDescription vertex_input;

  /* Used for batches. */
  Vector<VKVertexBuffer *> vbos;
  /* Used for immediate mode. */
  Vector<VKBufferWithOffset> buffers;

  VKVertexAttributeObject();
  void clear();

  void bind(render_graph::VKVertexBufferBindings &r_vertex_buffer_bindings) const;

  /** Copy assignment operator. */
  VKVertexAttributeObject &operator=(const VKVertexAttributeObject &other);

  void update_bindings(const VKContext &context, VKBatch &batch);
  void update_bindings(VKImmediate &immediate);

  void debug_print() const;

 private:
  /** Update unused bindings with a dummy binding. */
  void fill_unused_bindings(const VKShaderInterface &interface,
                            const AttributeMask occupied_attributes);
  void update_bindings(const GPUVertFormat &vertex_format,
                       VKVertexBuffer *vertex_buffer,
                       VKBufferWithOffset *immediate_vertex_buffer,
                       const int64_t vertex_len,
                       const VKShaderInterface &interface,
                       AttributeMask &r_occupied_attributes);
};

VKVertexAttributeObjectCache &vertex_attribute_object_cache_get();

/**
 * \brief Create a cache key from batch vertex buffers.
 */
VKVertexAttributeObjectCacheKey vk_batch_vertex_attribute_cache_key_create(
    VKBatch &batch,
    AttributeMask enabled_attr_mask);

/**
 * \brief Create a cache key from immediate mode vertex format.
 */
VKVertexAttributeObjectCacheKey vk_immediate_vertex_attribute_cache_key_create(
    const GPUVertFormat &format,
    AttributeMask enabled_attr_mask);

}  // namespace blender::gpu
