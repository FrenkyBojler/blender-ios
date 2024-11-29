/* SPDX-FileCopyrightText: 2024 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "bvh/octree.h"

#include "scene/object.h"
#include "scene/volume.h"

#include "util/progress.h"

CCL_NAMESPACE_BEGIN

__forceinline int Octree::flatten_index(int x, int y, int z) const
{
  return x + resolution_ * (y + z * resolution_);
}

Extrema<float> Octree::get_extrema(const int3 index_min, const int3 index_max) const
{
  const blocked_range3d<int> range(
      index_min.x, index_max.x, 32, index_min.y, index_max.y, 32, index_min.z, index_max.z, 32);

  const Extrema<float> identity = {FLT_MAX, -FLT_MAX};

  auto reduction_func = [&](const blocked_range3d<int> &r, Extrema<float> init) -> Extrema<float> {
    for (int z = r.cols().begin(); z < r.cols().end(); ++z) {
      for (int y = r.rows().begin(); y < r.rows().end(); ++y) {
        for (int x = r.pages().begin(); x < r.pages().end(); ++x) {
          init = join(init, sigmas_[flatten_index(x, y, z)]);
        }
      }
    }
    return init;
  };

  auto join_func = [](Extrema<float> a, Extrema<float> b) -> Extrema<float> { return join(a, b); };

  return parallel_reduce(range, identity, reduction_func, join_func);
}

/* Convert from position in object space to index space. */
__forceinline float3 Octree::position_to_index(const float3 p) const
{
  return (p - root_->bbox.min) * position_to_index_scale_;
}

int3 Octree::position_to_floor_index(const float3 p) const
{
  const float3 index = floor(position_to_index(p));
  return clamp(make_int3(int(index.x), int(index.y), int(index.z)), 0, resolution_);
}

int3 Octree::position_to_ceil_index(const float3 p) const
{
  if (any_zero(position_to_index_scale_)) {
    /* Degenerate octree, force max index. */
    return make_int3(resolution_);
  }
  const float3 index = ceil(position_to_index(p));
  return clamp(make_int3(int(index.x), int(index.y), int(index.z)), 0, resolution_);
}

/* Convert from index to position in object space. */
__forceinline float3 Octree::index_to_position(int x, int y, int z) const
{
  return root_->bbox.min + make_float3(x, y, z) * index_to_position_scale_;
}

__forceinline float3 Octree::voxel_size() const
{
  return index_to_position_scale_;
}

bool Octree::should_split(std::shared_ptr<OctreeNode> &node) const
{
  const int3 index_min = position_to_floor_index(node->bbox.min);
  const int3 index_max = position_to_ceil_index(node->bbox.max);
  node->sigma = get_extrema(index_min, index_max);

  const float3 bbox_size = node->bbox.size();
  if (any_zero(bbox_size)) {
    /* Degenerate octree, can happen for implicit volume. */
    return false;
  }

  /* The threshold is set so that ideally only one sample needs to be taken per node. Value taken
   * from "Volume Rendering for Pixar's Elemental". */
  return (node->sigma.range() * len(bbox_size) * scale_ > 1.442f &&
          node->depth < VOLUME_OCTREE_MAX_DEPTH);
}

void Octree::evaluate_volume_density(Device *device,
                                     Progress &progress,
                                     const Object *object,
                                     const Shader *shader)
{
  /* For heterogeneous volume, the grid resolution is 2^max_depth in each 3D dimension;
   * for homogeneous volume, only one grid is needed. */
  resolution_ = VolumeManager::is_homogeneous_volume(object, shader) ?
                    1 :
                    1 << VOLUME_OCTREE_MAX_DEPTH;
  index_to_position_scale_ = root_->bbox.size() / float(resolution_);
  position_to_index_scale_ = safe_divide(one_float3(), index_to_position_scale_);

  /* Initialize density field. */
  /* TODO(weizhen): maybe lower the resolution depending on the object size. */
  const int size = resolution_ * resolution_ * resolution_;
  sigmas_.resize(size);
  parallel_for(0, size, [&](int i) { sigmas_[i] = {0.0f, 0.0f}; });

  /* Min and max. */
  const int num_channels = 2;

  /* TODO: Evaluate shader on device. */
}

float Octree::volume_scale(const Object *object) const
{
  if (object) {
    const Geometry *geom = object->get_geometry();
    if (geom->is_volume()) {
      const Volume *volume = static_cast<const Volume *>(geom);
      if (volume->get_object_space()) {
        /* The density changes with object scale, we scale the density accordingly in the final
         * render. */
        if (volume->transform_applied) {
          const float3 unit = normalize(one_float3());
          return 1.0f / len(transform_direction(&object->get_tfm(), unit));
        }
      }
      else {
        /* The density does not change with object scale, we scale the node in the viewport to it's
         * true size. */
        if (!volume->transform_applied) {
          const float3 unit = normalize(one_float3());
          return len(transform_direction(&object->get_tfm(), unit));
        }
      }
    }
    else {
      /* TODO(weizhen): use the maximal scale of all instances. */
    }
  }

  return 1.0f;
}

std::shared_ptr<OctreeInternalNode> Octree::make_internal(std::shared_ptr<OctreeNode> &node)
{
  num_nodes_ += 8;
  auto internal = std::make_shared<OctreeInternalNode>(*node);

  /* Create bounding boxes for children. */
  const float3 center = internal->bbox.center();
  for (int i = 0; i < 8; i++) {
    const float3 t = make_float3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
    const BoundBox bbox(mix(internal->bbox.min, center, t), mix(center, internal->bbox.max, t));
    internal->children_[i] = std::make_shared<OctreeNode>(bbox, internal->depth + 1);
  }

  return internal;
}

void Octree::recursive_build(std::shared_ptr<OctreeNode> &octree_node)
{
  if (!should_split(octree_node)) {
    return;
  }

  /* Make the current node an internal node. */
  auto internal = make_internal(octree_node);

  for (auto &child : internal->children_) {
    task_pool_.push([&] { recursive_build(child); });
  }

  octree_node = internal;
}

void Octree::flatten(KernelOctreeNode *knodes,
                     const int current_index,
                     const std::shared_ptr<OctreeNode> &node,
                     int &child_index) const
{
  KernelOctreeNode &knode = knodes[current_index];
  knode.sigma = node->sigma;

  if (auto internal_ptr = std::dynamic_pointer_cast<OctreeInternalNode>(node)) {
    knode.first_child = child_index;
    child_index += 8;
    /* Loop through all the children and flatten in breath-first manner, so that children are
     * stored in contiguous indices. */
    for (int i = 0; i < 8; i++) {
      knodes[knode.first_child + i].parent = current_index;
      flatten(knodes, knode.first_child + i, internal_ptr->children_[i], child_index);
    }
  }
  else {
    knode.first_child = -1;
  }
}

void Octree::build(Device *device, Progress &progress, const Object *object, const Shader *shader)
{
  const char *name = object ? object->get_asset_name().c_str() : "world volume";
  string status = string_printf("Evaluating density for %s", name);
  progress.set_substatus(status);

  evaluate_volume_density(device, progress, object, shader);
  if (progress.get_cancel()) {
    return;
  }

  status = string_printf("Building octree for %s", name);
  progress.set_substatus(status);

  scale_ = volume_scale(object);
  recursive_build(root_);

  task_pool_.wait_work();

  is_built_ = true;
  sigmas_.clear();
}

Octree::Octree(const BoundBox &bbox)
{
  root_ = std::make_shared<OctreeNode>(bbox, 0);
  is_built_ = false;
}

bool Octree::is_built() const
{
  return is_built_;
}

int Octree::get_num_nodes() const
{
  return num_nodes_;
}

std::shared_ptr<OctreeNode> Octree::get_root() const
{
  return root_;
}

CCL_NAMESPACE_END
