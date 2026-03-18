/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_bvh.hh"

#include "BLI_assert.h"

#include "DNA_mesh_types.h"

#include "BKE_mesh.hh"
#include "BKE_mesh_runtime.hh"

#ifdef WITH_EMBREE

#  include <embree3/rtcore.h>
#  include <embree3/rtcore_geometry.h>
#  include <embree3/rtcore_ray.h>
#  include <embree3/rtcore_scene.h>

namespace blender::bke::bvh {

Tree::Tree() {}

Tree::~Tree()
{
  free();
}

static void rtc_error_func(void *, enum RTCError, const char *str) {}

static bool rtc_memory_monitor_func(void *userPtr, const ssize_t bytes, const bool)
{
  return true;
}

static bool rtc_progress_func(void *user_ptr, const double n)
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

namespace {

struct BvhBuildContext {
  RTCDevice device;
  RTCScene scene;
  RTCBuildQuality build_quality;
};

void set_tri_vertex_buffer(RTCGeometry geom_id, Span<float3> positions, const bool update)
{
  // const Attribute *attr_mP = NULL;
  size_t num_motion_steps = 1;
  // int t_mid = 0;
  // if (mesh->has_motion_blur()) {
  //   attr_mP = mesh->attributes.find(ATTR_STD_MOTION_VERTEX_POSITION);
  //   if (attr_mP) {
  //     num_motion_steps = mesh->get_motion_steps();
  //     t_mid = (num_motion_steps - 1) / 2;
  //     if (num_motion_steps > RTC_MAX_TIME_STEP_COUNT) {
  //       assert(0);
  //       num_motion_steps = RTC_MAX_TIME_STEP_COUNT;
  //     }
  //   }
  // }

  const int num_verts = positions.size();
  for (int t = 0; t < num_motion_steps; ++t) {
    const float3 *verts;
    // if (t == t_mid) {
    verts = positions.data();
    //}
    // else {
    //  int t_ = (t > t_mid) ? (t - 1) : t;
    //  verts = &attr_mP->data_float3()[t_ * num_verts];
    //}

    float *rtc_verts = (update) ?
                           (float *)rtcGetGeometryBufferData(geom_id, RTC_BUFFER_TYPE_VERTEX, t) :
                           (float *)rtcSetNewGeometryBuffer(geom_id,
                                                            RTC_BUFFER_TYPE_VERTEX,
                                                            t,
                                                            RTC_FORMAT_FLOAT3,
                                                            sizeof(float) * 3,
                                                            num_verts);

    BLI_assert(rtc_verts);
    if (rtc_verts) {
      for (size_t j = 0; j < num_verts; ++j) {
        rtc_verts[0] = verts[j].x;
        rtc_verts[1] = verts[j].y;
        rtc_verts[2] = verts[j].z;
        rtc_verts += 3;
      }
    }

    if (update) {
      rtcUpdateGeometryBuffer(geom_id, RTC_BUFFER_TYPE_VERTEX, t);
    }
  }
}

void add_triangles(BvhBuildContext ctx,
                   int id,
                   Span<float3> positions,
                   Span<int> corner_verts,
                   Span<int3> corner_tris)
{
  // size_t prim_offset = mesh->prim_offset;

  // const Attribute *attr_mP = NULL;
  // size_t num_motion_steps = 1;
  // if (mesh->has_motion_blur()) {
  //   attr_mP = mesh->attributes.find(ATTR_STD_MOTION_VERTEX_POSITION);
  //   if (attr_mP) {
  //     num_motion_steps = mesh->get_motion_steps();
  //   }
  // }

  // assert(num_motion_steps <= RTC_MAX_TIME_STEP_COUNT);
  // num_motion_steps = min(num_motion_steps, (size_t)RTC_MAX_TIME_STEP_COUNT);

  RTCGeometry geom_id = rtcNewGeometry(ctx.device, RTC_GEOMETRY_TYPE_TRIANGLE);
  rtcSetGeometryBuildQuality(geom_id, ctx.build_quality);
  // rtcSetGeometryTimeStepCount(geom_id, num_motion_steps);

  unsigned *rtc_indices = static_cast<unsigned *>(rtcSetNewGeometryBuffer(
      geom_id, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, sizeof(int) * 3, corner_tris.size()));
  BLI_assert(rtc_indices);
  // if (!rtc_indices) {
  //   VLOG_WARNING << "Embree could not create new geometry buffer for mesh " <<
  //   mesh->name.c_str()
  //                << ".\n";
  //   return;
  // }
  for (const int64_t i : corner_tris.index_range()) {
    rtc_indices[0] = corner_verts[corner_tris[i][0]];
    rtc_indices[1] = corner_verts[corner_tris[i][1]];
    rtc_indices[2] = corner_verts[corner_tris[i][2]];
    rtc_indices += 3;
  }

  set_tri_vertex_buffer(geom_id, positions, false);

  // rtcSetGeometryUserData(geom_id, (void *)prim_offset);
  // rtcSetGeometryOccludedFilterFunction(geom_id, kernel_embree_filter_occluded_func);
  // rtcSetGeometryIntersectFilterFunction(geom_id, kernel_embree_filter_intersection_func);
  // rtcSetGeometryMask(geom_id, 1);

  rtcCommitGeometry(geom_id);
  rtcAttachGeometryByID(ctx.scene, geom_id, id);
  rtcReleaseGeometry(geom_id);
}

void add_mesh(BvhBuildContext ctx, int id, const Mesh &mesh)
{
  const MLoopTri *looptri = BKE_mesh_runtime_looptri_ensure(&mesh);
  int looptri_len = BKE_mesh_runtime_looptri_len(&mesh);
  if (looptri_len == 0) {
    return;
  }

  add_triangles(
      ctx, id, mesh.vert_positions(), mesh.corner_verts(), Span<MLoopTri>(looptri, looptri_len));
}

}  // namespace

void Tree::build_single_mesh(const Mesh &mesh)
{
  rtc_device = rtcNewDevice("verbose=0");
  BLI_assert(rtc_device);

  rtcSetDeviceErrorFunction(rtc_device, rtc_error_func, nullptr);
  rtcSetDeviceMemoryMonitorFunction(rtc_device, rtc_memory_monitor_func, nullptr);

  rtc_scene = rtcNewScene(rtc_device);
  const RTCSceneFlags scene_flags = RTC_SCENE_FLAG_ROBUST;
  rtcSetSceneFlags(rtc_scene, scene_flags);
  RTCBuildQuality build_quality = RTC_BUILD_QUALITY_MEDIUM;
  rtcSetSceneBuildQuality(rtc_scene, build_quality);

  BvhBuildContext ctx{rtc_device, rtc_scene, build_quality};

  add_mesh(ctx, 0, mesh);

  rtcSetSceneProgressMonitorFunction(rtc_scene, rtc_progress_func, nullptr);
  rtcCommitScene(rtc_scene);
}

bool Tree::ray_intersect1(const Ray &ray, Hit &r_hit) const
{
  RTCIntersectContext rtc_ctx;
  rtcInitIntersectContext(&rtc_ctx);

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
  rtcIntersect1(rtc_scene, &rtc_ctx, &rtc_hit);

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
