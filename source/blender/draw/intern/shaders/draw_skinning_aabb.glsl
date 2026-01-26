/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "draw_skinning_infos.hh"
#pragma BLENDER_REQUIRE(gpu_shader_utildefines_lib.glsl)

uint float_to_sortable_uint(float f)
{
  uint u = floatBitsToUint(f);
  return (u & 0x80000000u) != 0u ? ~u : (u | 0x80000000u);
}

void main()
{
  uint gid = gl_GlobalInvocationID.x;

  const uint VERTICES_PER_THREAD = 8;
  uint base_idx = gid * VERTICES_PER_THREAD;
  uint num_vertices = uint(vertex_count_aabb);

  if (base_idx >= num_vertices) {
    return;
  }

  /* Precompute constants to reduce immediate constant cache misses. */
  const float INIT_MIN = 1e30;
  const float INIT_MAX = -1e30;

  float3 thread_min = float3(INIT_MIN);
  float3 thread_max = float3(INIT_MAX);
  bool found_valid = false;

  for (uint i = 0; i < VERTICES_PER_THREAD; ++i) {
    uint idx = base_idx + i;
    if (idx >= num_vertices)
      break;

    float3 pos = skinned_positions[idx].xyz;
    bool valid = all(equal(pos, pos));  // NaN check
    thread_min = valid ? min(thread_min, pos) : thread_min;
    thread_max = valid ? max(thread_max, pos) : thread_max;
    found_valid = found_valid || valid;
  }

  if (found_valid) {
    uint min_x = float_to_sortable_uint(thread_min.x);
    uint min_y = float_to_sortable_uint(thread_min.y);
    uint min_z = float_to_sortable_uint(thread_min.z);
    uint max_x = float_to_sortable_uint(thread_max.x);
    uint max_y = float_to_sortable_uint(thread_max.y);
    uint max_z = float_to_sortable_uint(thread_max.z);

    atomicMin(bounds_result_buf[0], min_x);
    atomicMin(bounds_result_buf[1], min_y);
    atomicMin(bounds_result_buf[2], min_z);
    atomicMax(bounds_result_buf[3], max_x);
    atomicMax(bounds_result_buf[4], max_y);
    atomicMax(bounds_result_buf[5], max_z);
  }
}
