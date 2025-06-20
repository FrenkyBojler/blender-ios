/* SPDX-FileCopyrightText: 2017-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_info.hh"

FRAGMENT_SHADER_CREATE_INFO(overlay_grid_mesh)

/**
 * Procedural mesh grid
 */

#include "draw_view_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"
#include "overlay_common_lib.glsl"

void main()
{
  fragColor = finalColor;
  /* NOTE: Checking for 0 for precision. */
  if (axis_tag.z == 0.0) {
    fragColor.rgb = uniform_buf.colors.grid_axis_z.rgb;
  }
  else if (axis_tag.x == 0.0) {
    fragColor.rgb = uniform_buf.colors.grid_axis_x.rgb;
  }
  else if (axis_tag.y == 0.0) {
    fragColor.rgb = uniform_buf.colors.grid_axis_y.rgb;
  }

  lineOutput = pack_line_data(gl_FragCoord.xy, edgeStart, edgePos);
}
