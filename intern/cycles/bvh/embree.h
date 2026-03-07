/* SPDX-FileCopyrightText: 2018-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#ifdef WITH_EMBREE

#  include <embree4/rtcore.h>
#  include <embree4/rtcore_scene.h>

#  include "bvh/bvh.h"
#  include "bvh/params.h"

#  include "util/vector.h"

CCL_NAMESPACE_BEGIN

class Hair;
class Mesh;
class PointCloud;
class AreaLight;
class PointLight;
class Light;

class BVHEmbree : public BVH {
 public:
  void build(Progress &progress,
             Stats *stats,
             RTCDevice rtc_device,
             const bool rtc_device_is_sycl_ = false);
  void refit(Progress &progress);

#  if defined(WITH_EMBREE_GPU) && RTC_VERSION >= 40302
  RTCError offload_scenes_to_gpu(const vector<RTCScene> &scenes);
#  endif

  const char *get_error_string(RTCError error_code);

  RTCScene scene;

  BVHEmbree(const BVHParams &params,
            const vector<Geometry *> &geometry,
            const vector<Object *> &objects);
  ~BVHEmbree() override;

 protected:
  void add_object(Object *ob, const int i);
  void add_instance(Object *ob, const int i);
  void add_curves(const Object *ob, const Hair *hair, const int i);
  void add_points(const Object *ob, const PointCloud *pointcloud, const int i);
  void add_triangles(const Object *ob, const Mesh *mesh, const int i);
  void add_light(Object *, const Light *, const int object_id);

 private:
  void set_tri_vertex_buffer(RTCGeometry geom_id, const Mesh *mesh, const bool update);
  void set_curve_vertex_buffer(RTCGeometry geom_id, const Hair *hair, const bool update);
  void set_point_vertex_buffer(RTCGeometry geom_id,
                               const PointCloud *pointcloud,
                               const bool update);
  RTCGeometry set_light_geometry_data(Object *, const Light *, const int object_id);
  void set_quad_index_buffer(const RTCGeometry geom_id);
  void set_quad_vertex_buffer(const RTCGeometry, const AreaLight *, const bool update);
  RTCGeometry set_sphere_light_geometry_data(const Object *,
                                             const PointLight *,
                                             const int object_id);
  RTCGeometry set_point_light_geometry_data(const Object *,
                                            const PointLight *,
                                            const int object_id);

  RTCDevice rtc_device;
  bool rtc_device_is_sycl;
  enum RTCBuildQuality build_quality;
};

CCL_NAMESPACE_END

#endif /* WITH_EMBREE */
