/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include <memory>
#include <type_traits>

#include "BLI_timeit.hh"

#include "BLI_math_color.h"
#include "BLI_vector.hh"
#include "BLI_task.hh"
#include "BLI_task_size_hints.hh"
#include "BLI_offset_indices.hh"
#include "BLI_enumerable_thread_specific.hh"

#include "BKE_geometry_set.hh"
#include "BKE_volume_grid.hh"
#include "BKE_volume_grid_fields.hh"
#include "BKE_volume_openvdb.hh"

#include "GPU_vertex_buffer.hh"

#include "overlay_base.hh"
#include "overlay_private.hh"

#ifdef WITH_OPENVDB
#  include <openvdb/tree/NodeManager.h>
#endif

#ifdef WITH_TBB
#  if defined(WIN32) && !defined(NOMINMAX)
#    define NOMINMAX
#    define TBB_MIN_MAX_CLEANUP
#  endif
#  include <tbb/parallel_reduce.h>
#  include <tbb/parallel_for.h>
#  ifdef WIN32
#    ifdef TBB_MIN_MAX_CLEANUP
#      undef NOMINMAX
#    endif
#  endif
#endif

namespace blender::draw::overlay {

template<typename TreeType, typename Value, typename Func, typename Reduction>
static Value leaf_parallel_reduce(const openvdb::tree::LeafManager<TreeType> &manager,
                                   Value init_value,
                                   Func func,
                                   Reduction reduction)
{
  using LeafRange = typename openvdb::tree::LeafManager<TreeType>::LeafRange;
  lazy_threading::send_hint();
  return tbb::parallel_reduce(
      manager.leafRange(1),
      init_value,
      [&](const LeafRange &subrange, const Value &init_value) {
        typename LeafRange::Iterator it = subrange.begin();
        if (!it) {
          return init_value;
        }

        Value accum = func(*it, init_value);
        ++it;

        for (; it; ++it) {
          accum = func(*it, accum);
        }

        return accum;
      },
      reduction);
}

template<typename TreeType, typename Func>
static void leaf_parallel_for(const openvdb::tree::LeafManager<TreeType> &manager, Func func)
{
  using LeafRange = typename openvdb::tree::LeafManager<TreeType>::LeafRange;
  lazy_threading::send_hint();
  tbb::parallel_for(manager.leafRange(1024), [func](const LeafRange &subrange) { func(subrange); });
}

static float3 grid_leaf_on_positions(const openvdb::GridBase &grid_base,
                                     Vector<float3> &r_position)
{
  threading::EnumerableThreadSpecific<Vector<float3>> thread_results;
  
  const VolumeGridType grid_type = bke::volume_grid::get_type(grid_base);
  BKE_volume_grid_type_to_static_type(grid_type, [&] <typename GridT> () {
    using TreeT = typename GridT::TreeType;
    using RootT = typename TreeT::RootNodeType;
    using InnerT = typename RootT::ChildNodeType;
    using LeafT = typename InnerT::LeafNodeType;
    const GridT &grid = static_cast<const GridT &>(grid_base);

    openvdb::tree::LeafManager<const TreeT> leafNodes(grid.tree());
    using LeafRange = typename openvdb::tree::LeafManager<const TreeT>::LeafRange;
    leaf_parallel_for<const TreeT>(leafNodes, [&](const LeafRange &subrange) {
        Vector<float3> &new_positions = thread_results.local();

        /* This cuts 30% of the whole function execution time. */
        int total_leafs = 0;
        for (typename LeafRange::Iterator leaf_iter = subrange.begin(); leaf_iter; ++leaf_iter) {
          total_leafs++;
        }

        new_positions.reserve(new_positions.size() + total_leafs * LeafT::DIM * LeafT::DIM * LeafT::DIM);

        for (typename LeafRange::Iterator leaf_iter = subrange.begin(); leaf_iter; ++leaf_iter) {
          for (typename LeafT::ValueOnCIter iter = leaf_iter->cbeginValueOn(); iter; ++iter) {
            const openvdb::Coord centre = iter.getCoord();
            new_positions.append(float3(centre.x(), centre.y(), centre.z()));
          }
        }
      });
  });

  Vector<Vector<float3>> positions;
  Vector<int> sizes;
  for (Vector<float3> &values : thread_results) {
    positions.append(std::move(values));
    sizes.append(positions.last().size());
  }
  sizes.append(0);
  const OffsetIndices<int> offsets = offset_indices::accumulate_counts_to_offsets(sizes.as_mutable_span());

  r_position.reinitialize(offsets.total_size());
  threading::parallel_for(offsets.index_range(), 1024 * 8, [&](const IndexRange range) {
    for (const int i : range) {
      r_position.as_mutable_span().slice(offsets[i]).copy_from(positions[i].as_span());
    }
  }, threading::accumulated_task_sizes([&](const IndexRange range) { return offsets[range].size(); }));

  return float3(1.0f);
}

static float3 grid_leaf_off_positions(const openvdb::GridBase &grid_base,
                                      Vector<float3> &r_position)
{
  const VolumeGridType grid_type = bke::volume_grid::get_type(grid_base);
  BKE_volume_grid_type_to_static_type(grid_type, [&] <typename GridT> () {
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

static float3 grid_root_tiles_positions(const openvdb::GridBase &grid_base,
                                        Vector<float3> &r_position)
{
  int64_t root_tile_size = -1;
  const VolumeGridType grid_type = bke::volume_grid::get_type(grid_base);
  BKE_volume_grid_type_to_static_type(grid_type, [&] <typename GridT> () {
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
  BKE_volume_grid_type_to_static_type(grid_type, [&] <typename GridT> () {
    using TreeT = typename GridT::TreeType;
    const GridT &grid = static_cast<const GridT &>(grid_base);

    for (typename TreeT::NodeCIter iter = grid.tree().cbeginNode(); iter; ++iter) {
      const openvdb::Coord centre = iter.getCoord();
      r_position[iter.getLevel()].append(float3(centre.x(), centre.y(), centre.z()));

      const openvdb::CoordBBox node_box = iter.getBoundingBox();
      r_sizes[iter.getLevel()] = float3(
          node_box.dim().x(), node_box.dim().y(), node_box.dim().z());
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
  BKE_volume_grid_type_to_static_type(grid_type, [&] <typename GridT> () {
    using TreeT = typename GridT::TreeType;
    const GridT &grid = static_cast<const GridT &>(grid_base);

    for (typename TreeT::ValueOffCIter iter = grid.tree().beginValueOff(); iter; ++iter) {
      if (!iter.isTileValue()) {
        continue;
      }
      const openvdb::Coord centre = iter.getCoord();
      r_position[iter.getLevel()].append(float3(centre.x(), centre.y(), centre.z()));

      const openvdb::CoordBBox node_box = iter.getBoundingBox();
      r_sizes[iter.getLevel()] = float3(
          node_box.dim().x(), node_box.dim().y(), node_box.dim().z());
    }
  });
}

struct BatchDeleter {
  void operator()(gpu::Batch *shader)
  {
    GPU_BATCH_DISCARD_SAFE(shader);
  }
};

static gpu::VertBuf &dens_tiles()
{
  static const GPUVertFormat format = [&]() {
    GPUVertFormat format{};
    GPU_vertformat_attr_add(&format, "pos_scale", gpu::VertAttrType::SFLOAT_32_32_32_32);
    return format;
  }();

  gpu::VertBuf &position_buffer = *GPU_vertbuf_create_with_format_ex(format, GPU_USAGE_FLAG_BUFFER_TEXTURE_ONLY);
  GPU_vertbuf_data_alloc(position_buffer, 2);
  MutableSpan<float4> positions = position_buffer.data<float4>();
  positions[0] = float4(10, 10, 1, 0);
  positions[1] = float4(-3, 40, -33, 0);
  
  return position_buffer;
}

class VolumeTopologyGrid : Overlay {
 private:
  const SelectionType selection_type_;

  PassSimple pass_ = {"volume_grid_pass_"};
  StaticShader grid_shader_ = {"workbench_overlay_volume_grid_pipline"};

  gpu::VertBufPtr grid_positions_ = nullptr;

 public:
  VolumeTopologyGrid(const SelectionType selection_type) : selection_type_(selection_type){};

  void begin_sync(Resources &res, const State &state) final
  {
    enabled_ = true;

    pass_.init();
  }

  void object_sync(Manager &manager,
                   const ObjectRef &ob_ref,
                   Resources &res,
                   const State &state) final
  {
  }

  void end_sync(Resources &res, const State &state) final
  {
    grid_positions_ = gpu::VertBufPtr(&dens_tiles());
    pass_.shader_set(grid_shader_.get());
    pass_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL, state.clipping_plane_count);
    pass_.bind_texture("positions", grid_positions_);
    pass_.draw_procedural(GPU_PRIM_TRIS, 90, 90 * 3);
    res.select_bind(pass_);
  }

  void draw(Framebuffer &framebuffer, Manager &manager, View &view) final
  {
    pass_.framebuffer_set(&framebuffer);
    manager.submit(pass_, view);
  }
};

}  // namespace blender::draw::overlay
