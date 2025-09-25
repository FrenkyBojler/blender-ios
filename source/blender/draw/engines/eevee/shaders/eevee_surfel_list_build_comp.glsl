/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Sort a buffer of surfel list by distance along a direction.
 * The resulting surfel lists are then the equivalent of a series of ray cast in the same
 * direction. The fact that the surfels are sorted gives proper occlusion.
 *
 * Dispatched as 1 thread per surfel.
 */

#include "infos/eevee_lightprobe_volume_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_surfel_list_build)

#include "eevee_surfel_list_lib.glsl"

void main()
{
  int surfel_index = int(gl_GlobalInvocationID.x);
  if (surfel_index >= int(capture_info_buf.surfel_len)) {
    return;
  }

  float ray_distance;
  int list_index = surfel_list_index_get(
      list_info_buf.ray_grid_size, surfel_buf[surfel_index].position, ray_distance);

  atomicAdd(list_counter_buf[list_index], 1);
  /* Do separate assignment to avoid reference to buffer in arguments which is tricky to cross
   * compile. */
  surfel_buf[surfel_index].ray_distance = ray_distance;
  surfel_buf[surfel_index].list = list_index;
}
