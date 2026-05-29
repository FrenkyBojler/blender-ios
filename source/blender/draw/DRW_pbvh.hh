/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#pragma once

#include <memory>
#include <variant>

#include "BLI_index_mask_fwd.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "BKE_paint_bvh.hh"

#include "DNA_customdata_types.h"

namespace blender {

namespace gpu {
class Batch;
class IndexBuf;
class StorageBuf;
class VertBuf;
}  // namespace gpu

namespace blender {
void GPU_storagebuf_free(gpu::StorageBuf *ssbo);
}  // namespace blender
struct Object;
namespace bke {
enum class AttrDomain : int8_t;
namespace pbvh {
class Node;
class DrawCache;
class Tree;
}  // namespace pbvh
}  // namespace bke

namespace draw::pbvh {

using GenericRequest = std::string;

enum class CustomRequest : int8_t {
  Position,
  Normal,
  Mask,
  FaceSet,
};

using AttributeRequest = std::variant<CustomRequest, GenericRequest>;

struct ViewportRequest {
  Vector<AttributeRequest> attributes;
  bool use_coarse_grids;
  friend bool operator==(const ViewportRequest &a, const ViewportRequest &b) = default;
  uint64_t hash() const;
};

/**
 * Combined draw data for multires PBVH optimization.
 * Stores a single large VBO/IBO per attribute with per-node ranges,
 * plus an indirect buffer for multi-draw indirect calls.
 */
/** Per-node offset/count mapping for combined vertex/index buffers. */
struct PBVHNodeRange {
  uint vertex_offset;
  uint index_offset;
  uint vertex_count;
  uint index_count;
};

struct PBVHDrawData {
  gpu::VertBuf *vbo = nullptr;
  std::shared_ptr<gpu::IndexBuf> ibo;
  Vector<PBVHNodeRange> node_ranges;
  std::shared_ptr<gpu::StorageBuf> indirect_buf;
  int node_count = 0;

  PBVHDrawData() = default;
  ~PBVHDrawData() = default;
  PBVHDrawData(const PBVHDrawData &other)
      : vbo(nullptr),
        ibo(other.ibo),
        node_ranges(other.node_ranges),
        indirect_buf(other.indirect_buf),
        node_count(other.node_count)
  {
  }
  PBVHDrawData &operator=(const PBVHDrawData &other)
  {
    if (this != &other) {
      vbo = nullptr;
      ibo = other.ibo;
      node_ranges = other.node_ranges;
      indirect_buf = other.indirect_buf;
      node_count = other.node_count;
    }
    return *this;
  }
  PBVHDrawData(PBVHDrawData &&other) noexcept
      : vbo(other.vbo),
        ibo(std::move(other.ibo)),
        node_ranges(std::move(other.node_ranges)),
        indirect_buf(std::move(other.indirect_buf)),
        node_count(other.node_count)
  {
    other.vbo = nullptr;
    other.node_count = 0;
  }
  PBVHDrawData &operator=(PBVHDrawData &&other) noexcept
  {
    if (this != &other) {
      vbo = other.vbo;
      ibo = std::move(other.ibo);
      node_ranges = std::move(other.node_ranges);
      indirect_buf = std::move(other.indirect_buf);
      node_count = other.node_count;
      other.vbo = nullptr;
      other.node_count = 0;
    }
    return *this;
  }
};

class DrawCache : public bke::pbvh::DrawCache {
 public:
  ~DrawCache() override = default;
  /**
   * Recalculate and copy data as necessary to prepare batches for drawing triangles for a
   * specific combination of attributes.
   */
  virtual Span<gpu::Batch *> ensure_tris_batches(const Object &object,
                                                 const ViewportRequest &request,
                                                 const IndexMask &nodes_to_update) = 0;
  /**
   * Recalculate and copy data as necessary to prepare batches for drawing wireframe geometry for a
   * specific combination of attributes.
   */
  virtual Span<gpu::Batch *> ensure_lines_batches(const Object &object,
                                                  const ViewportRequest &request,
                                                  const IndexMask &nodes_to_update) = 0;

  /**
   * Return the material index for each node (all faces in a node should have the same material
   * index, as ensured by the BVH building process).
   */
  virtual Span<int> ensure_material_indices(const Object &object) = 0;

  /**
   * Build combined draw data for multires PBVH optimization.
   * Creates single VBO/IBO with indirect draw buffer for Vulkan draw call reduction.
   * \return true if combined data was built (only for Grids PBVH type).
   */
  virtual bool ensure_combined_tris_draw_data(const Object &object,
                                              const ViewportRequest &request,
                                              const IndexMask &visible_nodes) = 0;

  /**
   * Build combined line draw data for multires PBVH wireframe optimization.
   * \return true if combined data was built.
   */
  virtual bool ensure_combined_lines_draw_data(const Object &object,
                                               const ViewportRequest &request,
                                               const IndexMask &visible_nodes) = 0;

  /**
   * Get the GPU storage buffer containing indirect draw commands.
   * Valid only after ensure_combined_tris_draw_data() returns true.
   * \return nullptr if no combined data exists.
   */
  virtual gpu::StorageBuf *get_tris_indirect_buf(const ViewportRequest &request,
                                                 const AttributeRequest &attr) = 0;

  /**
   * Get the GPU storage buffer containing indirect draw commands for lines.
   * \return nullptr if no combined data exists.
   */
  virtual gpu::StorageBuf *get_lines_indirect_buf() = 0;

  /**
   * Get the combined batch for indirect drawing.
   * \return nullptr if no combined data exists.
   */
  virtual gpu::Batch *get_combined_tris_batch(const ViewportRequest &request,
                                              const AttributeRequest &attr) = 0;

  /**
   * Get the combined batch for lines indirect drawing.
   * \return nullptr if no combined data exists.
   */
  virtual gpu::Batch *get_combined_lines_batch() = 0;

  /**
   * Get the PBVHDrawData for a given request and attribute.
   * \return nullptr if no combined data exists.
   */
  virtual PBVHDrawData *get_combined_draw_data(const ViewportRequest &request,
                                               const AttributeRequest &attr) = 0;

  /**
   * Get the PBVHDrawData for flat layout nodes.
   * \return nullptr if no flat layout combined data exists.
   */
  virtual PBVHDrawData *get_combined_draw_data_flat(const ViewportRequest &request,
                                                    const AttributeRequest &attr) = 0;

  /**
   * Get the PBVHDrawData for smooth layout nodes.
   * \return nullptr if no smooth layout combined data exists.
   */
  virtual PBVHDrawData *get_combined_draw_data_smooth(const ViewportRequest &request,
                                                      const AttributeRequest &attr) = 0;

  /**
   * Get the PBVHDrawData for lines.
   * \return nullptr if no combined data exists.
   */
  virtual PBVHDrawData *get_combined_lines_draw_data() = 0;
};

DrawCache &ensure_draw_data(std::unique_ptr<bke::pbvh::DrawCache> &ptr);

}  // namespace draw::pbvh
}  // namespace blender
