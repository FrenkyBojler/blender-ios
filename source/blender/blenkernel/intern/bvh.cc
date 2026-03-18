/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_bvh.hh"

#include "DNA_mesh_types.h"

#include "BLI_index_mask.hh"

#include "BKE_mesh.hh"
#include "BKE_mesh_runtime.hh"

#ifdef WITH_EMBREE

#  include <embree4/rtcore.h>

namespace blender::bke::bvh {

Tree::Tree() = default;

Tree::Tree(Tree &&other)
    : rtc_device(std::exchange(other.rtc_device, nullptr)),
      rtc_scene(std::exchange(other.rtc_scene, nullptr))
{
}

Tree &Tree::operator=(Tree &&other)
{
  if (this != &other) {
    this->free();
    this->rtc_device = std::exchange(other.rtc_device, nullptr);
    this->rtc_scene = std::exchange(other.rtc_scene, nullptr);
  }
  return *this;
}

Tree::~Tree()
{
  this->free();
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
  rtcReleaseScene(this->rtc_scene);
  this->rtc_scene = nullptr;
  rtcReleaseDevice(this->rtc_device);
  this->rtc_device = nullptr;
}

struct BvhBuildContext {
  RTCDevice device;
  RTCScene scene;
  RTCBuildQuality build_quality;
};

static void add_triangles(const BvhBuildContext &ctx,
                          const int id,
                          const Span<float3> positions,
                          const OffsetIndices<int> faces,
                          const Span<int> corner_verts,
                          const Span<int3> corner_tris,
                          const IndexMask &face_mask)
{
  RTCGeometry geom_id = rtcNewGeometry(ctx.device, RTC_GEOMETRY_TYPE_TRIANGLE);
  rtcSetGeometryBuildQuality(geom_id, ctx.build_quality);

  int tris_num = 0;
  face_mask.foreach_index_optimized<int>(
      [&](const int i) { tris_num += mesh::face_triangles_num(faces[i].size()); });

  uint3 *rtc_indices = static_cast<uint3 *>(rtcSetNewGeometryBuffer(
      geom_id, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, sizeof(int3), tris_num));
  if (face_mask.size() == faces.size()) {
    mesh::vert_tris_from_corner_tris(
        corner_verts, corner_tris, MutableSpan(rtc_indices, corner_tris.size()).cast<int3>());
  }
  else {
    int pos = 0;
    face_mask.foreach_index_optimized<int>([&](const int face) {
      for (const int tri : mesh::face_triangles_range(faces, face)) {
        rtc_indices[pos] = uint3(corner_verts[corner_tris[tri][0]],
                                 corner_verts[corner_tris[tri][1]],
                                 corner_verts[corner_tris[tri][2]]);
        pos++;
      }
    });
  }

  rtcSetSharedGeometryBuffer(geom_id,
                             RTC_BUFFER_TYPE_VERTEX,
                             0,
                             RTC_FORMAT_FLOAT3,
                             positions.data(),
                             0,
                             sizeof(float3),
                             positions.size());

  rtcCommitGeometry(geom_id);
  rtcAttachGeometryByID(ctx.scene, geom_id, id);
  rtcReleaseGeometry(geom_id);
}

Tree Tree::from_tris(const Mesh &mesh, const IndexMask &face_mask)
{
  Tree tree;
  tree.rtc_device = rtcNewDevice("verbose=0");

  rtcSetDeviceErrorFunction(tree.rtc_device, rtc_error_func, nullptr);
  rtcSetDeviceMemoryMonitorFunction(tree.rtc_device, rtc_memory_monitor_func, nullptr);

  tree.rtc_scene = rtcNewScene(tree.rtc_device);
  const RTCSceneFlags scene_flags = RTC_SCENE_FLAG_ROBUST;
  rtcSetSceneFlags(tree.rtc_scene, scene_flags);
  RTCBuildQuality build_quality = RTC_BUILD_QUALITY_MEDIUM;
  rtcSetSceneBuildQuality(tree.rtc_scene, build_quality);

  BvhBuildContext ctx{tree.rtc_device, tree.rtc_scene, build_quality};

  add_triangles(ctx,
                0,
                mesh.vert_positions(),
                mesh.faces(),
                mesh.corner_verts(),
                mesh.corner_tris(),
                face_mask);

  rtcSetSceneProgressMonitorFunction(tree.rtc_scene, rtc_progress_func, nullptr);
  rtcCommitScene(tree.rtc_scene);

  return tree;
}

Tree Tree::from_single_mesh(const Mesh &mesh)
{
  return from_tris(mesh, mesh.corner_tris().index_range());
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

std::optional<ClosestPointResult> Tree::closest_point(const float3 &point,
                                                      const float radius) const
{
  RTCPointQuery query{};
  query.x = point.x;
  query.y = point.y;
  query.z = point.z;
  query.time = 0.0f;
  query.radius = radius;
  RTCPointQueryContext context{};
  rtcInitPointQueryContext(&context);
  ClosestPointResult result;
  if (!rtcPointQuery(this->rtc_scene, &query, &context, nullptr, &result)) {
    return std::nullopt;
  }
  return result;
}

OptionallyOwnedTree tree_from_mesh_tris_mask(const Mesh &mesh, const IndexMask &mask)
{
  OptionallyOwnedTree result;
  if (mask.size() == mesh.faces_num) {
    result.tree = &mesh.bvh_tree();
  }
  else {
    result.owned_tree = std::make_unique<Tree>(Tree::from_tris(mesh, mask));
    result.tree = result.owned_tree.get();
  }
  return result;
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
