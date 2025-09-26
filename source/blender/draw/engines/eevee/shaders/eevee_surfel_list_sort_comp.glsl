/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * TODO
 *
 * Dispatched as 1 thread per surfel.
 */

#include "infos/eevee_lightprobe_volume_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_surfel_list_sort)

#include "gpu_shader_index_range_lib.glsl"

void main()
{
  int item_id = int(gl_GlobalInvocationID.x);
  if (item_id >= int(capture_info_buf.surfel_len)) {
    return;
  }

  int surfel_id = list_item_surfel_id_buf[item_id];
  int list_id = surfel_buf[surfel_id].list_id;
  float ray_distance = list_item_distance_buf[item_id];

  IndexRange list_range = IndexRange(list_range_buf[list_id * 2 + 0],
                                     list_range_buf[list_id * 2 + 1]);
  int prefix = 0;
  /* Prefix sum inside the list range. */
  for (int i = list_range.start(); i <= list_range.last(); i++) {
    if (list_item_distance_buf[i] > ray_distance) {
      prefix++;
    }
    else if (list_item_distance_buf[i] == ray_distance) {
      /* Resolve the case where 2 items have the same value. */
      if (list_item_distance_buf[i] < ray_distance) {
        prefix++;
      }
    }
  }

  int sorted_id = list_range.start() + prefix;
  sorted_surfel_id_buf[sorted_id] = surfel_id;
  surfel_buf[surfel_id].index_in_sorted_list = sorted_id;
}
