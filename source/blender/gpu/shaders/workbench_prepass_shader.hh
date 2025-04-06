/* SPDX-FileCopyrightText: 2016-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef SHADER_REFLECTION
#  include "gpu_shader_reflection.hh"
#else
#  include "gpu_shader_compat.hh"
#  include "gpu_shader_compat_glsl.hh"
#endif

#if 1 /* For prototyping purpose. */

/* Textures. */
/* Slot 0 is reserved by draw_hair. */
#  define WB_MATCAP_SLOT 1
#  define WB_TEXTURE_SLOT 2
#  define WB_TILE_ARRAY_SLOT 3
#  define WB_TILE_DATA_SLOT 4
#  define WB_CURVES_UV_SLOT 5
#  define WB_CURVES_COLOR_SLOT 6

/* UBOs (Storage buffers in Workbench Next). */
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

SRD_STAGE_INOUT_BEGIN(VertexOut)
SRD_STAGE_INOUT(VertexOut, position, float4, position)
SRD_STAGE_INOUT(VertexOut, smooth, float3, normal)
SRD_STAGE_INOUT(VertexOut, smooth, float3, color)
SRD_STAGE_INOUT(VertexOut, smooth, float, alpha)
SRD_STAGE_INOUT(VertexOut, smooth, float2, uv)
SRD_STAGE_INOUT(VertexOut, flat, int, object_id)
SRD_STAGE_INOUT(VertexOut, flat, float, roughness)
SRD_STAGE_INOUT(VertexOut, flat, float, metallic)
SRD_STAGE_INOUT(VertexOut, front_facing, bool, front_facing)
SRD_STAGE_INOUT_END(VertexOut)

SRD_FRAGMENT_OUT_BEGIN(FragmentOut)
SRD_FRAGMENT_OUT(FragmentOut, 0, float4, material)
SRD_FRAGMENT_OUT(FragmentOut, 1, float2, normal)
SRD_FRAGMENT_OUT(FragmentOut, 2, uint, object_id)
SRD_FRAGMENT_OUT_END(FragmentOut)

SRD_RESOURCE_BEGIN(DrawView)
SRD_RESOURCE_UNIFORM_BUF(DrawView, DRW_VIEW_UBO_SLOT, ViewMatrices, drw_view_buf, [DRW_VIEW_LEN])
SRD_RESOURCE_END(DrawView)

SRD_RESOURCE_BEGIN(DrawClipping)
SRD_RESOURCE_UNIFORM_BUF(DrawClipping, DRW_CLIPPING_UBO_SLOT, float4, drw_clipping_, [6])
SRD_RESOURCE_END(DrawClipping)

#define WORKBENCH_COLOR_TEXTURE (1 << 0)
#define WORKBENCH_COLOR_MATERIAL (1 << 1)
#define WORKBENCH_COLOR_VERTEX (1 << 2)
#define WORKBENCH_LIGHTING_FLAT 1
#define WORKBENCH_LIGHTING_STUDIO 2
#define WORKBENCH_LIGHTING_MATCAP 3

/* Input to common function needs to be redeclared with own binding points, or use common slot. */
SRD_RESOURCE_BEGIN(ImageTileData)
SRD_RESOURCE_SAMPLER(ImageTileData, WB_TILE_ARRAY_SLOT, sampler2DArray, tile_tx)
SRD_RESOURCE_SAMPLER(ImageTileData, WB_TILE_DATA_SLOT, sampler1DArray, map)
SRD_RESOURCE_END(ImageTileData)

/* clang-format off */
SRD_RESOURCE_BEGIN(WorkbenchPrepassCommon)
SRD_RESOURCE_SPECIALIZATION_CONSTANT(WorkbenchPrepassCommon, int, color_mode, WORKBENCH_COLOR_TEXTURE)
SRD_RESOURCE_SPECIALIZATION_CONSTANT(WorkbenchPrepassCommon, int, shading_mode, WORKBENCH_LIGHTING_MATCAP)
SRD_RESOURCE_UNIFORM_BUF(WorkbenchPrepassCommon, WB_WORLD_SLOT, WorldData, world_data,)
SRD_RESOURCE_SAMPLER(WorkbenchPrepassCommon, WB_MATCAP_SLOT, sampler2DArray, matcap_tx)
SRD_RESOURCE_SAMPLER(WorkbenchPrepassCommon, WB_TEXTURE_SLOT, sampler2D, imageTexture)
SRD_RESOURCE_STRUCT(WorkbenchPrepassCommon, ImageTileData, image_tile_data)
SRD_RESOURCE_PUSH_CONSTANT(WorkbenchPrepassCommon, bool,  isImageTile)
SRD_RESOURCE_PUSH_CONSTANT(WorkbenchPrepassCommon, bool,  imagePremult)
SRD_RESOURCE_PUSH_CONSTANT(WorkbenchPrepassCommon, float,  imageTransparencyCutoff)
SRD_RESOURCE_STORAGE_BUF(WorkbenchPrepassCommon, WB_MATERIAL_SLOT, READ, vec4, materials_data, [])
SRD_RESOURCE_END(WorkbenchPrepassCommon)
/* clang-format on */

SRD_RESOURCE_BEGIN(DrawResCustomId)
SRD_RESOURCE_STORAGE_BUF(DrawResCustomId, DRW_RESOURCE_ID_SLOT, READ, uint2, resource_id_buf, [])
SRD_RESOURCE_END(DrawResCustomId)

SRD_RESOURCE_BEGIN(DrawModelMat)
SRD_RESOURCE_STORAGE_BUF(DrawModelMat, DRW_OBJ_MAT_SLOT, READ, ObjectMatrices, drw_matrix_buf, [])
SRD_RESOURCE_END(DrawModelMat)

SRD_RESOURCE_BEGIN(DrawModelMatWithCustomId)
SRD_RESOURCE_STRUCT(DrawModelMatWithCustomId, DrawModelMat, mat)
SRD_RESOURCE_STRUCT(DrawModelMatWithCustomId, DrawResCustomId, res_id)
SRD_RESOURCE_END(DrawModelMatWithCustomId)

SRD_RESOURCE_BEGIN(WorkbenchPrepassOpaqueMesh)
SRD_RESOURCE_STRUCT(WorkbenchPrepassOpaqueMesh, DrawView, view)
SRD_RESOURCE_STRUCT(WorkbenchPrepassOpaqueMesh, DrawModelMatWithCustomId, model)
SRD_RESOURCE_STRUCT(WorkbenchPrepassOpaqueMesh, DrawClipping, clipping)
SRD_RESOURCE_STRUCT(WorkbenchPrepassOpaqueMesh, WorkbenchPrepassCommon, prepass)
SRD_RESOURCE_END(WorkbenchPrepassOpaqueMesh)

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

uint drw_resource_id_raw(DrawResCustomId res_id)
{
  return buffer_read(res_id.resource_id_buf, /*gpu_BaseInstance + gl_InstanceID*/ 0).x;
}

uint drw_resource_id(DrawResCustomId res_id)
{
  return drw_resource_id_raw(res_id) >> DRW_VIEW_SHIFT;
}

uint drw_custom_id(DrawResCustomId res_id)
{
  return buffer_read(res_id.resource_id_buf, /*gpu_BaseInstance + gl_InstanceID*/ 0).y;
}

mat4x4 drw_modelmat(DrawModelMatWithCustomId model)
{
  return buffer_read(model.mat.drw_matrix_buf, drw_resource_id(model.res_id)).model;
}

mat4x4 drw_modelinv(DrawModelMatWithCustomId model)
{
  return buffer_read(model.mat.drw_matrix_buf, drw_resource_id(model.res_id)).model_inverse;
}

vec3 drw_point_object_to_world(DrawModelMatWithCustomId model, vec3 lP)
{
  return (drw_modelmat(model) * vec4(lP, 1.0)).xyz;
}

uint drw_view_id = 0;
/* Returns the current active view. */
ViewMatrices drw_view(DrawView view)
{
  return buffer_read(view.drw_view_buf, drw_view_id);
}

vec4 drw_point_world_to_homogenous(DrawView view, vec3 P)
{
  return (drw_view(view).winmat * (drw_view(view).viewmat * vec4(P, 1.0)));
}

void view_clipping_distances(DrawClipping /*srd*/, vec3 /*wpos*/)
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
mat3x3 drw_normat(DrawModelMatWithCustomId model)
{
  return transpose(to_float3x3(drw_modelinv(model)));
}
mat3x3 drw_norinv(DrawModelMatWithCustomId model)
{
  return transpose(to_float3x3(drw_modelmat(model)));
}

vec3 drw_normal_object_to_view(DrawView view, DrawModelMatWithCustomId model, vec3 lN)
{
  return (to_float3x3(drw_view(view).viewmat) * (drw_normat(model) * lN));
}

void workbench_material_data_get(WorkbenchPrepassCommon srd,
                                 int handle,
                                 vec3 vertex_color,
                                 vec3 &color,
                                 float &alpha,
                                 float &roughness,
                                 float &metallic)
{
  vec4 data = vec4(0.0);
  if ((srd.color_mode & WORKBENCH_COLOR_MATERIAL) != 0) {
    data = buffer_read(srd.materials_data, handle);
  }
  color = (data.r == -1) ? vertex_color : data.rgb;

  uint encoded_data = floatBitsToUint(data.w);
  alpha = float((encoded_data >> 16u) & 0xFFu) * (1.0 / 255.0);
  roughness = float((encoded_data >> 8u) & 0xFFu) * (1.0 / 255.0);
  metallic = float(encoded_data & 0xFFu) * (1.0 / 255.0);
}

VertexOut prepass_mesh_vertex(VertexInMesh in, WorkbenchPrepassOpaqueMesh srd)
{
  VertexOut out;

  vec3 world_pos = drw_point_object_to_world(srd.model, in.pos);
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
vec2 workbench_normal_encode(bool front_face, vec3 n)
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

bool node_tex_tile_lookup(vec3 &co, ImageTileData srd)
{
  vec2 tile_pos = floor(co.xy);

  if (tile_pos.x < 0 || tile_pos.y < 0 || tile_pos.x >= 10) {
    return false;
  }

  float tile = 10.0 * tile_pos.y + tile_pos.x;
  if (tile >= textureSize(srd.map, 0).x) {
    return false;
  }

  /* Fetch tile information. */
  float tile_layer = texelFetch(srd.map, ivec2(tile, 0), 0).x;
  if (tile_layer < 0.0) {
    return false;
  }

  vec4 tile_info = texelFetch(srd.map, ivec2(tile, 1), 0);

  co = vec3(((co.xy - tile_pos) * tile_info.zw) + tile_info.xy, tile_layer);
  return true;
}

vec3 workbench_image_color(WorkbenchPrepassCommon srd, vec2 uvs)
{
  vec4 color;

  vec3 co = vec3(uvs, 0.0);
  if (srd.isImageTile) {
    if (node_tex_tile_lookup(co, srd.image_tile_data)) {
      color = texture(srd.image_tile_data.tile_tx, co);
    }
    else {
      color = vec4(1.0, 0.0, 1.0, 1.0);
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

FragmentOut prepass_fragment(VertexOut in, WorkbenchPrepassOpaqueMesh srd)
{
  FragmentOut out;
  out.object_id = uint(in.object_id);
  out.normal = workbench_normal_encode(in.front_facing, in.normal);
  out.material = vec4(in.color, workbench_float_pair_encode(in.roughness, in.metallic));

  if (WorkbenchPrepassCommon::color_mode == WORKBENCH_COLOR_TEXTURE) {
    out.material.rgb = workbench_image_color(srd.prepass, in.uv);
  }

  if (WorkbenchPrepassCommon::shading_mode == WORKBENCH_LIGHTING_MATCAP) {
    /* For matcaps, save front facing in alpha channel. */
    out.material.a = float(in.front_facing);
  }
  return out;
}

#endif

SRD_GRAPHIC_PIPELINE(__FILE__,
                     WorkbenchOpaquePrepass,
                     VertexInMesh,
                     VertexOut,
                     FragmentOut,
                     prepass_mesh_vertex,
                     WorkbenchPrepassOpaqueMesh,
                     prepass_fragment,
                     WorkbenchPrepassOpaqueMesh)
