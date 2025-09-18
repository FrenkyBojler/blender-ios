/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_extra_info.hh"

VERTEX_SHADER_CREATE_INFO(overlay_dome_hdr)

#include "draw_view_clipping_lib.glsl"
#include "draw_view_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"
#include "overlay_common_lib.glsl"
#include "select_lib.glsl"

#define M_PI 3.1415926535897932
#define M_2PI 6.2831853071795864

/* Apply Euler rotation to direction vector for dome light (same as Cycles) */
float3 apply_dome_rotation(float3 direction, float3 rotation)
{
  /* Apply Euler rotations in ZYX order (yaw, pitch, roll) - same as Cycles */
  float cos_x = cos(rotation.x), sin_x = sin(rotation.x);
  float cos_y = cos(rotation.y), sin_y = sin(rotation.y);  
  float cos_z = cos(rotation.z), sin_z = sin(rotation.z);
  
  /* Rotation matrix multiplication: R = Rz * Ry * Rx */
  float3x3 rot_matrix = float3x3(
    cos_z * cos_y, cos_z * sin_y * sin_x - sin_z * cos_x, cos_z * sin_y * cos_x + sin_z * sin_x,
    sin_z * cos_y, sin_z * sin_y * sin_x + cos_z * cos_x, sin_z * sin_y * cos_x - cos_z * sin_x,
    -sin_y,        cos_y * sin_x,                         cos_y * cos_x
  );
  
  return rot_matrix * direction;
}

/* Convert 3D position to equirectangular UV coordinates for HDR projection */
float2 dome_position_to_uv(float3 world_pos, float4x4 object_to_world_matrix, float3 dome_rotation, bool flip_u, bool flip_v)
{
  /* Get the inverse transformation matrix to transform from world to object space */
  float4x4 world_to_object = inverse(object_to_world_matrix);
  
  /* Transform world position to object space (dome local space) */
  float3 local_pos = (world_to_object * float4(world_pos, 1.0f)).xyz;
  
  /* Get direction vector in object space (normalized) */
  float3 dir = normalize(local_pos);
  
  /* Apply dome rotation (same logic as Cycles) */
  float3 rotated_dir = apply_dome_rotation(dir, dome_rotation);
  
  /* Convert to spherical coordinates */
  float theta = atan(rotated_dir.y, rotated_dir.x); /* Azimuth: -PI to PI */
  float phi = acos(clamp(rotated_dir.z, -1.0, 1.0)); /* Polar: 0 to PI */
  
  /* Convert to UV coordinates [0,1] */
  float u = (theta + float(M_PI)) / float(M_2PI); /* Map -PI..PI to 0..1 */
  float v = phi / float(M_PI); /* Map 0..PI to 0..1 */
  
  /* Apply UV flipping if enabled */
  if (flip_u) {
    u = 1.0f - u;  /* flip_u flips left-right (horizontally) */
  }
  if (flip_v) {
    v = 1.0f - v;  /* flip_v flips top-bottom (vertically) */
  }
  
  return float2(u, v);
}

void main()
{
  select_id_set(in_select_buf[gl_InstanceID]);

  /* Use standard ExtraInstanceData system like overlay_extra_vert.glsl */
  float4x4 inst_obmat = data_buf[gl_InstanceID].object_to_world;
  float4x4 input_mat = inst_obmat;
  
  /* Extract data packed inside the unused float4x4 members (same as overlay_extra_vert.glsl) */
  float4 inst_data = float4(input_mat[0][3], input_mat[1][3], input_mat[2][3], input_mat[3][3]);
  float4 color = data_buf[gl_InstanceID].color_;
  float inst_color_data = color.a;
  float4x4 obmat = input_mat;
  obmat[0][3] = obmat[1][3] = obmat[2][3] = 0.0f;
  obmat[3][3] = 1.0f;

  final_color = color;
  if (color.a < 0.0f) {
    final_color.a = 1.0f;
  }

  float3 lamp_area_size_3d = inst_data.xyz;
  float3 vpos = pos;

  /* Use same scaling logic as overlay_extra_vert.glsl for VCLASS_LIGHT_AREA_SHAPE */
  /* HACK: use alpha color for spots to pass the area_size. */
  if (inst_color_data < 0.0f) {
    float2 lamp_area_size = float2(-inst_color_data);
    vpos.xy *= lamp_area_size;
  }
  else {
    /* Use 3D scaling for dome lights and other area lights that set all three components */
    if (lamp_area_size_3d.z > 0.0f) {
      vpos *= lamp_area_size_3d;
    }
    else {
      float2 lamp_area_size = lamp_area_size_3d.xy;
      vpos.xy *= lamp_area_size;
    }
  }
  
  /* Extract dome data from the additional fields */
  float3 dome_rotation = data_buf[gl_InstanceID].dome_rotation;
  bool32_t dome_hdr_flag = data_buf[gl_InstanceID].has_hdr;
  bool32_t flip_u = data_buf[gl_InstanceID].flip_u;
  bool32_t flip_v = data_buf[gl_InstanceID].flip_v;

  /* Transform to world space */
  float3 world_pos = (obmat * float4(vpos, 1.0f)).xyz;
  
  /* Calculate UV coordinates for equirectangular projection */
  /* Use the original vertex position on the sphere (before scaling) */
  float3 dir = normalize(pos);
  
  /* Apply dome rotation if needed */
  if (dome_rotation.x != 0.0f || dome_rotation.y != 0.0f || dome_rotation.z != 0.0f) {
    dir = apply_dome_rotation(dir, dome_rotation);
  }
  
  /* Equirectangular projection (lat-long mapping) for Blender coordinate system */
  /* In Blender: X=depth, Y=width, Z=height */
  /* theta: angle around Z axis (longitude) - using Y/X for horizontal rotation */
  /* phi: angle from Z axis (latitude) - using Z for vertical */
  float theta = atan(dir.y, dir.x); /* -PI to PI */
  float phi = acos(clamp(dir.z, -1.0f, 1.0f)); /* 0 to PI */
  
  /* Map to UV coordinates [0,1] */
  float dome_u = (theta / float(M_2PI)) + 0.5f;
  float dome_v = phi / float(M_PI);
  
  /* Apply UV flipping if enabled */
  if (flip_u) {
    dome_u = 1.0f - dome_u;  /* flip_u flips left-right (horizontally) */
  }
  if (flip_v) {
    dome_v = 1.0f - dome_v;  /* flip_v flips top-bottom (vertically) */
  }
  
  uv_coords = float2(dome_u, dome_v);
  
  /* Pass HDR flag to fragment shader */
  has_hdr = dome_hdr_flag ? 1.0f : 0.0f;

  gl_Position = drw_point_world_to_homogenous(world_pos);

  /* Convert to screen position [0..sizeVp]. */
  edge_pos = edge_start = ((gl_Position.xy / gl_Position.w) * 0.5f + 0.5f) *
                          uniform_buf.size_viewport;

#if defined(SELECT_ENABLE)
  /* HACK: to avoid losing sub-pixel object in selections, we add a bit of randomness to the
   * wire to at least create one fragment that will pass the occlusion query. */
  gl_Position.xy += uniform_buf.size_viewport_inv * gl_Position.w *
                    ((gl_VertexID % 2 == 0) ? -1.0f : 1.0f);
#endif

  view_clipping_distances(world_pos);
}