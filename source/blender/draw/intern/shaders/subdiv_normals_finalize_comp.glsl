/* SPDX-FileCopyrightText: 2021-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Finalize normals after accumulating or interpolation.
 *
 * Normals are accumulated in the `subdiv_normals_accumulate_comp.glsl`, (custom) split normals are
 * interpolated as custom data layer in `subdiv_custom_data_interp_comp.glsl` using GPU_COMP_U16.
 */

#include "subdiv_lib.glsl"

#ifdef USE_GPU_SHADER_CREATE_INFO
#  ifdef CUSTOM_NORMALS
COMPUTE_SHADER_CREATE_INFO(subdiv_custom_normals_finalize)
#  else
COMPUTE_SHADER_CREATE_INFO(subdiv_normals_finalize)
#  endif
#else
#  ifdef CUSTOM_NORMALS
struct CustomNormal {
  float x;
  float y;
  float z;
};

layout(std430, binding = 0) readonly buffer inputNormals
{
  CustomNormal custom_normals[];
};
#  else
layout(std430, binding = 0) readonly buffer inputNormals
{
  vec3 vertex_normals[];
};

layout(std430, binding = 1) readonly buffer inputSubdivVertLoopMap
{
  uint vert_loop_map[];
};
#  endif

layout(std430, binding = 2) buffer outputPosNor
{
  PosNorLoop pos_nor[];
};
#endif

void main()
{
  /* We execute for each quad. */
  uint quad_index = get_global_invocation_index();
  if (quad_index >= total_dispatch_size) {
    return;
  }

  uint start_loop_index = quad_index * 4;

#ifdef CUSTOM_NORMALS
  for (int i = 0; i < 4; i++) {
    CustomNormal custom_normal = custom_normals[start_loop_index + i];
    vec3 nor = vec3(custom_normal.x, custom_normal.y, custom_normal.z);
    pos_nor[start_loop_index + i] = subdiv_set_vertex_nor(pos_nor[start_loop_index + i],
                                                          normalize(nor));
  }
#else
  for (int i = 0; i < 4; i++) {
    uint subdiv_vert_index = vert_loop_map[start_loop_index + i];
    vec3 nor = vertex_normals[subdiv_vert_index];
    pos_nor[start_loop_index + i] = subdiv_set_vertex_nor(pos_nor[start_loop_index + i], nor);
  }
#endif
}
