/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "BLI_math_color.h"
#include "BLI_vector.hh"

#include "BKE_geometry_set.hh"
#include "BKE_volume_grid.hh"
#include "BKE_volume_grid_fields.hh"
#include "BKE_volume_openvdb.hh"

#include "overlay_base.hh"
#include "overlay_private.hh"

namespace blender::draw::overlay {

static float3 grid_leaf_on_positions(const openvdb::GridBase &grid_base, Vector<float3> &r_position)
{
  const VolumeGridType grid_type = bke::volume_grid::get_type(grid_base);
  BKE_volume_grid_type_to_static_type(grid_type, [&](auto type_tag) {
    using GridT = typename decltype(type_tag)::type;
    using TreeT = typename GridT::TreeType;
    using RootT = typename TreeT::RootNodeType;
    using InnerT = typename RootT::ChildNodeType;
    using LeafT = typename InnerT::LeafNodeType;
    const GridT &grid = static_cast<const GridT &>(grid_base);

    for (typename TreeT::LeafCIter leaf_iter = grid.tree().cbeginLeaf(); leaf_iter; ++leaf_iter) {
      for (typename LeafT::ValueOnCIter iter = leaf_iter->cbeginValueOn(); iter; ++iter) {
        const openvdb::Coord centre = iter.getCoord();
        r_position.append(float3(centre.x(), centre.y(), centre.z()));
      }
    }
  });
  
  return float3(1.0f);
}

static float3 grid_leaf_off_positions(const openvdb::GridBase &grid_base, Vector<float3> &r_position)
{
  const VolumeGridType grid_type = bke::volume_grid::get_type(grid_base);
  BKE_volume_grid_type_to_static_type(grid_type, [&](auto type_tag) {
    using GridT = typename decltype(type_tag)::type;
    using TreeT = typename GridT::TreeType;
    using RootT = typename TreeT::RootNodeType;
    using InnerT = typename RootT::ChildNodeType;
    using LeafT = typename InnerT::LeafNodeType;
    const GridT &grid = static_cast<const GridT &>(grid_base);

    for (typename TreeT::LeafCIter leaf_iter = grid.tree().cbeginLeaf(); leaf_iter; ++leaf_iter) {
      for (typename LeafT::ValueOffCIter iter = leaf_iter->cbeginValueOff(); iter; ++iter) {
        const openvdb::Coord centre = iter.getCoord();
        r_position.append(float3(centre.x(), centre.y(), centre.z()));
      }
    }
  });
  
  return float3(1.0f);
}

static float3 grid_root_tiles_positions(const openvdb::GridBase &grid_base, Vector<float3> &r_position)
{
  int64_t root_tile_size = -1;
  const VolumeGridType grid_type = bke::volume_grid::get_type(grid_base);
  BKE_volume_grid_type_to_static_type(grid_type, [&](auto type_tag) {
    using GridT = typename decltype(type_tag)::type;
    using TreeT = typename GridT::TreeType;
    using RootT = typename TreeT::RootNodeType;
    const GridT &grid = static_cast<const GridT &>(grid_base);

    root_tile_size = RootT::ChildNodeType::DIM;

    for (typename RootT::ChildOffCIter iter = grid.tree().cbeginRootTiles(); iter; ++iter) {
      const openvdb::Coord centre = iter.getCoord();
      r_position.append(float3(centre.x(), centre.y(), centre.z()));
    }
  });
  
  return float3(root_tile_size);
}

static void grid_all_child_nodes_positions(const openvdb::GridBase &grid_base,
                                           Array<float3> &r_sizes,
                                           Array<Vector<float3>> &r_position)
{
  r_sizes.reinitialize(grid_base.baseTree().treeDepth());
  r_position.reinitialize(grid_base.baseTree().treeDepth());

  const VolumeGridType grid_type = bke::volume_grid::get_type(grid_base);
  BKE_volume_grid_type_to_static_type(grid_type, [&](auto type_tag) {
    using GridT = typename decltype(type_tag)::type;
    using TreeT = typename GridT::TreeType;
    const GridT &grid = static_cast<const GridT &>(grid_base);

    for (typename TreeT::NodeCIter iter = grid.tree().cbeginNode(); iter; ++iter) {
      const openvdb::Coord centre = iter.getCoord();
      r_position[iter.getLevel()].append(float3(centre.x(), centre.y(), centre.z()));
      
      const openvdb::CoordBBox node_box = iter.getBoundingBox();
      r_sizes[iter.getLevel()] = float3(node_box.dim().x(), node_box.dim().y(), node_box.dim().z());
    }
  });
}

static void grid_all_tiles_positions(const openvdb::GridBase &grid_base,
                                     Array<float3> &r_sizes,
                                     Array<Vector<float3>> &r_position)
{
  r_sizes.reinitialize(grid_base.baseTree().treeDepth());
  r_position.reinitialize(grid_base.baseTree().treeDepth());

  const VolumeGridType grid_type = bke::volume_grid::get_type(grid_base);
  BKE_volume_grid_type_to_static_type(grid_type, [&](auto type_tag) {
    using GridT = typename decltype(type_tag)::type;
    using TreeT = typename GridT::TreeType;
    const GridT &grid = static_cast<const GridT &>(grid_base);

    for (typename TreeT::ValueOffCIter iter = grid.tree().beginValueOff(); iter; ++iter) {
      if (!iter.isTileValue()) {
        continue;
      }
      const openvdb::Coord centre = iter.getCoord();
      r_position[iter.getLevel()].append(float3(centre.x(), centre.y(), centre.z()));
      
      const openvdb::CoordBBox node_box = iter.getBoundingBox();
      r_sizes[iter.getLevel()] = float3(node_box.dim().x(), node_box.dim().y(), node_box.dim().z());
    }
  });
}

struct BatchDeleter {
  void operator()(gpu::Batch *shader)
  {
    GPU_BATCH_DISCARD_SAFE(shader);
  }
};

static gpu::Batch *batch_for_voxels(const Span<float3> position, const float3 voxel_size)
{
  static const GPUVertFormat format = [&]() {
    GPUVertFormat format{};
    GPU_vertformat_attr_add(&format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
    return format;
  }();

  gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
  GPU_vertbuf_data_alloc(*vbo, position.size() * 8);

  std::array<float3, 8> voxel_corners = {
    float3(0.5f, 0.5f, 0.5f),
    float3(0.5f, -0.5f, 0.5f),
    float3(-0.5f, -0.5f, 0.5f),
    float3(-0.5f, 0.5f, 0.5f),
    float3(0.5f, 0.5f, -0.5f),
    float3(0.5f, -0.5f, -0.5f),
    float3(-0.5f, -0.5f, -0.5f),
    float3(-0.5f, 0.5f, -0.5f)};

  std::transform(voxel_corners.begin(),
                 voxel_corners.end(),
                 voxel_corners.begin(),
                 [&](const float3 point) -> float3 {
                   return (point + float3(0.5f)) * voxel_size;
                 });

  MutableSpan<float3> voxel_positions = vbo->data<float3>();
  threading::parallel_for(position.index_range(), 2048, [&](const IndexRange range) {
    for (const int i : range) {
      const float3 centre = position[i];
      std::transform(voxel_corners.begin(),
                     voxel_corners.end(),
                     voxel_positions.begin() + i * 8,
                     [&](const float3 point) -> float3 {
                       return centre + point;
                     });
    }
  });

  GPUIndexBufBuilder elb;
  GPU_indexbuf_init(&elb, GPU_PRIM_LINES, position.size() * 12, position.size() * 8);
  MutableSpan<uint2> lines = GPU_indexbuf_get_data(&elb).cast<uint2>();

  static const std::array<uint2, 12> voxel_edges = {
    uint2(0, 1),
    uint2(1, 2),
    uint2(2, 3),
    uint2(3, 0),
    uint2(4, 5),
    uint2(5, 6),
    uint2(6, 7),
    uint2(7, 4),
    uint2(4, 0),
    uint2(5, 1),
    uint2(6, 2),
    uint2(7, 3)};

  threading::parallel_for(position.index_range(), 2048, [&](const IndexRange range) {
    for (const int i : range) {
      const uint2 voxel_start_i = uint2(i * 8);
      std::transform(voxel_edges.begin(),
                     voxel_edges.end(),
                     lines.begin() + i * 12,
                     [&](const uint2 edge) -> uint2 {
                       return edge + voxel_start_i;
                     });
    }
  });

  gpu::IndexBuf *ibo = GPU_indexbuf_build_ex(&elb, 0, position.size() * 8, false);

  return GPU_batch_create_ex(GPU_PRIM_LINES, vbo, ibo, GPU_BATCH_OWNS_VBO | GPU_BATCH_OWNS_INDEX);
}

class VolumeTopologyGrid : Overlay {
 private:
  const SelectionType selection_type_;

  PassSimple topology_pass_ = {"topology_pass_"};

  ShapeInstanceBuf<ExtraInstanceData> voxel_buf_ = {selection_type_, "voxel_buf_"};

  Vector<std::unique_ptr<gpu::Batch, BatchDeleter>> batches_;

 public:
  VolumeTopologyGrid(const SelectionType selection_type) : selection_type_(selection_type){};

  void begin_sync(Resources &res, const State &state) final
  {
    if (!state.is_space_v3d()) {
      enabled_ = false;
      return;
    }

    if (!state.show_grid_overlay()) {
      enabled_ = false;
      return;
    }

    const StringRef grid_name = state.grid_to_show();
    if (grid_name.is_empty()) {
      enabled_ = false;
      return;
    }

    if (!(state.show_grid_root_nodes() || state.show_grid_disabled_root_nodes() ||
          state.show_grid_internal_nodes() || state.show_grid_disabled_internal_nodes() ||
          state.show_grid_leaf_nodes() || state.show_grid_disabled_leaf_nodes()))
    {
      enabled_ = false;
      return;
    }

    enabled_ = true;

    auto &pass = topology_pass_;
    pass.init();
    pass.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL,
                   state.clipping_plane_count);
    res.select_bind(pass);

    voxel_buf_.clear();
  }

  void object_sync(Manager &manager,
                   const ObjectRef &ob_ref,
                   Resources &res,
                   const State &state) final
  {
    if (!enabled_) {
      return;
    }

    const Object *eval_object = ob_ref.object;
    if (eval_object == nullptr) {
      return;
    }

    const bke::ObjectRuntime *object_runtime = eval_object->runtime;
    if (object_runtime == nullptr) {
      return;
    }

    const bke::GeometrySet *eval_geometry = object_runtime->geometry_set_eval;
    if (eval_geometry == nullptr) {
      return;
    }

    const Volume *volume = eval_geometry->get_volume();
    if (volume == nullptr) {
      return;
    }

    const StringRef grid_name = state.grid_to_show();
    const bke::VolumeGridData *grid_to_view = BKE_volume_grid_find(volume, grid_name);
    if (grid_to_view == nullptr) {
      return;
    }

    bke::VolumeTreeAccessToken token;
    const openvdb::GridBase &grid_base = grid_to_view->grid(token);
    Vector<float3> position;

    batches_.clear();

    if (state.show_grid_root_nodes()) {
      const float3 voxel_size = grid_root_tiles_positions(grid_base, position);
      if (!position.is_empty()) {
        batches_.append(std::unique_ptr<gpu::Batch, BatchDeleter>(batch_for_voxels(position.as_span(), voxel_size)));
      }
    }

    if (state.show_grid_leaf_nodes()) {
      position.clear();
      const float3 voxel_size = grid_leaf_on_positions(grid_base, position);
      if (!position.is_empty()) {
        batches_.append(std::unique_ptr<gpu::Batch, BatchDeleter>(batch_for_voxels(position.as_span(), voxel_size)));
      }
    }

    if (state.show_grid_disabled_leaf_nodes()) {
      position.clear();
      const float3 voxel_size = grid_leaf_off_positions(grid_base, position);
      if (!position.is_empty()) {
        batches_.append(std::unique_ptr<gpu::Batch, BatchDeleter>(batch_for_voxels(position.as_span(), voxel_size)));
      }
    }

    if (state.show_grid_internal_nodes()) {
      Array<float3> sizes;
      Array<Vector<float3>> position;
      grid_all_child_nodes_positions(grid_base, sizes, position);

      for (const int i : sizes.index_range()) {
        if (position[i].is_empty()) {
          continue;
        }
        batches_.append(std::unique_ptr<gpu::Batch, BatchDeleter>(batch_for_voxels(position[i].as_span(), sizes[i])));
      }
    }

    if (state.show_grid_disabled_internal_nodes()) {
      Array<float3> sizes;
      Array<Vector<float3>> position;
      grid_all_tiles_positions(grid_base, sizes, position);

      for (const int i : sizes.index_range()) {
        if (position[i].is_empty()) {
          continue;
        }
        batches_.append(std::unique_ptr<gpu::Batch, BatchDeleter>(batch_for_voxels(position[i].as_span(), sizes[i])));
      }
    }

    const float4x4 transform = BKE_volume_transform_to_blender(grid_to_view->transform());
    voxel_buf_.append({transform, float4(1.0f), 1.0f}, res.select_id(ob_ref));
  }

  void end_sync(Resources &res, const State & /*state*/) final
  {
    if (!enabled_) {
      return;
    }

    topology_pass_.shader_set(res.shaders->extra_shape.get());
    topology_pass_.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
    topology_pass_.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);

    for (const auto &item : batches_) {
      voxel_buf_.end_sync(topology_pass_, item.get());
    }
  }

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final
  {
    if (!enabled_) {
      return;
    }

    GPU_framebuffer_bind(framebuffer);
    manager.submit(topology_pass_, view);
  }
};

}  // namespace blender::draw::overlay
