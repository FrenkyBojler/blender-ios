/* SPDX-FileCopyrightText: 2021-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * GPU generated indirection buffer. Updated on topology change.
 * One thread processes one curve.
 */

#include "draw_curves_info.hh"

COMPUTE_SHADER_CREATE_INFO(draw_curves_topology)

#include "gpu_shader_offset_indices_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

void main()
{
  if (gl_GlobalInvocationID.x >= uint(curves_count)) {
    return;
  }
  uint curve_id = gl_GlobalInvocationID.x + uint(curves_start);

  /* This range represent the number of cyclic curves before this curve (start).
   * The size (0 or 1) is a boolean telling if this curve is cyclic. */
  IndexRange cyclic_offset = IndexRange(0, 0);
  if (use_cyclic) {
    cyclic_offset = offset_indices::load_range_from_buffer(cyclic_offsets_buf, curve_id);
  }

  IndexRange points = offset_indices::load_range_from_buffer(evaluated_offsets_buf, curve_id);
  int index_start = points.start() + cyclic_offset.start();
  int num_segment = points.size() + cyclic_offset.size();

  int indirection_index_count = num_segment + (is_ribbon_topology ? 1 : -1);
  index_start += (is_ribbon_topology ? curve_id : -curve_id);

  for (int i = 0; i < indirection_index_count; i++) {
    int value = int((i == 0) ? curve_id : -i);
    if (i == num_segment) {
      value = INT_MAX;
    }
    indirection_buf[index_start + i] = value;
  }
}
