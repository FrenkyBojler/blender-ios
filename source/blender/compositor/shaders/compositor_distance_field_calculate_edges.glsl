/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_compositor_jump_flooding_lib.glsl"
#include "gpu_shader_compositor_texture_utilities.glsl"

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);

  bool is_masked = texture_load(mask_tx, texel).x != 0.0f;
  bool is_edge = false;

  if (is_masked)
  {
    bool has_masked_neighbors = false;
	  bool has_non_masked_neighbors = false;

    for (int j = -1; j <= 1; j++) {
      for (int i = -1; i <= 1; i++) {
        int2 offset = int2(i, j);
  
        /* Exempt the center pixel. */
        if (all(equal(offset, int2(0)))) {
          continue;
        }

        if (!include_diagonal)
        {
          if (abs(j) == abs(i))
            continue;
        }
  
        if (texture_load(mask_tx, texel + offset).x == 0.0f) {
          has_non_masked_neighbors = true;
        }
		    else {
          has_masked_neighbors = true;
		    }
  
        /* Both are true, no need to continue. */
        if (has_non_masked_neighbors && has_masked_neighbors) {
          break;
        }
      }
    }

    is_edge = (has_non_masked_neighbors && has_masked_neighbors);
  }

  int2 jump_flood_value = initialize_jump_flooding_value(texel, is_edge);

  imageStore(edges_img, texel, int4(jump_flood_value, int2(0)));
}