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
  float dist_max;
};

struct RayHit {
  float3 position;
  /* Ng. Not normalized. */
  float3 normal;
  float2 bary_coord;
  int index;
  float distance;
};

struct ClosestPointResult {
  float3 position;
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

  static Tree from_tris(const Mesh &mesh, const IndexMask &mask);
  static Tree from_single_mesh(const Mesh &mesh);

  void free();

  std::optional<RayHit> ray_intersect(const Ray &ray) const;
  std::optional<RayHit> ray_intersect(const float3 &origin,
                                      const float3 &direction,
                                      float dist_max = std::numeric_limits<float>::max()) const;

  void ray_intersect_all(const Ray &ray, FunctionRef<void(const RayHit &)> fn) const;

  std::optional<ClosestPointResult> closest_point(
      const float3 &point, float radius = std::numeric_limits<float>::max()) const;

  void range_query(const float3 &point, const float radius, FunctionRef<bool(int)> fn) const;
};

struct OptionallyOwnedTree {
  std::unique_ptr<Tree> owned_tree;
  const Tree *tree;
};

OptionallyOwnedTree tree_from_mesh_tris_mask(const Mesh &mesh, const IndexMask &mask);

inline std::optional<RayHit> Tree::ray_intersect(const float3 &origin,
                                                 const float3 &direction,
                                                 const float dist_max) const
{
  Ray ray;
  ray.origin = origin;
  ray.direction = direction;
  ray.dist_min = 0.0f;
  ray.dist_max = dist_max;
  return this->ray_intersect(ray);
}

}  // namespace bke::bvh
}  // namespace blender
