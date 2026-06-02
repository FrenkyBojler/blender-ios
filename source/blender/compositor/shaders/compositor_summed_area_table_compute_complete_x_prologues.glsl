/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_summed_area_table_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_summed_area_table_compute_complete_x_prologues)

#include "gpu_shader_compositor_texture_utilities.glsl"

/* Shared memory for initializing the reduction data. */
shared float4 reduction_data[gl_WorkGroupSize.x];

/* See the compute_complete_x_prologues function for a description of this shader. */
void main()
{
  /* Note that the X prologues are stored transposed, hence the horizontal dispatch domain, even
   * though, conceptually, the dispatch domain covers the vertical axis of the image. */
  int x = int(gl_GlobalInvocationID.x);

  float4 accumulated_color = float4(0.0f);
  for (int y = 0; y < texture_size(incomplete_x_prologues_tx).y; y++) {
    accumulated_color += texture_load(incomplete_x_prologues_tx, int2(x, y), float4(0.0f));
    imageStore(complete_x_prologues_img, int2(x, y), accumulated_color);

    if (gl_WorkGroupID.x == 0) {
      /* Note that the first row of sums is the result of summing the prologues of a virtual block
       * that is before the first row of blocks and we assume that those prologues are all zeros,
       * so we set the sum to zero in that case. This is implemented by setting the sums of the
       * first vertical work-group to zero, white latter work-groups are summed as usual and
       * stored starting from the second row. */
      imageStore(complete_x_prologues_sum_img, int2(y, 0), float4(0.0f));
    }

    /* Load initial values into shared memory. */
    if (gl_LocalInvocationIndex < gl_WorkGroupSize.x) {
      reduction_data[gl_LocalInvocationIndex] = accumulated_color;
    }
    barrier();

    /* Only thread 0 accumulates all shared values into a single local variable.
     * Only reads from shared memory (no writes), so there is no store-load data race. */
    if (gl_LocalInvocationIndex == 0) {
      float4 reduced_sum = reduction_data[0];
      for (int i = 1; i < int(gl_WorkGroupSize.x); i++) {
        reduced_sum += reduction_data[i];
      }
      /* Note that we store using a transposed texel, but that is only to undo the transposition
       * mentioned above. Also note that we start from the second row because the first row is
       * set to zero as mentioned above. */
      imageStore(complete_x_prologues_sum_img, int2(y, gl_WorkGroupID.x + 1), reduced_sum);
    }
  }
}
