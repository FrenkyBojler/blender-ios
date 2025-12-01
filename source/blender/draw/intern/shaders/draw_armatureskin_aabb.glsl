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

  uint idx0 = base_idx;
  uint idx1 = base_idx + 1u;
  uint idx2 = base_idx + 2u;
  uint idx3 = base_idx + 3u;
  uint idx4 = base_idx + 4u;
  uint idx5 = base_idx + 5u;
  uint idx6 = base_idx + 6u;
  uint idx7 = base_idx + 7u;

  if (idx0 < num_vertices) {
    float3 pos0 = skinned_positions[idx0].xyz;
    bool valid0 = all(equal(pos0, pos0));
    thread_min = valid0 ? min(thread_min, pos0) : thread_min;
    thread_max = valid0 ? max(thread_max, pos0) : thread_max;
    found_valid = found_valid || valid0;
  }

  if (idx1 < num_vertices) {
    float3 pos1 = skinned_positions[idx1].xyz;
    bool valid1 = all(equal(pos1, pos1));
    thread_min = valid1 ? min(thread_min, pos1) : thread_min;
    thread_max = valid1 ? max(thread_max, pos1) : thread_max;
    found_valid = found_valid || valid1;
  }

  if (idx2 < num_vertices) {
    float3 pos2 = skinned_positions[idx2].xyz;
    bool valid2 = all(equal(pos2, pos2));
    thread_min = valid2 ? min(thread_min, pos2) : thread_min;
    thread_max = valid2 ? max(thread_max, pos2) : thread_max;
    found_valid = found_valid || valid2;
  }

  if (idx3 < num_vertices) {
    float3 pos3 = skinned_positions[idx3].xyz;
    bool valid3 = all(equal(pos3, pos3));
    thread_min = valid3 ? min(thread_min, pos3) : thread_min;
    thread_max = valid3 ? max(thread_max, pos3) : thread_max;
    found_valid = found_valid || valid3;
  }

  if (idx4 < num_vertices) {
    float3 pos4 = skinned_positions[idx4].xyz;
    bool valid4 = all(equal(pos4, pos4));
    thread_min = valid4 ? min(thread_min, pos4) : thread_min;
    thread_max = valid4 ? max(thread_max, pos4) : thread_max;
    found_valid = found_valid || valid4;
  }

  if (idx5 < num_vertices) {
    float3 pos5 = skinned_positions[idx5].xyz;
    bool valid5 = all(equal(pos5, pos5));
    thread_min = valid5 ? min(thread_min, pos5) : thread_min;
    thread_max = valid5 ? max(thread_max, pos5) : thread_max;
    found_valid = found_valid || valid5;
  }

  if (idx6 < num_vertices) {
    float3 pos6 = skinned_positions[idx6].xyz;
    bool valid6 = all(equal(pos6, pos6));
    thread_min = valid6 ? min(thread_min, pos6) : thread_min;
    thread_max = valid6 ? max(thread_max, pos6) : thread_max;
    found_valid = found_valid || valid6;
  }

  if (idx7 < num_vertices) {
    float3 pos7 = skinned_positions[idx7].xyz;
    bool valid7 = all(equal(pos7, pos7));
    thread_min = valid7 ? min(thread_min, pos7) : thread_min;
    thread_max = valid7 ? max(thread_max, pos7) : thread_max;
    found_valid = found_valid || valid7;
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
