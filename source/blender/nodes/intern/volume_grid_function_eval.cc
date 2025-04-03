/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "FN_multi_function.hh"

#include "BKE_anonymous_attribute_make.hh"
#include "BKE_geometry_fields.hh"
#include "BKE_node.hh"
#include "BKE_node_socket_value.hh"
#include "BKE_volume_grid.hh"
#include "BKE_volume_openvdb.hh"

#include <fmt/format.h>
#include <openvdb/Grid.h>
#include <openvdb/math/Transform.h>
#include <openvdb/tools/Merge.h>

// #define DEBUG_TIME
#ifdef DEBUG_TIME
#  include "BLI_timeit.hh"
#endif

#include "volume_grid_function_eval.hh"

namespace blender::nodes {

template<typename GridT>
static constexpr bool is_supported_grid_type = is_same_any_v<GridT,
                                                             openvdb::FloatGrid,
                                                             openvdb::Vec3fGrid,
                                                             openvdb::BoolGrid,
                                                             openvdb::Int32Grid,
                                                             openvdb::Vec4fGrid>;

template<typename Fn> static void to_typed_grid(const openvdb::GridBase &grid_base, Fn &&fn)
{
  const VolumeGridType grid_type = bke::volume_grid::get_type(grid_base);
  BKE_volume_grid_type_to_static_type(grid_type, [&](auto type_tag) {
    using GridT = typename decltype(type_tag)::type;
    if constexpr (is_supported_grid_type<GridT>) {
      fn(static_cast<const GridT &>(grid_base));
    }
    else {
      BLI_assert_unreachable();
    }
  });
}

template<typename Fn> static void to_typed_grid(openvdb::GridBase &grid_base, Fn &&fn)
{
  const VolumeGridType grid_type = bke::volume_grid::get_type(grid_base);
  BKE_volume_grid_type_to_static_type(grid_type, [&](auto type_tag) {
    using GridT = typename decltype(type_tag)::type;
    if constexpr (is_supported_grid_type<GridT>) {
      fn(static_cast<GridT &>(grid_base));
    }
    else {
      BLI_assert_unreachable();
    }
  });
}

using LeafNodeMask = openvdb::util::NodeMask<3u>;
using GetVoxelsFn = FunctionRef<void(MutableSpan<openvdb::Coord> r_voxels)>;
using ProcessLeafFn = FunctionRef<void(const LeafNodeMask &leaf_node_mask,
                                       const openvdb::CoordBBox &leaf_bbox,
                                       GetVoxelsFn get_voxels_fn)>;
using ProcessTilesFn = FunctionRef<void(Span<openvdb::CoordBBox> tiles)>;
using ProcessVoxelsFn = FunctionRef<void(Span<openvdb::Coord> voxels)>;

class VoxelFieldContext : public fn::FieldContext {
 private:
  const openvdb::math::Transform &transform_;
  Span<openvdb::Coord> voxels_;

 public:
  VoxelFieldContext(const openvdb::math::Transform &transform, const Span<openvdb::Coord> voxels)
      : transform_(transform), voxels_(voxels)
  {
  }

  GVArray get_varray_for_input(const fn::FieldInput &field_input,
                               const IndexMask & /*mask*/,
                               ResourceScope & /*scope*/) const override
  {
    const bke::AttributeFieldInput *attribute_field_input =
        dynamic_cast<const bke::AttributeFieldInput *>(&field_input);
    if (attribute_field_input == nullptr) {
      return {};
    }
    if (attribute_field_input->attribute_name() != "position") {
      return {};
    }

    Array<float3> positions(voxels_.size());
    threading::parallel_for(positions.index_range(), 1024, [&](const IndexRange range) {
      for (const int64_t i : range) {
        const openvdb::Coord &voxel = voxels_[i];
        const openvdb::Vec3d center = transform_.indexToWorld(voxel);
        positions[i] = float3(center.x(), center.y(), center.z());
      }
    });
    return VArray<float3>::ForContainer(std::move(positions));
  }
};

class TilesFieldContext : public fn::FieldContext {
 private:
  const openvdb::math::Transform &transform_;
  Span<openvdb::CoordBBox> tiles_;

 public:
  TilesFieldContext(const openvdb::math::Transform &transform,
                    const Span<openvdb::CoordBBox> tiles)
      : transform_(transform), tiles_(tiles)
  {
  }

  GVArray get_varray_for_input(const fn::FieldInput &field_input,
                               const IndexMask & /*mask*/,
                               ResourceScope & /*scope*/) const override
  {
    const bke::AttributeFieldInput *attribute_field_input =
        dynamic_cast<const bke::AttributeFieldInput *>(&field_input);
    if (attribute_field_input == nullptr) {
      return {};
    }
    if (attribute_field_input->attribute_name() != "position") {
      return {};
    }

    Array<float3> positions(tiles_.size());
    threading::parallel_for(positions.index_range(), 1024, [&](const IndexRange range) {
      for (const int64_t i : range) {
        const openvdb::CoordBBox &tile = tiles_[i];
        const openvdb::Vec3d center = transform_.indexToWorld(tile.getCenter());
        positions[i] = float3(center.x(), center.y(), center.z());
      }
    });
    return VArray<float3>::ForContainer(std::move(positions));
  }
};

template<typename LeafNodeT>
static void parallel_grid_topology_tasks_leaf_node(const LeafNodeT &node,
                                                   const ProcessLeafFn process_leaf_fn,
                                                   Vector<openvdb::Coord, 1024> &r_coords)
{
  using NodeMaskT = typename LeafNodeT::NodeMaskType;

  const int on_count = node.onVoxelCount();
  const int on_count_threshold = 50;
  if (on_count >= on_count_threshold) {
    const NodeMaskT &value_mask = node.getValueMask();
    const openvdb::CoordBBox bbox = node.getNodeBoundingBox();
    process_leaf_fn(value_mask, bbox, [&](MutableSpan<openvdb::Coord> r_voxels) {
      for (auto value_iter = node.cbeginValueOn(); value_iter.test(); ++value_iter) {
        r_voxels[value_iter.pos()] = value_iter.getCoord();
      }
    });
  }
  else {
    for (auto value_iter = node.cbeginValueOn(); value_iter.test(); ++value_iter) {
      const openvdb::Coord coord = value_iter.getCoord();
      r_coords.append(coord);
    }
  }
}

template<typename InternalNodeT>
static void parallel_grid_topology_tasks_internal_node(const InternalNodeT &node,
                                                       const ProcessLeafFn process_leaf_fn,
                                                       const ProcessVoxelsFn process_voxels_fn,
                                                       const ProcessTilesFn process_tiles_fn)
{
  using ChildNodeT = typename InternalNodeT::ChildNodeType;
  using LeafNodeT = typename InternalNodeT::LeafNodeType;
  using NodeMaskT = typename InternalNodeT::NodeMaskType;
  using UnionT = typename InternalNodeT::UnionType;

  const NodeMaskT &child_mask = node.getChildMask();
  const UnionT *table = node.getTable();

  Vector<int, 512> child_indices;
  for (auto child_mask_iter = child_mask.beginOn(); child_mask_iter.test(); ++child_mask_iter) {
    child_indices.append(child_mask_iter.pos());
  }

  threading::parallel_for(child_indices.index_range(), 8, [&](const IndexRange range) {
    Vector<openvdb::Coord, 1024> gathered_voxels;
    for (const int child_index : child_indices.as_span().slice(range)) {
      const ChildNodeT &child = *table[child_index].getChild();
      if constexpr (std::is_same_v<ChildNodeT, LeafNodeT>) {
        parallel_grid_topology_tasks_leaf_node(child, process_leaf_fn, gathered_voxels);
        if (gathered_voxels.size() >= 512) {
          process_voxels_fn(gathered_voxels);
          gathered_voxels.clear();
        }
      }
      else {
        parallel_grid_topology_tasks_internal_node(
            child, process_leaf_fn, process_voxels_fn, process_tiles_fn);
      }
    }
    if (!gathered_voxels.is_empty()) {
      process_voxels_fn(gathered_voxels);
      gathered_voxels.clear();
    }
  });

  const NodeMaskT &value_mask = node.getValueMask();
  Vector<openvdb::CoordBBox> tile_bboxes;
  for (auto value_mask_iter = value_mask.beginOn(); value_mask_iter.test(); ++value_mask_iter) {
    const openvdb::Index32 index = value_mask_iter.pos();
    const openvdb::Coord tile_origin = node.offsetToGlobalCoord(index);
    const openvdb::CoordBBox tile_bbox = openvdb::CoordBBox::createCube(tile_origin,
                                                                        ChildNodeT::DIM);
    tile_bboxes.append(tile_bbox);
  }
  if (!tile_bboxes.is_empty()) {
    process_tiles_fn(tile_bboxes);
  }
}

static void parallel_grid_topology_tasks(const openvdb::MaskTree &mask_tree,
                                         const ProcessLeafFn process_leaf_fn,
                                         const ProcessVoxelsFn process_voxels_fn,
                                         const ProcessTilesFn process_tiles_fn)
{
#ifdef DEBUG_TIME
  SCOPED_TIMER(__func__);
#endif
  for (auto root_child_iter = mask_tree.cbeginRootChildren(); root_child_iter.test();
       ++root_child_iter)
  {
    const auto &internal_node = *root_child_iter;
    parallel_grid_topology_tasks_internal_node(
        internal_node, process_leaf_fn, process_voxels_fn, process_tiles_fn);
  }
}

BLI_NOINLINE static void process_leaf_node(const mf::MultiFunction &fn,
                                           const Span<bke::SocketValueVariant *> input_values,
                                           const Span<bke::SocketValueVariant *> output_values,
                                           const Span<const openvdb::GridBase *> input_grids,
                                           MutableSpan<openvdb::GridBase::Ptr> output_grids,
                                           const openvdb::math::Transform &transform,
                                           const LeafNodeMask &leaf_node_mask,
                                           const openvdb::CoordBBox &leaf_bbox,
                                           const GetVoxelsFn get_voxels_fn)
{
  IndexMaskMemory memory;
  const IndexMask index_mask = IndexMask::from_predicate(
      IndexRange(LeafNodeMask::SIZE), GrainSize(LeafNodeMask::SIZE), memory, [&](const int64_t i) {
        return leaf_node_mask.isOn(i);
      });

  AlignedBuffer<8192, 8> allocation_buffer;
  ResourceScope scope;
  scope.linear_allocator().provide_buffer(allocation_buffer);
  mf::ParamsBuilder params{fn, &index_mask};
  mf::ContextBuilder context;

  const openvdb::Coord any_voxel_in_leaf = leaf_bbox.min();

  for (const int input_i : input_values.index_range()) {
    const bke::SocketValueVariant &value_variant = *input_values[input_i];
    if (const openvdb::GridBase *grid_base = input_grids[input_i]) {
      to_typed_grid(*grid_base, [&](const auto &grid) {
        using GridT = typename std::decay_t<decltype(grid)>;
        using ValueT = typename GridT::ValueType;
        using BlenderValueT = typename bke::BlenderTypeByOpenvdb<ValueT>;
        const auto &tree = grid.tree();

        if (const auto *leaf_node = tree.probeLeaf(any_voxel_in_leaf)) {
          const Span values = {leaf_node->buffer().data(), LeafNodeMask::SIZE};
          const LeafNodeMask &input_leaf_mask = leaf_node->valueMask();
          const LeafNodeMask missing_mask = leaf_node_mask & !input_leaf_mask;
          if (missing_mask.isOff()) {
            /* All values availables. */
            params.add_readonly_single_input(values.template cast<BlenderValueT>());
          }
          else {
            /* TODO: Sometimes it may be guaranteed that the background values are set
             * correctly. */
            MutableSpan copied_values = scope.linear_allocator().construct_array_copy(values);
            const auto &background = tree.background();
            for (auto missing_it = missing_mask.beginOn(); missing_it.test(); ++missing_it) {
              const int index = missing_it.pos();
              copied_values[index] = background;
            }
            params.add_readonly_single_input(
                copied_values.as_span().template cast<BlenderValueT>());
          }
        }
        else {
          const auto &single_value = *reinterpret_cast<const BlenderValueT *>(
              &tree.getValue(any_voxel_in_leaf));
          params.add_readonly_single_input_value(single_value);
        }
      });
    }
    else if (value_variant.is_context_dependent_field()) {
      const fn::GField field = value_variant.get<fn::GField>();
      const CPPType &type = field.cpp_type();
      Array<openvdb::Coord> voxels(index_mask.min_array_size());
      get_voxels_fn(voxels);
      VoxelFieldContext field_context{transform, voxels};
      fn::FieldEvaluator evaluator{field_context, &index_mask};
      GMutableSpan values{
          type,
          scope.linear_allocator().allocate(voxels.size() * type.size(), type.alignment()),
          voxels.size()};
      evaluator.add_with_destination(field, values);
      evaluator.evaluate();
      params.add_readonly_single_input(values);
    }
    else {
      params.add_readonly_single_input(value_variant.get_single_ptr());
    }
  }

  for (const int output_i : output_values.index_range()) {
    openvdb::GridBase &grid_base = *output_grids[output_i];
    to_typed_grid(grid_base, [&](auto &grid) {
      using GridT = typename std::decay_t<decltype(grid)>;
      using ValueT = typename GridT::ValueType;
      using BlenderValueT = typename bke::BlenderTypeByOpenvdb<ValueT>;

      auto &tree = grid.tree();
      auto *leaf_node = tree.probeLeaf(any_voxel_in_leaf);
      /* Should have been added before. */
      BLI_assert(leaf_node);
      MutableSpan values = {leaf_node->buffer().data(), LeafNodeMask::SIZE};
      params.add_uninitialized_single_output(values.template cast<BlenderValueT>());
    });
  }

  fn.call_auto(index_mask, params, context);
}

BLI_NOINLINE static void process_voxels(const mf::MultiFunction &fn,
                                        const Span<bke::SocketValueVariant *> input_values,
                                        const Span<bke::SocketValueVariant *> output_values,
                                        const Span<const openvdb::GridBase *> input_grids,
                                        MutableSpan<openvdb::GridBase::Ptr> output_grids,
                                        const openvdb::math::Transform &transform,
                                        const Span<openvdb::Coord> voxels)
{
  const int64_t voxels_num = voxels.size();
  const IndexMask index_mask{voxels_num};
  AlignedBuffer<8192, 8> allocation_buffer;
  ResourceScope scope;
  scope.linear_allocator().provide_buffer(allocation_buffer);
  mf::ParamsBuilder params{fn, &index_mask};
  mf::ContextBuilder context;

  for (const int input_i : input_values.index_range()) {
    const bke::SocketValueVariant &value_variant = *input_values[input_i];
    if (const openvdb::GridBase *grid_base = input_grids[input_i]) {
      to_typed_grid(*grid_base, [&](const auto &grid) {
        using ValueType = typename std::decay_t<decltype(grid)>::ValueType;
        const auto &tree = grid.tree();
        auto accessor = grid.getConstUnsafeAccessor();

        MutableSpan<ValueType> values = scope.linear_allocator().allocate_array<ValueType>(
            voxels_num);
        for (const int64_t i : IndexRange(voxels_num)) {
          const openvdb::Coord &coord = voxels[i];
          values[i] = tree.getValue(coord, accessor);
        }
        params.add_readonly_single_input(
            Span<ValueType>(values).template cast<bke::BlenderTypeByOpenvdb<ValueType>>());
      });
    }
    else if (value_variant.is_context_dependent_field()) {
      const fn::GField field = value_variant.get<fn::GField>();
      const CPPType &type = field.cpp_type();
      VoxelFieldContext field_context{transform, voxels};
      fn::FieldEvaluator evaluator{field_context, voxels_num};
      GMutableSpan values{
          type,
          scope.linear_allocator().allocate(voxels_num * type.size(), type.alignment()),
          voxels_num};
      evaluator.add_with_destination(field, values);
      evaluator.evaluate();
      params.add_readonly_single_input(values);
    }
    else {
      params.add_readonly_single_input(value_variant.get_single_ptr());
    }
  }

  for ([[maybe_unused]] const int output_i : output_values.index_range()) {
    const int param_index = input_values.size() + output_i;
    const CPPType &type = fn.param_type(param_index).data_type().single_type();
    void *buffer = scope.linear_allocator().allocate(voxels_num * type.size(), type.alignment());
    params.add_uninitialized_single_output(GMutableSpan{type, buffer, voxels_num});
  }

  fn.call_auto(index_mask, params, context);

  for (const int output_i : output_values.index_range()) {
    openvdb::GridBase &grid_base = *output_grids[output_i];
    to_typed_grid(grid_base, [&](auto &grid) {
      using GridT = std::decay_t<decltype(grid)>;
      using ValueType = typename GridT::ValueType;
      const int param_index = input_values.size() + output_i;
      const ValueType *computed_values = static_cast<const ValueType *>(
          params.computed_array(param_index).data());

      auto accessor = grid.getUnsafeAccessor();
      for (const int64_t i : IndexRange(voxels_num)) {
        const openvdb::Coord &coord = voxels[i];
        const ValueType &value = computed_values[i];
        accessor.setValue(coord, value);
      }
    });
  }
}

BLI_NOINLINE static void process_tiles(const mf::MultiFunction &fn,
                                       const Span<bke::SocketValueVariant *> input_values,
                                       const Span<bke::SocketValueVariant *> output_values,
                                       const Span<const openvdb::GridBase *> input_grids,
                                       MutableSpan<openvdb::GridBase::Ptr> output_grids,
                                       const openvdb::math::Transform &transform,
                                       const Span<openvdb::CoordBBox> tiles)
{
  const int64_t tiles_num = tiles.size();
  const IndexMask index_mask{tiles_num};

  AlignedBuffer<8192, 8> allocation_buffer;
  ResourceScope scope;
  scope.linear_allocator().provide_buffer(allocation_buffer);
  mf::ParamsBuilder params{fn, &index_mask};
  mf::ContextBuilder context;

  for (const int input_i : input_values.index_range()) {
    const bke::SocketValueVariant &value_variant = *input_values[input_i];
    if (const openvdb::GridBase *grid_base = input_grids[input_i]) {
      to_typed_grid(*grid_base, [&](const auto &grid) {
        using GridT = std::decay_t<decltype(grid)>;
        using ValueType = typename GridT::ValueType;
        const auto &tree = grid.tree();
        auto accessor = grid.getConstUnsafeAccessor();

        MutableSpan<ValueType> values = scope.linear_allocator().allocate_array<ValueType>(
            tiles_num);
        for (const int64_t i : IndexRange(tiles_num)) {
          const openvdb::CoordBBox &tile = tiles[i];
          const openvdb::Coord coord_in_tile = tile.min();
          values[i] = tree.getValue(coord_in_tile, accessor);
        }
        params.add_readonly_single_input(
            values.template cast<bke::BlenderTypeByOpenvdb<ValueType>>().as_span());
      });
    }
    else if (value_variant.is_context_dependent_field()) {
      const fn::GField field = value_variant.get<fn::GField>();
      const CPPType &type = field.cpp_type();
      TilesFieldContext field_context{transform, tiles};
      fn::FieldEvaluator evaluator{field_context, tiles_num};
      GMutableSpan values{
          type,
          scope.linear_allocator().allocate(tiles_num * type.size(), type.alignment()),
          tiles_num};
      evaluator.add_with_destination(field, values);
      evaluator.evaluate();
      params.add_readonly_single_input(values);
    }
    else {
      params.add_readonly_single_input(value_variant.get_single_ptr());
    }
  }

  for ([[maybe_unused]] const int output_i : output_values.index_range()) {
    const int param_index = input_values.size() + output_i;
    const CPPType &type = fn.param_type(param_index).data_type().single_type();
    void *buffer = scope.linear_allocator().allocate(tiles_num * type.size(), type.alignment());
    params.add_uninitialized_single_output(GMutableSpan{type, buffer, tiles_num});
  }

  fn.call_auto(index_mask, params, context);

  for (const int output_i : output_values.index_range()) {
    const int param_index = input_values.size() + output_i;
    openvdb::GridBase &grid_base = *output_grids[output_i];
    to_typed_grid(grid_base, [&](auto &grid) {
      using GridT = typename std::decay_t<decltype(grid)>;
      using TreeT = typename GridT::TreeType;
      using ValueType = typename GridT::ValueType;
      auto &tree = grid.tree();

      const ValueType *computed_values = static_cast<const ValueType *>(
          params.computed_array(param_index).data());

      const auto set_tile_value =
          [&](auto &node, const openvdb::Coord &coord_in_tile, auto value) {
            const openvdb::Index n = node.coordToOffset(coord_in_tile);
            BLI_assert(node.isChildMaskOff(n));
            /* TODO: Figure out how to do this without const_cast, although the same is done in
             * `openvdb_ax/openvdb_ax/compiler/VolumeExecutable.cc` which has a similar purpose. */
            using UnionType = typename std::decay_t<decltype(node)>::UnionType;
            auto *table = const_cast<UnionType *>(node.getTable());
            table[n].setValue(value);
          };

      for (const int i : IndexRange(tiles_num)) {
        const openvdb::CoordBBox tile = tiles[i];
        const openvdb::Coord coord_in_tile = tile.min();
        const auto &computed_value = computed_values[i];
        using InternalNode1 = typename TreeT::RootNodeType::ChildNodeType;
        using InternalNode2 = typename InternalNode1::ChildNodeType;
        if (auto *node = tree.template probeNode<InternalNode2>(coord_in_tile)) {
          set_tile_value(*node, coord_in_tile, computed_value);
        }
        else if (auto *node = tree.template probeNode<InternalNode1>(coord_in_tile)) {
          set_tile_value(*node, coord_in_tile, computed_value);
        }
        else {
          BLI_assert_unreachable();
        }
      }
    });
  }
}

void execute_multi_function_on_value_variant__volume_grid(
    const mf::MultiFunction &fn,
    const Span<bke::SocketValueVariant *> input_values,
    const Span<bke::SocketValueVariant *> output_values)
{
#ifdef DEBUG_TIME
  SCOPED_TIMER(__func__);
#endif
  const int inputs_num = input_values.size();
  Array<bke::VolumeTreeAccessToken> input_volume_tokens(inputs_num);
  Array<const openvdb::GridBase *> input_grids(inputs_num, nullptr);

  for (const int input_i : IndexRange(inputs_num)) {
    bke::SocketValueVariant &value_variant = *input_values[input_i];
    if (value_variant.is_volume_grid()) {
      const bke::GVolumeGrid g_volume_grid = value_variant.get<bke::GVolumeGrid>();
      input_grids[input_i] = &g_volume_grid->grid(input_volume_tokens[input_i]);
    }
    else if (value_variant.is_context_dependent_field()) {
      /* Nothing to do here. */
    }
    else {
      value_variant.convert_to_single();
    }
  }

  bool has_incompatible_transforms = false;
  const openvdb::math::Transform *transform = nullptr;
  for (const openvdb::GridBase *grid : input_grids) {
    if (!grid) {
      continue;
    }
    const openvdb::math::Transform &other_transform = grid->transform();
    if (!transform) {
      transform = &other_transform;
      continue;
    }
    if (*transform != other_transform) {
      has_incompatible_transforms = true;
      break;
    }
  }
  BLI_assert(transform != nullptr);

  if (has_incompatible_transforms) {
    /* TODO */
    BLI_assert_unreachable();
    return;
  }

  openvdb::MaskTree mask_tree;
  {
#ifdef DEBUG_TIME
    SCOPED_TIMER("create_mask_tree");
#endif
    for (const openvdb::GridBase *grid : input_grids) {
      if (!grid) {
        continue;
      }
      to_typed_grid(*grid, [&](const auto &grid) { mask_tree.topologyUnion(grid.tree()); });
    }
  }

  Array<openvdb::GridBase::Ptr> output_grids(output_values.size());
  {
#ifdef DEBUG_TIME
    SCOPED_TIMER("create_output_grids");
#endif
    for (const int i : output_values.index_range()) {
      const int param_index = input_values.size() + i;
      const CPPType &cpp_type = fn.param_type(param_index).data_type().single_type();
      /* TODO: Cleanup and error handling. */
      const VolumeGridType grid_type = *bke::socket_type_to_grid_type(
          *bke::geo_nodes_base_cpp_type_to_socket_type(cpp_type));

      openvdb::GridBase::Ptr grid;
      BKE_volume_grid_type_to_static_type(grid_type, [&](auto type_tag) {
        using GridT = typename decltype(type_tag)::type;
        using TreeT = typename GridT::TreeType;
        using ValueType = typename TreeT::ValueType;
        const ValueType background{};
        auto tree = std::make_shared<TreeT>(mask_tree, background, openvdb::TopologyCopy());
        grid = openvdb::createGrid(std::move(tree));
      });

      grid->setTransform(transform->copy());
      output_grids[i] = std::move(grid);
    }
  }

  parallel_grid_topology_tasks(
      mask_tree,
      [&](const LeafNodeMask &leaf_node_mask,
          const openvdb::CoordBBox &leaf_bbox,
          const GetVoxelsFn get_voxels_fn) {
        process_leaf_node(fn,
                          input_values,
                          output_values,
                          input_grids,
                          output_grids,
                          *transform,
                          leaf_node_mask,
                          leaf_bbox,
                          get_voxels_fn);
      },
      [&](const Span<openvdb::Coord> voxels) {
        process_voxels(
            fn, input_values, output_values, input_grids, output_grids, *transform, voxels);
      },
      [&](const Span<openvdb::CoordBBox> tiles) {
        process_tiles(
            fn, input_values, output_values, input_grids, output_grids, *transform, tiles);
      });

  for (const int i : output_values.index_range()) {
    /* TODO: Don't compute unused outputs. */
    if (bke::SocketValueVariant *output_value = output_values[i]) {
      output_value->set(bke::GVolumeGrid(std::move(output_grids[i])));
    }
  }
}

}  // namespace blender::nodes
