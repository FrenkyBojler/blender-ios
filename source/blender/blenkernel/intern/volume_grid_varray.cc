/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef WITH_OPENVDB

#  include "BLI_virtual_array.hh"

#  include "BKE_volume_grid.hh"
#  include "BKE_volume_grid_process.hh"
#  include "BKE_volume_grid_varray.hh"

#  include <openvdb/Grid.h>

namespace blender::bke::volume_grid {

int GridNodeIndexMapping::size() const
{
  return size_;
}

template<typename NodeT> IndexRange GridNodeIndexMapping::get_node_range(const NodeT &node) const
{
  return node_ranges_.lookup({NodeT::LEVEL, node.origin()});
}

template<typename LeafNodeT>
static int gather_index_mapping_from_leaf_node(const LeafNodeT &node,
                                               const GridValueOnOff grid_value_filter,
                                               const int start,
                                               Map<GridNodeKey, IndexRange> &node_ranges)
{
  int count = 0;
  switch (grid_value_filter) {
    case GridValueOnOff::On:
      count += node.getValueMask().countOn();
      break;
    case GridValueOnOff::Off:
      count += node.getValueMask().countOff();
      break;
    case GridValueOnOff::Dense:
      count += LeafNodeT::NUM_VALUES;
      break;
  }

  node_ranges.add_new({LeafNodeT::LEVEL, node.origin()}, IndexRange(start, count));
  return count;
}

template<typename InternalNodeT>
static int gather_index_mapping_from_internal_node(const InternalNodeT &node,
                                                   const GridValueOnOff grid_value_filter,
                                                   const int start,
                                                   Map<GridNodeKey, IndexRange> &node_ranges)
{
  using ChildNodeT = typename InternalNodeT::ChildNodeType;
  using LeafNodeT = typename InternalNodeT::LeafNodeType;
  using NodeMaskT = typename InternalNodeT::NodeMaskType;
  using UnionT = typename InternalNodeT::UnionType;

  int count = 0;
  switch (grid_value_filter) {
    case GridValueOnOff::On:
      count += node.getValueMask().countOn();
      break;
    case GridValueOnOff::Off:
      count += node.getValueMask().countOff();
      break;
    case GridValueOnOff::Dense:
      count += InternalNodeT::NUM_VALUES;
      break;
  }

  const UnionT *table = node.getTable();

  const NodeMaskT &child_mask = node.getChildMask();
  auto child_mask_iter = child_mask.beginOn();
  for (; child_mask_iter.test(); ++child_mask_iter) {
    const ChildNodeT &child_node = *table[child_mask_iter.pos()].getChild();
    if constexpr (std::is_same_v<ChildNodeT, LeafNodeT>) {
      gather_index_mapping_from_leaf_node(
          child_node, grid_value_filter, start + count, node_ranges);
    }
    else {
      /* Recurse into lower-level internal nodes. */
      count += gather_index_mapping_from_internal_node(
          child_node, grid_value_filter, start + count, node_ranges);
    }
  }

  node_ranges.add_new({InternalNodeT::LEVEL, node.origin()}, IndexRange(start, count));
  return count;
}

template<typename TreeT>
static int gather_index_mapping_from_tree(const TreeT &tree,
                                          const GridValueOnOff grid_value_filter,
                                          const int start,
                                          Map<GridNodeKey, IndexRange> &node_ranges)
{
  int count = 0;
  for (auto root_child_iter = tree.cbeginRootChildren(); root_child_iter.test(); ++root_child_iter)
  {
    const auto &internal_node = *root_child_iter;
    count += gather_index_mapping_from_internal_node(
        internal_node, grid_value_filter, start + count, node_ranges);
  }
  return count;
}

std::shared_ptr<GridNodeIndexMapping> GridNodeIndexMapping::from_grid(
    const VolumeGridData &grid, const GridValueOnOff grid_value_filter)
{
  std::shared_ptr<GridNodeIndexMapping> index_mapping = std::make_shared<GridNodeIndexMapping>();

  VolumeTreeAccessToken access_token;
  const openvdb::GridBase &grid_base = grid.grid(access_token);

  to_typed_grid(grid_base, [&](const auto &grid) {
    index_mapping->size_ = gather_index_mapping_from_tree(
        grid.tree(), grid_value_filter, 0, index_mapping->node_ranges_);
  });

  return index_mapping;
}

inline IndexRange index_mask_segment_range(const IndexMaskSegment &segment)
{
  if (segment.is_empty()) {
    return {};
  }
  return IndexRange::from_begin_end_inclusive(segment[0], segment.last());
}

template<typename T>
using ForeachValueFn = FunctionRef<void(const int index,
                                        const int pos,
                                        const openvdb::CoordBBox &coord_bbox,
                                        const bool active,
                                        const T &value)>;

template<int32_t DIM, typename NodeT, typename MaskIteratorT, typename ValueBufferT>
static void foreach_value_in_mask(const IndexRange range,
                                  const NodeT &node,
                                  const IndexRange node_range,
                                  MaskIteratorT mask_iter,
                                  const ValueBufferT &value_buffer,
                                  ForeachValueFn<typename NodeT::ValueType> fn)
{
  BLI_assert(!range.is_empty());
  BLI_assert(!node_range.is_empty());

  /* Overlap of the range with the node range. */
  const IndexRange range_in_node = range.intersect(node_range);
  if (range_in_node.is_empty()) {
    return;
  }

  /* Number of values in the range that are not in the node range. */
  const int unused_in_node_range = range_in_node.start() - node_range.start();
  /* Offset of the node range relative to the index range. */
  const int pos_start = range_in_node.start() - range.start();
  /* Advance to the start of the range. */
  for ([[maybe_unused]] const int mask_i : IndexRange(unused_in_node_range)) {
    if (!mask_iter.test()) {
      /* Mask range starts after node values (only contains child node values). */
      return;
    }
    ++mask_iter;
  }
  /* Handle values in range. */
  for (const int i : range_in_node.index_range()) {
    const int index = range_in_node[i];
    if (!mask_iter.test()) {
      /* Some values in range are child node values. */
      return;
    }
    const openvdb::Index mask_pos = mask_iter.pos();
    const openvdb::Coord origin = node.offsetToGlobalCoord(mask_pos);
    const openvdb::CoordBBox bbox = openvdb::CoordBBox::createCube(origin, DIM);
    const int pos = pos_start + i;
    if constexpr (NodeT::LEVEL == 0) {
      /* Leaf node buffer. */
      fn(index, pos, bbox, *mask_iter, value_buffer.getValue(mask_pos));
    }
    else {
      /* Internal node value table. */
      fn(index, pos, bbox, *mask_iter, value_buffer[mask_pos].getValue());
    }
    ++mask_iter;
  }
}

inline IndexMaskSegment index_mask_segment_intersect(const IndexMaskSegment &segment,
                                                     const IndexRange range)
{
  const auto lower_it = std::lower_bound(segment.begin(), segment.end(), range.first());
  const auto upper_it = std::upper_bound(segment.begin(), segment.end(), range.last());
  const IndexMaskSegment segment_isect = segment.slice(IndexRange::from_begin_end(
      lower_it != segment.end() ? (lower_it - segment.begin()) : 0,
      upper_it != segment.end() ? (upper_it - segment.begin()) : segment.size()));
  return segment_isect;
}

/* Indices in mask_segment must be relative to the node range. */
template<int32_t DIM, typename NodeT, typename MaskIteratorT, typename ValueBufferT>
static void foreach_value_in_mask(const IndexMaskSegment &segment,
                                  const NodeT &node,
                                  const IndexRange node_range,
                                  MaskIteratorT mask_iter,
                                  const ValueBufferT &value_buffer,
                                  ForeachValueFn<typename NodeT::ValueType> fn)
{
  BLI_assert(!segment.is_empty());
  BLI_assert(!node_range.is_empty());

  /* Early exit to skip binary search if segment does not overlap the node index range. */
  if (segment[0] >= node_range.last() || segment.last() < node_range.first()) {
    return;
  }
  /* Overlap of the segment with the node range. */
  const IndexMaskSegment segment_in_node = index_mask_segment_intersect(segment, node_range);
  if (segment_in_node.is_empty()) {
    return;
  }

  /* Number of values in the segment that are not in the node range. */
  const int unused_in_node_range = segment_in_node[0] - node_range.start();
  /* Offset of the node range relative to the segment. */
  const int pos_start = segment_in_node[0] - segment[0];
  /* Advance to the start of the segment. */
  for ([[maybe_unused]] const int mask_i : IndexRange(unused_in_node_range)) {
    if (!mask_iter.test()) {
      /* Mask range starts after node values (only contains child node values). */
      return;
    }
    ++mask_iter;
  }
  /* Handle values in range. */
  int mask_index = segment_in_node[0];
  for (const int i : segment_in_node.index_range()) {
    /* Move up to the desired index. */
    const int index = segment_in_node[i];
    while (mask_index < index) {
      if (!mask_iter.test()) {
        /* Some values in range are child node values. */
        return;
      }
      ++mask_index;
      ++mask_iter;
    }

    if (!mask_iter.test()) {
      /* Some values in range are child node values. */
      return;
    }
    const openvdb::Index mask_pos = mask_iter.pos();
    const openvdb::Coord origin = node.offsetToGlobalCoord(mask_pos);
    const openvdb::CoordBBox bbox = openvdb::CoordBBox::createCube(origin, DIM);
    const int pos = pos_start + i;
    if constexpr (NodeT::LEVEL == 0) {
      /* Leaf node buffer. */
      fn(index, pos, bbox, *mask_iter, value_buffer.getValue(mask_pos));
    }
    else {
      /* Internal node value table. */
      fn(index, pos, bbox, *mask_iter, value_buffer[mask_pos].getValue());
    }
    ++mask_index;
    ++mask_iter;
  }
}

template<typename LeafNodeT>
static void foreach_value_in_leaf_node(const IndexRange range,
                                       const LeafNodeT &node,
                                       const IndexRange node_range,
                                       const GridValueOnOff grid_value_filter,
                                       ForeachValueFn<typename LeafNodeT::ValueType> fn)
{
  using NodeMaskT = typename LeafNodeT::NodeMaskType;

  BLI_assert(!range.intersect(node_range).is_empty());

  const NodeMaskT &value_mask = node.getValueMask();
  switch (grid_value_filter) {
    case GridValueOnOff::On:
      foreach_value_in_mask<1>(range, node, node_range, value_mask.beginOn(), node.buffer(), fn);
      break;
    case GridValueOnOff::Off:
      foreach_value_in_mask<1>(range, node, node_range, value_mask.beginOff(), node.buffer(), fn);
      break;
    case GridValueOnOff::Dense:
      foreach_value_in_mask<1>(
          range, node, node_range, value_mask.beginDense(), node.buffer(), fn);
      break;
  }
}

template<typename LeafNodeT>
static void foreach_value_in_leaf_node(const IndexMaskSegment segment,
                                       const LeafNodeT &node,
                                       const IndexRange node_range,
                                       const GridValueOnOff active_filter,
                                       ForeachValueFn<typename LeafNodeT::ValueType> fn)
{
  using NodeMaskT = typename LeafNodeT::NodeMaskType;

  BLI_assert(!index_mask_segment_range(segment).intersect(node_range).is_empty());

  const NodeMaskT &value_mask = node.getValueMask();
  switch (active_filter) {
    case GridValueOnOff::On:
      foreach_value_in_mask<1>(segment, node, node_range, value_mask.beginOn(), node.buffer(), fn);
      break;
    case GridValueOnOff::Off:
      foreach_value_in_mask<1>(
          segment, node, node_range, value_mask.beginOff(), node.buffer(), fn);
      break;
    case GridValueOnOff::Dense:
      foreach_value_in_mask<1>(
          segment, node, node_range, value_mask.beginDense(), node.buffer(), fn);
      break;
  }
}

template<typename InternalNodeT>
static void foreach_value_in_internal_node(const IndexRange range,
                                           const InternalNodeT &node,
                                           const IndexRange node_range,
                                           const GridNodeIndexMapping &index_mapping,
                                           const GridValueOnOff active_filter,
                                           ForeachValueFn<typename InternalNodeT::ValueType> fn)
{
  using ChildNodeT = typename InternalNodeT::ChildNodeType;
  using LeafNodeT = typename InternalNodeT::LeafNodeType;
  using NodeMaskT = typename InternalNodeT::NodeMaskType;
  using UnionT = typename InternalNodeT::UnionType;

  BLI_assert(!range.intersect(node_range).is_empty());

  const UnionT *table = node.getTable();

  /* Tiles. */
  const NodeMaskT &value_mask = node.getValueMask();
  switch (active_filter) {
    case GridValueOnOff::On:
      foreach_value_in_mask<ChildNodeT::DIM>(
          range, node, node_range, value_mask.beginOn(), table, fn);
      break;
    case GridValueOnOff::Off:
      foreach_value_in_mask<ChildNodeT::DIM>(
          range, node, node_range, value_mask.beginOff(), table, fn);
      break;
    case GridValueOnOff::Dense:
      foreach_value_in_mask<ChildNodeT::DIM>(
          range, node, node_range, value_mask.beginDense(), table, fn);
      break;
  }

  /* TODO [1.] */
  const NodeMaskT &child_mask = node.getChildMask();
  auto child_mask_iter = child_mask.beginOn();
  for (; child_mask_iter.test(); ++child_mask_iter) {
    const ChildNodeT &child_node = *table[child_mask_iter.pos()].getChild();
    const IndexRange child_node_range = index_mapping.get_node_range(child_node);
    if (!range.intersect(child_node_range).is_empty()) {
      /* Found start node. */
      break;
    }
  }
  for (; child_mask_iter.test(); ++child_mask_iter) {
    const ChildNodeT &child_node = *table[child_mask_iter.pos()].getChild();
    const IndexRange child_node_range = index_mapping.get_node_range(child_node);
    if (range.intersect(child_node_range).is_empty()) {
      /* Found end node. */
      break;
    }

    if constexpr (std::is_same_v<ChildNodeT, LeafNodeT>) {
      foreach_value_in_leaf_node(range, child_node, child_node_range, active_filter, fn);
    }
    else {
      /* Recurse into lower-level internal nodes. */
      foreach_value_in_internal_node(
          range, child_node, child_node_range, index_mapping, active_filter, fn);
    }
  }
}

template<typename InternalNodeT>
static void foreach_value_in_internal_node(const IndexMaskSegment &segment,
                                           const InternalNodeT &node,
                                           const IndexRange node_range,
                                           const GridNodeIndexMapping &index_mapping,
                                           const GridValueOnOff active_filter,
                                           ForeachValueFn<typename InternalNodeT::ValueType> fn)
{
  using ChildNodeT = typename InternalNodeT::ChildNodeType;
  using LeafNodeT = typename InternalNodeT::LeafNodeType;
  using NodeMaskT = typename InternalNodeT::NodeMaskType;
  using UnionT = typename InternalNodeT::UnionType;

  const IndexRange segment_range = index_mask_segment_range(segment);
  BLI_assert(!segment_range.intersect(node_range).is_empty());

  const UnionT *table = node.getTable();

  /* Tiles. */
  const NodeMaskT &value_mask = node.getValueMask();
  switch (active_filter) {
    case GridValueOnOff::On:
      foreach_value_in_mask<ChildNodeT::DIM>(
          segment, node, node_range, value_mask.beginOn(), table, fn);
      break;
    case GridValueOnOff::Off:
      foreach_value_in_mask<ChildNodeT::DIM>(
          segment, node, node_range, value_mask.beginOff(), table, fn);
      break;
    case GridValueOnOff::Dense:
      foreach_value_in_mask<ChildNodeT::DIM>(
          segment, node, node_range, value_mask.beginDense(), table, fn);
      break;
  }

  const NodeMaskT &child_mask = node.getChildMask();
  auto child_mask_iter = child_mask.beginOn();
  for (; child_mask_iter.test(); ++child_mask_iter) {
    const ChildNodeT &child_node = *table[child_mask_iter.pos()].getChild();
    const IndexRange child_node_range = index_mapping.get_node_range(child_node);
    if (!segment_range.intersect(child_node_range).is_empty()) {
      /* Found start node. */
      break;
    }
  }
  for (; child_mask_iter.test(); ++child_mask_iter) {
    const ChildNodeT &child_node = *table[child_mask_iter.pos()].getChild();
    const IndexRange child_node_range = index_mapping.get_node_range(child_node);
    if (segment_range.intersect(child_node_range).is_empty()) {
      /* Found end node. */
      break;
    }

    if constexpr (std::is_same_v<ChildNodeT, LeafNodeT>) {
      foreach_value_in_leaf_node(segment, child_node, child_node_range, active_filter, fn);
    }
    else {
      /* Recurse into lower-level internal nodes. */
      foreach_value_in_internal_node(
          segment, child_node, child_node_range, index_mapping, active_filter, fn);
    }
  }
}

template<typename TreeT>
static void foreach_value_in_tree(const IndexRange range,
                                  const TreeT tree,
                                  const GridNodeIndexMapping &index_mapping,
                                  const GridValueOnOff active_filter,
                                  ForeachValueFn<typename TreeT::ValueType> fn)
{
  if (range.is_empty()) {
    return;
  }

  /* TODO [1.] */
  auto root_child_iter = tree.cbeginRootChildren();
  for (; root_child_iter.test(); ++root_child_iter) {
    const auto &internal_node = *root_child_iter;
    const IndexRange internal_node_range = index_mapping.get_node_range(internal_node);
    if (!range.intersect(internal_node_range).is_empty()) {
      /* Found start node. */
      break;
    }
  }
  for (; root_child_iter.test(); ++root_child_iter) {
    const auto &internal_node = *root_child_iter;
    const IndexRange internal_node_range = index_mapping.get_node_range(internal_node);
    if (range.intersect(internal_node_range).is_empty()) {
      /* Found end node. */
      break;
    }

    /* Handle values in range. */
    foreach_value_in_internal_node(
        range, internal_node, internal_node_range, index_mapping, active_filter, fn);
  }
}

template<typename TreeT>
static void foreach_value_in_tree(const IndexMaskSegment &segment,
                                  const TreeT tree,
                                  const GridNodeIndexMapping &index_mapping,
                                  const GridValueOnOff active_filter,
                                  ForeachValueFn<typename TreeT::ValueType> fn)
{
  if (segment.is_empty()) {
    return;
  }

  const IndexRange segment_range = index_mask_segment_range(segment);

  /* TODO [1.] */
  auto root_child_iter = tree.cbeginRootChildren();
  for (; root_child_iter.test(); ++root_child_iter) {
    const auto &internal_node = *root_child_iter;
    const IndexRange internal_node_range = index_mapping.get_node_range(internal_node);
    if (!segment_range.intersect(internal_node_range).is_empty()) {
      /* Found start node. */
      break;
    }
  }
  for (; root_child_iter.test(); ++root_child_iter) {
    const auto &internal_node = *root_child_iter;
    const IndexRange internal_node_range = index_mapping.get_node_range(internal_node);
    if (segment_range.intersect(internal_node_range).is_empty()) {
      /* Found end node. */
      break;
    }

    /* Handle values in range. */
    foreach_value_in_internal_node(
        segment, internal_node, internal_node_range, index_mapping, active_filter, fn);
  }
}

template<typename TreeT>
static void foreach_value_in_tree(const IndexMask &index_mask,
                                  const TreeT tree,
                                  const GridNodeIndexMapping &index_mapping,
                                  const GridValueOnOff active_filter,
                                  ForeachValueFn<typename TreeT::ValueType> fn)
{
  index_mask.foreach_segment_optimized([&](const auto segment) {
    if constexpr (std::is_same_v<std::decay_t<decltype(segment)>, IndexRange>) {
      const IndexRange range = segment;
      foreach_value_in_tree(range, tree, index_mapping, active_filter, fn);
    }
    else {
      const IndexMaskSegment indices = segment;
      foreach_value_in_tree(segment, tree, index_mapping, active_filter, fn);
    }
  });
}

template<typename T, typename TreeT> class VArrayImpl_For_GridValues final : public VArrayImpl<T> {
 private:
  using TreeType = TreeT;
  using ValueType = typename TreeT::ValueType;
  using ArrayValueFn = std::function<T(
      const openvdb::CoordBBox &coord_bbox, bool active, const ValueType &grid_value)>;

  VolumeTreeAccessToken access_token_;
  std::shared_ptr<const TreeType> tree_;
  std::shared_ptr<const GridNodeIndexMapping> index_mapping_;
  GridValueOnOff grid_value_filter_;
  ArrayValueFn fn_;

 public:
  VArrayImpl_For_GridValues(VolumeTreeAccessToken &&access_token,
                            std::shared_ptr<const TreeType> tree,
                            std::shared_ptr<const GridNodeIndexMapping> index_mapping,
                            const GridValueOnOff grid_value_filter,
                            ArrayValueFn fn)
      : VArrayImpl<T>(index_mapping->size()),
        access_token_(std::move(access_token)),
        tree_(std::move(tree)),
        index_mapping_(std::move(index_mapping)),
        grid_value_filter_(grid_value_filter),
        fn_(std::move(fn))
  {
  }

  T get(const int64_t index) const override
  {
    T result;
    foreach_value_in_tree(
        IndexRange(index, 1),
        *tree_,
        *index_mapping_,
        grid_value_filter_,
        [&](const int /*index*/,
            const int /*pos*/,
            const openvdb::CoordBBox &coord_bbox,
            const bool active,
            const ValueType &value) { result = fn_(coord_bbox, active, value); });
    return result;
  }

  void materialize(const IndexMask &mask, T *dst, const bool dst_is_uninitialized) const override
  {
    const ForeachValueFn<ValueType> store_initialized = [&](const int index,
                                                            const int /*pos*/,
                                                            const openvdb::CoordBBox &coord_bbox,
                                                            const bool active,
                                                            const ValueType &value) {
      dst[index] = fn_(coord_bbox, active, value);
    };
    const ForeachValueFn<ValueType> store_uninitialized = [&](const int index,
                                                              const int /*pos*/,
                                                              const openvdb::CoordBBox &coord_bbox,
                                                              const bool active,
                                                              const ValueType &value) {
      new (dst + index) T(fn_(coord_bbox, active, value));
    };

    if constexpr (std::is_trivially_copyable_v<T>) {
      foreach_value_in_tree(mask, *tree_, *index_mapping_, grid_value_filter_, store_initialized);
    }
    else {
      if (dst_is_uninitialized) {
        foreach_value_in_tree(
            mask, *tree_, *index_mapping_, grid_value_filter_, store_uninitialized);
      }
      else {
        foreach_value_in_tree(
            mask, *tree_, *index_mapping_, grid_value_filter_, store_initialized);
      }
    }
  }

  void materialize_compressed(const IndexMask &mask,
                              T *dst,
                              const bool dst_is_uninitialized) const override
  {
    const ForeachValueFn<ValueType> store_initialized = [&](const int /*index*/,
                                                            const int pos,
                                                            const openvdb::CoordBBox &coord_bbox,
                                                            const bool active,
                                                            const ValueType &value) {
      dst[pos] = fn_(coord_bbox, active, value);
    };
    const ForeachValueFn<ValueType> store_uninitialized = [&](const int /*index*/,
                                                              const int pos,
                                                              const openvdb::CoordBBox &coord_bbox,
                                                              const bool active,
                                                              const ValueType &value) {
      new (dst + pos) T(fn_(coord_bbox, active, value));
    };

    if constexpr (std::is_trivially_copyable_v<T>) {
      foreach_value_in_tree(mask, *tree_, *index_mapping_, grid_value_filter_, store_initialized);
    }
    else {
      if (dst_is_uninitialized) {
        foreach_value_in_tree(
            mask, *tree_, *index_mapping_, grid_value_filter_, store_uninitialized);
      }
      else {
        foreach_value_in_tree(
            mask, *tree_, *index_mapping_, grid_value_filter_, store_initialized);
      }
    }
  }
};

VArray<int3> varray_for_grid_min_coordinates(const VolumeGridData &grid,
                                             std::shared_ptr<GridNodeIndexMapping> index_mapping,
                                             const GridValueOnOff grid_value_filter)
{
  VolumeTreeAccessToken access_token;
  const openvdb::GridBase &grid_base = grid.grid(access_token);

  VArray<int3> result;
  to_typed_grid(grid_base, [&](const auto &grid) {
    using GridT = std::decay_t<decltype(grid)>;
    using TreeT = typename GridT::TreeType;
    using ValueT = typename TreeT::ValueType;
    result = VArray<int3>::from<VArrayImpl_For_GridValues<int3, TreeT>>(
        std::move(access_token),
        grid.treePtr(),
        std::move(index_mapping),
        grid_value_filter,
        [&](const openvdb::CoordBBox &coord_bbox, bool /*active*/, const ValueT & /*grid_value*/) {
          return int3(coord_bbox.min().asPointer());
        });
  });
  return result;
}

}  // namespace blender::bke::volume_grid

#endif
