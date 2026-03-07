/* SPDX-FileCopyrightText: 2018-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* This class implements a ray accelerator for Cycles using Intel's Embree library.
 * It supports triangles, curves, object and deformation blur and instancing.
 *
 * Since Embree allows object to be either curves or triangles but not both, Cycles object IDs are
 * mapped to Embree IDs by multiplying by two and adding one for curves.
 *
 * This implementation shares RTCDevices between Cycles instances. Eventually each instance should
 * get a separate RTCDevice to correctly keep track of memory usage.
 *
 * Vertex and index buffers are duplicated between Cycles device arrays and Embree. These could be
 * merged, which would require changes to intersection refinement, shader setup, mesh light
 * sampling and a few other places in Cycles where direct access to vertex data is required.
 */

#ifdef WITH_EMBREE

#  include <embree4/rtcore_geometry.h>

#  include "bvh/embree.h"

#  include "scene/hair.h"
#  include "scene/light.h"
#  include "scene/mesh.h"
#  include "scene/object.h"
#  include "scene/pointcloud.h"

#  include "util/log.h"
#  include "util/math_intersect.h"
#  include "util/progress.h"
#  include "util/stats.h"

CCL_NAMESPACE_BEGIN

static_assert(Object::MAX_MOTION_STEPS <= RTC_MAX_TIME_STEP_COUNT,
              "Object and Embree max motion steps inconsistent");
static_assert(Object::MAX_MOTION_STEPS == Geometry::MAX_MOTION_STEPS,
              "Object and Geometry max motion steps inconsistent");

static size_t unaccounted_mem = 0;

static bool rtc_memory_monitor_func(void *userPtr, const ssize_t bytes, const bool /*unused*/)
{
  Stats *stats = (Stats *)userPtr;
  if (stats) {
    if (bytes > 0) {
      stats->mem_alloc(bytes);
    }
    else {
      stats->mem_free(-bytes);
    }
  }
  else {
    /* A stats pointer may not yet be available. Keep track of the memory usage for later. */
    if (bytes >= 0) {
      atomic_add_and_fetch_z(&unaccounted_mem, bytes);
    }
    else {
      atomic_sub_and_fetch_z(&unaccounted_mem, -bytes);
    }
  }
  return true;
}

static void rtc_error_func(void * /*unused*/, enum RTCError /*unused*/, const char *str)
{
  LOG_WARNING << str;
}

static double progress_start_time = 0.0;

static bool rtc_progress_func(void *user_ptr, const double n)
{
  Progress *progress = (Progress *)user_ptr;

  if (time_dt() - progress_start_time < 0.25) {
    return true;
  }

  const string msg = string_printf("Building BVH %.0f%%", n * 100.0);
  progress->set_substatus(msg);
  progress_start_time = time_dt();

  return !progress->get_cancel();
}

BVHEmbree::BVHEmbree(const BVHParams &params_,
                     const vector<Geometry *> &geometry_,
                     const vector<Object *> &objects_)
    : BVH(params_, geometry_, objects_),
      scene(nullptr),
      rtc_device(nullptr),
      build_quality(RTC_BUILD_QUALITY_REFIT)
{
  SIMD_SET_FLUSH_TO_ZERO;
}

BVHEmbree::~BVHEmbree()
{
  if (scene) {
    rtcReleaseScene(scene);
  }
}

void BVHEmbree::build(Progress &progress,
                      Stats *stats,
                      RTCDevice rtc_device_,
                      const bool rtc_device_is_sycl_)
{
  rtc_device = rtc_device_;
  rtc_device_is_sycl = rtc_device_is_sycl_;
  assert(rtc_device);

  rtcSetDeviceErrorFunction(rtc_device, rtc_error_func, nullptr);
  rtcSetDeviceMemoryMonitorFunction(rtc_device, rtc_memory_monitor_func, stats);

  progress.set_substatus("Building BVH");

  if (scene) {
    rtcReleaseScene(scene);
    scene = nullptr;
  }

  const bool dynamic = params.bvh_type == BVH_TYPE_DYNAMIC;
  const bool compact = params.use_compact_structure;

  scene = rtcNewScene(rtc_device);
  const RTCSceneFlags scene_flags = (dynamic ? RTC_SCENE_FLAG_DYNAMIC : RTC_SCENE_FLAG_NONE) |
                                    (compact ? RTC_SCENE_FLAG_COMPACT : RTC_SCENE_FLAG_NONE) |
                                    RTC_SCENE_FLAG_ROBUST |
                                    RTC_SCENE_FLAG_FILTER_FUNCTION_IN_ARGUMENTS;
  rtcSetSceneFlags(scene, scene_flags);
  build_quality = dynamic ? RTC_BUILD_QUALITY_LOW :
                            (params.use_spatial_split ? RTC_BUILD_QUALITY_HIGH :
                                                        RTC_BUILD_QUALITY_MEDIUM);
  rtcSetSceneBuildQuality(scene, build_quality);

  int i = 0;
  for (Object *ob : objects) {
    if (params.top_level) {
      if (!ob->is_traceable()) {
        ++i;
        continue;
      }
      if (!ob->get_geometry()->is_instanced()) {
        add_object(ob, i);
      }
      else {
        add_instance(ob, i);
      }
    }
    else {
      add_object(ob, i);
    }
    ++i;
    if (progress.get_cancel()) {
      return;
    }
  }

  if (progress.get_cancel()) {
    return;
  }

  rtcSetSceneProgressMonitorFunction(scene, rtc_progress_func, &progress);
  rtcCommitScene(scene);
}

const char *BVHEmbree::get_error_string(RTCError error_code)
{
#  if RTC_VERSION >= 40303
  return rtcGetErrorString(error_code);
#  else
  switch (error_code) {
    case RTC_ERROR_NONE:
      return "no error";
    case RTC_ERROR_UNKNOWN:
      return "unknown error";
    case RTC_ERROR_INVALID_ARGUMENT:
      return "invalid argument error";
    case RTC_ERROR_INVALID_OPERATION:
      return "invalid operation error";
    case RTC_ERROR_OUT_OF_MEMORY:
      return "out of memory error";
    case RTC_ERROR_UNSUPPORTED_CPU:
      return "unsupported cpu error";
    case RTC_ERROR_CANCELLED:
      return "cancelled";
    default:
      /* We should never end here unless enum for RTC errors would change. */
      return "unknown error";
  }
#  endif
}

#  if defined(WITH_EMBREE_GPU) && RTC_VERSION >= 40302
/* offload_scenes_to_gpu() uses rtcGetDeviceError() which also resets Embree error status,
 * we propagate its value so it doesn't get lost. */
RTCError BVHEmbree::offload_scenes_to_gpu(const vector<RTCScene> &scenes)
{
  /* Having BVH on GPU is more performance-critical than texture data.
   * In order to ensure good performance even when running out of GPU
   * memory, we force BVH to migrate to GPU before allocating other textures
   * that may not fit. */
  for (const RTCScene &embree_scene : scenes) {
    RTCSceneFlags scene_flags = rtcGetSceneFlags(embree_scene);
    scene_flags = scene_flags | RTC_SCENE_FLAG_PREFETCH_USM_SHARED_ON_GPU;
    rtcSetSceneFlags(embree_scene, scene_flags);
    rtcCommitScene(embree_scene);
    /* In case of any errors from Embree, we should stop
     * the execution and propagate the error. */
    RTCError error_code = rtcGetDeviceError(rtc_device);
    if (error_code != RTC_ERROR_NONE) {
      return error_code;
    }
  }
  return RTC_ERROR_NONE;
}
#  endif

void BVHEmbree::add_object(Object *ob, const int i)
{
  Geometry *geom = ob->get_geometry();

  if (geom->is_mesh() || geom->is_volume()) {
    Mesh *mesh = static_cast<Mesh *>(geom);
    if (mesh->num_triangles() > 0) {
      add_triangles(ob, mesh, i);
    }
  }
  else if (geom->is_hair()) {
    Hair *hair = static_cast<Hair *>(geom);
    if (hair->is_traceable()) {
      add_curves(ob, hair, i);
    }
  }
  else if (geom->is_pointcloud()) {
    PointCloud *pointcloud = static_cast<PointCloud *>(geom);
    if (pointcloud->num_points() > 0) {
      add_points(ob, pointcloud, i);
    }
  }
  else {
    assert(geom->is_light());
    const Light *light = static_cast<const Light *>(geom);
    if (light->need_bvh()) {
      add_light(ob, light, i);
    }
  }
}

void BVHEmbree::add_instance(Object *ob, const int i)
{
  BVHEmbree *instance_bvh = static_cast<BVHEmbree *>(ob->get_geometry()->bvh.get());
  assert(instance_bvh != nullptr);

  const size_t num_object_motion_steps = ob->use_motion() ? ob->get_motion().size() : 1;
  const size_t num_motion_steps = min(num_object_motion_steps, (size_t)RTC_MAX_TIME_STEP_COUNT);
  assert(num_object_motion_steps <= RTC_MAX_TIME_STEP_COUNT);

  RTCGeometry geom_id = rtcNewGeometry(rtc_device, RTC_GEOMETRY_TYPE_INSTANCE);
  rtcSetGeometryInstancedScene(geom_id, instance_bvh->scene);
  rtcSetGeometryTimeStepCount(geom_id, num_motion_steps);

  if (ob->use_motion()) {
    array<DecomposedTransform> decomp(ob->get_motion().size());
    transform_motion_decompose(decomp.data(), ob->get_motion().data(), ob->get_motion().size());
    for (size_t step = 0; step < num_motion_steps; ++step) {
      RTCQuaternionDecomposition rtc_decomp;
      rtcInitQuaternionDecomposition(&rtc_decomp);
      rtcQuaternionDecompositionSetQuaternion(
          &rtc_decomp, decomp[step].x.w, decomp[step].x.x, decomp[step].x.y, decomp[step].x.z);
      rtcQuaternionDecompositionSetScale(
          &rtc_decomp, decomp[step].y.w, decomp[step].z.w, decomp[step].w.w);
      rtcQuaternionDecompositionSetTranslation(
          &rtc_decomp, decomp[step].y.x, decomp[step].y.y, decomp[step].y.z);
      rtcQuaternionDecompositionSetSkew(
          &rtc_decomp, decomp[step].z.x, decomp[step].z.y, decomp[step].w.x);
      rtcSetGeometryTransformQuaternion(geom_id, step, &rtc_decomp);
    }
  }
  else {
    rtcSetGeometryTransform(
        geom_id, 0, RTC_FORMAT_FLOAT3X4_ROW_MAJOR, (const float *)&ob->get_tfm());
  }

  rtcSetGeometryUserData(geom_id,
#  if RTC_VERSION >= 40400
                         (void *)rtcGetSceneTraversable(instance_bvh->scene)
#  else
                         (void *)instance_bvh->scene
#  endif
  );

  rtcSetGeometryMask(geom_id, ob->visibility_for_tracing());
  rtcSetGeometryEnableFilterFunctionFromArguments(geom_id, true);

  rtcCommitGeometry(geom_id);
  rtcAttachGeometryByID(scene, geom_id, i * 2);
  rtcReleaseGeometry(geom_id);
}

void BVHEmbree::add_triangles(const Object *ob, const Mesh *mesh, const int i)
{
  const size_t prim_offset = mesh->prim_offset;

  const Attribute *attr_mP = nullptr;
  size_t num_motion_steps = 1;
  if (mesh->has_motion_blur()) {
    attr_mP = mesh->attributes.find(ATTR_STD_MOTION_VERTEX_POSITION);
    if (attr_mP) {
      num_motion_steps = mesh->get_motion_steps();
    }
  }

  assert(num_motion_steps <= RTC_MAX_TIME_STEP_COUNT);
  num_motion_steps = min(num_motion_steps, (size_t)RTC_MAX_TIME_STEP_COUNT);

  const size_t num_triangles = mesh->num_triangles();

  RTCGeometry geom_id = rtcNewGeometry(rtc_device, RTC_GEOMETRY_TYPE_TRIANGLE);
  rtcSetGeometryBuildQuality(geom_id, build_quality);
  rtcSetGeometryTimeStepCount(geom_id, num_motion_steps);

  const int *triangles = mesh->get_triangles().data();
  if (!rtc_device_is_sycl) {
    rtcSetSharedGeometryBuffer(geom_id,
                               RTC_BUFFER_TYPE_INDEX,
                               0,
                               RTC_FORMAT_UINT3,
                               triangles,
                               0,
                               sizeof(int) * 3,
                               num_triangles);
  }
  else {
    /* NOTE(sirgienko): If the Embree device is a SYCL device, then Embree execution will
     * happen on GPU, and we cannot use standard host pointers at this point. So instead
     * of making a shared geometry buffer - a new Embree buffer will be created and data
     * will be copied. */
    int *triangles_buffer = nullptr;
#  if RTC_VERSION >= 40400
    rtcSetNewGeometryBufferHostDevice(
#  else
    triangles_buffer = (int *)rtcSetNewGeometryBuffer(
#  endif
        geom_id,
        RTC_BUFFER_TYPE_INDEX,
        0,
        RTC_FORMAT_UINT3,
        sizeof(int) * 3,
        num_triangles
#  if RTC_VERSION >= 40400
        ,
        (void **)(&triangles_buffer),
        nullptr
#  endif
    );
    assert(triangles_buffer);
    if (triangles_buffer) {
      static_assert(sizeof(int) == sizeof(uint));
      std::memcpy(triangles_buffer, triangles, sizeof(int) * 3 * (num_triangles));
    }
  }
  set_tri_vertex_buffer(geom_id, mesh, false);

  rtcSetGeometryUserData(geom_id, (void *)prim_offset);
  rtcSetGeometryMask(geom_id, ob->visibility_for_tracing());
  rtcSetGeometryEnableFilterFunctionFromArguments(geom_id, true);

  rtcCommitGeometry(geom_id);
  rtcAttachGeometryByID(scene, geom_id, i * 2);
  rtcReleaseGeometry(geom_id);
}

void BVHEmbree::set_tri_vertex_buffer(RTCGeometry geom_id, const Mesh *mesh, const bool update)
{
  const Attribute *attr_mP = nullptr;
  size_t num_motion_steps = 1;
  int t_mid = 0;
  if (mesh->has_motion_blur()) {
    attr_mP = mesh->attributes.find(ATTR_STD_MOTION_VERTEX_POSITION);
    if (attr_mP) {
      num_motion_steps = mesh->get_motion_steps();
      t_mid = (num_motion_steps - 1) / 2;
      if (num_motion_steps > RTC_MAX_TIME_STEP_COUNT) {
        assert(0);
        num_motion_steps = RTC_MAX_TIME_STEP_COUNT;
      }
    }
  }
  const size_t num_verts = mesh->get_verts().size();

  for (int t = 0; t < num_motion_steps; ++t) {
    const float3 *verts;
    if (t == t_mid) {
      verts = mesh->get_verts().data();
    }
    else {
      const int t_ = (t > t_mid) ? (t - 1) : t;
      verts = &attr_mP->data_float3()[t_ * num_verts];
    }

    if (update) {
      rtcUpdateGeometryBuffer(geom_id, RTC_BUFFER_TYPE_VERTEX, t);
    }
    else {
      if (!rtc_device_is_sycl) {
        static_assert(sizeof(float3) == 16,
                      "Embree requires that each buffer element be readable with 16-byte SSE load "
                      "instructions");
        rtcSetSharedGeometryBuffer(geom_id,
                                   RTC_BUFFER_TYPE_VERTEX,
                                   t,
                                   RTC_FORMAT_FLOAT3,
                                   verts,
                                   0,
                                   sizeof(float3),
                                   num_verts);
      }
      else {
        /* NOTE(sirgienko): If the Embree device is a SYCL device, then Embree execution will
         * happen on GPU, and we cannot use standard host pointers at this point. So instead
         * of making a shared geometry buffer - a new Embree buffer will be created and data
         * will be copied. */
        /* As float3 is packed on GPU side, we map it to packed_float3. */
        /* There is no need for additional padding in rtcSetNewGeometryBuffer since Embree 3.6:
         * "Fixed automatic vertex buffer padding when using rtcSetNewGeometry API function". */
        packed_float3 *verts_buffer = nullptr;
#  if RTC_VERSION >= 40400
        rtcSetNewGeometryBufferHostDevice(
#  else
        verts_buffer = (packed_float3 *)rtcSetNewGeometryBuffer(
#  endif
            geom_id,
            RTC_BUFFER_TYPE_VERTEX,
            t,
            RTC_FORMAT_FLOAT3,
            sizeof(packed_float3),
            num_verts
#  if RTC_VERSION >= 40400
            ,
            (void **)(&verts_buffer),
            nullptr
#  endif
        );
        assert(verts_buffer);
        if (verts_buffer) {
          for (size_t i = (size_t)0; i < num_verts; ++i) {
            verts_buffer[i].x = verts[i].x;
            verts_buffer[i].y = verts[i].y;
            verts_buffer[i].z = verts[i].z;
          }
        }
      }
    }
  }
}

/**
 * Packs the hair motion curve data control variables (CVs) into float4s as [x y z radius]
 */
template<typename T>
void pack_motion_verts(const size_t num_curves,
                       const Hair *hair,
                       const T *verts,
                       const float *curve_radius,
                       float4 *rtc_verts,
                       CurveShapeType curve_shape)
{
  for (size_t j = 0; j < num_curves; ++j) {
    const Hair::Curve c = hair->get_curve(j);
    int fk = c.first_key;

    if (curve_shape == CURVE_THICK_LINEAR) {
      for (int k = 0; k < c.num_keys; ++k, ++fk) {
        rtc_verts[k].x = verts[fk].x;
        rtc_verts[k].y = verts[fk].y;
        rtc_verts[k].z = verts[fk].z;
        rtc_verts[k].w = curve_radius[fk];
      }
      rtc_verts += c.num_keys;
    }
    else {
      for (int k = 1; k < c.num_keys + 1; ++k, ++fk) {
        rtc_verts[k].x = verts[fk].x;
        rtc_verts[k].y = verts[fk].y;
        rtc_verts[k].z = verts[fk].z;
        rtc_verts[k].w = curve_radius[fk];
      }
      /* Duplicate Embree's Catmull-Rom spline CVs at the start and end of each curve. */
      rtc_verts[0] = rtc_verts[1];
      rtc_verts[c.num_keys + 1] = rtc_verts[c.num_keys];
      rtc_verts += c.num_keys + 2;
    }
  }
}

void BVHEmbree::set_curve_vertex_buffer(RTCGeometry geom_id, const Hair *hair, const bool update)
{
  const Attribute *attr_mP = nullptr;
  size_t num_motion_steps = 1;
  if (hair->has_motion_blur()) {
    attr_mP = hair->attributes.find(ATTR_STD_MOTION_VERTEX_POSITION);
    if (attr_mP) {
      num_motion_steps = hair->get_motion_steps();
    }
  }

  const size_t num_curves = hair->num_curves();
  size_t num_keys = 0;
  for (size_t j = 0; j < num_curves; ++j) {
    const Hair::Curve c = hair->get_curve(j);
    num_keys += c.num_keys;
  }

  /* Catmull-Rom splines need extra CVs at the beginning and end of each curve. */
  size_t num_keys_embree = num_keys;
  num_keys_embree += num_curves * 2;

  /* Copy the CV data to Embree */
  const int t_mid = (num_motion_steps - 1) / 2;
  const float *curve_radius = hair->get_curve_radius().data();
  for (int t = 0; t < num_motion_steps; ++t) {
    // As float4 and float3 are no longer interchangeable the 2 types need to be
    // handled separately. Attributes are float4s where the radius is stored in w and
    // the middle motion vector is from the mesh points which are stored float3s with
    // the radius stored in another array.
    float4 *rtc_verts = nullptr;
    if (update) {
      rtc_verts = (float4 *)rtcGetGeometryBufferData(geom_id, RTC_BUFFER_TYPE_VERTEX, t);
    }
    else {
#  if RTC_VERSION >= 40400
      rtcSetNewGeometryBufferHostDevice(
#  else
      rtc_verts = (float4 *)rtcSetNewGeometryBuffer(
#  endif
          geom_id,
          RTC_BUFFER_TYPE_VERTEX,
          t,
          RTC_FORMAT_FLOAT4,
          sizeof(float) * 4,
          num_keys_embree
#  if RTC_VERSION >= 40400
          ,
          (void **)(&rtc_verts),
          nullptr
#  endif
      );
    }

    assert(rtc_verts);
    if (rtc_verts) {
      const size_t num_curves = hair->num_curves();
      if (t == t_mid || attr_mP == nullptr) {
        const float3 *verts = hair->get_curve_keys().data();
        pack_motion_verts<float3>(
            num_curves, hair, verts, curve_radius, rtc_verts, hair->curve_shape);
      }
      else {
        const int t_ = (t > t_mid) ? (t - 1) : t;
        const float4 *verts = &attr_mP->data_float4()[t_ * num_keys];
        pack_motion_verts<float4>(
            num_curves, hair, verts, curve_radius, rtc_verts, hair->curve_shape);
      }
    }

    if (update) {
      rtcUpdateGeometryBuffer(geom_id, RTC_BUFFER_TYPE_VERTEX, t);
    }
  }
}

void BVHEmbree::set_point_vertex_buffer(RTCGeometry geom_id,
                                        const PointCloud *pointcloud,
                                        const bool update)
{
  const Attribute *attr_mP = nullptr;
  size_t num_motion_steps = 1;
  if (pointcloud->has_motion_blur()) {
    attr_mP = pointcloud->attributes.find(ATTR_STD_MOTION_VERTEX_POSITION);
    if (attr_mP) {
      num_motion_steps = pointcloud->get_motion_steps();
    }
  }

  const size_t num_points = pointcloud->num_points();

  /* Copy the point data to Embree */
  const int t_mid = (num_motion_steps - 1) / 2;
  const float *radius = pointcloud->get_radius().data();
  for (int t = 0; t < num_motion_steps; ++t) {
    // As float4 and float3 are no longer interchangeable the 2 types need to be
    // handled separately. Attributes are float4s where the radius is stored in w and
    // the middle motion vector is from the mesh points which are stored float3s with
    // the radius stored in another array.

    float4 *rtc_verts = nullptr;
    if (update) {
      rtc_verts = (float4 *)rtcGetGeometryBufferData(geom_id, RTC_BUFFER_TYPE_VERTEX, t);
    }
    else {
#  if RTC_VERSION >= 40400
      rtcSetNewGeometryBufferHostDevice(
#  else
      rtc_verts = (float4 *)rtcSetNewGeometryBuffer(
#  endif
          geom_id,
          RTC_BUFFER_TYPE_VERTEX,
          t,
          RTC_FORMAT_FLOAT4,
          sizeof(float) * 4,
          num_points
#  if RTC_VERSION >= 40400
          ,
          (void **)(&rtc_verts),
          nullptr
#  endif
      );
    }

    assert(rtc_verts);
    if (rtc_verts) {
      if (t == t_mid || attr_mP == nullptr) {
        /* Pack the motion points into a float4 as [x y z radius]. */
        const float3 *verts = pointcloud->get_points().data();
        for (size_t j = 0; j < num_points; ++j) {
          rtc_verts[j].x = verts[j].x;
          rtc_verts[j].y = verts[j].y;
          rtc_verts[j].z = verts[j].z;
          rtc_verts[j].w = radius[j];
        }
      }
      else {
        /* Motion blur is already packed as [x y z radius]. */
        const int t_ = (t > t_mid) ? (t - 1) : t;
        const float4 *verts = &attr_mP->data_float4()[t_ * num_points];
        std::copy_n(verts, num_points, rtc_verts);
      }
    }

    if (update) {
      rtcUpdateGeometryBuffer(geom_id, RTC_BUFFER_TYPE_VERTEX, t);
    }
  }
}

void BVHEmbree::add_points(const Object *ob, const PointCloud *pointcloud, const int i)
{
  const size_t prim_offset = pointcloud->prim_offset;

  const Attribute *attr_mP = nullptr;
  size_t num_motion_steps = 1;
  if (pointcloud->has_motion_blur()) {
    attr_mP = pointcloud->attributes.find(ATTR_STD_MOTION_VERTEX_POSITION);
    if (attr_mP) {
      num_motion_steps = pointcloud->get_motion_steps();
    }
  }

  const enum RTCGeometryType type = RTC_GEOMETRY_TYPE_SPHERE_POINT;

  RTCGeometry geom_id = rtcNewGeometry(rtc_device, type);

  rtcSetGeometryBuildQuality(geom_id, build_quality);
  rtcSetGeometryTimeStepCount(geom_id, num_motion_steps);

  set_point_vertex_buffer(geom_id, pointcloud, false);

  rtcSetGeometryUserData(geom_id, (void *)prim_offset);
  rtcSetGeometryMask(geom_id, ob->visibility_for_tracing());
  rtcSetGeometryEnableFilterFunctionFromArguments(geom_id, true);

  rtcCommitGeometry(geom_id);
  rtcAttachGeometryByID(scene, geom_id, i * 2);
  rtcReleaseGeometry(geom_id);
}

void BVHEmbree::add_curves(const Object *ob, const Hair *hair, const int i)
{
  const size_t prim_offset = hair->curve_segment_offset;

  const Attribute *attr_mP = nullptr;
  size_t num_motion_steps = 1;
  if (hair->has_motion_blur()) {
    attr_mP = hair->attributes.find(ATTR_STD_MOTION_VERTEX_POSITION);
    if (attr_mP) {
      num_motion_steps = hair->get_motion_steps();
    }
  }

  assert(num_motion_steps <= RTC_MAX_TIME_STEP_COUNT);
  num_motion_steps = min(num_motion_steps, (size_t)RTC_MAX_TIME_STEP_COUNT);

  const size_t num_curves = hair->num_curves();
  size_t num_segments = 0;
  for (size_t j = 0; j < num_curves; ++j) {
    const Hair::Curve c = hair->get_curve(j);
    assert(c.num_segments() > 0);
    num_segments += c.num_segments();
  }

  const enum RTCGeometryType type = (hair->curve_shape == CURVE_THICK_LINEAR ?
                                         RTC_GEOMETRY_TYPE_ROUND_LINEAR_CURVE :
                                     hair->curve_shape == CURVE_RIBBON ?
                                         RTC_GEOMETRY_TYPE_FLAT_CATMULL_ROM_CURVE :
                                         RTC_GEOMETRY_TYPE_ROUND_CATMULL_ROM_CURVE);

  RTCGeometry geom_id = rtcNewGeometry(rtc_device, type);
  rtcSetGeometryTessellationRate(geom_id, params.curve_subdivisions + 1);
  unsigned *rtc_indices = nullptr;
#  if RTC_VERSION >= 40400
  rtcSetNewGeometryBufferHostDevice(
#  else
  rtc_indices = (unsigned *)rtcSetNewGeometryBuffer(
#  endif
      geom_id,
      RTC_BUFFER_TYPE_INDEX,
      0,
      RTC_FORMAT_UINT,
      sizeof(int),
      num_segments
#  if RTC_VERSION >= 40400
      ,
      (void **)(&rtc_indices),
      nullptr
#  endif
  );

  size_t rtc_index = 0;
  for (size_t j = 0; j < num_curves; ++j) {
    const Hair::Curve c = hair->get_curve(j);
    for (size_t k = 0; k < c.num_segments(); ++k) {
      rtc_indices[rtc_index] = c.first_key + k;
      if (hair->curve_shape != CURVE_THICK_LINEAR) {
        /* Room for extra CVs at Catmull-Rom splines. */
        rtc_indices[rtc_index] += j * 2;
      }

      ++rtc_index;
    }
  }

  rtcSetGeometryBuildQuality(geom_id, build_quality);
  rtcSetGeometryTimeStepCount(geom_id, num_motion_steps);

  set_curve_vertex_buffer(geom_id, hair, false);

  rtcSetGeometryUserData(geom_id, (void *)prim_offset);
  rtcSetGeometryMask(geom_id, ob->visibility_for_tracing());
  rtcSetGeometryEnableFilterFunctionFromArguments(geom_id, true);

  rtcCommitGeometry(geom_id);
  rtcAttachGeometryByID(scene, geom_id, i * 2 + 1);
  rtcReleaseGeometry(geom_id);
}

void BVHEmbree::set_quad_index_buffer(const RTCGeometry geom_id)
{
  int4 *rtc_indices = nullptr;
  const RTCBufferType buffer_type = RTC_BUFFER_TYPE_INDEX;
  const RTCFormat index_format = RTC_FORMAT_UINT4;

#  if RTC_VERSION >= 40400
  rtcSetNewGeometryBufferHostDevice(
      geom_id, buffer_type, 0, index_format, sizeof(int4), 1, (void **)(&rtc_indices), nullptr);
#  else
  rtc_indices = (unsigned *)rtcSetNewGeometryBuffer(
      geom_id, buffer_type, 0, index_format, sizeof(int4), 1);
#  endif

  assert(rtc_indices);
  if (rtc_indices) {
    rtc_indices[0] = make_int4(0, 1, 2, 3);
  }
}

void BVHEmbree::set_quad_vertex_buffer(const RTCGeometry geom,
                                       const AreaLight *light,
                                       const bool update)
{
  float3 *rtc_verts = nullptr;
  const RTCBufferType buffer_type = RTC_BUFFER_TYPE_VERTEX;

  if (update) {
    rtc_verts = (float3 *)rtcGetGeometryBufferData(geom, buffer_type, 0);
  }
  else {
    const RTCFormat vertex_format = RTC_FORMAT_FLOAT3;

#  if RTC_VERSION >= 40400
    rtcSetNewGeometryBufferHostDevice(
        geom, buffer_type, 0, vertex_format, sizeof(float3), 4, (void **)(&rtc_verts), nullptr);
#  else
    rtc_verts = (float3 *)rtcSetNewGeometryBuffer(
        geom_id, buffer_type, 0, vertex_format, sizeof(float3), 4);
#  endif
  }

  assert(rtc_verts);
  if (rtc_verts) {
    rtc_verts[0] = -0.5f * (AREA_LIGHT_AXIS_U + AREA_LIGHT_AXIS_V);
    rtc_verts[1] = 0.5f * (AREA_LIGHT_AXIS_U - AREA_LIGHT_AXIS_V);
    rtc_verts[2] = 0.5f * (AREA_LIGHT_AXIS_U + AREA_LIGHT_AXIS_V);
    rtc_verts[3] = 0.5f * (AREA_LIGHT_AXIS_V - AREA_LIGHT_AXIS_U);

    if (update) {
      rtcUpdateGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0);
    }
  }
}

bool filter_backface(const RTCRay *ray, const RTCHit *hit)
{
  const float3 D = make_float3(ray->dir_x, ray->dir_y, ray->dir_z);
  const float3 N = make_float3(hit->Ng_x, hit->Ng_y, hit->Ng_z);

  return dot(D, N) <= 0.0f;
}

RTC_SYCL_INDIRECTLY_CALLABLE void ellipse_filter_func(const RTCFilterFunctionNArguments *args)
{
  const RTCHit *hit = (const RTCHit *)args->hit;
  /* Filter out hits outside of the ellipse. */
  if (sqr(hit->u - 0.5f) + sqr(hit->v - 0.5f) > 0.25f) {
    *args->valid = 0;
    return;
  }

  const RTCRay *ray = (const RTCRay *)args->ray;

  /* Filter out backface. */
  if (filter_backface(ray, hit)) {
    *args->valid = 0;
    return;
  }

  /* TODO(weizhen): maybe we can even filter the angle, but need to set userdata for that. */
}

RTC_SYCL_INDIRECTLY_CALLABLE void rectangle_filter_func(const RTCFilterFunctionNArguments *args)
{
  const RTCHit *hit = (const RTCHit *)args->hit;
  const RTCRay *ray = (const RTCRay *)args->ray;

  /* Filter out backface. */
  if (filter_backface(ray, hit)) {
    *args->valid = 0;
    return;
  }

  /* TODO(weizhen): maybe we can even filter the angle, but need to set userdata for that. */
}

void point_light_bounds_func(const struct RTCBoundsFunctionArguments *args)
{
  RTCBounds *bounds_o = args->bounds_o;
  bounds_o->lower_x = bounds_o->lower_y = bounds_o->lower_z = -POINT_LIGHT_RADIUS;
  bounds_o->upper_x = bounds_o->upper_y = bounds_o->upper_z = POINT_LIGHT_RADIUS;
}

RTC_SYCL_INDIRECTLY_CALLABLE void point_light_intersect_func(
    const RTCIntersectFunctionNArguments *args)
{
  RTCRayHit *rayhit = (RTCRayHit *)args->rayhit;

  const float3 ray_P = make_float3(rayhit->ray.org_x, rayhit->ray.org_y, rayhit->ray.org_z);
  const float3 ray_D = make_float3(rayhit->ray.dir_x, rayhit->ray.dir_y, rayhit->ray.dir_z);

  float3 P;
  const float3 Ng = normalize(ray_P);
  rayhit->hit.Ng_x = Ng.x;
  rayhit->hit.Ng_y = Ng.y;
  rayhit->hit.Ng_z = Ng.z;
  if (ray_disk_intersect(ray_P,
                         ray_D,
                         rayhit->ray.tnear,
                         rayhit->ray.tfar,
                         zero_float3(),
                         Ng,
                         POINT_LIGHT_RADIUS,
                         &P,
                         &rayhit->ray.tfar))
  {
    rayhit->hit.primID = 0;
    rayhit->hit.geomID = 0;
    rayhit->hit.instID[0] = args->context->instID[0];
    // *args->valid = -1;

    /* TODO(weizhen): invoke filter func. */

    // RTCFilterFunctionNArguments fargs;
    // fargs.valid = (int *)&imask;
    // fargs.geometryUserPtr = ptr;
    // fargs.context = args->context;
    // fargs.ray = (RTCRayN *)args->rayhit;
    // fargs.hit = (RTCHitN *)args->hit;
    // fargs.N = 1;

    // rtcInvokeIntersectFilterFromGeometry(args, &fargs);
  }
  else {
    // *args->valid = 0;
  }

  // it should update the hit distance of the ray(tfar member) and
  //     the hit(u, v, Ng, instID, geomID, primID members)
}

RTC_SYCL_INDIRECTLY_CALLABLE void point_light_occluded_func(
    const RTCOccludedFunctionNArguments *args)
{
  printf("occluded\n");
}

RTCGeometry BVHEmbree::set_point_light_geometry_data(const Object *ob,
                                                     const PointLight *light,
                                                     const int object_id)
{
  RTCGeometry geom = rtcNewGeometry(rtc_device, RTC_GEOMETRY_TYPE_USER);

  rtcSetGeometryUserPrimitiveCount(geom, 1);
  rtcSetGeometryUserData(geom, (void *)light->prim_offset);
  rtcSetGeometryBoundsFunction(geom, point_light_bounds_func, nullptr);
  rtcSetGeometryIntersectFunction(geom, point_light_intersect_func);
  rtcSetGeometryOccludedFunction(geom, point_light_occluded_func);
  return geom;
}

RTC_SYCL_INDIRECTLY_CALLABLE void sphere_light_intersect_func(
    const RTCIntersectFunctionNArguments *args)
{
  RTCRayHit *rayhit = (RTCRayHit *)args->rayhit;

  const float3 ray_P = make_float3(rayhit->ray.org_x, rayhit->ray.org_y, rayhit->ray.org_z);
  const float3 ray_D = make_float3(rayhit->ray.dir_x, rayhit->ray.dir_y, rayhit->ray.dir_z);

  float3 P;
  const float3 Ng = normalize(ray_P);
  rayhit->hit.Ng_x = Ng.x;
  rayhit->hit.Ng_y = Ng.y;
  rayhit->hit.Ng_z = Ng.z;
  if (ray_sphere_intersect(ray_P,
                           ray_D,
                           rayhit->ray.tnear,
                           rayhit->ray.tfar,
                           zero_float3(),
                           POINT_LIGHT_RADIUS,
                           &P,
                           &rayhit->ray.tfar))
  {
    rayhit->hit.primID = 0;
    rayhit->hit.geomID = 0;
    rayhit->hit.instID[0] = args->context->instID[0];
    // *args->valid = -1;

    /* TODO(weizhen): invoke filter func. */

    // RTCFilterFunctionNArguments fargs;
    // fargs.valid = (int *)&imask;
    // fargs.geometryUserPtr = ptr;
    // fargs.context = args->context;
    // fargs.ray = (RTCRayN *)args->rayhit;
    // fargs.hit = (RTCHitN *)args->hit;
    // fargs.N = 1;

    // rtcInvokeIntersectFilterFromGeometry(args, &fargs);
  }
  else {
    // *args->valid = 0;
  }

  // it should update the hit distance of the ray(tfar member) and
  //     the hit(u, v, Ng, instID, geomID, primID members)
}

RTCGeometry BVHEmbree::set_sphere_light_geometry_data(const Object *ob,
                                                      const PointLight *light,
                                                      const int object_id)
{
  RTCGeometry geom = rtcNewGeometry(rtc_device, RTC_GEOMETRY_TYPE_USER);

  rtcSetGeometryUserPrimitiveCount(geom, 1);
  rtcSetGeometryUserData(geom, (void *)light->prim_offset);
  rtcSetGeometryBoundsFunction(geom, point_light_bounds_func, nullptr);
  rtcSetGeometryIntersectFunction(geom, sphere_light_intersect_func);
  rtcSetGeometryOccludedFunction(geom, point_light_occluded_func);
  return geom;
}

RTCGeometry BVHEmbree::set_light_geometry_data(Object *ob, const Light *light, const int object_id)
{
  RTCGeometry geom;
  if (light->is_area_light()) {
    const AreaLight *area_light = static_cast<const AreaLight *>(light);
    geom = rtcNewGeometry(rtc_device, RTC_GEOMETRY_TYPE_QUAD);
    set_quad_index_buffer(geom);
    set_quad_vertex_buffer(geom, area_light, false);
    rtcSetGeometryIntersectFilterFunction(
        geom, area_light->get_ellipse() ? ellipse_filter_func : rectangle_filter_func);
    rtcSetGeometryUserData(geom, (void *)light->prim_offset);
  }
  else if (light->is_point_light() || light->is_spot_light()) {
    const PointLight *point_light = static_cast<const PointLight *>(light);
    if (point_light->get_is_sphere()) {
      geom = set_sphere_light_geometry_data(ob, point_light, object_id);
    }
    else {
      geom = set_point_light_geometry_data(ob, point_light, object_id);
    }
  }
  else {
    /* Distant lights are not supposed to be in the BVH. */
    /* TODO(weizhen): check `distant_light_intersect`, maybe we do need to add distant light in the
     * bvh? */
    assert(false);
    geom = rtcNewGeometry(rtc_device, RTC_GEOMETRY_TYPE_SPHERE_POINT);
  }
  return geom;
}

void BVHEmbree::add_light(Object *ob, const Light *light, const int object_id)

{
  /* TODO(weizhen): support motion blur. */
  const size_t num_motion_steps = 1;

  RTCGeometry geom = set_light_geometry_data(ob, light, object_id);

  rtcSetGeometryBuildQuality(geom, build_quality);
  rtcSetGeometryTimeStepCount(geom, num_motion_steps);

  rtcSetGeometryMask(geom, ob->visibility_for_tracing());
  rtcSetGeometryEnableFilterFunctionFromArguments(geom, true);

  rtcCommitGeometry(geom);
  rtcAttachGeometryByID(scene, geom, object_id * 2);
  rtcReleaseGeometry(geom);
}

void BVHEmbree::refit(Progress &progress)
{
  progress.set_substatus("Refitting BVH nodes");

  /* Update all vertex buffers, then tell Embree to rebuild/-fit the BVHs. */
  unsigned geom_id = 0;
  for (Object *ob : objects) {
    if (!params.top_level || (ob->is_traceable() && !ob->get_geometry()->is_instanced())) {
      Geometry *geom = ob->get_geometry();

      if (geom->is_mesh() || geom->is_volume()) {
        Mesh *mesh = static_cast<Mesh *>(geom);
        if (mesh->num_triangles() > 0) {
          RTCGeometry geom = rtcGetGeometry(scene, geom_id);
          set_tri_vertex_buffer(geom, mesh, true);
          rtcSetGeometryUserData(geom, (void *)mesh->prim_offset);
          rtcCommitGeometry(geom);
        }
      }
      else if (geom->is_hair()) {
        Hair *hair = static_cast<Hair *>(geom);
        if (hair->is_traceable()) {
          RTCGeometry geom = rtcGetGeometry(scene, geom_id + 1);
          set_curve_vertex_buffer(geom, hair, true);
          rtcSetGeometryUserData(geom, (void *)hair->curve_segment_offset);
          rtcCommitGeometry(geom);
        }
      }
      else if (geom->is_pointcloud()) {
        PointCloud *pointcloud = static_cast<PointCloud *>(geom);
        if (pointcloud->num_points() > 0) {
          RTCGeometry geom = rtcGetGeometry(scene, geom_id);
          set_point_vertex_buffer(geom, pointcloud, true);
          rtcCommitGeometry(geom);
        }
      }
      else if (geom->is_light()) {
        /* TODO(weizhen): */
      }
    }
    geom_id += 2;
  }

  rtcCommitScene(scene);
}

CCL_NAMESPACE_END

#endif /* WITH_EMBREE */
