/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_function_ref.hh"
#include "BLI_index_mask_fwd.hh"
#include "BLI_math_vector_types.hh"

#include <limits>
#include <memory>
#include <optional>

struct RTCDeviceTy;
struct RTCSceneTy;

namespace blender {

struct Mesh;

namespace bke::bvh {

struct Ray {
  float3 origin;
  float3 direction;
  /* Start of ray segment relative to ray length. */
  float dist_min = 0.0f;
  /* End of ray segment relative to ray length. */
  float dist_max = std::numeric_limits<float>::max();

  Ray() = default;
  Ray(const float3 &origin, const float3 &direction) : origin(origin), direction(direction) {}
  Ray(const float3 &origin, const float3 &direction, const float radius)
      : origin(origin), direction(direction), dist_max(radius)
  {
  }
};

struct RayHit {
  /* Ng. Not normalized. */
  float3 normal;
  float2 bary_coord;
  int index;
  float distance;
  float3 position(const Ray &ray) const
  {
    return ray.origin + ray.direction * distance;
  }
};

struct ClosestPointResult {
  float3 position;
  float2 bary_coord;
  uint32_t index;
  /* Currently unused. */
  uint32_t geomID;
};

class Tree {
 public:
  struct FallbackTree {
    virtual ~FallbackTree() = default;
  };

 private:
#ifdef WITH_EMBREE
  RTCDeviceTy *rtc_device = nullptr;
  RTCSceneTy *rtc_scene = nullptr;
#else
  std::unique_ptr<FallbackTree> fallback_tree_;
#endif

 public:
  Tree();
  Tree(const Tree &) = delete;
  Tree &operator=(const Tree &) = delete;
  Tree(Tree &&);
  Tree &operator=(Tree &&);
  ~Tree();

  /**
   * Create a BVH tree from a subset of the mesh faces. #from_single_mesh should be used when all
   * faces are contained in the mask.
   * \param tris_num: Pre-calculated number of triangles in the mask, technically redundant with
   * the mask, but passed as an argument to avoid recalculating it.
   */
  static Tree from_tris(const Mesh &mesh, const IndexMask &mask, int tris_num);
  /** Create a BVH tree from the entire mesh. */
  static Tree from_single_mesh(const Mesh &mesh);

  void free();

  std::optional<RayHit> ray_intersect(const Ray &ray) const;

  void ray_intersect_all(const Ray &ray, FunctionRef<void(const RayHit &)> fn) const;

  std::optional<ClosestPointResult> closest_point(
      const float3 &point, float radius = std::numeric_limits<float>::max()) const;

  void range_query(const float3 &point, const float radius, FunctionRef<bool(int)> fn) const;
};

}  // namespace bke::bvh
}  // namespace blender
