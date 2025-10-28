/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "draw_skinning_infos.hh"
#pragma BLENDER_REQUIRE(gpu_shader_utildefines_lib.glsl)

uint float_to_sortable_uint(float f) {
  uint u = floatBitsToUint(f);
  return (u & 0x80000000u) != 0u ? ~u : (u | 0x80000000u);
}

shared vec3 local_min[64];
shared vec3 local_max[64];

void main() {
  uint gid = gl_GlobalInvocationID.x;
  uint lid = gl_LocalInvocationID.x;
  uint group_size = gl_WorkGroupSize.x;
  uint num_vertices = uint(vertex_count_aabb);

  vec3 thread_min = vec3(1e30);
  vec3 thread_max = vec3(-1e30);
  bool found_valid = false;

  for (uint i = gid; i < num_vertices; i += gl_NumWorkGroups.x * group_size) {
    vec3 pos = skinned_positions[i].xyz;

    /* Only process finite positions */
    if (all(equal(pos, pos))) { /* NaN check */
      thread_min = min(thread_min, pos);
      thread_max = max(thread_max, pos);
      found_valid = true;
    }
  }

  /* If no valid vertices found, use neutral values that won't affect the result */
  if (!found_valid) {
    thread_min = vec3(1e30);
    thread_max = vec3(-1e30);
  }

  local_min[lid] = thread_min;
  local_max[lid] = thread_max;

  barrier();

  for (uint stride = group_size / 2u; stride > 0u; stride /= 2u) {
    if (lid < stride) {
      local_min[lid] = min(local_min[lid], local_min[lid + stride]);
      local_max[lid] = max(local_max[lid], local_max[lid + stride]);
    }
    barrier();
  }

  if (lid == 0u) {
    vec3 group_min = local_min[0];
    vec3 group_max = local_max[0];

    if (group_min.x < 1e29) {
      atomicMin(bounds_result_buf[0], float_to_sortable_uint(group_min.x));
      atomicMin(bounds_result_buf[1], float_to_sortable_uint(group_min.y));
      atomicMin(bounds_result_buf[2], float_to_sortable_uint(group_min.z));
    }

    if (group_max.x > -1e29) {
      atomicMax(bounds_result_buf[3], float_to_sortable_uint(group_max.x));
      atomicMax(bounds_result_buf[4], float_to_sortable_uint(group_max.y));
      atomicMax(bounds_result_buf[5], float_to_sortable_uint(group_max.z));
    }
  }
}