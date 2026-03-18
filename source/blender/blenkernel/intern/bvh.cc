/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_bvh.hh"

#include "DNA_mesh_types.h"

#include "BKE_mesh.hh"
#include "BKE_mesh_runtime.hh"

#ifdef WITH_EMBREE

#  include <embree4/rtcore.h>

namespace blender::bke::bvh {

Tree::Tree() = default;

Tree::~Tree()
{
  free();
}

static void rtc_error_func(void * /*userPtr*/, RTCError /*error*/, const char * /*str*/) {}

static bool rtc_memory_monitor_func(void * /*userPtr*/, const ssize_t /*bytes*/, const bool)
{
  return true;
}

static bool rtc_progress_func(void * /*user_ptr*/, const double /*n*/)
{
  return true;
}

void Tree::free()
{
  rtcReleaseScene(rtc_scene);
  rtc_scene = nullptr;
  rtcReleaseDevice(rtc_device);
  rtc_device = nullptr;
}

struct BvhBuildContext {
  RTCDevice device;
  RTCScene scene;
  RTCBuildQuality build_quality;
};

static void add_triangles(const BvhBuildContext &ctx,
                          const int id,
                          const Span<float3> positions,
                          const Span<int> corner_verts,
                          const Span<int3> corner_tris)
{
  RTCGeometry geom_id = rtcNewGeometry(ctx.device, RTC_GEOMETRY_TYPE_TRIANGLE);
  rtcSetGeometryBuildQuality(geom_id, ctx.build_quality);

  unsigned *rtc_indices = static_cast<unsigned *>(rtcSetNewGeometryBuffer(
      geom_id, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, sizeof(int) * 3, corner_tris.size()));
  for (const int64_t i : corner_tris.index_range()) {
    rtc_indices[0] = corner_verts[corner_tris[i][0]];
    rtc_indices[1] = corner_verts[corner_tris[i][1]];
    rtc_indices[2] = corner_verts[corner_tris[i][2]];
    rtc_indices += 3;
  }

  float *rtc_verts = static_cast<float *>(rtcSetNewGeometryBuffer(
      geom_id, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, sizeof(float3), positions.size()));
  std::ranges::copy(positions.cast<float>(), rtc_verts);

  // rtcSetGeometryUserData(geom_id, (void *)prim_offset);
  // rtcSetGeometryOccludedFilterFunction(geom_id, kernel_embree_filter_occluded_func);
  // rtcSetGeometryIntersectFilterFunction(geom_id, kernel_embree_filter_intersection_func);
  // rtcSetGeometryMask(geom_id, 1);

  rtcCommitGeometry(geom_id);
  rtcAttachGeometryByID(ctx.scene, geom_id, id);
  rtcReleaseGeometry(geom_id);
}

static void add_mesh(const BvhBuildContext &ctx, const int id, const Mesh &mesh)
{
  add_triangles(ctx, id, mesh.vert_positions(), mesh.corner_verts(), mesh.corner_tris());
}

void Tree::build_single_mesh(const Mesh &mesh)
{
  this->rtc_device = rtcNewDevice("verbose=0");

  rtcSetDeviceErrorFunction(rtc_device, rtc_error_func, nullptr);
  rtcSetDeviceMemoryMonitorFunction(rtc_device, rtc_memory_monitor_func, nullptr);

  this->rtc_scene = rtcNewScene(rtc_device);
  const RTCSceneFlags scene_flags = RTC_SCENE_FLAG_ROBUST;
  rtcSetSceneFlags(rtc_scene, scene_flags);
  RTCBuildQuality build_quality = RTC_BUILD_QUALITY_MEDIUM;
  rtcSetSceneBuildQuality(rtc_scene, build_quality);

  BvhBuildContext ctx{rtc_device, rtc_scene, build_quality};

  add_mesh(ctx, 0, mesh);

  rtcSetSceneProgressMonitorFunction(rtc_scene, rtc_progress_func, nullptr);
  rtcCommitScene(rtc_scene);
}

bool Tree::ray_intersect1(const Ray &ray, RayHit &r_hit) const
{
  RTCRayHit rtc_hit;
  rtc_hit.ray.org_x = ray.origin.x;
  rtc_hit.ray.org_y = ray.origin.y;
  rtc_hit.ray.org_z = ray.origin.z;
  rtc_hit.ray.dir_x = ray.direction.x;
  rtc_hit.ray.dir_y = ray.direction.y;
  rtc_hit.ray.dir_z = ray.direction.z;
  rtc_hit.ray.tnear = ray.dist_min;
  rtc_hit.ray.tfar = ray.dist_max;
  rtc_hit.ray.time = ray.time; /* Motion blur time */
  rtc_hit.ray.mask = ray.mask;
  rtc_hit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
  rtc_hit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
  rtcIntersect1(rtc_scene, &rtc_hit);

  if (rtc_hit.hit.geomID == RTC_INVALID_GEOMETRY_ID ||
      rtc_hit.hit.primID == RTC_INVALID_GEOMETRY_ID)
  {
    return false;
  }

  r_hit.ray.origin = float3(rtc_hit.ray.org_x, rtc_hit.ray.org_y, rtc_hit.ray.org_z);
  r_hit.ray.dist_min = rtc_hit.ray.tnear;
  r_hit.ray.direction = float3(rtc_hit.ray.dir_x, rtc_hit.ray.dir_y, rtc_hit.ray.dir_z);
  r_hit.ray.time = rtc_hit.ray.time;
  r_hit.ray.dist_max = rtc_hit.ray.tfar;
  r_hit.ray.mask = rtc_hit.ray.mask;
  r_hit.ray.id = rtc_hit.ray.id;
  r_hit.ray.flags = rtc_hit.ray.flags;

  r_hit.hit.normal = float3(rtc_hit.hit.Ng_x, rtc_hit.hit.Ng_y, rtc_hit.hit.Ng_z);
  r_hit.hit.uv = float2(rtc_hit.hit.u, rtc_hit.hit.v);
  r_hit.hit.primitive_id = rtc_hit.hit.primID;
  r_hit.hit.geometry_id = rtc_hit.hit.geomID;
  static constexpr int MAX_INSTANCE_ID_COPY = std::min(Hit::MAX_INSTANCE_LEVEL,
                                                       RTC_MAX_INSTANCE_LEVEL_COUNT);
  for (int i = 0; i < MAX_INSTANCE_ID_COPY; ++i) {
    r_hit.hit.instance_id[i] = rtc_hit.hit.instID[i];
  }
  for (int i = MAX_INSTANCE_ID_COPY; i < Hit::MAX_INSTANCE_LEVEL; ++i) {
    r_hit.hit.instance_id[i] = Hit::INVALID_INSTANCE_ID;
  }

  return true;
}

}  // namespace blender::bke::bvh

#else /* WITH_BVH_EMBREE */

namespace blender::bke::bvh {

Tree::Tree() {}

Tree::~Tree() {}

void Tree::free() {}

void Tree::build_single_mesh(const Mesh &mesh)
{
  UNUSED_VARS(mesh);
}

bool Tree::ray_intersect1(const Ray &ray, Hit &r_hit) const
{
  UNUSED_VARS(ray, r_hit);
  return false;
}

}  // namespace blender::bke::bvh

#endif /* WITH_BVH_EMBREE */
