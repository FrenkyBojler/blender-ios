/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_extra_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_extra_camera_pano)

#include "draw_view_clipping_lib.glsl"
#include "draw_view_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"
#include "overlay_common_lib.glsl"
#include "select_lib.glsl"

void main()
{
  select_id_set(in_select_buf[gl_InstanceID]);

  float4x4 inst_obmat = data_buf[gl_InstanceID].object_to_world;
  float4x4 input_mat = inst_obmat;

  float4 inst_data = float4(input_mat[0][3], input_mat[1][3], input_mat[2][3], input_mat[3][3]);
  float4 color = data_buf[gl_InstanceID].color_;
  float4x4 obmat = input_mat;
  obmat[0][3] = obmat[1][3] = obmat[2][3] = 0.0f;
  obmat[3][3] = 1.0f;

  final_color = color;
  if (color.a < 0.0f) {
    final_color.a = 1.0f;
  }

  float2 camera_corner = inst_data.xy;
  float2 camera_center = inst_data.zw;
  float camera_dist = color.a;

  float3 vpos = pos;
  float3 vofs = float3(0.0f);

  if (flag_test(vclass, VCLASS_CAMERA_FRAME)) {
    if (camera_dist > 0.0f) {
      vpos.z = -abs(camera_dist);
    }
    else {
      vpos.z *= -abs(camera_dist);
    }
    vpos.xy = (camera_center + camera_corner * vpos.xy) * abs(vpos.z);
  }

  float3 world_pos = (obmat * float4(vofs + vpos, 1.0f)).xyz;

  gl_Position = drw_point_world_to_homogenous(world_pos);

  edge_pos = edge_start = ((gl_Position.xy / gl_Position.w) * 0.5f + 0.5f) *
                          uniform_buf.size_viewport;

#if defined(SELECT_ENABLE)
  gl_Position.xy += uniform_buf.size_viewport_inv * gl_Position.w *
                    ((gl_VertexID % 2 == 0) ? -1.0f : 1.0f);
#endif

  view_clipping_distances(world_pos);
}
