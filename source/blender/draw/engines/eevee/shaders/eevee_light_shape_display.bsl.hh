/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "draw_view_infos.hh"
#include "infos/eevee_light_infos.hh"
#include "infos/eevee_volume_resolved_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(draw_view)
FRAGMENT_SHADER_CREATE_INFO(eevee_light_data)
FRAGMENT_SHADER_CREATE_INFO(eevee_volume_lib)

#include "draw_view_lib.glsl"
#include "eevee_light_lib.glsl"
#include "eevee_reverse_z_lib.bsl.hh"
#include "eevee_volume_lib.bsl.hh"
#include "gpu_shader_math_constants_lib.glsl"
#include "gpu_shader_math_matrix_transform_lib.glsl"

namespace eevee::light {

struct ShapeDisplayResources {
  [[legacy_info]] ShaderCreateInfo draw_view;
  [[legacy_info]] ShaderCreateInfo eevee_light_data;
  [[legacy_info]] ShaderCreateInfo eevee_volume_lib;
};

struct ShapeDisplayVertOut {
  [[smooth]] float2 lP;
  [[smooth]] float3 P;
  [[flat]] float3 radiance;
  [[flat]] int light_index;
  [[flat]] uint light_type;
};

struct ShapeDisplayFragOut {
  [[frag_color(0)]] float4 out_color;
};

float shape_display_light_radiance_get(LightData light)
{
  if (is_sun_light(light.type)) {
    float radius = light.sun().shape_radius;
    return M_1_PI * (1.0f + 1.0f / max(radius * radius, 1e-20f));
  }

  if (is_area_light(light.type)) {
    float area = light.area().size.x * light.area().size.y * 4.0f;
    if (light.type == LIGHT_ELLIPSE) {
      area *= M_PI * 0.25f;
    }
    return 1.0f / max(M_PI * area, 1e-20f);
  }

  float radius = light.local().local.shape_radius;
  float area = 4.0f * M_PI * radius * radius;
  return 1.0f / max(M_PI * area, 1e-20f);
}

float3 shape_display_light_position_get(LightData light, float2 quad_pos)
{
  if (is_sun_light(light.type)) {
    float distance = drw_view_far() * 0.5f;
    float radius = distance * light.sun().shape_radius;
    float3 center = drw_view_position() - light.sun().direction * distance;
    float3 view_right = drw_view().viewinv[0].xyz;
    float3 view_up = drw_view().viewinv[1].xyz;
    return center + (view_right * quad_pos.x + view_up * quad_pos.y) * radius;
  }

  if (is_area_light(light.type)) {
    return transform_point(light.object_to_world, float3(quad_pos * light.area().size, 0.0f));
  }

  float radius = light.local().local.shape_radius;
  float3 center = light_position_get(light);

  if (is_oriented_disk_light(light.type)) {
    return center + (light_x_axis(light) * quad_pos.x + light_y_axis(light) * quad_pos.y) * radius;
  }

  float3 view_right = drw_view().viewinv[0].xyz;
  float3 view_up = drw_view().viewinv[1].xyz;

  if (is_sphere_light(light.type)) {
    float3 L = center - drw_view_position();
    float dist = length(L);
    if (dist > radius) {
      L /= dist;
      make_orthonormal_basis(L, view_right, view_up);
      radius = light_sphere_disk_radius(radius, dist);
    }
    else {
      radius = drw_view_far();
    }
  }

  return center + (view_right * quad_pos.x + view_up * quad_pos.y) * radius;
}

float2 shape_display_quad_position_get(int vertex_id)
{
  int quad_vertex_id = vertex_id % 6;
  float x = (quad_vertex_id == 0 || quad_vertex_id == 2 || quad_vertex_id == 5) ? -1.0f : 1.0f;
  float y = (quad_vertex_id == 0 || quad_vertex_id == 1 || quad_vertex_id == 3) ? -1.0f : 1.0f;
  return float2(x, y);
}

[[vertex]] [[clip_control]]
void shape_display_vert([[resource_table]] const ShapeDisplayResources & /*srt*/,
                        [[vertex_id]] const int vertex_id,
                        [[out]] ShapeDisplayVertOut &v_out,
                        [[position]] float4 &out_position)
{
  v_out.lP = shape_display_quad_position_get(vertex_id);
  v_out.P = float3(0.0f);
  v_out.radiance = float3(0.0f);
  v_out.light_index = 0;
  v_out.light_type = uint(LIGHT_RECT);
  out_position = float4(0.0f, 0.0f, 0.0f, 1.0f);

  int draw_index = vertex_id / 6;
  int visible_count = int(light_cull_buf.visible_count + light_cull_buf.sun_lights_len);

  if (draw_index >= visible_count) {
    return;
  }

  int light_index;
  if (draw_index < int(light_cull_buf.visible_count)) {
    light_index = draw_index;
  }
  else {
    int offset = draw_index - int(light_cull_buf.visible_count);
    light_index = int(light_cull_buf.local_lights_len) + offset;
  }

  LightData light = light_buf[light_index];
  if (!light.visible_camera) {
    return;
  }

  v_out.light_type = uint(light.type);
  v_out.light_index = light_index;
  v_out.radiance = light.color * shape_display_light_radiance_get(light);

  v_out.P = shape_display_light_position_get(light, v_out.lP);
  out_position = reverse_z::transform(drw_point_world_to_homogenous(v_out.P));
}

[[fragment]]
void shape_display_frag([[resource_table]] const ShapeDisplayResources & /*srt*/,
                        [[frag_coord]] const float4 frag_co,
                        [[in]] const ShapeDisplayVertOut &v_out,
                        [[out]] ShapeDisplayFragOut &frag_out)
{
  eLightType light_type = eLightType(v_out.light_type);
  bool is_circle = is_sun_light(light_type) || light_type == LIGHT_ELLIPSE ||
                   is_point_light(light_type);
  if (is_circle && dot(v_out.lP, v_out.lP) > 1.0f) {
    gpu_discard_fragment();
    return;
  }

  LightData light = light_buf[v_out.light_index];
  float3 P = v_out.P;
  float depth = reverse_z::read(frag_co.z);

  float2 uvs = frag_co.xy * uniform_buf.volumes.main_view_extent_inv;
  float3 radiance = v_out.radiance;
  if (is_spot_light(light_type)) {
    radiance *= light_spot_attenuation(light, -drw_world_incident_vector(P));
  }

  VolumeResolveSample vol = volume_resolve(
      float3(uvs, depth), volume_transmittance_tx, volume_scattering_tx);
  frag_out.out_color = float4(radiance * vol.transmittance, 1.0f);
}

PipelineGraphic shape_display(shape_display_vert, shape_display_frag);

}  // namespace eevee::light
