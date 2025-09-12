/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

// TODO: we should include a library that will map the vulkan extension to `gpu_*` attributes.
// This allows metal to map to their internal functions.
// See https://github.com/KhronosGroup/GLSL/blob/main/extensions/ext/GLSL_EXT_ray_query.txt

void main()
{
  uint ray_index = gl_GlobalInvocationID.x;

  rayQueryEXT query;
  rayQueryInitializeEXT(query,
                        scene_as,
                        gl_RayFlagsTerminateOnFirstHitEXT,
                        intersection_mask_in,
                        ray_pos_in[ray_index].xyz,
                        0.01,
                        ray_dir_in[ray_index].xyz,
                        5.0);
  rayQueryProceedEXT(query);

  bool is_hit = rayQueryGetIntersectionTypeEXT(query, true) !=
                gl_RayQueryCommittedIntersectionNoneEXT;

  hit_out[ray_index] = uint(is_hit);
}
