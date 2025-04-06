/* SPDX-FileCopyrightText: 2016-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_srd_cpp.hh"

#if 1 /* For prototyping purpose. */

/* Textures. */
/* Slot 0 is reserved by draw_hair. */
#  define WB_MATCAP_SLOT 1
#  define WB_TEXTURE_SLOT 2
#  define WB_TILE_ARRAY_SLOT 3
#  define WB_TILE_DATA_SLOT 4
#  define WB_CURVES_UV_SLOT 5
#  define WB_CURVES_COLOR_SLOT 6

/* UBOs (Storage buffers in WB_ Next). */
#  define WB_MATERIAL_SLOT 0
#  define WB_WORLD_SLOT 1

/* We target hardware with at least 12 UBO slots (Guaranteed by GL 4.3). */
#  define DRW_VIEW_UBO_SLOT 11
#  define DRW_VIEW_CULLING_UBO_SLOT 10
#  define DRW_OBJ_DATA_INFO_UBO_SLOT 9
#  define DRW_LAYER_ATTR_UBO_SLOT 7
/* Slots 0-6 are reserved for engine use. */
/* TODO(fclem): Legacy. To be removed once we remove the old DRW. */
#  define DRW_OBJ_INFOS_UBO_SLOT 6
/* TODO(fclem): Remove in favor of engine-side clipping UBO. */
#  define DRW_CLIPPING_UBO_SLOT 5

struct LightData {
  float4 direction;
  float4 specular_color;
  float4 diffuse_color_wrap; /* rgb: diffuse col a: wrapped lighting factor */
};

struct WorldData {
  float2 viewport_size;
  float2 viewport_size_inv;
  float4 object_outline_color;
  float4 shadow_direction_vs;
  float shadow_focus;
  float shadow_shift;
  float shadow_mul;
  float shadow_add;
  /* - 16 bytes alignment - */
  LightData lights[4];
  float4 ambient_color;

  int cavity_sample_start;
  int cavity_sample_end;
  float cavity_sample_count_inv;
  float cavity_jitter_scale;

  float cavity_valley_factor;
  float cavity_ridge_factor;
  float cavity_attenuation;
  float cavity_distance;

  float curvature_ridge;
  float curvature_valley;
  float ui_scale;
  float _pad0;

  int matcap_orientation;
  bool use_specular;
  float xray_alpha;
  int _pad1;

  float4 background_color;
};

struct ObjectMatrices {
  float4x4 model;
  float4x4 model_inverse;
};

#  define DRW_VIEW_LEN 1
#  define DRW_VIEW_SHIFT 0

/* We target hardware with at least 12 SSBO slots (NOT Guaranteed by GL 4.3). */
#  define DRW_RESOURCE_ID_SLOT 11
#  define DRW_OBJ_MAT_SLOT 10
#  define DRW_OBJ_INFOS_SLOT 9
#  define DRW_OBJ_ATTR_SLOT 8

struct ViewMatrices {
  float4x4 viewmat;
  float4x4 viewinv;
  float4x4 winmat;
  float4x4 wininv;
};
#endif

SRD_VERTEX_IN_BEGIN(VertexInMesh)
SRD_VERTEX_IN(VertexInMesh, 0, float3, pos)
SRD_VERTEX_IN(VertexInMesh, 1, float3, nor)
SRD_VERTEX_IN(VertexInMesh, 2, float4, ac)
SRD_VERTEX_IN(VertexInMesh, 3, float2, au)
SRD_VERTEX_IN_END(VertexInMesh)

SRD_VERTEX_OUT_BEGIN(VertexOut)
SRD_VERTEX_OUT(VertexOut, position, float4, position)
SRD_VERTEX_OUT(VertexOut, smooth, float3, normal)
SRD_VERTEX_OUT(VertexOut, smooth, float3, color)
SRD_VERTEX_OUT(VertexOut, smooth, float, alpha)
SRD_VERTEX_OUT(VertexOut, smooth, float2, uv)
SRD_VERTEX_OUT(VertexOut, flat, int, object_id)
SRD_VERTEX_OUT(VertexOut, flat, float, roughness)
SRD_VERTEX_OUT(VertexOut, flat, float, metallic)
SRD_VERTEX_OUT_END(VertexOut)

SRD_FRAGMENT_IN_BEGIN(FragmentIn)
SRD_FRAGMENT_IN(FragmentIn, struct, VertexOut, v_out)
SRD_FRAGMENT_IN(FragmentIn, front_facing, bool, front_facing)
SRD_FRAGMENT_IN_END(FragmentIn)

SRD_FRAGMENT_OUT_BEGIN(FragmentOut)
SRD_FRAGMENT_OUT(FragmentOut, 0, float4, material)
SRD_FRAGMENT_OUT(FragmentOut, 1, float2, normal)
SRD_FRAGMENT_OUT(FragmentOut, 2, uint, object_id)
SRD_FRAGMENT_OUT_END(FragmentOut)

SRD_RESOURCE_BEGIN(DRW_View)
SRD_RESOURCE_UNIFORM_BUF(DRW_View, DRW_VIEW_UBO_SLOT, ViewMatrices, drw_view_buf, [DRW_VIEW_LEN])
SRD_RESOURCE_END(DRW_View)

SRD_RESOURCE_BEGIN(DRW_Clipping)
SRD_RESOURCE_UNIFORM_BUF(DRW_Clipping, DRW_CLIPPING_UBO_SLOT, float4, drw_clipping_, [6])
SRD_RESOURCE_END(DRW_Clipping)

#define WB_COLOR_TEXTURE (1 << 0)
#define WB_COLOR_MATERIAL (1 << 1)
#define WB_COLOR_VERTEX (1 << 2)
#define WB_LIGHTING_FLAT 1
#define WB_LIGHTING_STUDIO 2
#define WB_LIGHTING_MATCAP 3

/* Input to common function needs to be redeclared with own binding points, or use common slot. */
SRD_RESOURCE_BEGIN(ImageTileData)
SRD_RESOURCE_SAMPLER(ImageTileData, WB_TILE_ARRAY_SLOT, sampler2DArray, tile_tx)
SRD_RESOURCE_SAMPLER(ImageTileData, WB_TILE_DATA_SLOT, sampler1DArray, map)
SRD_RESOURCE_END(ImageTileData)

SRD_RESOURCE_BEGIN(WB_PrepassCommon)
SRD_RESOURCE_SPECIALIZATION_CONSTANT(WB_PrepassCommon, int, color_mode, WB_COLOR_TEXTURE)
SRD_RESOURCE_SPECIALIZATION_CONSTANT(WB_PrepassCommon, int, shading_mode, WB_LIGHTING_MATCAP)
SRD_RESOURCE_UNIFORM_BUF(WB_PrepassCommon, WB_WORLD_SLOT, WorldData, world_data, )
SRD_RESOURCE_SAMPLER(WB_PrepassCommon, WB_MATCAP_SLOT, sampler2DArray, matcap_tx)
SRD_RESOURCE_SAMPLER(WB_PrepassCommon, WB_TEXTURE_SLOT, sampler2D, imageTexture)
SRD_RESOURCE_STRUCT(WB_PrepassCommon, ImageTileData, image_tile_data)
SRD_RESOURCE_PUSH_CONSTANT(WB_PrepassCommon, bool, isImageTile)
SRD_RESOURCE_PUSH_CONSTANT(WB_PrepassCommon, bool, imagePremult)
SRD_RESOURCE_PUSH_CONSTANT(WB_PrepassCommon, float, imageTransparencyCutoff)
SRD_RESOURCE_STORAGE_BUF(WB_PrepassCommon, WB_MATERIAL_SLOT, READ, float4, materials_data, [])
SRD_RESOURCE_END(WB_PrepassCommon)

SRD_RESOURCE_BEGIN(DRW_ResCustomId)
SRD_RESOURCE_STORAGE_BUF(DRW_ResCustomId, DRW_RESOURCE_ID_SLOT, READ, uint2, resource_id_buf, [])
SRD_RESOURCE_END(DRW_ResCustomId)

SRD_RESOURCE_BEGIN(DRW_ModelMat)
SRD_RESOURCE_STORAGE_BUF(DRW_ModelMat, DRW_OBJ_MAT_SLOT, READ, ObjectMatrices, drw_matrix_buf, [])
SRD_RESOURCE_END(DRW_ModelMat)

SRD_RESOURCE_BEGIN(DRW_ModelMatWithCustomId)
SRD_RESOURCE_STRUCT(DRW_ModelMatWithCustomId, DRW_ModelMat, mat)
SRD_RESOURCE_STRUCT(DRW_ModelMatWithCustomId, DRW_ResCustomId, res_id)
SRD_RESOURCE_END(DRW_ModelMatWithCustomId)

SRD_RESOURCE_BEGIN(WB_PrepassOpaqueMesh)
SRD_RESOURCE_STRUCT(WB_PrepassOpaqueMesh, DRW_View, view)
SRD_RESOURCE_STRUCT(WB_PrepassOpaqueMesh, DRW_ModelMatWithCustomId, model)
SRD_RESOURCE_STRUCT(WB_PrepassOpaqueMesh, DRW_Clipping, clipping)
SRD_RESOURCE_STRUCT(WB_PrepassOpaqueMesh, WB_PrepassCommon, prepass)
SRD_RESOURCE_END(WB_PrepassOpaqueMesh)

/* Preprocessor outputs this for GLSL compatibility. */
#ifdef GPU_LANG_GLSL
#  if SRD_ENABLED(ImageCommon)
SRD_SAMPLER_DECLARE(ImageCommon, 0, sampler2D, image)
#  else
SRD_SAMPLER_DECLARE_DUMMY(ImageCommon, 0, sampler2D, image)
#  endif
#endif

/* Shader code is not wanted when this file is included for shader reflections.
 * Guard all usage explicitly. This way, this can be included in many places without too much
 * overhead. This also allow arbitrary placement of the code w.r.t. the SRD. */
#ifndef NO_SHADER_CODE

uint drw_resource_id_raw(DRW_ResCustomId res_id)
{
  return buffer_read(res_id.resource_id_buf, /*gpu_BaseInstance + gl_InstanceID*/ 0).x;
}

uint drw_resource_id(DRW_ResCustomId res_id)
{
  return drw_resource_id_raw(res_id) >> DRW_VIEW_SHIFT;
}

uint drw_custom_id(DRW_ResCustomId res_id)
{
  return buffer_read(res_id.resource_id_buf, /*gpu_BaseInstance + gl_InstanceID*/ 0).y;
}

float4x4 drw_modelmat(DRW_ModelMatWithCustomId model)
{
  return buffer_read(model.mat.drw_matrix_buf, drw_resource_id(model.res_id)).model;
}

float4x4 drw_modelinv(DRW_ModelMatWithCustomId model)
{
  return buffer_read(model.mat.drw_matrix_buf, drw_resource_id(model.res_id)).model_inverse;
}

float3 drw_point_object_to_world(DRW_ModelMatWithCustomId model, float3 lP)
{
  return (drw_modelmat(model) * float4(lP, 1.0)).xyz;
}

/* TODO(fclem): Needs to be decorated / set into */
uint drw_view_id = 0;
/* Returns the current active view. */
ViewMatrices drw_view(DRW_View view)
{
  return buffer_read(view.drw_view_buf, drw_view_id);
}

float4 drw_point_world_to_homogenous(DRW_View view, float3 P)
{
  return (drw_view(view).winmat * (drw_view(view).viewmat * float4(P, 1.0)));
}

void view_clipping_distances(DRW_Clipping /*srd*/, float3 /*wpos*/)
{
  /* ... */
}

/**
 * Usually Normal matrix is `transpose(inverse(ViewMatrix * ModelMatrix))`.
 *
 * But since it is slow to multiply matrices we decompose it. Decomposing
 * inversion and transposition both invert the product order leaving us with
 * the same original order:
 * transpose(ViewMatrixInverse) * transpose(ModelMatrixInverse)
 *
 * Knowing that the view matrix is orthogonal, the transpose is also the inverse.
 * NOTE: This is only valid because we are only using the mat3 of the ViewMatrixInverse.
 * ViewMatrix * transpose(ModelMatrixInverse)
 */
float3x3 drw_normat(DRW_ModelMatWithCustomId model)
{
  return transpose(to_float3x3(drw_modelinv(model)));
}
float3x3 drw_norinv(DRW_ModelMatWithCustomId model)
{
  return transpose(to_float3x3(drw_modelmat(model)));
}

float3 drw_normal_object_to_view(DRW_View view, DRW_ModelMatWithCustomId model, float3 lN)
{
  return (to_float3x3(drw_view(view).viewmat) * (drw_normat(model) * lN));
}

void workbench_material_data_get(WB_PrepassCommon srd,
                                 int handle,
                                 float3 vertex_color,
                                 float3 &color,
                                 float &alpha,
                                 float &roughness,
                                 float &metallic)
{
  float4 data = float4(0.0);
  if ((srd.color_mode & WB_COLOR_MATERIAL) != 0) {
    data = buffer_read(srd.materials_data, handle);
  }
  color = (data.r == -1) ? vertex_color : data.rgb;

  uint encoded_data = floatBitsToUint(data.w);
  alpha = float((encoded_data >> 16u) & 0xFFu) * (1.0 / 255.0);
  roughness = float((encoded_data >> 8u) & 0xFFu) * (1.0 / 255.0);
  metallic = float(encoded_data & 0xFFu) * (1.0 / 255.0);
}

VertexOut prepass_mesh_vertex(VertexInMesh in, WB_PrepassOpaqueMesh srd)
{
  VertexOut out;

  float3 world_pos = drw_point_object_to_world(srd.model, in.pos);
  out.position = drw_point_world_to_homogenous(srd.view, world_pos);

  view_clipping_distances(srd.clipping, world_pos);

  out.uv = in.au;

  out.normal = normalize(drw_normal_object_to_view(srd.view, srd.model, in.nor));

  int resource_id = drw_resource_id(srd.model.res_id);
  int custom_id = int(drw_custom_id(srd.model.res_id));

  out.object_id = int(uint(resource_id) & 0xFFFFu) + 1;

  workbench_material_data_get(
      srd.prepass, custom_id, in.ac.rgb, out.color, out.alpha, out.roughness, out.metallic);
  return out;
}

/* From http://aras-p.info/texts/CompactNormalStorage.html
 * Using Method #4: Sphere-map Transform */
float2 workbench_normal_encode(bool front_face, float3 n)
{
  n = normalize(front_face ? n : -n);
  float p = sqrt(n.z * 8.0 + 8.0);
  n.xy = clamp(n.xy / p + 0.5, 0.0, 1.0);
  return n.xy;
}

/* Encode 2 float into 1 with the desired precision. */
float workbench_float_pair_encode(float v1, float v2)
{
  /* Encoding into the alpha of a RGBA16F texture. (10bit mantissa) */
  uint TARGET_BITCOUNT = 8u;
  uint METALLIC_BITS = 3u; /* Metallic channel is less important. */
  uint ROUGHNESS_BITS = (TARGET_BITCOUNT - METALLIC_BITS);
  // const uint v1_mask = ~(0xFFFFFFFFu << ROUGHNESS_BITS);
  // const uint v2_mask = ~(0xFFFFFFFFu << METALLIC_BITS);
  /* Same as above because some compiler are very dumb and think we use medium int. */
  const int v1_mask = 0x1F;
  const int v2_mask = 0x7;
  int iv1 = int(v1 * float(v1_mask));
  int iv2 = int(v2 * float(v2_mask)) << int(ROUGHNESS_BITS);
  return float(iv1 | iv2);
}

bool node_tex_tile_lookup(float3 &co, ImageTileData srd)
{
  float2 tile_pos = floor(co.xy);

  if (tile_pos.x < 0 || tile_pos.y < 0 || tile_pos.x >= 10) {
    return false;
  }

  float tile = 10.0 * tile_pos.y + tile_pos.x;
  if (tile >= textureSize(srd.map, 0).x) {
    return false;
  }

  /* Fetch tile information. */
  float tile_layer = texelFetch(srd.map, int2(tile, 0), 0).x;
  if (tile_layer < 0.0) {
    return false;
  }

  float4 tile_info = texelFetch(srd.map, int2(tile, 1), 0);

  co = float3(((co.xy - tile_pos) * tile_info.zw) + tile_info.xy, tile_layer);
  return true;
}

float3 workbench_image_color(WB_PrepassCommon srd, float2 uvs)
{
  float4 color;

  float3 co = float3(uvs, 0.0);
  if (srd.isImageTile) {
    if (node_tex_tile_lookup(co, srd.image_tile_data)) {
      color = texture(srd.image_tile_data.tile_tx, co);
    }
    else {
      color = float4(1.0, 0.0, 1.0, 1.0);
    }
  }
  else {
    color = texture(srd.imageTexture, uvs);
  }

  /* Unpremultiply if stored multiplied, since straight alpha is expected by shaders. */
  if (srd.imagePremult && !(color.a == 0.0 || color.a == 1.0)) {
    color.rgb /= color.a;
  }

  if (color.a < srd.imageTransparencyCutoff) {
    discard;
  }

  return color.rgb;
}

FragmentOut prepass_fragment(FragmentIn in, WB_PrepassOpaqueMesh srd)
{
  FragmentOut out;
  out.object_id = uint(in.v_out.object_id);
  out.normal = workbench_normal_encode(in.front_facing, in.v_out.normal);
  out.material = float4(in.v_out.color,
                        workbench_float_pair_encode(in.v_out.roughness, in.v_out.metallic));

  if (srd.prepass.color_mode == WB_COLOR_TEXTURE) {
    out.material.rgb = workbench_image_color(srd.prepass, in.v_out.uv);
  }

  if (srd.prepass.shading_mode == WB_LIGHTING_MATCAP) {
    /* For matcaps, save front facing in alpha channel. */
    out.material.a = float(in.front_facing);
  }
  return out;
}

#endif

SRD_GRAPHIC_PIPELINE(__FILE__,
                     WB_OpaquePrepass,
                     VertexInMesh,
                     VertexOut,
                     FragmentIn,
                     FragmentOut,
                     prepass_mesh_vertex,
                     WB_PrepassOpaqueMesh,
                     prepass_fragment,
                     WB_PrepassOpaqueMesh)
