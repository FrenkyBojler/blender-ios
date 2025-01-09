/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/gpu_shader_index_info.hh"

COMPUTE_SHADER_CREATE_INFO(gpu_shader_index_curve_segment_points)


void main()
{
  ivec3 gid = ivec3(gl_GlobalInvocationID);
  ivec3 nthreads = ivec3(gl_NumWorkGroups * uvec3(gl_WorkGroupSize));
  int next_segment = nthreads.y * nthreads.z;
  for (int segment = gid.y + gid.z * nthreads.y; segment < segments_num; segment += next_segment) {
    int offset = segments[segment].evaluated_points_offset;
    int offset_next = segments[segment + 1].evaluated_points_offset;
    for (int eval_index = offset + gid.x; eval_index < offset_next; eval_index += nthreads.x) {
      out_indices[eval_index] = segment;
    }
  }
}
