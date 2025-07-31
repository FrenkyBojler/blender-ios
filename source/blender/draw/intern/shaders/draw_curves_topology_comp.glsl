/* SPDX-FileCopyrightText: 2021-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * GPU generated indirection buffer. Updated on topology change.
 * One thread processes one curve.
 */

#include "draw_curves_info.hh"

COMPUTE_SHADER_CREATE_INFO(draw_curves_topology)

#include "gpu_shader_utildefines_lib.glsl"

void main()
{
  if (gl_GlobalInvocationID.x >= uint(curves_count)) {
    return;
  }
  uint curve_id = gl_GlobalInvocationID.x + uint(curves_start);

  uint index_start = evaluated_offsets_buf[curve_id];
  uint num_segment = evaluated_offsets_buf[curve_id + 1] - index_start;

  uint indirection_index_count = num_segment + (is_ribbon_topology ? 1 : -1);
  index_start += (is_ribbon_topology ? curve_id : -curve_id);

  for (uint i = 0; i < indirection_index_count; i++) {
    int value = int((i == 0u) ? curve_id : -i);
    if (i == num_segment) {
      value = INT_MAX;
    }
    indirection_buf[index_start + i] = value;
  }
}
