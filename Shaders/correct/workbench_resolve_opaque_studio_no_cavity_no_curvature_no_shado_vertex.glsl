#version 450
#extension GL_ARB_shader_draw_parameters : enable
#define GPU_ARB_shader_draw_parameters
#define gpu_BaseInstance (gl_BaseInstanceARB)
#define GPU_ARB_clip_control
#define gl_VertexID gl_VertexIndex
#define gpu_InstanceIndex (gl_InstanceIndex)
#define gl_InstanceID (gpu_InstanceIndex - gpu_BaseInstance)
#extension GL_ARB_shader_viewport_layer_array: enable
#extension GL_ARB_shader_stencil_export: enable
#define GPU_ARB_shader_stencil_export 1
#define GPU_VERTEX_SHADER
#line 1 "gpu_shader_compat_glsl.glsl"
 





#pragma no_processing








#define RESHAPE(name, mat_to, mat_from) \
  mat_to to_##name(mat_from m) \
  { \
    return mat_to(m); \
  }


RESHAPE(float2x2, mat2x2, mat3x3)
RESHAPE(float2x2, mat2x2, mat4x4)
RESHAPE(float3x3, mat3x3, mat4x4)
RESHAPE(float3x3, mat3x3, mat2x2)
RESHAPE(float4x4, mat4x4, mat2x2)
RESHAPE(float4x4, mat4x4, mat3x3)


RESHAPE(float3x3, mat3x3, mat3x4)

#undef RESHAPE



#define constexpr const


#define bool32_t bool
#define bool2 bvec2
#define bool3 bvec3
#define bool4 bvec4

#define float2 vec2
#define float3 vec3
#define float4 vec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4
#define uint2 uvec2
#define uint3 uvec3
#define uint4 uvec4

#define packed_float2 float2
#define packed_int2 int2
#define packed_uint2 uint2
#define packed_float3 float3
#define packed_int3 int3
#define packed_uint3 uint3
#define packed_float4 float4
#define packed_int4 int4
#define packed_uint4 uint4

#define float2x2 mat2x2
#define float3x2 mat3x2
#define float4x2 mat4x2
#define float2x3 mat2x3
#define float3x3 mat3x3
#define float4x3 mat4x3
#define float2x4 mat2x4
#define float3x4 mat3x4
#define float4x4 mat4x4


#define char int
#define char2 int2
#define char3 int3
#define char4 int4
#define short int
#define short2 int2
#define short3 int3
#define short4 int4
#define uchar uint
#define uchar2 uint2
#define uchar3 uint3
#define uchar4 uint4
#define ushort uint
#define ushort2 uint2
#define ushort3 uint3
#define ushort4 uint4
#define half float
#define half2 float2
#define half3 float3
#define half4 float4


#define int32_t int
#define uint32_t uint



#define imageStoreFast imageStore
#define imageLoadFast imageLoad


#define sampler2DDepth sampler2D
#define sampler2DArrayDepth sampler2DArray
#define samplerCubeDepth sampler2D
#define samplerCubeArrayDepth sampler2DArray

#define usampler2DArrayAtomic usampler2DArray
#define usampler2DAtomic usampler2D
#define usampler3DAtomic usampler3D
#define isampler2DArrayAtomic isampler2DArray
#define isampler2DAtomic isampler2D
#define isampler3DAtomic isampler3D


#define imageFence(image)


#define select(A, B, mask) mix(A, B, mask)


#define float_array float[]
#define float2_array vec2[]
#define float3_array vec3[]
#define float4_array vec4[]
#define int_array int[]
#define int2_array int2[]
#define int3_array int3[]
#define int4_array int4[]
#define uint_array uint[]
#define uint2_array uint2[]
#define uint3_array uint3[]
#define uint4_array uint4[]
#define bool_array bool[]
#define bool2_array bool2[]
#define bool3_array bool3[]
#define bool4_array bool4[]
#define ARRAY_T(type) type[]
#define ARRAY_V

#define SHADER_LIBRARY_CREATE_INFO(a)
#define VERTEX_SHADER_CREATE_INFO(a)
#define FRAGMENT_SHADER_CREATE_INFO(a)
#define COMPUTE_SHADER_CREATE_INFO(a)

#define ATTR_FALLTHROUGH

#define _in_sta
#define _in_end
#define _out_sta
#define _out_end
#define _inout_sta
#define _inout_end
#define _shared_sta
#define _shared_end


#define _ref(_type, _var) inout _type _var

#define _ctor(_type) _type(
#define _rotc() )


#define specialization_constant_get(create_info, _res) _res
#define shared_variable_get(create_info, _res) _res
#define push_constant_get(create_info, _res) _res
#define interface_get(create_info, _res) _res
#define attribute_get(create_info, _res) _res
#define buffer_get(create_info, _res) _res
#define sampler_get(create_info, _res) _res
#define image_get(create_info, _res) _res
#define srt_access(create_info, _res) access_##create_info##_##_res()


#define static
#define constant
#define device
#define thread
#define threadgroup






struct string_t {
  uint hash;
};








uint as_uint(string_t str)
{
  return str.hash;
}

float4 texelFetchExtend(sampler2D samp, int2 texel, int lvl)
{
  texel = clamp(texel, int2(0), textureSize(samp, lvl).xy - 1);
  return texelFetch(samp, texel, lvl);
}




#ifdef GPU_FRAGMENT_SHADER
#  define gpu_discard_fragment() discard
#  define gpu_dfdx(x) dFdx(x)
#  define gpu_dfdy(x) dFdy(x)
#  define gpu_fwidth(x) fwidth(x)
#else
#  define gpu_discard_fragment()
#  define gpu_dfdx(x) x
#  define gpu_dfdy(x) x
#  define gpu_fwidth(x) x
#endif

#define GPU_SHADER
#define GPU_INTEL
#define OS_UNIX
#define GPU_VULKAN
#define GPU_VERTEX_SHADER
#define DRAW_VIEW_CREATE_INFO 
#define CREATE_INFO_workbench_resolve_opaque_studio_no_cavity_no_curvature_no_shadow
#define CREATE_INFO_workbench_resolve_vert_infos_
#define CREATE_INFO_workbench_resolve_frag_infos_
#define CREATE_INFO_workbench_resolve_Resources
#define CREATE_INFO_draw_view
#define CREATE_INFO_workbench_World
#define CREATE_INFO_workbench_resolve_FragOut
#define ENTRY_POINT_main
#define ENTRY_POINT_workbench_resolve_frag
#define ENTRY_POINT_workbench_resolve_vert
#define SRT_CONSTANT_use_cavity 0
#define SRT_CONSTANT_use_curvature 0
#define SRT_CONSTANT_use_shadow 0
#define SRT_CONSTANT_lighting_mode 0
#define USE_GPU_SHADER_CREATE_INFO

#line 750 "source/blender/gpu/vulkan/vk_shader.cc"
const bool use_cavity=false;
const bool use_curvature=false;
const bool use_shadow=false;
const int lighting_mode=0;


#define CREATE_INFO_RES_PASS_workbench_resolve_Resources \
layout(binding = 0) uniform sampler2D depth_tx; \
layout(binding = 1) uniform sampler2D normal_tx; \
layout(binding = 2) uniform sampler2D material_tx; \

#define CREATE_INFO_RES_PASS_draw_view \
layout(binding = 3, std140) uniform _drw_view_buf { ViewMatrices_host_shared_uniform_ drw_view_buf[DRW_VIEW_LEN]; }; \

#define CREATE_INFO_RES_PASS_workbench_World \
layout(binding = 4, std140) uniform _world_data { WorldData_host_shared_uniform_ world_data; }; \






void main_function_();
void main() {
  main_function_();
gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
}
#define main main_function_

#line 1 "draw_defines.hh"
 
#line 15
#define DRW_VIEW_UBO_SLOT 11
#define DRW_VIEW_CULLING_UBO_SLOT 10
#define DRW_OBJ_DATA_INFO_UBO_SLOT 9
#define DRW_LAYER_ATTR_UBO_SLOT 7
#line 21
#define DRW_OBJ_INFOS_UBO_SLOT 6

#define DRW_CLIPPING_UBO_SLOT 5
#line 26
#define DRW_RESOURCE_ID_SLOT 11
#define DRW_OBJ_MAT_SLOT 10
#define DRW_OBJ_INFOS_SLOT 9
#define DRW_OBJ_ATTR_SLOT 8
#line 32
#define DRW_DEBUG_DRAW_SLOT 14
#define DRW_DEBUG_DRAW_FEEDBACK_SLOT 15

#define DRW_COMMAND_GROUP_SIZE 64
#define DRW_FINALIZE_GROUP_SIZE 64

#define DRW_VISIBILITY_GROUP_SIZE 32
#line 45
#define DRW_VIEW_MAX 64

#define DRW_POINTCLOUD_STRIP_TILE_SIZE 8
#line 50
#define OVERLAY_GLOBALS_SLOT 7
#line 1 "GPU_shader_shared_utils.hh"
 
#line 33
#ifdef GLSL_CPP_STUBS
#line 37
#  define BLI_STATIC_ASSERT(cond, msg)
#  define BLI_STATIC_ASSERT_ALIGN(type_, align_)
#  define BLI_STATIC_ASSERT_SIZE(type_, size_)
#  define ENUM_OPERATORS(a)
#  define UNUSED_VARS(a) (void)a

#  define cosf cos
#  define sinf sin
#  define tanf tan
#  define acosf acos
#  define asinf asin
#  define atanf atan
#  define floorf floor
#  define ceilf ceil
#  define sqrtf sqrt
#  define expf exp

#elif defined(GPU_SHADER)
#line 58
#  define BLI_STATIC_ASSERT(cond, msg)
#  define BLI_STATIC_ASSERT_ALIGN(type_, align_)
#  define BLI_STATIC_ASSERT_SIZE(type_, size_)
#  define ENUM_OPERATORS(a)
#  define UNUSED_VARS(a)

#  define cosf cos
#  define sinf sin
#  define tanf tan
#  define acosf acos
#  define asinf asin
#  define atanf atan
#  define floorf floor
#  define ceilf ceil
#  define sqrtf sqrt
#  define expf exp

#else
#line 147
#  if defined(GPU_VERTEX_SHADER)
#    define GPU_THREAD uint3(gl_VertexID, gl_InstanceID, 0)
#  elif defined(GPU_FRAGMENT_SHADER)
#    define GPU_THREAD uint3(gl_FragCoord.x, gl_FragCoord.y, 0)
#  elif defined(GPU_COMPUTE_SHADER)
#    define GPU_THREAD gl_GlobalInvocationID
#  else
#    define GPU_THREAD error_not_in_a_shader_question_mark
#  endif
#endif
#line 1 "draw_command_shared.hh"
 
#line 29
#define DrawGroup_host_shared_ DrawGroup
#define DrawGroup_host_shared_uniform_ DrawGroup
#line 28
struct                 DrawGroup {

  uint next;
#line 39
  uint start;

  uint len;

  uint front_facing_len;
#line 46
  int vertex_len;
  int vertex_first;

  int base_index;
#line 54
  uint total_counter;

  uint front_facing_counter;
  uint back_facing_counter;
#line 61
#ifndef GPU_SHADER
#line 75
#else
  uint _cpu_reserved_1;
  uint _cpu_reserved_2;

  uint _cpu_reserved_3;
  uint _cpu_reserved_4;
  uint _cpu_reserved_5;
  uint _cpu_reserved_6;
#endif
};
#line 28
                                         DrawGroup DrawGroup_ctor_() {DrawGroup r;r.next=0u;r.start=0u;r.len=0u;r.front_facing_len=0u;r.vertex_len=0;r.vertex_first=0;r.base_index=0;r.total_counter=0u;r.front_facing_counter=0u;r.back_facing_counter=0u;r._cpu_reserved_1=0u;r._cpu_reserved_2=0u;r._cpu_reserved_3=0u;r._cpu_reserved_4=0u;r._cpu_reserved_5=0u;r._cpu_reserved_6=0u;return r;}
#line 85
BLI_STATIC_ASSERT_ALIGN(DrawGroup, 16)
#line 93
#define DrawPrototype_host_shared_ DrawPrototype
#define DrawPrototype_host_shared_uniform_ DrawPrototype
#line 92
struct                 DrawPrototype {

  uint group_id;

  uint res_index;

  uint custom_id;

  uint instance_len;
};
#line 92
                                             DrawPrototype DrawPrototype_ctor_() {DrawPrototype r;r.group_id=0u;r.res_index=0u;r.custom_id=0u;r.instance_len=0u;return r;}
#line 108
#line 1 "draw_shader_shared.hh"
 
#line 55
#define DRW_SHADER_SHARED_H

#define DRW_RESOURCE_CHUNK_LEN 512
#line 60
#define DRW_GRID_PER_VOLUME_MAX 16
#line 64
#define DRW_ATTRIBUTE_PER_CURVES_MAX 15
#line 70
#if !defined(DRW_VIEW_LEN) && !defined(GLSL_CPP_STUBS)

#  define drw_view_id 0
#  define DRW_VIEW_LEN 1
#  define DRW_VIEW_SHIFT 0
#  define DRW_VIEW_FROM_RESOURCE_ID
#else
#line 82
uint drw_view_id = 0;
#line 88
#  define DRW_VIEW_SHIFT \
    ((DRW_VIEW_LEN > 32) ? 6 : \
     (DRW_VIEW_LEN > 16) ? 5 : \
     (DRW_VIEW_LEN > 8)  ? 4 : \
     (DRW_VIEW_LEN > 4)  ? 3 : \
     (DRW_VIEW_LEN > 2)  ? 2 : \
                           1)
#  define DRW_VIEW_MASK ~(0xFFFFFFFFu << DRW_VIEW_SHIFT)
#  define DRW_VIEW_FROM_RESOURCE_ID drw_view_id = (drw_resource_id_raw() & DRW_VIEW_MASK)
#endif
#line 100
#define FrustumCorners_host_shared_ FrustumCorners
#define FrustumCorners_host_shared_uniform_ FrustumCorners
#line 99
struct                 FrustumCorners {
  float4 corners[8];
};
#line 99
                                              FrustumCorners FrustumCorners_ctor_() {FrustumCorners r;r.corners[0]=float4(0);r.corners[1]=float4(0);r.corners[2]=float4(0);r.corners[3]=float4(0);r.corners[4]=float4(0);r.corners[5]=float4(0);r.corners[6]=float4(0);r.corners[7]=float4(0);return r;}
#line 104
#define FrustumPlanes_host_shared_ FrustumPlanes
#define FrustumPlanes_host_shared_uniform_ FrustumPlanes
#line 103
struct                 FrustumPlanes {
#line 110
  float4 planes[6];
};
#line 103
                                             FrustumPlanes FrustumPlanes_ctor_() {FrustumPlanes r;r.planes[0]=float4(0);r.planes[1]=float4(0);r.planes[2]=float4(0);r.planes[3]=float4(0);r.planes[4]=float4(0);r.planes[5]=float4(0);return r;}
#line 114
#define ViewCullingData_host_shared_ ViewCullingData
#define ViewCullingData_host_shared_uniform_ ViewCullingData
#line 113
struct                 ViewCullingData {
#line 116
         FrustumCorners frustum_corners;
         FrustumPlanes frustum_planes;
  float4 bound_sphere;
};
#line 113
                                               ViewCullingData ViewCullingData_ctor_() {ViewCullingData r;r.frustum_corners=FrustumCorners_ctor_();r.frustum_planes=FrustumPlanes_ctor_();r.bound_sphere=float4(0);return r;}
#line 122
#define ViewMatrices_host_shared_ ViewMatrices
#define ViewMatrices_host_shared_uniform_ ViewMatrices
#line 121
struct                 ViewMatrices {
  float4x4 viewmat;
  float4x4 viewinv;
  float4x4 winmat;
  float4x4 wininv;
};
#line 121
                                            ViewMatrices ViewMatrices_ctor_() {ViewMatrices r;r.viewmat=float4x4(0);r.viewinv=float4x4(0);r.winmat=float4x4(0);r.wininv=float4x4(0);return r;}
#line 135
#define ObjectMatrices_host_shared_ ObjectMatrices
#define ObjectMatrices_host_shared_uniform_ ObjectMatrices
#line 134
struct                 ObjectMatrices {
  float4x4 model;
  float4x4 model_inverse;
#line 142
};
#line 134
                                              ObjectMatrices ObjectMatrices_ctor_() {ObjectMatrices r;r.model=float4x4(0);r.model_inverse=float4x4(0);return r;}
#line 145
constant static constexpr uint32_t OBJECT_SELECTED = (1u << 0u);
constant static constexpr uint32_t OBJECT_FROM_DUPLI = (1u << 1u);
constant static constexpr uint32_t OBJECT_FROM_SET = (1u << 2u);
constant static constexpr uint32_t OBJECT_ACTIVE = (1u << 3u);
constant static constexpr uint32_t OBJECT_NEGATIVE_SCALE = (1u << 4u);
constant static constexpr uint32_t OBJECT_HOLDOUT = (1u << 5u);
#line 153
constant static constexpr uint32_t OBJECT_ACTIVE_EDIT_MODE = (1u << 6u);
#line 155
constant static constexpr uint32_t OBJECT_NO_INFO = ~OBJECT_HOLDOUT;

#define eObjectInfoFlag uint32_t
#line 144

#define eObjectInfoFlag_host_shared_ eObjectInfoFlag
#line 144

eObjectInfoFlag eObjectInfoFlag_ctor_() { return eObjectInfoFlag(0); }
#line 163
#define ObjectInfos_host_shared_ ObjectInfos
#define ObjectInfos_host_shared_uniform_ ObjectInfos
#line 162
struct                 ObjectInfos {

  packed_float3 orco_add;
  uint object_attrs_offset;
  packed_float3 orco_mul;
  uint object_attrs_len;

  float4 ob_color;
  uint index;

  uint light_and_shadow_set_membership;
  float random;
       eObjectInfoFlag flag;
  float shadow_terminator_normal_offset;
  float shadow_terminator_geometry_offset;
  float _pad1;
  float _pad2;
#line 184
};

       #line 162
                                           ObjectInfos ObjectInfos_ctor_() {ObjectInfos r;r.orco_add=packed_float3(0);r.object_attrs_offset=0u;r.orco_mul=packed_float3(0);r.object_attrs_len=0u;r.ob_color=float4(0);r.index=0u;r.light_and_shadow_set_membership=0u;r.random=0.0f;r.flag=eObjectInfoFlag_ctor_();r.shadow_terminator_normal_offset=0.0f;r.shadow_terminator_geometry_offset=0.0f;r._pad1=0.0f;r._pad2=0.0f;return r;}
#line 186
uint receiver_light_set_get(ObjectInfos object_infos)
{
  return object_infos.light_and_shadow_set_membership & 0xFFu;
}

       uint blocker_shadow_set_get(ObjectInfos object_infos)
{
  return (object_infos.light_and_shadow_set_membership >> 8u) & 0xFFu;
}
#line 197
#define ObjectBounds_host_shared_ ObjectBounds
#define ObjectBounds_host_shared_uniform_ ObjectBounds
#line 196
struct                 ObjectBounds {
#line 201
  float4 bounding_corners[4];

  float4 bounding_sphere;

#define _inner_sphere_radius bounding_corners[3].w
#line 212
};
       #line 196
                                            ObjectBounds ObjectBounds_ctor_() {ObjectBounds r;r.bounding_sphere=float4(0);r.bounding_corners[0]=float4(0);r.bounding_corners[1]=float4(0);r.bounding_corners[2]=float4(0);r.bounding_corners[3]=float4(0);return r;}
#line 218
bool drw_bounds_corners_are_valid(ObjectBounds bounds)
{
  return bounds.bounding_sphere.w != -1.0f;
}
#line 226
       bool drw_bounds_are_valid(ObjectBounds bounds)
{
  return bounds.bounding_sphere.w >= 0.0f;
}
#line 238
#define VolumeInfos_host_shared_ VolumeInfos
#define VolumeInfos_host_shared_uniform_ VolumeInfos
#line 237
struct                 VolumeInfos {

  float4x4 grids_xform[DRW_GRID_PER_VOLUME_MAX];

  float4 color_mul;
  float density_scale;
  float temperature_mul;
  float temperature_bias;
  float _pad;
};
#line 237
                                           VolumeInfos VolumeInfos_ctor_() {VolumeInfos r;r.color_mul=float4(0);r.density_scale=0.0f;r.temperature_mul=0.0f;r.temperature_bias=0.0f;r._pad=0.0f;for(int i=0;i < DRW_GRID_PER_VOLUME_MAX;i++){r.grids_xform[i]=float4x4(0);}return r;}
#line 249
#define CurvesInfos_host_shared_ CurvesInfos
#define CurvesInfos_host_shared_uniform_ CurvesInfos
#line 248
struct                 CurvesInfos {
#line 253
  uint4 is_point_attribute[DRW_ATTRIBUTE_PER_CURVES_MAX];
#line 256
  uint vertex_per_segment;

  uint half_cylinder_face_count;
  uint _pad0;
  uint _pad1;
};
#line 248
                                           CurvesInfos CurvesInfos_ctor_() {CurvesInfos r;r.vertex_per_segment=0u;r.half_cylinder_face_count=0u;r._pad0=0u;r._pad1=0u;for(int i=0;i < DRW_ATTRIBUTE_PER_CURVES_MAX;i++){r.is_point_attribute[i]=uint4(0);}return r;}
#line 263
#pragma pack(push, 4)

#define ObjectAttribute_host_shared_ ObjectAttribute
#line 264
struct                 ObjectAttribute {
#line 267
  float data_x;
  float data_y;
  float data_z;
  float data_w;
  uint hash_code;
#line 280
};
#line 264
                                               ObjectAttribute ObjectAttribute_ctor_() {ObjectAttribute r;r.data_x=0.0f;r.data_y=0.0f;r.data_z=0.0f;r.data_w=0.0f;r.hash_code=0u;return r;}
#line 281
#pragma pack(pop)
#line 284
BLI_STATIC_ASSERT_ALIGN(ObjectAttribute, 20)
#line 287
#define LayerAttribute_host_shared_ LayerAttribute
#define LayerAttribute_host_shared_uniform_ LayerAttribute
#line 286
struct                 LayerAttribute {
  float4 data;
  uint hash_code;
  uint buffer_length;
  uint _pad1;
  uint _pad2;
#line 296
};
#line 286
                                              LayerAttribute LayerAttribute_ctor_() {LayerAttribute r;r.data=float4(0);r.hash_code=0u;r.buffer_length=0u;r._pad1=0u;r._pad2=0u;return r;}
#line 306
#define DrawCommandArray_host_shared_ DrawCommandArray
#define DrawCommandArray_host_shared_uniform_ DrawCommandArray
#line 305
struct                 DrawCommandArray {
  uint vertex_len;
  uint instance_len;
  uint vertex_first;
  uint instance_first;

  uint _pad0;
  uint _pad1;
  uint _pad2;
  uint _pad3;
};
#line 305
                                                DrawCommandArray DrawCommandArray_ctor_() {DrawCommandArray r;r.vertex_len=0u;r.instance_len=0u;r.vertex_first=0u;r.instance_first=0u;r._pad0=0u;r._pad1=0u;r._pad2=0u;r._pad3=0u;return r;}
#line 319
#define DrawCommandIndexed_host_shared_ DrawCommandIndexed
#define DrawCommandIndexed_host_shared_uniform_ DrawCommandIndexed
#line 318
struct                 DrawCommandIndexed {
  uint vertex_len;
  uint instance_len;
  uint vertex_first;
  uint base_index;

  uint instance_first;
  uint _pad0;
  uint _pad1;
  uint _pad2;
};
#line 318
                                                  DrawCommandIndexed DrawCommandIndexed_ctor_() {DrawCommandIndexed r;r.vertex_len=0u;r.instance_len=0u;r.vertex_first=0u;r.base_index=0u;r.instance_first=0u;r._pad0=0u;r._pad1=0u;r._pad2=0u;return r;}
#line 331

#define DrawCommand_union0_host_shared_ DrawCommand_union0
#define DrawCommand_union0_host_shared_uniform_ DrawCommand_union0
#line 331
struct                 DrawCommand_union0 {
  float4 data0;
  float4 data1;

};
#line 331
                                                  DrawCommand_union0 DrawCommand_union0_ctor_() {DrawCommand_union0 r;r.data0=float4(0);r.data1=float4(0);return r;}
#line 330

#define DrawCommand_host_shared_ DrawCommand
#define DrawCommand_host_shared_uniform_ DrawCommand
#line 330
struct                 DrawCommand {
         DrawCommand_union0 union0;
#line 383
};
#line 387
#ifndef GPU_METAL
DrawCommand DrawCommand_ctor_();
DrawCommandArray _array(const DrawCommand this_);
void _array_set_(_ref(DrawCommand ,this_), DrawCommandArray value);
DrawCommandIndexed _indexed(const DrawCommand this_);
void _indexed_set_(_ref(DrawCommand ,this_), DrawCommandIndexed value);
#endif
#line 330
                                           DrawCommand DrawCommand_ctor_() {DrawCommand r;r.union0=DrawCommand_union0_ctor_();return r;}
#line 336
DrawCommandArray _array(const DrawCommand this_)       {
  DrawCommandArray val;
  val.vertex_len = floatBitsToUint(this_.union0.data0.x);
  val.instance_len = floatBitsToUint(this_.union0.data0.y);
  val.vertex_first = floatBitsToUint(this_.union0.data0.z);
  val.instance_first = floatBitsToUint(this_.union0.data0.w);
  val._pad0 = floatBitsToUint(this_.union0.data1.x);
  val._pad1 = floatBitsToUint(this_.union0.data1.y);
  val._pad2 = floatBitsToUint(this_.union0.data1.z);
  val._pad3 = floatBitsToUint(this_.union0.data1.w);
  return val;
}
#line 349
void _array_set_(_ref(DrawCommand ,this_), DrawCommandArray value) {
  this_.union0.data0.x = uintBitsToFloat(value.vertex_len);
  this_.union0.data0.y = uintBitsToFloat(value.instance_len);
  this_.union0.data0.z = uintBitsToFloat(value.vertex_first);
  this_.union0.data0.w = uintBitsToFloat(value.instance_first);
  this_.union0.data1.x = uintBitsToFloat(value._pad0);
  this_.union0.data1.y = uintBitsToFloat(value._pad1);
  this_.union0.data1.z = uintBitsToFloat(value._pad2);
  this_.union0.data1.w = uintBitsToFloat(value._pad3);
}
#line 360
DrawCommandIndexed _indexed(const DrawCommand this_)       {
  DrawCommandIndexed val;
  val.vertex_len = floatBitsToUint(this_.union0.data0.x);
  val.instance_len = floatBitsToUint(this_.union0.data0.y);
  val.vertex_first = floatBitsToUint(this_.union0.data0.z);
  val.base_index = floatBitsToUint(this_.union0.data0.w);
  val.instance_first = floatBitsToUint(this_.union0.data1.x);
  val._pad0 = floatBitsToUint(this_.union0.data1.y);
  val._pad1 = floatBitsToUint(this_.union0.data1.z);
  val._pad2 = floatBitsToUint(this_.union0.data1.w);
  return val;
}
#line 373
void _indexed_set_(_ref(DrawCommand ,this_), DrawCommandIndexed value) {
  this_.union0.data0.x = uintBitsToFloat(value.vertex_len);
  this_.union0.data0.y = uintBitsToFloat(value.instance_len);
  this_.union0.data0.z = uintBitsToFloat(value.vertex_first);
  this_.union0.data0.w = uintBitsToFloat(value.base_index);
  this_.union0.data1.x = uintBitsToFloat(value.instance_first);
  this_.union0.data1.y = uintBitsToFloat(value._pad0);
  this_.union0.data1.z = uintBitsToFloat(value._pad1);
  this_.union0.data1.w = uintBitsToFloat(value._pad2);
}
#line 386
#define DispatchCommand_host_shared_ DispatchCommand
#define DispatchCommand_host_shared_uniform_ DispatchCommand
#line 385
struct                 DispatchCommand {
  uint num_groups_x;
  uint num_groups_y;
  uint num_groups_z;
  uint _pad0;
};
#line 385
                                               DispatchCommand DispatchCommand_ctor_() {DispatchCommand r;r.num_groups_x=0u;r.num_groups_y=0u;r.num_groups_z=0u;r._pad0=0u;return r;}
#line 399
#define DRWDebugVertPair_host_shared_ DRWDebugVertPair
#define DRWDebugVertPair_host_shared_uniform_ DRWDebugVertPair
#line 398
struct                 DRWDebugVertPair {
#line 401
  uint pos1_x;
  uint pos1_y;
  uint pos1_z;

  uint vert_color;

  uint pos2_x;
  uint pos2_y;
  uint pos2_z;

  uint lifetime;
};

       #line 398
                                                DRWDebugVertPair DRWDebugVertPair_ctor_() {DRWDebugVertPair r;r.pos1_x=0u;r.pos1_y=0u;r.pos1_z=0u;r.vert_color=0u;r.pos2_x=0u;r.pos2_y=0u;r.pos2_z=0u;r.lifetime=0u;return r;}
#line 414
DRWDebugVertPair debug_line_make(uint in_pos1_x,
                                        uint in_pos1_y,
                                        uint in_pos1_z,
                                        uint in_pos2_x,
                                        uint in_pos2_y,
                                        uint in_pos2_z,
                                        uint in_vert_color,
                                        uint in_lifetime)
{
  DRWDebugVertPair debug_vert;
  debug_vert.pos1_x = in_pos1_x;
  debug_vert.pos1_y = in_pos1_y;
  debug_vert.pos1_z = in_pos1_z;
  debug_vert.pos2_x = in_pos2_x;
  debug_vert.pos2_y = in_pos2_y;
  debug_vert.pos2_z = in_pos2_z;
  debug_vert.vert_color = in_vert_color;
  debug_vert.lifetime = in_lifetime;
  return debug_vert;
}

       uint debug_color_pack(float4 v_color)
{
  v_color = clamp(v_color, 0.0f, 1.0f);
  uint result = 0;
  result |= uint(v_color.x * 255.0) << 0u;
  result |= uint(v_color.y * 255.0) << 8u;
  result |= uint(v_color.z * 255.0) << 16u;
  result |= uint(v_color.w * 255.0) << 24u;
  return result;
}
#line 447
#define DRW_DEBUG_DRAW_VERT_MAX (2 * 1024) - 1
#line 452
#define DRWDebugDrawBuffer_host_shared_ DRWDebugDrawBuffer
#define DRWDebugDrawBuffer_host_shared_uniform_ DRWDebugDrawBuffer
#line 451
struct                 DRWDebugDrawBuffer {
         DrawCommand command;
         DRWDebugVertPair verts[DRW_DEBUG_DRAW_VERT_MAX];
};
#line 451
                                                  DRWDebugDrawBuffer DRWDebugDrawBuffer_ctor_() {DRWDebugDrawBuffer r;r.command=DrawCommand_ctor_();for(int i=0;i < DRW_DEBUG_DRAW_VERT_MAX;i++){r.verts[i]=DRWDebugVertPair_ctor_();}return r;}
#line 457
#define drw_debug_draw_v_count(buf) buf[0].pos1_x
#line 462
#define drw_debug_draw_offset 1






#line 1 "draw_view_infos.hh"
 
#line 7
#ifdef GPU_SHADER
#line 13
#endif

#ifdef GLSL_CPP_STUBS

#  define DRAW_VIEW_CREATE_INFO
#  define DRW_VIEW_CULLING_INFO
#  define USE_WORLD_CLIP_PLANES

#  define DRW_VIEW_LEN DRW_VIEW_MAX
#endif
#line 39
#ifdef CREATE_INFO_RES_PASS_draw_resource_id_varying
CREATE_INFO_RES_PASS_draw_resource_id_varying
#endif
#ifdef CREATE_INFO_RES_BATCH_draw_resource_id_varying
CREATE_INFO_RES_BATCH_draw_resource_id_varying
#endif
#ifdef CREATE_INFO_RES_GEOMETRY_draw_resource_id_varying
CREATE_INFO_RES_GEOMETRY_draw_resource_id_varying
#endif
#ifdef CREATE_INFO_RES_SHARED_VARS_draw_resource_id_varying
CREATE_INFO_RES_SHARED_VARS_draw_resource_id_varying
#endif

#ifdef CREATE_INFO_RES_PASS_draw_resource_id
CREATE_INFO_RES_PASS_draw_resource_id
#endif
#ifdef CREATE_INFO_RES_BATCH_draw_resource_id
CREATE_INFO_RES_BATCH_draw_resource_id
#endif
#ifdef CREATE_INFO_RES_GEOMETRY_draw_resource_id
CREATE_INFO_RES_GEOMETRY_draw_resource_id
#endif
#ifdef CREATE_INFO_RES_SHARED_VARS_draw_resource_id
CREATE_INFO_RES_SHARED_VARS_draw_resource_id
#endif

#ifdef CREATE_INFO_RES_PASS_draw_resource_with_custom_id
CREATE_INFO_RES_PASS_draw_resource_with_custom_id
#endif
#ifdef CREATE_INFO_RES_BATCH_draw_resource_with_custom_id
CREATE_INFO_RES_BATCH_draw_resource_with_custom_id
#endif
#ifdef CREATE_INFO_RES_GEOMETRY_draw_resource_with_custom_id
CREATE_INFO_RES_GEOMETRY_draw_resource_with_custom_id
#endif
#ifdef CREATE_INFO_RES_SHARED_VARS_draw_resource_with_custom_id
CREATE_INFO_RES_SHARED_VARS_draw_resource_with_custom_id
#endif
#line 84
#ifdef CREATE_INFO_RES_PASS_draw_modelmat_common
CREATE_INFO_RES_PASS_draw_modelmat_common
#endif
#ifdef CREATE_INFO_RES_BATCH_draw_modelmat_common
CREATE_INFO_RES_BATCH_draw_modelmat_common
#endif
#ifdef CREATE_INFO_RES_GEOMETRY_draw_modelmat_common
CREATE_INFO_RES_GEOMETRY_draw_modelmat_common
#endif
#ifdef CREATE_INFO_RES_SHARED_VARS_draw_modelmat_common
CREATE_INFO_RES_SHARED_VARS_draw_modelmat_common
#endif

#ifdef CREATE_INFO_RES_PASS_draw_modelmat
CREATE_INFO_RES_PASS_draw_modelmat
#endif
#ifdef CREATE_INFO_RES_BATCH_draw_modelmat
CREATE_INFO_RES_BATCH_draw_modelmat
#endif
#ifdef CREATE_INFO_RES_GEOMETRY_draw_modelmat
CREATE_INFO_RES_GEOMETRY_draw_modelmat
#endif
#ifdef CREATE_INFO_RES_SHARED_VARS_draw_modelmat
CREATE_INFO_RES_SHARED_VARS_draw_modelmat
#endif

#ifdef CREATE_INFO_RES_PASS_draw_modelmat_with_custom_id
CREATE_INFO_RES_PASS_draw_modelmat_with_custom_id
#endif
#ifdef CREATE_INFO_RES_BATCH_draw_modelmat_with_custom_id
CREATE_INFO_RES_BATCH_draw_modelmat_with_custom_id
#endif
#ifdef CREATE_INFO_RES_GEOMETRY_draw_modelmat_with_custom_id
CREATE_INFO_RES_GEOMETRY_draw_modelmat_with_custom_id
#endif
#ifdef CREATE_INFO_RES_SHARED_VARS_draw_modelmat_with_custom_id
CREATE_INFO_RES_SHARED_VARS_draw_modelmat_with_custom_id
#endif
#line 129
#ifdef CREATE_INFO_RES_PASS_draw_view
CREATE_INFO_RES_PASS_draw_view
#endif
#ifdef CREATE_INFO_RES_BATCH_draw_view
CREATE_INFO_RES_BATCH_draw_view
#endif
#ifdef CREATE_INFO_RES_GEOMETRY_draw_view
CREATE_INFO_RES_GEOMETRY_draw_view
#endif
#ifdef CREATE_INFO_RES_SHARED_VARS_draw_view
CREATE_INFO_RES_SHARED_VARS_draw_view
#endif

#ifdef CREATE_INFO_RES_PASS_draw_view_culling
CREATE_INFO_RES_PASS_draw_view_culling
#endif
#ifdef CREATE_INFO_RES_BATCH_draw_view_culling
CREATE_INFO_RES_BATCH_draw_view_culling
#endif
#ifdef CREATE_INFO_RES_GEOMETRY_draw_view_culling
CREATE_INFO_RES_GEOMETRY_draw_view_culling
#endif
#ifdef CREATE_INFO_RES_SHARED_VARS_draw_view_culling
CREATE_INFO_RES_SHARED_VARS_draw_view_culling
#endif
#line 161
#ifdef CREATE_INFO_RES_PASS_drw_clipped
CREATE_INFO_RES_PASS_drw_clipped
#endif
#ifdef CREATE_INFO_RES_BATCH_drw_clipped
CREATE_INFO_RES_BATCH_drw_clipped
#endif
#ifdef CREATE_INFO_RES_GEOMETRY_drw_clipped
CREATE_INFO_RES_GEOMETRY_drw_clipped
#endif
#ifdef CREATE_INFO_RES_SHARED_VARS_drw_clipped
CREATE_INFO_RES_SHARED_VARS_drw_clipped
#endif
#line 180
#ifdef CREATE_INFO_RES_PASS_draw_resource_finalize
CREATE_INFO_RES_PASS_draw_resource_finalize
#endif
#ifdef CREATE_INFO_RES_BATCH_draw_resource_finalize
CREATE_INFO_RES_BATCH_draw_resource_finalize
#endif
#ifdef CREATE_INFO_RES_GEOMETRY_draw_resource_finalize
CREATE_INFO_RES_GEOMETRY_draw_resource_finalize
#endif
#ifdef CREATE_INFO_RES_SHARED_VARS_draw_resource_finalize
CREATE_INFO_RES_SHARED_VARS_draw_resource_finalize
#endif

#ifdef CREATE_INFO_RES_PASS_draw_view_finalize
CREATE_INFO_RES_PASS_draw_view_finalize
#endif
#ifdef CREATE_INFO_RES_BATCH_draw_view_finalize
CREATE_INFO_RES_BATCH_draw_view_finalize
#endif
#ifdef CREATE_INFO_RES_GEOMETRY_draw_view_finalize
CREATE_INFO_RES_GEOMETRY_draw_view_finalize
#endif
#ifdef CREATE_INFO_RES_SHARED_VARS_draw_view_finalize
CREATE_INFO_RES_SHARED_VARS_draw_view_finalize
#endif

#ifdef CREATE_INFO_RES_PASS_draw_visibility_compute
CREATE_INFO_RES_PASS_draw_visibility_compute
#endif
#ifdef CREATE_INFO_RES_BATCH_draw_visibility_compute
CREATE_INFO_RES_BATCH_draw_visibility_compute
#endif
#ifdef CREATE_INFO_RES_GEOMETRY_draw_visibility_compute
CREATE_INFO_RES_GEOMETRY_draw_visibility_compute
#endif
#ifdef CREATE_INFO_RES_SHARED_VARS_draw_visibility_compute
CREATE_INFO_RES_SHARED_VARS_draw_visibility_compute
#endif

#ifdef CREATE_INFO_RES_PASS_draw_command_generate
CREATE_INFO_RES_PASS_draw_command_generate
#endif
#ifdef CREATE_INFO_RES_BATCH_draw_command_generate
CREATE_INFO_RES_BATCH_draw_command_generate
#endif
#ifdef CREATE_INFO_RES_GEOMETRY_draw_command_generate
CREATE_INFO_RES_GEOMETRY_draw_command_generate
#endif
#ifdef CREATE_INFO_RES_SHARED_VARS_draw_command_generate
CREATE_INFO_RES_SHARED_VARS_draw_command_generate
#endif
#line 235
#ifdef GLSL_CPP_STUBS

#  define resource_id_buf uint2(0)
#endif
#line 1 "draw_view_lib.glsl"
 
#line 9
SHADER_LIBRARY_CREATE_INFO(draw_view)

#if !defined(DRAW_VIEW_CREATE_INFO) && !defined(GLSL_CPP_STUBS)
#  error Missing draw_view additional create info on shader create info
#endif
#line 16
ViewMatrices drw_view()
{
  return drw_view_buf[drw_view_id];
}
#line 22
bool drw_view_is_perspective()
{
  return drw_view().winmat[3][3] == 0.0f;
}
#line 28
float3 drw_view_forward()
{
  return drw_view().viewinv[2].xyz;
}
#line 34
float3 drw_view_up()
{
  return drw_view().viewinv[1].xyz;
}
#line 40
float3 drw_view_position()
{
  return drw_view().viewinv[3].xyz;
}
#line 46
float drw_view_z_distance(float3 P)
{
  return dot(P - drw_view_position(), -drw_view_forward());
}
#line 52
float drw_view_far()
{
  if (drw_view_is_perspective()) {
    return -drw_view().winmat[3][2] / (drw_view().winmat[2][2] + 1.0f);
  }
  return -(drw_view().winmat[3][2] - 1.0f) / drw_view().winmat[2][2];
}
#line 61
float drw_view_near()
{
  if (drw_view_is_perspective()) {
    return -drw_view().winmat[3][2] / (drw_view().winmat[2][2] - 1.0f);
  }
  return -(drw_view().winmat[3][2] + 1.0f) / drw_view().winmat[2][2];
}
#line 73
float3 drw_world_incident_vector(float3 P)
{
  return drw_view_is_perspective() ? normalize(drw_view_position() - P) : drw_view_forward();
}
#line 82
float3 drw_view_incident_vector(float3 vP)
{
  return drw_view_is_perspective() ? normalize(-vP) : float3(0.0f, 0.0f, 1.0f);
}
#line 90
float3 drw_screen_to_ndc(float3 ss_P)
{
  return ss_P * 2.0f - 1.0f;
}
float2 drw_screen_to_ndc(float2 ss_P)
{
  return ss_P * 2.0f - 1.0f;
}
float drw_screen_to_ndc(float ss_P)
{
  return ss_P * 2.0f - 1.0f;
}
#line 106
float3 drw_ndc_to_screen(float3 ndc_P)
{
  return ndc_P * 0.5f + 0.5f;
}
float2 drw_ndc_to_screen(float2 ndc_P)
{
  return ndc_P * 0.5f + 0.5f;
}
float drw_ndc_to_screen(float ndc_P)
{
  return ndc_P * 0.5f + 0.5f;
}
#line 123
float3 drw_normal_view_to_world(float3 vN)
{
  return (to_float3x3(drw_view().viewinv) * vN);
}

float3 drw_normal_world_to_view(float3 N)
{
  return (to_float3x3(drw_view().viewmat) * N);
}
#line 139
float3 drw_perspective_divide(float4 hs_P)
{
  return hs_P.xyz / hs_P.w;
}

float3 drw_point_view_to_world(float3 vP)
{
  return (drw_view().viewinv * float4(vP, 1.0f)).xyz;
}
float4 drw_point_view_to_homogenous(float3 vP)
{
  return (drw_view().winmat * float4(vP, 1.0f));
}
float3 drw_point_view_to_ndc(float3 vP)
{
  return drw_perspective_divide(drw_point_view_to_homogenous(vP));
}

float3 drw_point_world_to_view(float3 P)
{
  return (drw_view().viewmat * float4(P, 1.0f)).xyz;
}
float4 drw_point_world_to_homogenous(float3 P)
{
  return (drw_view().winmat * (drw_view().viewmat * float4(P, 1.0f)));
}
float3 drw_point_world_to_ndc(float3 P)
{
  return drw_perspective_divide(drw_point_world_to_homogenous(P));
}

float3 drw_point_ndc_to_view(float3 ssP)
{
  return drw_perspective_divide(drw_view().wininv * float4(ssP, 1.0f));
}
float3 drw_point_ndc_to_world(float3 ssP)
{
  return drw_point_view_to_world(drw_point_ndc_to_view(ssP));
}
#line 185
float3 drw_point_view_to_screen(float3 vP)
{
  return drw_ndc_to_screen(drw_point_view_to_ndc(vP));
}
float3 drw_point_world_to_screen(float3 vP)
{
  return drw_ndc_to_screen(drw_point_world_to_ndc(vP));
}

float3 drw_point_screen_to_view(float3 ssP)
{
  return drw_point_ndc_to_view(drw_screen_to_ndc(ssP));
}
float3 drw_point_screen_to_world(float3 ssP)
{
  return drw_point_view_to_world(drw_point_screen_to_view(ssP));
}

float drw_depth_view_to_screen(float v_depth)
{
  return drw_point_view_to_screen(float3(0.0f, 0.0f, v_depth)).z;
}
float drw_depth_screen_to_view(float ss_depth)
{
  return drw_point_screen_to_view(float3(0.0f, 0.0f, ss_depth)).z;
}


#line 1 "gpu_shader_fullscreen_lib.glsl"
 
#line 10
void fullscreen_vertex(int vertex_id, _ref(float4 ,out_position))
{
  int v = vertex_id % 3;
  float x = -1.0f + float((v & 1) << 2);
  float y = -1.0f + float((v & 2) << 1);
  out_position = float4(x, y, 1.0f, 1.0f);
}

void fullscreen_vertex(int vertex_id, _ref(float4 ,out_position), _ref(float2 ,out_uv))
{
  fullscreen_vertex(vertex_id, out_position);
  out_uv = (out_position.xy + 1.0f) * 0.5f;
}
#line 1 "workbench_defines.hh"
 
#line 5
#define WB_RESOLVE_GROUP_SIZE 8
#line 11
#define WB_MATCAP_SLOT 2
#define WB_TEXTURE_SLOT 3
#define WB_TILE_ARRAY_SLOT 4
#define WB_TILE_DATA_SLOT 5
#define WB_CURVES_UV_SLOT 6
#define WB_CURVES_COLOR_SLOT 7
#line 19
#define WB_MATERIAL_SLOT 0
#define WB_WORLD_SLOT 1
#line 1 "workbench_shader_shared.hh"
 
#line 9
#define WORKBENCH_SHADER_SHARED_H
#line 12
#define SolidLightData_host_shared_ SolidLightData
#define SolidLightData_host_shared_uniform_ SolidLightData
#line 11
struct                 SolidLightData {
  float4 direction;
  float4 specular_color;
  float4 diffuse_color_wrap;
};
#line 11
                                              SolidLightData SolidLightData_ctor_() {SolidLightData r;r.direction=float4(0);r.specular_color=float4(0);r.diffuse_color_wrap=float4(0);return r;}
#line 18
#define WorldData_host_shared_ WorldData
#define WorldData_host_shared_uniform_ WorldData
#line 17
struct                 WorldData {
  float2 viewport_size;
  float2 viewport_size_inv;
  float4 object_outline_color;
  float4 shadow_direction_vs;
  float shadow_focus;
  float shadow_shift;
  float shadow_mul;
  float shadow_add;

         SolidLightData lights[4];
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
  bool32_t use_specular;
  float xray_alpha;
  int _pad1;

  float4 background_color;
};
#line 17
                                         WorldData WorldData_ctor_() {WorldData r;r.viewport_size=float2(0);r.viewport_size_inv=float2(0);r.object_outline_color=float4(0);r.shadow_direction_vs=float4(0);r.shadow_focus=0.0f;r.shadow_shift=0.0f;r.shadow_mul=0.0f;r.shadow_add=0.0f;r.ambient_color=float4(0);r.cavity_sample_start=0;r.cavity_sample_end=0;r.cavity_sample_count_inv=0.0f;r.cavity_jitter_scale=0.0f;r.cavity_valley_factor=0.0f;r.cavity_ridge_factor=0.0f;r.cavity_attenuation=0.0f;r.cavity_distance=0.0f;r.curvature_ridge=0.0f;r.curvature_valley=0.0f;r.ui_scale=0.0f;r._pad0=0.0f;r.matcap_orientation=0;r.use_specular=bool32_t(0);r.xray_alpha=0.0f;r._pad1=0;r.background_color=float4(0);r.lights[0]=SolidLightData_ctor_();r.lights[1]=SolidLightData_ctor_();r.lights[2]=SolidLightData_ctor_();r.lights[3]=SolidLightData_ctor_();return r;}
#line 54
#define ExtrudedFrustum_host_shared_ ExtrudedFrustum
#define ExtrudedFrustum_host_shared_uniform_ ExtrudedFrustum
#line 53
struct                 ExtrudedFrustum {

  float4 corners[16];
  float4 planes[12];
  int corners_count;
  int planes_count;
  int _pad0;
  int _pad1;
};
#line 53
                                               ExtrudedFrustum ExtrudedFrustum_ctor_() {ExtrudedFrustum r;r.corners_count=0;r.planes_count=0;r._pad0=0;r._pad1=0;r.corners[0]=float4(0);r.corners[1]=float4(0);r.corners[2]=float4(0);r.corners[3]=float4(0);r.corners[4]=float4(0);r.corners[5]=float4(0);r.corners[6]=float4(0);r.corners[7]=float4(0);r.corners[8]=float4(0);r.corners[9]=float4(0);r.corners[10]=float4(0);r.corners[11]=float4(0);r.corners[12]=float4(0);r.corners[13]=float4(0);r.corners[14]=float4(0);r.corners[15]=float4(0);r.planes[0]=float4(0);r.planes[1]=float4(0);r.planes[2]=float4(0);r.planes[3]=float4(0);r.planes[4]=float4(0);r.planes[5]=float4(0);r.planes[6]=float4(0);r.planes[7]=float4(0);r.planes[8]=float4(0);r.planes[9]=float4(0);r.planes[10]=float4(0);r.planes[11]=float4(0);return r;}
#line 64
#define ShadowPassData_host_shared_ ShadowPassData
#define ShadowPassData_host_shared_uniform_ ShadowPassData
#line 63
struct                 ShadowPassData {
  float4 far_plane;
  packed_float3 light_direction_ws;
  int _padding;
};
#line 63
                                              ShadowPassData ShadowPassData_ctor_() {ShadowPassData r;r.far_plane=float4(0);r.light_direction_ws=packed_float3(0);r._padding=0;return r;}
#line 68
#line 1 "workbench_common.bsl.hh"
 
#line 6
#pragma create_info
#line 13
#define EPSILON 0.00001f

#define CAVITY_BUFFER_RANGE 4.0f
#line 19
#define access_workbench_World_world_data() world_data
#ifdef CREATE_INFO_RES_PASS_workbench_World
CREATE_INFO_RES_PASS_workbench_World
#endif
#ifdef CREATE_INFO_RES_BATCH_workbench_World
CREATE_INFO_RES_BATCH_workbench_World
#endif
#ifdef CREATE_INFO_RES_GEOMETRY_workbench_World
CREATE_INFO_RES_GEOMETRY_workbench_World
#endif
#ifdef CREATE_INFO_RES_SHARED_VARS_workbench_World
CREATE_INFO_RES_SHARED_VARS_workbench_World
#endif
#line 19
struct workbench_World {
#line 29
int _pad;};
#line 34
#ifndef GPU_METAL
workbench_World workbench_World_ctor_();
workbench_World workbench_World_new_();
#endif
#line 19
                               workbench_World workbench_World_ctor_() {workbench_World r;r._pad=0;return r;}
#line 22
       workbench_World workbench_World_new_()
{
  workbench_World result;
  result._pad = 0;
  return result;
#line 20
}
#line 25
float3 workbench_normal_decode(float4 enc)
{
  float2 fenc = enc.xy * 4.0f - 2.0f;
  float f = dot(fenc, fenc);
  float g = sqrt(1.0f - f / 4.0f);
  float3 n;
  n.xy = fenc * g;
  n.z = 1 - f / 2;
  return n;
}
#line 38
float2 workbench_normal_encode(bool front_face, float3 n)
{
  n = normalize(front_face ? n : -n);
  float p = sqrt(n.z * 8.0f + 8.0f);
  n.xy = clamp(n.xy / p + 0.5f, 0.0f, 1.0f);
  return n.xy;
}
#line 47
#define TARGET_BITCOUNT 8u
#define METALLIC_BITS 3u
#define ROUGHNESS_BITS (TARGET_BITCOUNT - METALLIC_BITS)
#line 52
float workbench_float_pair_encode(float v1, float v2)
{
#line 57
  constexpr int v1_mask = 0x1F;
  constexpr int v2_mask = 0x7;
  int iv1 = int(v1 * float(v1_mask));
  int iv2 = int(v2 * float(v2_mask)) << int(ROUGHNESS_BITS);
  return float(iv1 | iv2);
}

void workbench_float_pair_decode(float data, _ref(float ,v1), _ref(float ,v2))
{
#line 69
  constexpr int v1_mask = 0x1F;
  constexpr int v2_mask = 0x7;
  int idata = int(data);
  v1 = float(idata & v1_mask) * (1.0f / float(v1_mask));
  v2 = float(idata >> int(ROUGHNESS_BITS)) * (1.0f / float(v2_mask));
}


#line 1 "workbench_cavity.bsl.hh"
 
#line 6
#pragma create_info
#line 12
SHADER_LIBRARY_CREATE_INFO(draw_view)
#line 19
#define access_workbench_Cavity_jitter_tx() jitter_tx
#define access_workbench_Cavity_cavity_samples() cavity_samples
#ifdef CREATE_INFO_RES_PASS_workbench_Cavity
CREATE_INFO_RES_PASS_workbench_Cavity
#endif
#ifdef CREATE_INFO_RES_BATCH_workbench_Cavity
CREATE_INFO_RES_BATCH_workbench_Cavity
#endif
#ifdef CREATE_INFO_RES_GEOMETRY_workbench_Cavity
CREATE_INFO_RES_GEOMETRY_workbench_Cavity
#endif
#ifdef CREATE_INFO_RES_SHARED_VARS_workbench_Cavity
CREATE_INFO_RES_SHARED_VARS_workbench_Cavity
#endif
#line 19
struct workbench_Cavity {
#line 30
int _pad;};
#line 33
#ifndef GPU_METAL
workbench_Cavity workbench_Cavity_ctor_();
workbench_Cavity workbench_Cavity_new_();
#endif
#line 19
                                workbench_Cavity workbench_Cavity_ctor_() {workbench_Cavity r;r._pad=0;return r;}
#line 23
       workbench_Cavity workbench_Cavity_new_()
{
  workbench_Cavity result;
  result._pad = 0;
  return result;
#line 21
}
#line 24

#if defined(CREATE_INFO_workbench_Cavity) && defined(CREATE_INFO_workbench_World)
#line 24
void workbench_cavity_compute(const workbench_Cavity  cavity,
                    const workbench_World  world,
                    sampler2DDepth depth_tx,
                    sampler2D normal_tx,
                    float2 screenco,
                    _ref(float ,cavities),
                    _ref(float ,edges))
{
  cavities = edges = 0.0f;

  float depth = texture(depth_tx, screenco).x;
#line 37
  if (depth == 1.0f || depth == 0.0f) {
    return;
  }
#line 43
  float3 position = drw_point_screen_to_view(float3(screenco, depth));
  float3 normal = workbench_normal_decode(texture(normal_tx, screenco));

  float2 jitter_co = (screenco * srt_access(workbench_World, world_data).viewport_size.xy) * srt_access(workbench_World, world_data).cavity_jitter_scale;
  float3 noise = texture(srt_access(workbench_Cavity, jitter_tx), jitter_co).rgb;
#line 51
  float2 offset;
  float homcoord = drw_view().winmat[2][3] * position.z + drw_view().winmat[3][3];
  offset.x = drw_view().winmat[0][0] * srt_access(workbench_World, world_data).cavity_distance / homcoord;
  offset.y = drw_view().winmat[1][1] * srt_access(workbench_World, world_data).cavity_distance / homcoord;

  offset *= 0.5f;
#line 59
  float2 rotX = noise.rg;
  float2 rotY = float2(-rotX.y, rotX.x);

  int sample_start = srt_access(workbench_World, world_data).cavity_sample_start;
  int sample_end = srt_access(workbench_World, world_data).cavity_sample_end;
  for (int i = sample_start; i < sample_end && i < 512; i++) {
#line 67
    float3 sample_coord = srt_access(workbench_Cavity, cavity_samples)[i].xyz;

    float2 dir_jittered = float2(dot(sample_coord.xy, rotX), dot(sample_coord.xy, rotY));
    dir_jittered.xy *= sample_coord.z + noise.b;

    float2 uvcoords = screenco + dir_jittered * offset;

    if (any(greaterThan(abs(uvcoords - 0.5f), float2(0.5f)))) {
      continue;
    }

    float s_depth = texture(depth_tx, uvcoords).r;

    bool is_background = (s_depth == 1.0f);

    s_depth = (is_background) ? depth : s_depth;
    float3 s_pos = drw_point_screen_to_view(float3(uvcoords, s_depth));

    if (is_background) {
      s_pos.z -= srt_access(workbench_World, world_data).cavity_distance;
    }

    float3 dir = s_pos - position;
    float len = length(dir);
    float f_cavities = dot(dir, normal);
    float f_edge = -f_cavities;
    float f_bias = 0.05f * len + 0.0001f;

    float attenuation = 1.0f / (len * (1.0f + len * len * srt_access(workbench_World, world_data).cavity_attenuation));
#line 98
    if (f_cavities > -f_bias) {
      cavities += f_cavities * attenuation;
    }

    if (f_edge > f_bias) {
      edges += f_edge * attenuation;
    }
  }
  cavities *= srt_access(workbench_World, world_data).cavity_sample_count_inv;
  edges *= srt_access(workbench_World, world_data).cavity_sample_count_inv;
#line 110
  cavities = clamp(cavities * srt_access(workbench_World, world_data).cavity_valley_factor, 0.0f, 1.0f);
  edges = edges * srt_access(workbench_World, world_data).cavity_ridge_factor;
}
#endif
#line 115
#line 1 "workbench_curvature.bsl.hh"
 
#line 11
float workbench_curvature_soft_clamp(float curvature, float control)
{
  if (curvature < 0.5f / control) {
    return curvature * (1.0f - curvature * control);
  }
  return 0.25f / control;
}
#line 20
#if defined(CREATE_INFO_workbench_World)
#line 19
void workbench_curvature_compute(const workbench_World  world,
                       usampler2D object_id_tx,
                       sampler2D normal_tx,
                       float2 uv,
                       _ref(float ,curvature))
{
  curvature = 0.0f;
#line 29
  float3 offset = float3(srt_access(workbench_World, world_data).viewport_size_inv, 0.0f) * srt_access(workbench_World, world_data).ui_scale;
  uint object_up = texture(object_id_tx, uv + offset.zy).r;
  uint object_down = texture(object_id_tx, uv - offset.zy).r;
  uint object_right = texture(object_id_tx, uv + offset.xz).r;
  uint object_left = texture(object_id_tx, uv - offset.xz).r;
#line 36
  if ((object_up != object_down) || (object_right != object_left)) {
    return;
  }

  if ((object_up == object_right) && (object_right == 0u)) {
    return;
  }

  float normal_up = workbench_normal_decode(texture(normal_tx, uv + offset.zy)).g;
  float normal_down = workbench_normal_decode(texture(normal_tx, uv - offset.zy)).g;
  float normal_right = workbench_normal_decode(texture(normal_tx, uv + offset.xz)).r;
  float normal_left = workbench_normal_decode(texture(normal_tx, uv - offset.xz)).r;

  float normal_diff = (normal_up - normal_down) + (normal_right - normal_left);

  if (normal_diff < 0) {
    curvature = -2.0f * workbench_curvature_soft_clamp(-normal_diff, srt_access(workbench_World, world_data).curvature_valley);
  }
  else {
    curvature = 2.0f * workbench_curvature_soft_clamp(normal_diff, srt_access(workbench_World, world_data).curvature_ridge);
  }
}
#endif
#line 60
#line 1 "workbench_matcap.bsl.hh"
 
#line 13
float2 workbench_matcap_uv_compute(float3 I, float3 N, bool flipped)
{

  float a = 1.0f / (1.0f + I.z);
  float b = -I.x * I.y * a;
  float3 b1 = float3(1.0f - I.x * I.x * a, b, -I.x);
  float3 b2 = float3(b, 1.0f - I.y * I.y * a, -I.y);
  float2 matcap_uv = float2(dot(b1, N), dot(b2, N));
  if (flipped) {
    matcap_uv.x = -matcap_uv.x;
  }
  return matcap_uv * 0.496f + 0.5f;
}
#line 28
#if defined(CREATE_INFO_workbench_World)
#line 27
float3 workbench_get_matcap_lighting(const workbench_World  world,
                           sampler2D diffuse_matcap,
                           sampler2D specular_matcap,
                           float3 base_color,
                           float3 N,
                           float3 I)
{
#line 36
  bool flipped = srt_access(workbench_World, world_data).matcap_orientation != 0;
  float2 uv = workbench_matcap_uv_compute(I, N, flipped);

  float3 diffuse = textureLod(diffuse_matcap, uv, 0.0f).rgb;
  float3 specular = textureLod(specular_matcap, uv, 0.0f).rgb;

  return diffuse * base_color + specular * float(srt_access(workbench_World, world_data).use_specular);
}
#endif

#if defined(CREATE_INFO_workbench_World)
#line 45
float3 workbench_get_matcap_lighting(const workbench_World  world,
                           sampler2DArray matcap,
                           float3 base_color,
                           float3 N,
                           float3 I)
{
#line 53
  bool flipped = srt_access(workbench_World, world_data).matcap_orientation != 0;
  float2 uv = workbench_matcap_uv_compute(I, N, flipped);

  float3 diffuse = textureLod(matcap, float3(uv, 0.0f), 0.0f).rgb;
  float3 specular = textureLod(matcap, float3(uv, 1.0f), 0.0f).rgb;

  return diffuse * base_color + specular * float(srt_access(workbench_World, world_data).use_specular);
}
#endif
#line 63
#line 1 "gpu_shader_utildefines_lib.glsl"
 
#line 9
#ifndef FLT_MAX
#  define FLT_MAX uintBitsToFloat(0x7F7FFFFFu)
#  define FLT_MIN uintBitsToFloat(0x00800000u)
#  define FLT_EPSILON 1.192092896e-07F
#endif
#ifndef SHRT_MAX
#  define SHRT_MAX 0x00007FFF
#  define INT_MAX 0x7FFFFFFF
#  define USHRT_MAX 0x0000FFFFu
#  define UINT_MAX 0xFFFFFFFFu
#endif
#define NAN_FLT uintBitsToFloat(0x7FC00000u)
#define FLT_11_MAX uintBitsToFloat(0x477E0000)
#define FLT_10_MAX uintBitsToFloat(0x477C0000)
#define FLT_11_11_10_MAX float3(FLT_11_MAX, FLT_11_MAX, FLT_10_MAX)

#define UNPACK2(a) (a)[0], (a)[1]
#define UNPACK3(a) (a)[0], (a)[1], (a)[2]
#define UNPACK4(a) (a)[0], (a)[1], (a)[2], (a)[3]
#line 32
#define saturate(a) clamp(a, 0.0f, 1.0f)

#define isfinite(a) (!isinf(a) && !isnan(a))
#line 37
#define in_range_inclusive(val, min_v, max_v) (all(greaterThanEqual(val, min_v)) && all(lessThanEqual(val, max_v)))
#define in_range_exclusive(val, min_v, max_v) (all(greaterThan(val, min_v)) && all(lessThan(val, max_v)))
#define in_texture_range(texel, tex) (all(greaterThanEqual(texel, int2(0))) && all(lessThan(texel, textureSize(tex, 0).xy)))
#define in_image_range(texel, tex) (all(greaterThanEqual(texel, int2(0))) && all(lessThan(texel, imageSize(tex).xy)))

#define weighted_sum(val0, val1, val2, val3, weights) ((val0 * weights[0] + val1 * weights[1] + val2 * weights[2] + val3 * weights[3]) * safe_rcp(weights[0] + weights[1] + weights[2] + weights[3]))
#define weighted_sum_array(val, weights) ((val[0] * weights[0] + val[1] * weights[1] + val[2] * weights[2] + val[3] * weights[3]) * safe_rcp(weights[0] + weights[1] + weights[2] + weights[3]))
#line 46
bool flag_test(uint flag, uint val)
{
  return (flag & val) != 0u;
}
bool flag_test(int flag, uint val)
{
  return flag_test(uint(flag), val);
}
bool flag_test(int flag, int val)
{
  return (flag & val) != 0;
}

void set_flag_from_test(_ref(uint ,value), bool test, uint flag)
{
  if (test) {
    value |= flag;
  }
  else {
    value &= ~flag;
  }
}
void set_flag_from_test(_ref(int ,value), bool test, int flag)
{
  if (test) {
    value |= flag;
  }
  else {
    value &= ~flag;
  }
}
#line 79
#define SET_FLAG_FROM_TEST(value, test, flag) set_flag_from_test(value, test, flag)
#line 85
bool bitmask64_test(uint2 bitmask, uint bit_index)
{
  uint bitmask32 = (bit_index >= 32u) ? bitmask.y : bitmask.x;
  return flag_test(bitmask32, 1u << (bit_index & 0x1Fu));
}
#line 94
uint packUvec2x16(uint2 a)
{
  a = (a & 0xFFFFu) << uint2(0u, 16u);
  return a.x | a.y;
}
uint2 unpackUvec2x16(uint a)
{
  return (uint2(a) >> uint2(0u, 16u)) & uint2(0xFFFFu);
}
#line 107
uint packUvec4x8(uint4 a)
{
  a = (a & 0xFFu) << uint4(0u, 8u, 16u, 24u);
  return a.x | a.y | a.z | a.w;
}
uint4 unpackUvec4x8(uint a)
{
  return (uint4(a) >> uint4(0u, 8u, 16u, 24u)) & uint4(0xFFu);
}
#line 121
int floatBitsToOrderedInt(float value)
{
#line 128
  int int_value = floatBitsToInt(value);
  return (int_value < 0) ? (int_value ^ 0x7FFFFFFF) : int_value;
}
float orderedIntBitsToFloat(int int_value)
{
  return intBitsToFloat((int_value < 0) ? (int_value ^ 0x7FFFFFFF) : int_value);
}
#line 1 "workbench_world_light.bsl.hh"
 
#line 16
float4 workbench_fast_rcp(float4 v)
{
  return intBitsToFloat(0x7eef370b - floatBitsToInt(v));
}

float3 workbench_brdf_approx(float3 spec_color, float roughness, float NV)
{
#line 25
  float fresnel = exp2(-8.35f * NV) * (1.0f - roughness);
  return mix(spec_color, float3(1.0f), fresnel);
}
#line 30
float4 workbench_blinn_specular(float4 shininess, float4 spec_angle, float4 NL)
{
#line 34
  float4 normalization_factor = shininess * 0.125f + 1.0f;
  float4 spec_light = pow(spec_angle, shininess) * NL * normalization_factor;

  return spec_light;
}
#line 41
float4 workbench_wrapped_lighting(float4 NL, float4 w)
{
  float4 w_1 = w + 1.0f;
  float4 denom = workbench_fast_rcp(w_1 * w_1);
  return clamp((NL + w) * denom, 0.0f, 1.0f);
}
#line 49
#if defined(CREATE_INFO_workbench_World)
#line 48
float3 workbench_get_world_lighting(const workbench_World  world,
                          float3 base_color,
                          float roughness,
                          float metallic,
                          float3 N,
                          float3 I)
{
#line 57
  float3 specular_color, diffuse_color;

  if (srt_access(workbench_World, world_data).use_specular) {
    diffuse_color = mix(base_color, float3(0.0f), metallic);
    specular_color = mix(float3(0.05f), base_color, metallic);
  }
  else {
    diffuse_color = base_color;
    specular_color = float3(0.0f);
  }

  float3 specular_light = srt_access(workbench_World, world_data).ambient_color.rgb;
  float3 diffuse_light = srt_access(workbench_World, world_data).ambient_color.rgb;
  float4 wrap = float4(srt_access(workbench_World, world_data).lights[0].diffuse_color_wrap.a,
                       srt_access(workbench_World, world_data).lights[1].diffuse_color_wrap.a,
                       srt_access(workbench_World, world_data).lights[2].diffuse_color_wrap.a,
                       srt_access(workbench_World, world_data).lights[3].diffuse_color_wrap.a);

  if (srt_access(workbench_World, world_data).use_specular) {

    float3 R = -reflect(I, N);

    float4 spec_angle, spec_NL, wrapped_NL;
#line 83
{
#line 80
                                           {
      float3 L = srt_access(workbench_World, world_data).lights[0].direction.xyz;
      float3 half_dir = normalize(L + I);
      wrapped_NL[0] = dot(L, R);
      spec_angle[0] = saturate(dot(half_dir, N));
      spec_NL[0] = saturate(dot(L, N));
    }
#line 80
                                           {
      float3 L = srt_access(workbench_World, world_data).lights[1].direction.xyz;
      float3 half_dir = normalize(L + I);
      wrapped_NL[1] = dot(L, R);
      spec_angle[1] = saturate(dot(half_dir, N));
      spec_NL[1] = saturate(dot(L, N));
    }
#line 80
                                           {
      float3 L = srt_access(workbench_World, world_data).lights[2].direction.xyz;
      float3 half_dir = normalize(L + I);
      wrapped_NL[2] = dot(L, R);
      spec_angle[2] = saturate(dot(half_dir, N));
      spec_NL[2] = saturate(dot(L, N));
    }
#line 80
                                           {
      float3 L = srt_access(workbench_World, world_data).lights[3].direction.xyz;
      float3 half_dir = normalize(L + I);
      wrapped_NL[3] = dot(L, R);
      spec_angle[3] = saturate(dot(half_dir, N));
      spec_NL[3] = saturate(dot(L, N));
    }
#line 86
    }

    float4 gloss = float4(1.0f - roughness);

    gloss *= 1.0f - wrap;
    float4 shininess = exp2(10.0f * gloss + 1.0f);

    float4 spec_light = workbench_blinn_specular(shininess, spec_angle, spec_NL);
#line 96
    float4 w = mix(wrap, float4(1.0f), roughness);
    float4 spec_env = workbench_wrapped_lighting(wrapped_NL, w);

    spec_light = mix(spec_light, spec_env, wrap * wrap);
#line 105
{
#line 102
                                           {
      specular_light += spec_light[0] * srt_access(workbench_World, world_data).lights[0].specular_color.rgb;
    }
#line 102
                                           {
      specular_light += spec_light[1] * srt_access(workbench_World, world_data).lights[1].specular_color.rgb;
    }
#line 102
                                           {
      specular_light += spec_light[2] * srt_access(workbench_World, world_data).lights[2].specular_color.rgb;
    }
#line 102
                                           {
      specular_light += spec_light[3] * srt_access(workbench_World, world_data).lights[3].specular_color.rgb;
    }
#line 104
    }

    float NV = saturate(dot(N, I));
    specular_color = workbench_brdf_approx(specular_color, roughness, NV);
  }
  specular_light *= specular_color;
#line 112
  float4 diff_NL;
#line 116
{
#line 113
                                         {
    diff_NL[0] = dot(srt_access(workbench_World, world_data).lights[0].direction.xyz, N);
  }
#line 113
                                         {
    diff_NL[1] = dot(srt_access(workbench_World, world_data).lights[1].direction.xyz, N);
  }
#line 113
                                         {
    diff_NL[2] = dot(srt_access(workbench_World, world_data).lights[2].direction.xyz, N);
  }
#line 113
                                         {
    diff_NL[3] = dot(srt_access(workbench_World, world_data).lights[3].direction.xyz, N);
  }
#line 115
  }

  float4 diff_light = workbench_wrapped_lighting(diff_NL, wrap);
#line 125
{
#line 120
                                         {
    diffuse_light += diff_light[0] * srt_access(workbench_World, world_data).lights[0].diffuse_color_wrap.rgb;
  }
#line 120
                                         {
    diffuse_light += diff_light[1] * srt_access(workbench_World, world_data).lights[1].diffuse_color_wrap.rgb;
  }
#line 120
                                         {
    diffuse_light += diff_light[2] * srt_access(workbench_World, world_data).lights[2].diffuse_color_wrap.rgb;
  }
#line 120
                                         {
    diffuse_light += diff_light[3] * srt_access(workbench_World, world_data).lights[3].diffuse_color_wrap.rgb;
  }
#line 122
  }
#line 126
  float spec_energy = dot(specular_color, float3(0.33333f));

  diffuse_light *= diffuse_color * (1.0f - spec_energy);

  return diffuse_light + specular_light;
}
#endif

#if defined(CREATE_INFO_workbench_World)
#line 133
float workbench_get_shadow(const workbench_World  world, float3 N, bool force_shadow)
{
#line 137
  float light_factor = -dot(N, srt_access(workbench_World, world_data).shadow_direction_vs.xyz);
  float shadow_mix = smoothstep(srt_access(workbench_World, world_data).shadow_shift, srt_access(workbench_World, world_data).shadow_focus, light_factor);
  shadow_mix *= force_shadow ? 0.0f : srt_access(workbench_World, world_data).shadow_mul;
  return shadow_mix + srt_access(workbench_World, world_data).shadow_add;
}
#endif
#line 144
#line 1 "workbench_composite.bsl.hh"
 
#line 10
#pragma create_info
#line 22
#define WORKBENCH_LIGHTING_STUDIO 0
#define WORKBENCH_LIGHTING_MATCAP 1
#define WORKBENCH_LIGHTING_FLAT 2
#line 28
#define access_workbench_resolve_Resources_lighting_mode() lighting_mode
#define access_workbench_resolve_Resources_use_cavity() use_cavity
#define access_workbench_resolve_Resources_use_curvature() use_curvature
#define access_workbench_resolve_Resources_use_shadow() use_shadow
#define access_workbench_resolve_Resources_draw_view() draw_view
#define access_workbench_resolve_Resources_depth_tx() depth_tx
#define access_workbench_resolve_Resources_normal_tx() normal_tx
#define access_workbench_resolve_Resources_material_tx() material_tx
#define access_workbench_resolve_Resources_object_id_tx() object_id_tx
#define access_workbench_resolve_Resources_stencil_tx() stencil_tx
#define access_workbench_resolve_Resources_matcap_tx() matcap_tx
#define access_workbench_resolve_Resources_world() workbench_World_new_()
#define access_workbench_resolve_Resources_cavity() workbench_Cavity_new_()
#ifdef CREATE_INFO_RES_PASS_workbench_resolve_Resources
CREATE_INFO_RES_PASS_workbench_resolve_Resources
#endif
#ifdef CREATE_INFO_RES_BATCH_workbench_resolve_Resources
CREATE_INFO_RES_BATCH_workbench_resolve_Resources
#endif
#ifdef CREATE_INFO_RES_GEOMETRY_workbench_resolve_Resources
CREATE_INFO_RES_GEOMETRY_workbench_resolve_Resources
#endif
#ifdef CREATE_INFO_RES_SHARED_VARS_workbench_resolve_Resources
CREATE_INFO_RES_SHARED_VARS_workbench_resolve_Resources
#endif
#line 28
struct workbench_resolve_Resources {
#line 46
                           workbench_World  world;

                                                  workbench_Cavity  cavity;
#line 58
};
#line 61
#ifndef GPU_METAL
workbench_resolve_Resources workbench_resolve_Resources_ctor_();
workbench_resolve_Resources workbench_resolve_Resources_new_();
#endif
#line 28
                                           workbench_resolve_Resources workbench_resolve_Resources_ctor_() {workbench_resolve_Resources r;r.world=workbench_World_ctor_();r.cavity=workbench_Cavity_ctor_();return r;}
#line 50
       workbench_resolve_Resources workbench_resolve_Resources_new_()
{
  workbench_resolve_Resources result;
  result.world = workbench_World_new_();
  result.cavity = workbench_Cavity_new_();
  return result;
#line 48
}
#line 51

#if defined(ENTRY_POINT_workbench_resolve_vert)
#line 51
           void workbench_resolve_vert(                                                              )
{

#if defined(GPU_VERTEX_SHADER)
#line 53
  fullscreen_vertex(gl_VertexID, gl_Position);

#endif
#line 54
}
#endif
struct workbench_resolve_FragOut {
                    float4 color;
};
#line 56
                                         workbench_resolve_FragOut workbench_resolve_FragOut_ctor_() {workbench_resolve_FragOut r;r.color=float4(0);return r;}
#line 60

#if defined(CREATE_INFO_workbench_resolve_Resources)
#line 60

#if defined(ENTRY_POINT_workbench_resolve_frag)
#line 60
             void workbench_resolve_frag(

                                                                  )
{
#if defined(GPU_FRAGMENT_SHADER)
#line 63
  workbench_resolve_Resources srt = workbench_resolve_Resources_ctor_();
  float2 uv = gl_FragCoord.xy / float2(textureSize(srt_access(workbench_resolve_Resources, depth_tx), 0).xy);

  float depth = texture(srt_access(workbench_resolve_Resources, depth_tx), uv).r;
  if (depth == 1.0f) {

    gpu_discard_fragment();
    return;
  }
#line 74
  float3 P = drw_point_screen_to_view(float3(uv, 0.5f));
  float3 V = drw_view_incident_vector(P);
  float3 N = workbench_normal_decode(texture(srt_access(workbench_resolve_Resources, normal_tx), uv));
  float4 mat_data = texture(srt_access(workbench_resolve_Resources, material_tx), uv);

  float3 base_color = mat_data.rgb;
  float4 color = float4(1.0f);
#line 83
#if SRT_CONSTANT_lighting_mode == WORKBENCH_LIGHTING_MATCAP
#line 82
                                                                                                             {

    N = (mat_data.a > 0.0f) ? N : -N;
    color.rgb = workbench_get_matcap_lighting(srt_access(workbench_resolve_Resources, world), srt_access(workbench_resolve_Resources, matcap_tx), base_color, N, V);
  }

#elif SRT_CONSTANT_lighting_mode == WORKBENCH_LIGHTING_STUDIO
#line 87
                                                                                                                  {
    float roughness = 0.0f, metallic = 0.0f;
    workbench_float_pair_decode(mat_data.a, roughness, metallic);
    color.rgb = workbench_get_world_lighting(srt_access(workbench_resolve_Resources, world), base_color, roughness, metallic, N, V);
  }

#elif SRT_CONSTANT_lighting_mode == WORKBENCH_LIGHTING_FLAT
#line 92
                                                                                                                {
    color.rgb = base_color;
  }
#endif
  float cavity = 0.0f, edges = 0.0f, curvature = 0.0f;

#if SRT_CONSTANT_use_cavity
#line 97
                                                                             {
    workbench_cavity_compute(
        srt_access(workbench_resolve_Resources, cavity), srt_access(workbench_resolve_Resources, world), srt_access(workbench_resolve_Resources, depth_tx), srt_access(workbench_resolve_Resources, normal_tx), uv, cavity, edges);
  }
#endif

#if SRT_CONSTANT_use_curvature
#line 102
                                                                                {
    workbench_curvature_compute(srt_access(workbench_resolve_Resources, world), srt_access(workbench_resolve_Resources, object_id_tx), srt_access(workbench_resolve_Resources, normal_tx), uv, curvature);
  }

#endif
#line 105
  color.rgb *= clamp((1.0f - cavity) * (1.0f + edges) * (1.0f + curvature), 0.0f, 4.0f);
#line 108
#if SRT_CONSTANT_use_shadow
#line 107
                                                                             {
    bool shadow = texture(srt_access(workbench_resolve_Resources, stencil_tx), uv).r != 0;
    color.rgb *= workbench_get_shadow(srt_access(workbench_resolve_Resources, world), N, shadow);
  }
#endif
  workbench_resolve_FragOut_color = color;

#endif
#line 113
}
#endif
#endif
#line 143
void main() { workbench_resolve_vert(); }
