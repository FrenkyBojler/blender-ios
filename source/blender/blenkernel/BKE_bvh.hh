/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_vector_types.hh"

struct RTCDeviceTy;
struct RTCSceneTy;

namespace blender {

struct Mesh;

namespace bke::bvh {

struct Ray {
  static constexpr unsigned int MASK_FULL = 0xffffffff;
  static constexpr unsigned int FLAGS_NONE = 0;

  /* Coordinate of ray origin */
  float3 origin = float3(0.0f);
  /* Start of ray segment relative to ray length */
  float dist_min = 0.0f;

  /* Ray direction */
  float3 direction = float3(1.0f, 0.0f, 0.0f);

  /* Time of this ray for motion blur */
  float time = 0.0f;

  /* End of ray segment relative to ray length (set to hit distance) */
  float dist_max = 1.0f;

  /* Ray mask */
  unsigned int mask = MASK_FULL;
  /* Ray ID */
  unsigned int id = 0;
  /* Ray flags */
  unsigned int flags = FLAGS_NONE;
};

struct Hit {
  static constexpr unsigned int INVALID_INSTANCE_ID = 0xffffffff;
  static constexpr int MAX_INSTANCE_LEVEL = 8;

  /* Geometry normal */
  float3 normal;
  /* Barycentric UV of hit */
  float2 uv;

  /* Primitive ID */
  unsigned int primitive_id;
  /* Geometry ID */
  unsigned int geometry_id;
  /* Instance ID */
  unsigned int instance_id[MAX_INSTANCE_LEVEL];
};

struct RayHit {
  Ray ray;
  Hit hit;
};

class Tree {
 private:
  RTCDeviceTy *rtc_device = nullptr;
  RTCSceneTy *rtc_scene = nullptr;

 public:
  Tree();
  Tree(const Tree &) = delete;
  ~Tree();

  Tree &operator=(const Tree &) = delete;

  void free();

  void build_single_mesh(const Mesh &mesh);

  bool ray_intersect1(const Ray &ray, RayHit &r_hit) const;
};

}  // namespace bke::bvh
}  // namespace blender
