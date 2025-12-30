/* SPDX-FileCopyrightText: 2018-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once
#pragma create_info

#include "draw_view_infos.hh"  // IWYU pragma: export

VERTEX_SHADER_CREATE_INFO(draw_modelmat_with_custom_id)

#include "draw_curves_lib.glsl"
#include "draw_model_lib.glsl"
#include "draw_view_clipping_lib.glsl"
#include "draw_view_lib.glsl"
#include "gpu_shader_math_base_lib.glsl"
#include "workbench_common.bsl.hh"
#include "workbench_image.bsl.hh"
#include "workbench_matcap.bsl.hh"
#include "workbench_material.bsl.hh"
#include "workbench_world_light.bsl.hh"

namespace workbench::prepass {

/* TODO(fclem): Move to workbench. */
#define WORKBENCH_LIGHTING_STUDIO 0
#define WORKBENCH_LIGHTING_MATCAP 1
#define WORKBENCH_LIGHTING_FLAT 2

/* Special function only to be used with calculate_transparent_weight(). */
float linear_zdepth(float depth, float4x4 proj_mat)
{
  if (proj_mat[3][3] == 0.0f) {
    float d = 2.0f * depth - 1.0f;
    return -proj_mat[3][2] / (d + proj_mat[2][2]);
  }
  /* Return depth from near plane. */
  float z_delta = -2.0f / proj_mat[2][2];
  return depth * z_delta;
}

/* Based on :
 * McGuire and Bavoil, Weighted Blended Order-Independent Transparency, Journal of
 * Computer Graphics Techniques (JCGT), vol. 2, no. 2, 122–141, 2013
 */
float calculate_transparent_weight(float frag_z)
{
  float z = linear_zdepth(frag_z, drw_view().winmat);
#if 0
  /* Eq 10 : Good for surfaces with varying opacity (like particles) */
  float a = min(1.0f, alpha * 10.0f) + 0.01f;
  float b = -frag_z * 0.95f + 1.0f;
  float w = a * a * a * 3e2f * b * b * b;
#else
  /* Eq 7 put more emphasis on surfaces closer to the view. */
  // float w = 10.0f / (1e-5f + pow(abs(z) / 5.0f, 2.0f) + pow(abs(z) / 200.0f, 6.0f)); /* Eq 7 */
  // float w = 10.0f / (1e-5f + pow(abs(z) / 10.0f, 3.0f) + pow(abs(z) / 200.0f, 6.0f)); /* Eq 8 */
  // float w = 10.0f / (1e-5f + pow(abs(z) / 200.0f, 4.0f)); /* Eq 9 */
  /* Same as eq 7, but optimized. */
  float a = abs(z) / 5.0f;
  float b = abs(z) / 200.0f;
  b *= b;
  float w = 10.0f / ((1e-5f + a * a) + b * (b * b)); /* Eq 7 */
#endif
  return clamp(w, 1e-2f, 3e2f);
}

/* From http://libnoise.sourceforge.net/noisegen/index.html */
float integer_noise(int n)
{
  /* Integer bit-shifts cause precision issues due to overflow
   * in a number of workbench tests. Use uint instead. */
  uint nn = (uint(n) >> 13u) ^ uint(n);
  nn = (nn * (nn * nn * 60493u + 19990303u) + 1376312589u) & 0x7fffffffu;
  return (float(nn) / 1073741824.0f);
}

float3 hair_random_normal(float3 tangent, float3 binor, float3 nor, float rand)
{
  /* To "simulate" anisotropic shading, randomize hair normal per strand. */
  nor = normalize(mix(nor, -tangent, rand * 0.1f));
  float cos_theta = (rand * 2.0f - 1.0f) * 0.2f;
  float sin_theta = sin_from_cos(cos_theta);
  nor = nor * sin_theta + binor * cos_theta;
  return nor;
}

void hair_random_material(float rand, float3 &color, float &roughness, float &metallic)
{
  /* Center noise around 0. */
  rand -= 0.5f;
  rand *= 0.1f;
  /* Add some variation to the hairs to avoid uniform look. */
  metallic = saturate(metallic + rand);
  roughness = saturate(roughness + rand);
  /* Modulate by color intensity to reduce very high contrast when color is dark. */
  color = saturate(color + rand * (color + 0.05f));
}

struct VertOut {
  [[smooth]] float3 normal_interp;
  [[smooth]] float3 color_interp;
  [[smooth]] float2 uv_interp;
  [[smooth]] float alpha_interp;
  [[flat]] int object_id;
  [[flat]] float roughness;
  [[flat]] float metallic;
};

struct MeshIn {
  [[attribute(0)]] float3 pos;
  [[attribute(1)]] float3 nor;
  [[attribute(2)]] float4 ac;
  [[attribute(3)]] float2 au;
};

struct Mesh {
  [[legacy_info]] ShaderCreateInfo draw_view;
  [[legacy_info]] ShaderCreateInfo draw_modelmat_with_custom_id;
  [[legacy_info]] ShaderCreateInfo drw_clipped;

  [[compilation_constant]] const bool use_clipping;
};

[[vertex]] void vert_mesh([[resource_table]] Mesh &mesh,
                          [[resource_table]] workbench::color::Materials &materials,
                          [[in]] const MeshIn &v_in,
                          [[out]] VertOut &v_out,
                          [[position]] float4 &out_position)
{
  float3 world_pos = drw_point_object_to_world(v_in.pos);
  out_position = drw_point_world_to_homogenous(world_pos);

  if (mesh.use_clipping) {
    view_clipping_distances(world_pos);
  }

  v_out.uv_interp = v_in.au;

  v_out.normal_interp = normalize(drw_normal_object_to_view(v_in.nor));

  v_out.object_id = int(drw_resource_id() & 0xFFFFu) + 1;

  materials.material_data_get(int(drw_custom_id()),
                              v_in.ac.rgb,
                              v_out.color_interp,
                              v_out.alpha_interp,
                              v_out.roughness,
                              v_out.metallic);
}

struct Curves {
  [[legacy_info]] ShaderCreateInfo draw_view;
  [[legacy_info]] ShaderCreateInfo draw_modelmat_with_custom_id;
  [[legacy_info]] ShaderCreateInfo draw_curves;
  [[legacy_info]] ShaderCreateInfo draw_curves_infos;
  [[legacy_info]] ShaderCreateInfo drw_clipped;

  [[compilation_constant]] const bool use_clipping;

  [[sampler(WB_CURVES_COLOR_SLOT) /*, frequency(batch)*/]] samplerBuffer ac;
  [[sampler(WB_CURVES_UV_SLOT) /*, frequency(batch)*/]] samplerBuffer au;
  [[push_constant]] const int emitter_object_id;
};

[[vertex]] void vert_curves([[resource_table]] Curves &curves,
                            [[resource_table]] workbench::color::Materials &materials,
                            [[vertex_id]] const int vert_id,
                            [[out]] VertOut &v_out,
                            [[position]] float4 &out_position)
{
  const auto &drw_curves = buffer_get(draw_curves_infos, drw_curves);

  const curves::Point ls_pt = curves::point_get(uint(vert_id));
  const curves::Point ws_pt = curves::object_to_world(ls_pt, drw_modelmat());
  const curves::ShapePoint pt = curves::shape_point_get(ws_pt, drw_world_incident_vector(ws_pt.P));
  float3 world_pos = pt.P;

  out_position = drw_point_world_to_homogenous(world_pos);

  float hair_rand = integer_noise(ws_pt.curve_id);

  float3 nor = pt.N;
  if (drw_curves.half_cylinder_face_count == 1) {
    /* Very cheap smooth normal using attribute interpolator.
     * Using the correct normals over the cylinder (-1..1) leads to unwanted result as the
     * interpolation is not spherical but linear. So we use a smaller range (-SQRT2..SQRT2) in
     * which the linear interpolation is close enough to the desired result. */
    nor = pt.N + pt.curve_N;
  }
  else if (drw_curves.half_cylinder_face_count == 0) {
    nor = hair_random_normal(pt.curve_T, pt.curve_B, pt.curve_N, hair_rand);
  }

  if (curves.use_clipping) {
    view_clipping_distances(world_pos);
  }

  v_out.uv_interp = curves::get_customdata_vec2(ws_pt.curve_id, curves.au);

  v_out.normal_interp = normalize(drw_normal_world_to_view(nor));

  materials.material_data_get(int(drw_custom_id()),
                              curves::get_customdata_vec3(ws_pt.curve_id, curves.ac),
                              v_out.color_interp,
                              v_out.alpha_interp,
                              v_out.roughness,
                              v_out.metallic);

  /* Hairs have lots of layer and can rapidly become the most prominent surface.
   * So we lower their alpha artificially. */
  v_out.alpha_interp *= 0.3f;

  hair_random_material(hair_rand, v_out.color_interp, v_out.roughness, v_out.metallic);

  v_out.object_id = int(drw_resource_id() & 0xFFFFu) + 1;

  if (curves.emitter_object_id != 0) {
    v_out.object_id = int(uint(curves.emitter_object_id) & 0xFFFFu) + 1;
  }
}

struct PointCloud {
  [[legacy_info]] ShaderCreateInfo draw_view;
  [[legacy_info]] ShaderCreateInfo draw_modelmat_with_custom_id;
  [[legacy_info]] ShaderCreateInfo draw_pointcloud;
  [[legacy_info]] ShaderCreateInfo drw_clipped;

  [[compilation_constant]] const bool use_clipping;
};

[[vertex]] void vert_pointcloud([[resource_table]] PointCloud &point_cloud,
                                [[resource_table]] workbench::color::Materials &materials,
                                [[vertex_id]] const int vert_id,
                                [[out]] VertOut &v_out,
                                [[position]] float4 &out_position)
{
  float3 world_pos;
  /* TODO */
  //   pointcloud_get_pos_and_nor(world_pos, v_out.normal_interp);

  v_out.normal_interp = normalize(drw_normal_world_to_view(v_out.normal_interp));

  out_position = drw_point_world_to_homogenous(world_pos);

  if (point_cloud.use_clipping) {
    view_clipping_distances(world_pos);
  }

  v_out.uv_interp = float2(0.0f);

  materials.material_data_get(int(drw_custom_id()),
                              float3(1.0f),
                              v_out.color_interp,
                              v_out.alpha_interp,
                              v_out.roughness,
                              v_out.metallic);

  v_out.object_id = int(drw_resource_id() & 0xFFFFu) + 1;
}

struct Resources {
  [[legacy_info]] ShaderCreateInfo draw_view;

  [[compilation_constant]] const int lighting_mode;
  [[compilation_constant]] const bool use_texture;

  [[push_constant]] const bool force_shadowing;

  [[resource_table, condition(use_texture)]] srt_t<workbench::color::Texture> texture;

  [[sampler(WB_MATCAP_SLOT), condition(lighting_mode == 1 /* WORKBENCH_LIGHTING_MATCAP */)]]
  sampler2DArray matcap_tx;
};

struct OpaqueOut {
  [[frag_color(0)]] float4 material;
  [[frag_color(1)]] float2 normal;
  [[frag_color(2)]] uint object_id;
};

[[fragment]] void frag_opaque([[resource_table]] Resources &srt,
                              [[in]] const VertOut &v_out,
                              [[out]] OpaqueOut &frag_out)
{
  frag_out.object_id = uint(v_out.object_id);
  frag_out.normal = workbench::normal_encode(gl_FrontFacing, v_out.normal_interp);

  frag_out.material = float4(v_out.color_interp,
                             workbench::float_pair_encode(v_out.roughness, v_out.metallic));

  if (srt.use_texture) [[static_branch]] {
    frag_out.material.rgb = workbench::color::image_color(srt.texture, v_out.uv_interp);
  }

  if (srt.lighting_mode == WORKBENCH_LIGHTING_MATCAP) [[static_branch]] {
    /* For matcaps, save front facing in alpha channel. */
    frag_out.material.a = float(gl_FrontFacing);
  }
}

struct TransparentOut {
  [[frag_color(0)]] float4 transparent_accum;
  [[frag_color(1)]] float4 revealage_accum;
  [[frag_color(2)]] uint object_id;
};

[[fragment]] void frag_transparent([[resource_table]] Resources &srt,
                                   [[resource_table]] workbench::World &world,
                                   [[frag_coord]] const float4 frag_co,
                                   [[in]] const VertOut &v_out,
                                   [[out]] TransparentOut &frag_out)
{
  /* Normal and Incident vector are in view-space. Lighting is evaluated in view-space. */
  float2 uv_viewport = frag_co.xy * world.world_data.viewport_size_inv;
  float3 vP = drw_point_screen_to_view(float3(uv_viewport, 0.5f));
  float3 I = drw_view_incident_vector(vP);
  float3 N = normalize(v_out.normal_interp);

  float3 color = v_out.color_interp;

  if (srt.use_texture) [[static_branch]] {
    color = workbench::color::image_color(srt.texture, v_out.uv_interp);
  }

  float3 shaded_color = float3(0.0f, 1.0f, 1.0f);
  if (srt.lighting_mode == WORKBENCH_LIGHTING_MATCAP) [[static_branch]] {
    shaded_color = workbench::get_matcap_lighting(world, srt.matcap_tx, color, N, I);
  }
  else if (srt.lighting_mode == WORKBENCH_LIGHTING_STUDIO) [[static_branch]] {
    shaded_color = workbench::get_world_lighting(
        world, color, v_out.roughness, v_out.metallic, N, I);
  }
  else if (srt.lighting_mode == WORKBENCH_LIGHTING_FLAT) [[static_branch]] {
    shaded_color = color;
  }

  shaded_color *= workbench::get_shadow(world, N, srt.force_shadowing);

  /* Listing 4 */
  float alpha = v_out.alpha_interp * world.world_data.xray_alpha;
  float weight = calculate_transparent_weight(frag_co.z) * alpha;
  frag_out.transparent_accum = float4(shaded_color * weight, alpha);
  frag_out.revealage_accum = float4(weight);

  frag_out.object_id = uint(v_out.object_id);
}

/* clang-format off */
PipelineGraphic mesh_opaque_flat_material_clip(vert_mesh, frag_opaque, Resources{.lighting_mode = 2 /* WORKBENCH_LIGHTING_FLAT */, .use_texture = false}, Mesh{.use_clipping = true});
/* clang-format on */

}  // namespace workbench::prepass

/* clang-format off */
// CREATE_INFO_VARIANT(workbench_prepass_mesh_opaque_flat_material_clip, drw_clipped, workbench_color_material, workbench_lighting_flat, workbench_opaque, workbench_mesh, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_mesh_opaque_flat_material_no_clip, workbench_color_material, workbench_lighting_flat, workbench_opaque, workbench_mesh, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_mesh_opaque_flat_texture_clip, drw_clipped, workbench_color_texture, workbench_lighting_flat, workbench_opaque, workbench_mesh, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_mesh_opaque_flat_texture_no_clip, workbench_color_texture, workbench_lighting_flat, workbench_opaque, workbench_mesh, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_mesh_opaque_flat_vertex_clip, drw_clipped, workbench_color_vertex, workbench_lighting_flat, workbench_opaque, workbench_mesh, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_mesh_opaque_flat_vertex_no_clip, workbench_color_vertex, workbench_lighting_flat, workbench_opaque, workbench_mesh, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_mesh_opaque_studio_material_clip, drw_clipped, workbench_color_material, workbench_lighting_studio, workbench_opaque, workbench_mesh, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_mesh_opaque_studio_material_no_clip, workbench_color_material, workbench_lighting_studio, workbench_opaque, workbench_mesh, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_mesh_opaque_studio_texture_clip, drw_clipped, workbench_color_texture, workbench_lighting_studio, workbench_opaque, workbench_mesh, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_mesh_opaque_studio_texture_no_clip, workbench_color_texture, workbench_lighting_studio, workbench_opaque, workbench_mesh, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_mesh_opaque_studio_vertex_clip, drw_clipped, workbench_color_vertex, workbench_lighting_studio, workbench_opaque, workbench_mesh, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_mesh_opaque_studio_vertex_no_clip, workbench_color_vertex, workbench_lighting_studio, workbench_opaque, workbench_mesh, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_mesh_opaque_matcap_material_clip, drw_clipped, workbench_color_material, workbench_lighting_matcap, workbench_opaque, workbench_mesh, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_mesh_opaque_matcap_material_no_clip, workbench_color_material, workbench_lighting_matcap, workbench_opaque, workbench_mesh, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_mesh_opaque_matcap_texture_clip, drw_clipped, workbench_color_texture, workbench_lighting_matcap, workbench_opaque, workbench_mesh, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_mesh_opaque_matcap_texture_no_clip, workbench_color_texture, workbench_lighting_matcap, workbench_opaque, workbench_mesh, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_mesh_opaque_matcap_vertex_clip, drw_clipped, workbench_color_vertex, workbench_lighting_matcap, workbench_opaque, workbench_mesh, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_mesh_opaque_matcap_vertex_no_clip, workbench_color_vertex, workbench_lighting_matcap, workbench_opaque, workbench_mesh, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_curves_opaque_flat_material_clip, drw_clipped, workbench_color_material, workbench_lighting_flat, workbench_opaque, workbench_curves, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_curves_opaque_flat_material_no_clip, workbench_color_material, workbench_lighting_flat, workbench_opaque, workbench_curves, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_curves_opaque_flat_texture_clip, drw_clipped, workbench_color_texture, workbench_lighting_flat, workbench_opaque, workbench_curves, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_curves_opaque_flat_texture_no_clip, workbench_color_texture, workbench_lighting_flat, workbench_opaque, workbench_curves, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_curves_opaque_flat_vertex_clip, drw_clipped, workbench_color_vertex, workbench_lighting_flat, workbench_opaque, workbench_curves, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_curves_opaque_flat_vertex_no_clip, workbench_color_vertex, workbench_lighting_flat, workbench_opaque, workbench_curves, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_curves_opaque_studio_material_clip, drw_clipped, workbench_color_material, workbench_lighting_studio, workbench_opaque, workbench_curves, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_curves_opaque_studio_material_no_clip, workbench_color_material, workbench_lighting_studio, workbench_opaque, workbench_curves, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_curves_opaque_studio_texture_clip, drw_clipped, workbench_color_texture, workbench_lighting_studio, workbench_opaque, workbench_curves, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_curves_opaque_studio_texture_no_clip, workbench_color_texture, workbench_lighting_studio, workbench_opaque, workbench_curves, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_curves_opaque_studio_vertex_clip, drw_clipped, workbench_color_vertex, workbench_lighting_studio, workbench_opaque, workbench_curves, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_curves_opaque_studio_vertex_no_clip, workbench_color_vertex, workbench_lighting_studio, workbench_opaque, workbench_curves, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_curves_opaque_matcap_material_clip, drw_clipped, workbench_color_material, workbench_lighting_matcap, workbench_opaque, workbench_curves, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_curves_opaque_matcap_material_no_clip, workbench_color_material, workbench_lighting_matcap, workbench_opaque, workbench_curves, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_curves_opaque_matcap_texture_clip, drw_clipped, workbench_color_texture, workbench_lighting_matcap, workbench_opaque, workbench_curves, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_curves_opaque_matcap_texture_no_clip, workbench_color_texture, workbench_lighting_matcap, workbench_opaque, workbench_curves, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_curves_opaque_matcap_vertex_clip, drw_clipped, workbench_color_vertex, workbench_lighting_matcap, workbench_opaque, workbench_curves, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_curves_opaque_matcap_vertex_no_clip, workbench_color_vertex, workbench_lighting_matcap, workbench_opaque, workbench_curves, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_ptcloud_opaque_flat_material_clip, drw_clipped, workbench_color_material, workbench_lighting_flat, workbench_opaque, workbench_pointcloud, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_ptcloud_opaque_flat_material_no_clip, workbench_color_material, workbench_lighting_flat, workbench_opaque, workbench_pointcloud, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_ptcloud_opaque_flat_texture_clip, drw_clipped, workbench_color_texture, workbench_lighting_flat, workbench_opaque, workbench_pointcloud, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_ptcloud_opaque_flat_texture_no_clip, workbench_color_texture, workbench_lighting_flat, workbench_opaque, workbench_pointcloud, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_ptcloud_opaque_flat_vertex_clip, drw_clipped, workbench_color_vertex, workbench_lighting_flat, workbench_opaque, workbench_pointcloud, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_ptcloud_opaque_flat_vertex_no_clip, workbench_color_vertex, workbench_lighting_flat, workbench_opaque, workbench_pointcloud, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_ptcloud_opaque_studio_material_clip, drw_clipped, workbench_color_material, workbench_lighting_studio, workbench_opaque, workbench_pointcloud, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_ptcloud_opaque_studio_material_no_clip, workbench_color_material, workbench_lighting_studio, workbench_opaque, workbench_pointcloud, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_ptcloud_opaque_studio_texture_clip, drw_clipped, workbench_color_texture, workbench_lighting_studio, workbench_opaque, workbench_pointcloud, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_ptcloud_opaque_studio_texture_no_clip, workbench_color_texture, workbench_lighting_studio, workbench_opaque, workbench_pointcloud, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_ptcloud_opaque_studio_vertex_clip, drw_clipped, workbench_color_vertex, workbench_lighting_studio, workbench_opaque, workbench_pointcloud, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_ptcloud_opaque_studio_vertex_no_clip, workbench_color_vertex, workbench_lighting_studio, workbench_opaque, workbench_pointcloud, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_ptcloud_opaque_matcap_material_clip, drw_clipped, workbench_color_material, workbench_lighting_matcap, workbench_opaque, workbench_pointcloud, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_ptcloud_opaque_matcap_material_no_clip, workbench_color_material, workbench_lighting_matcap, workbench_opaque, workbench_pointcloud, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_ptcloud_opaque_matcap_texture_clip, drw_clipped, workbench_color_texture, workbench_lighting_matcap, workbench_opaque, workbench_pointcloud, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_ptcloud_opaque_matcap_texture_no_clip, workbench_color_texture, workbench_lighting_matcap, workbench_opaque, workbench_pointcloud, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_ptcloud_opaque_matcap_vertex_clip, drw_clipped, workbench_color_vertex, workbench_lighting_matcap, workbench_opaque, workbench_pointcloud, workbench_prepass)
// CREATE_INFO_VARIANT(workbench_prepass_ptcloud_opaque_matcap_vertex_no_clip, workbench_color_vertex, workbench_lighting_matcap, workbench_opaque, workbench_pointcloud, workbench_prepass)
/* clang-format on */
