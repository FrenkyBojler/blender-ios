/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_bvh.hh"

#include "BLI_math_geom.hh"
#include "BLI_math_vector.hh"
#include "DNA_mesh_types.h"

#include "BLI_index_mask.hh"

#include "BKE_mesh.hh"
#include "BKE_mesh_runtime.hh"

#ifdef WITH_EMBREE

#  include <embree4/rtcore.h>

/* Similar to Cycles simd.h. */

#  if defined(FREE_WINDOWS64)
#    include <windows.h>
#  elif defined(_MSC_VER) && !defined(__KERNEL_NEON__)
#    include <intrin.h>
#  elif (defined(__x86_64__) || defined(__i386__))
#    include <x86intrin.h>
#  elif defined(__KERNEL_NEON__)
#    define SSE2NEON_PRECISE_MINMAX 1
#    include <sse2neon.h>
#  endif

/* Floating Point Control, for Embree. */
#  if defined(__x86_64__) || defined(_M_X64)
#    define SIMD_SET_FLUSH_TO_ZERO \
      _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON); \
      _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#  elif defined(__aarch64__) || defined(_M_ARM64)
/* The get/set denormals to zero was implemented in sse2neon v1.5.0.
 * Keep the compatibility code until the minimum library version is increased. */
#    if defined(_MM_SET_FLUSH_ZERO_MODE)
#      define SIMD_SET_FLUSH_TO_ZERO \
        _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON); \
        _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#    elif !defined(_M_ARM64)
#      define _MM_FLUSH_ZERO_ON 24
#      define __get_fpcr(__fpcr) __asm__ __volatile__("mrs %0,fpcr" : "=r"(__fpcr))
#      define __set_fpcr(__fpcr) __asm__ __volatile__("msr fpcr,%0" : : "ri"(__fpcr))
#      define SIMD_SET_FLUSH_TO_ZERO set_fz(_MM_FLUSH_ZERO_ON);
#      define SIMD_GET_FLUSH_TO_ZERO get_fz(_MM_FLUSH_ZERO_ON)
#    else
#      define _MM_FLUSH_ZERO_ON 24
#      define __get_fpcr(__fpcr) __fpcr = _ReadStatusReg(0x5A20)
#      define __set_fpcr(__fpcr) _WriteStatusReg(0x5A20, __fpcr)
#      define SIMD_SET_FLUSH_TO_ZERO set_fz(_MM_FLUSH_ZERO_ON);
#      define SIMD_GET_FLUSH_TO_ZERO get_fz(_MM_FLUSH_ZERO_ON)
#    endif
#  else
#    define SIMD_SET_FLUSH_TO_ZERO
#  endif

namespace blender::bke::bvh {

Tree::Tree()
{
  SIMD_SET_FLUSH_TO_ZERO;
};

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

static bool all_faces_are_triangles(const Mesh &mesh)
{
  return mesh.corners_num == mesh.faces_num * 3;
}

static void add_mesh_faces(const BvhBuildContext &ctx,
                           const int id,
                           const Mesh &mesh,
                           const IndexMask &face_mask)
{
  RTCGeometry geom_id = rtcNewGeometry(ctx.device, RTC_GEOMETRY_TYPE_TRIANGLE);
  rtcSetGeometryBuildQuality(geom_id, ctx.build_quality);

  const Span<int> corner_verts = mesh.corner_verts();
  if (face_mask.size() == mesh.faces_num) {
    if (all_faces_are_triangles(mesh)) {
      rtcSetSharedGeometryBuffer(geom_id,
                                 RTC_BUFFER_TYPE_INDEX,
                                 0,
                                 RTC_FORMAT_UINT3,
                                 corner_verts.data(),
                                 0,
                                 sizeof(int3),
                                 corner_verts.cast<int3>().size());
    }
    else {
      const Span<int3> corner_tris = mesh.corner_tris();
      uint3 *rtc_indices = static_cast<uint3 *>(rtcSetNewGeometryBuffer(
          geom_id, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, sizeof(int3), corner_tris.size()));
      mesh::vert_tris_from_corner_tris(
          corner_verts, corner_tris, MutableSpan(rtc_indices, corner_tris.size()).cast<int3>());
    }
  }
  else {
    const OffsetIndices faces = mesh.faces();
    const Span<int3> corner_tris = mesh.corner_tris();
    int tris_num = 0;
    face_mask.foreach_index_optimized<int>(
        [&](const int i) { tris_num += mesh::face_triangles_num(faces[i].size()); });

    uint3 *rtc_indices = static_cast<uint3 *>(rtcSetNewGeometryBuffer(
        geom_id, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, sizeof(int3), tris_num));
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

  const Span<float3> positions = mesh.vert_positions();
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
  const RTCSceneFlags scene_flags = RTCSceneFlags(RTC_SCENE_FLAG_ROBUST |
                                                  RTC_SCENE_FLAG_FILTER_FUNCTION_IN_ARGUMENTS);
  rtcSetSceneFlags(tree.rtc_scene, scene_flags);
  RTCBuildQuality build_quality = RTC_BUILD_QUALITY_MEDIUM;
  rtcSetSceneBuildQuality(tree.rtc_scene, build_quality);

  BvhBuildContext ctx{tree.rtc_device, tree.rtc_scene, build_quality};

  add_mesh_faces(ctx, 0, mesh, face_mask);

  rtcSetSceneProgressMonitorFunction(tree.rtc_scene, rtc_progress_func, nullptr);
  rtcCommitScene(tree.rtc_scene);

  return tree;
}

Tree Tree::from_single_mesh(const Mesh &mesh)
{
  return from_tris(mesh, IndexRange(mesh.faces_num));
}

std::optional<RayHit> Tree::ray_intersect(const Ray &ray) const
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
  rtc_hit.ray.time = 0.0f; /* Motion blur time */
  rtc_hit.ray.mask = 0xffffffff;
  rtc_hit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
  rtc_hit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
  rtcIntersect1(rtc_scene, &rtc_hit);
  if (rtc_hit.hit.geomID == RTC_INVALID_GEOMETRY_ID ||
      rtc_hit.hit.primID == RTC_INVALID_GEOMETRY_ID)
  {
    return std::nullopt;
  }

  RayHit hit;
  hit.position = float3(rtc_hit.ray.org_x + rtc_hit.ray.tfar * rtc_hit.ray.dir_x,
                        rtc_hit.ray.org_y + rtc_hit.ray.tfar * rtc_hit.ray.dir_y,
                        rtc_hit.ray.org_z + rtc_hit.ray.tfar * rtc_hit.ray.dir_z);
  hit.normal = float3(rtc_hit.hit.Ng_x, rtc_hit.hit.Ng_y, rtc_hit.hit.Ng_z);
  hit.bary_coord = float2(rtc_hit.hit.u, rtc_hit.hit.v);
  hit.index = rtc_hit.hit.primID;
  hit.distance = rtc_hit.ray.tfar;
  return hit;
}

void Tree::ray_intersect_all(const Ray &ray, FunctionRef<void(const RayHit &)> fn) const
{
  struct AllHitsContext {
    RTCRayQueryContext rtc_context;
    FunctionRef<void(const RayHit &)> *fn;
    float3 origin;
    float3 direction;
  };

  AllHitsContext ctx;
  rtcInitRayQueryContext(&ctx.rtc_context);
  ctx.fn = &fn;
  ctx.origin = ray.origin;
  ctx.direction = ray.direction;

  RTCRayHit rtc_hit;
  rtc_hit.ray.org_x = ray.origin.x;
  rtc_hit.ray.org_y = ray.origin.y;
  rtc_hit.ray.org_z = ray.origin.z;
  rtc_hit.ray.dir_x = ray.direction.x;
  rtc_hit.ray.dir_y = ray.direction.y;
  rtc_hit.ray.dir_z = ray.direction.z;
  rtc_hit.ray.tnear = ray.dist_min;
  rtc_hit.ray.tfar = ray.dist_max;
  rtc_hit.ray.time = 0.0f;
  rtc_hit.ray.mask = 0xffffffff;
  rtc_hit.ray.id = 0;
  rtc_hit.ray.flags = 0;
  rtc_hit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
  rtc_hit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;

  RTCIntersectArguments args;
  rtcInitIntersectArguments(&args);
  args.context = &ctx.rtc_context;
  args.filter = [](const RTCFilterFunctionNArguments *filter_args) {
    AllHitsContext *ctx = reinterpret_cast<AllHitsContext *>(filter_args->context);
    const RTCHit rtc_hit = rtcGetHitFromHitN(filter_args->hit, filter_args->N, 0);
    const RTCRay rtc_ray = rtcGetRayFromRayN(filter_args->ray, filter_args->N, 0);

    RayHit hit;
    hit.position = ctx->origin + rtc_ray.tfar * ctx->direction;
    hit.normal = float3(rtc_hit.Ng_x, rtc_hit.Ng_y, rtc_hit.Ng_z);
    hit.bary_coord = float2(rtc_hit.u, rtc_hit.v);
    hit.index = int(rtc_hit.primID);
    hit.distance = rtc_ray.tfar;
    (*ctx->fn)(hit);

    /* Reject hit to continue traversal for all remaining intersections. */
    filter_args->valid[0] = 0;
  };

  rtcIntersect1(this->rtc_scene, &rtc_hit, &args);
}

struct ClosestPointUserData {
  RTCScene rtc_scene;
  ClosestPointResult &result;
};

static bool closest_point_fn(RTCPointQueryFunctionArguments *args)
{
  const auto &user_data = *static_cast<ClosestPointUserData *>(args->userPtr);
  const RTCScene scene = user_data.rtc_scene;
  RTCGeometry geom = rtcGetGeometry(scene, args->geomID);

  const float3 *positions = static_cast<const float3 *>(
      rtcGetGeometryBufferData(geom, RTC_BUFFER_TYPE_VERTEX, 0));
  const uint3 *indices = static_cast<const uint3 *>(
      rtcGetGeometryBufferData(geom, RTC_BUFFER_TYPE_INDEX, 0));
  const uint3 tri = indices[args->primID];

  float3 nearest_position;
  closest_on_tri_to_point_v3(
      nearest_position, &args->query->x, positions[tri[0]], positions[tri[1]], positions[tri[2]]);

  const float distance = math::distance(float3(&args->query->x), nearest_position);
  if (distance < args->query->radius) {
    args->query->radius = distance;
    user_data.result.position = nearest_position;
    user_data.result.index = args->primID;
    user_data.result.geomID = args->geomID;
    return true;
  }
  return false;
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
  ClosestPointUserData user_data(this->rtc_scene, result);
  if (!rtcPointQuery(this->rtc_scene, &query, &context, closest_point_fn, &user_data)) {
    return std::nullopt;
  }
  return result;
}

void Tree::range_query(const float3 &point, const float radius, FunctionRef<bool(int)> fn) const
{
  RTCPointQuery query{};
  query.x = point.x;
  query.y = point.y;
  query.z = point.z;
  query.time = 0.0f;
  query.radius = radius;
  RTCPointQueryContext context{};
  rtcInitPointQueryContext(&context);
  rtcPointQuery(
      this->rtc_scene,
      &query,
      &context,
      [](RTCPointQueryFunctionArguments *args) -> bool {
        FunctionRef<bool(int)> fn = *static_cast<FunctionRef<bool(int)> *>(args->userPtr);
        return fn(args->primID);
      },
      &fn);
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

void Tree::ray_intersect_all(const Ray &ray, FunctionRef<void(const RayHit &)> fn) const
{
  UNUSED_VARS(ray, fn);
}

}  // namespace blender::bke::bvh

#endif /* WITH_BVH_EMBREE */
