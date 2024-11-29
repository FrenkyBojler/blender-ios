/* SPDX-FileCopyrightText: 2020-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "graph/node.h"

#include "scene/mesh.h"

CCL_NAMESPACE_BEGIN

class Object;
class Octree;

class Volume : public Mesh {
 public:
  NODE_DECLARE

  Volume();

  NODE_SOCKET_API(float, clipping)
  NODE_SOCKET_API(float, step_size)
  NODE_SOCKET_API(bool, object_space)
  NODE_SOCKET_API(float, velocity_scale)

  void clear(bool preserve_shaders = false) override;
};

class VolumeManager {
 public:
  VolumeManager();
  ~VolumeManager();

  void device_update(Device *, DeviceScene *, const Scene *, Progress &);
  void device_free(DeviceScene *);

  /* Check whether the shader is a homogeneous volume. */
  static bool is_homogeneous_volume(const Object *, const Shader *);

 private:
  /* Initialize octrees from the volumes in the scene. */
  void initialize_octree(const Scene *);

  /* Build octrees based on the volume density. */
  void build_octree(Device *, Progress &);

  /* Converting the octrees into an array for uploading to the kernel. */
  void flatten_octree(DeviceScene *, const Scene *) const;

  /* Count all the nodes of the octrees. */
  int num_octree_nodes() const;

  /* One octree per object per shader. */
  std::map<std::pair<const Object *, const Shader *>, std::shared_ptr<Octree>> object_octrees_;

  bool need_rebuild_;
};

CCL_NAMESPACE_END
