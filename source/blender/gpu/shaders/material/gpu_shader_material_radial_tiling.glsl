/* SPDX-FileCopyrightText: 2024 Tenkai Raiko
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "gpu_shader_math_base_lib.glsl"

/* Define macro flags for code translation. */
/* No macro flags necessary, as code is translated to GLSL by default. */

/* The rounded polygon calculation functions are defined in radial_tiling_generic.h. */
#include "radial_tiling_generic.h"

/* Undefine macro flags used for code translation. */
/* No macro flags necessary, as code is translated to GLSL by default. */

void node_radial_tiling(vec3 coord,
                        float r_gon_sides,
                        float r_gon_roundness,
                        float normalize_r_gon_parameter,
                        float calculate_r_gon_parameter_field,
                        float calculate_segment_id,
                        float calculate_max_unit_parameter,
                        float calculate_x_axis_A_angle_bisector,
                        out vec3 out_segment_coordinates,
                        out float out_segment_id,
                        out float out_max_unit_parameter,
                        out float out_x_axis_A_angle_bisector)
{
  if (bool(calculate_r_gon_parameter_field) || bool(calculate_max_unit_parameter) ||
      bool(calculate_x_axis_A_angle_bisector))
  {
    vec4 out_variables = calculate_out_variables(bool(calculate_r_gon_parameter_field),
                                                 bool(calculate_max_unit_parameter),
                                                 bool(normalize_r_gon_parameter),
                                                 max(r_gon_sides, 2.0),
                                                 clamp(r_gon_roundness, 0.0, 1.0),
                                                 vec2(coord.x, coord.y));

    out_segment_coordinates = vec3(out_variables.y, out_variables.x, 0.0);
    out_max_unit_parameter = out_variables.z;
    out_x_axis_A_angle_bisector = out_variables.w;
  }

  if (bool(calculate_segment_id)) {
    out_segment_id = calculate_out_segment_id(max(r_gon_sides, 2.0), vec2(coord.x, coord.y));
  }
}
