/* SPDX-FileCopyrightText: 2020-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "graph/node.h"

#include "scene/mesh.h"

#ifdef WITH_OPENVDB
#  include <openvdb/openvdb.h>
#endif

CCL_NAMESPACE_BEGIN

class Object;
class Octree;

class Volume : public Mesh {
 public:
  NODE_DECLARE

  Volume();

  NODE_SOCKET_API(float, clipping)
  NODE_SOCKET_API(bool, object_space)
  NODE_SOCKET_API(float, velocity_scale)

  virtual void clear(bool preserve_shaders = false) override;
};

class VolumeManager {
 public:
  VolumeManager();
  ~VolumeManager();

  void device_update(Device *, DeviceScene *, const Scene *, Progress &);
  void device_free(DeviceScene *);

  /* TODO(weizhen): check if `shader->has_volume_spatial_varying` is reliable in these cases. */
  /* TODO(weizhen): check if all cases are covered. */
  /* Tag volume octree for update when scene changes. */
  void tag_update();
  void tag_update(const Shader *shader);
  void tag_update(const Object *object, const uint32_t flag);
  void tag_update(const Geometry *geometry);

  /* Check whether the shader is a homogeneous volume. */
  static bool is_homogeneous_volume(const Object *, const Shader *);

 private:
  /* Initialize octrees from the volumes in the scene. */
  void initialize_octree_(const Scene *);

  /* Build octrees according to the volume density. */
  void build_octree_(Device *, Progress &);

  /* Converting the octrees into an array for uploading to the kernel. */
  void flatten_octree_(DeviceScene *, const Scene *) const;

  /* Count all the nodes of the octrees. */
  int num_octree_nodes_() const;

  /* When running Blender with `--verbose 5`, an octree visualization is written to `filename`,
   * which is a Python script that can be run inside Blender. */
  std::string visualize_octree_(const DeviceScene *, const char *filename) const;

  /* One octree per object per shader. */
  std::map<std::pair<const Object *, const Shader *>, std::shared_ptr<Octree>> object_octrees_;

  bool need_rebuild_;
  bool update_visualization_ = false;

#ifdef WITH_OPENVDB
  /* Create SDF grid for mesh volumes, to determine whether a certain point is in the
   * interior of the mesh. This reduces evaluation time needed for heterogeneous volume. */
  openvdb::BoolGrid::ConstPtr mesh_to_sdf_grid_(const Mesh *mesh,
                                                const Shader *shader,
                                                const float half_width);
  openvdb::BoolGrid::ConstPtr get_vdb_(const Geometry *, const Shader *) const;
  std::map<std::pair<const Geometry *, const Shader *>, openvdb::BoolGrid::ConstPtr> vdb_map_;
#endif
};

CCL_NAMESPACE_END
