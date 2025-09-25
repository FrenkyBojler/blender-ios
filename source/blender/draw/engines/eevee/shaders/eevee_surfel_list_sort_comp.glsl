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

void main()
{
  int item_id = int(gl_GlobalInvocationID.x);
  if (item_id >= int(capture_info_buf.surfel_len)) {
    return;
  }

  float ray_distance = list_item_distance_buf[item_id];
  int prefix = 0;
  IndexRange list_range = list_range_buf[list_id];
  /* Prefix sum inside the list range. */
  for (int i = list_range.start; i < list_range.end(); i++) {
    if (list_item_distance_buf[i] < ray_distance) {
      prefix++;
    }
  }

  int surfel_id = list_item_buf[item_id].surfel_id;
}
