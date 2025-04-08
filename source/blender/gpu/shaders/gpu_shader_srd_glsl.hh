/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#define SRD_GLUE(a, b) a##b
#define SRD_CAT(a, b) SRD_GLUE(a, b)

#define SRD_IMPL(srd) SRD_IMPL_##srd

#define SRD_HANDLE_DECL(typename) \
  struct typename \
  { \
    uint _pad; \
  };

#define HANDLE(srd, name) srd##_##name##_t
#define RESOURCE(srd, name) srd##_##name##_r
#define SRD_BUFFER(srd, name) srd##_##name##_b

#define SRD_ACCESS_READ readonly
#define SRD_ACCESS_WRITE writeonly
#define SRD_ACCESS_READ_WRITE
#define SRD_ACCESS(srd) SRD_ACCESS_##srd

/* For testing. Should use `glsl_shader_defines.glsl` instead. */
#define bool32_t bool
#define bool2 bvec2
#define bool3 bvec3
#define bool4 bvec4
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define float4x4 mat4
#define float3x3 mat3
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4
#define uint2 uvec2
#define uint3 uvec3
#define uint4 uvec4

#define sampler2D_DataVec_t float4
#define sampler2D_SizeVec_t int2
#define sampler2D_IntCoord_t int2
#define sampler2D_FltCoord_t float2

#define sampler2DArray_DataVec_t float4
#define sampler2DArray_SizeVec_t int3
#define sampler2DArray_IntCoord_t int3
#define sampler2DArray_FltCoord_t float3

#define sampler1DArray_DataVec_t float4
#define sampler1DArray_SizeVec_t int2
#define sampler1DArray_IntCoord_t int2
#define sampler1DArray_FltCoord_t float2

/* -------------------------------------------------------------------- */
/** \name Sampler
 * \{ */

#define SRD_SAMPLER_DECLARE(srd, _binding, type, name) \
  layout(binding = _binding) uniform type RESOURCE(srd, name); \
\
  type##_SizeVec_t textureSize(HANDLE(srd, name) img, int lod) \
  { \
    return textureSize(RESOURCE(srd, name), lod); \
  } \
  type##_DataVec_t texelFetch(HANDLE(srd, name) img, type##_IntCoord_t uv, int lod) \
  { \
    return texelFetch(RESOURCE(srd, name), uv, lod); \
  } \
  type##_DataVec_t texture(HANDLE(srd, name) img, type##_FltCoord_t uv) \
  { \
    return texture(RESOURCE(srd, name), uv); \
  } \
  type##_DataVec_t texture(HANDLE(srd, name) img, type##_FltCoord_t uv, float bias) \
  { \
    return texture(RESOURCE(srd, name), uv, bias); \
  }

#define SRD_SAMPLER_DUMMY(srd, binding, type, name) \
  type##_SizeVec_t textureSize(HANDLE(srd, name) img, int lod) \
  { \
    return type##_SizeVec_t(0); \
  } \
  type##_DataVec_t texelFetch(HANDLE(srd, name) img, type##_IntCoord_t uv, int lod) \
  { \
    return type##_DataVec_t(0); \
  } \
  type##_DataVec_t texture(HANDLE(srd, name) img, type##_FltCoord_t uv) \
  { \
    return type##_DataVec_t(0); \
  } \
  type##_DataVec_t texture(HANDLE(srd, name) img, type##_FltCoord_t uv, float bias) \
  { \
    return type##_DataVec_t(0); \
  }

/** \} */

/* -------------------------------------------------------------------- */
/** \name Uniform Buffer
 * \{ */

#define SRD_UNIFORM_BUF_DECLARE(srd, _binding, type, name, array) \
  layout(binding = _binding, std140) uniform SRD_BUFFER(srd, name) \
  { \
    type data array; \
  } \
  RESOURCE(srd, name); \
\
  type buffer_read(HANDLE(srd, name) buf) \
  { \
    return RESOURCE(srd, name).data[0]; \
  } \
  type buffer_read(HANDLE(srd, name) buf, uint offset) \
  { \
    return RESOURCE(srd, name).data[offset]; \
  }

#define SRD_UNIFORM_BUF_DUMMY(srd, binding, type, name, array) \
  type buffer_read(HANDLE(srd, name) buf) \
  { \
    type data; \
    return data; \
  } \
  type buffer_read(HANDLE(srd, name) buf, uint offset) \
  { \
    return buffer_read(buf); \
  }

/** \} */

/* -------------------------------------------------------------------- */
/** \name Stoage Buffer
 * \{ */

#define SRD_STORAGE_BUF_DECLARE(srd, _binding, access, type, name, array) \
  layout(binding = _binding, std430) SRD_ACCESS(access) buffer SRD_BUFFER(srd, name) \
  { \
    type data array; \
  } \
  RESOURCE(srd, name); \
\
  type buffer_read(HANDLE(srd, name) buf) \
  { \
    return RESOURCE(srd, name).data[0]; \
  } \
  type buffer_read(HANDLE(srd, name) buf, uint offset) \
  { \
    return RESOURCE(srd, name).data[offset]; \
  } \
  void buffer_write(HANDLE(srd, name) buf, type data) \
  { \
    RESOURCE(srd, name).data[0] = data; \
  } \
  void buffer_write(HANDLE(srd, name) buf, uint offset, type data) \
  { \
    RESOURCE(srd, name).data[offset] = data; \
  }

#define SRD_STORAGE_BUF_DUMMY(srd, binding, access, type, name, array) \
  type buffer_read(HANDLE(srd, name) buf) \
  { \
    type data; \
    return data; \
  } \
  type buffer_read(HANDLE(srd, name) buf, uint offset) \
  { \
    return buffer_read(buf); \
  } \
  void buffer_write(HANDLE(srd, name) buf, type data) {} \
  void buffer_write(HANDLE(srd, name) buf, uint offset, type data) {}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Images
 * \{ */

/** \} */

#define SRD_STRUCT_BEGIN(srd) struct srd {
#define SRD_STRUCT_END(srd) \
  } \
  ;

#define SRD_VERTEX_IN_BEGIN(srd) SRD_STRUCT_BEGIN(srd)
#define SRD_VERTEX_IN_END(srd) SRD_STRUCT_END(srd)
#define SRD_VERTEX_IN(srd, binding, type, name) type name;

#define SRD_VERTEX_OUT_BEGIN(srd) SRD_STRUCT_BEGIN(srd)
#define SRD_VERTEX_OUT_END(srd) SRD_STRUCT_END(srd)
#define SRD_VERTEX_OUT(srd, qual, type, name) type name;

#define SRD_FRAGMENT_IN_BEGIN(srd) SRD_STRUCT_BEGIN(srd)
#define SRD_FRAGMENT_IN_END(srd) SRD_STRUCT_END(srd)
#define SRD_FRAGMENT_IN(srd, qual, type, name) type name;

#define SRD_FRAGMENT_OUT_BEGIN(srd) SRD_STRUCT_BEGIN(srd)
#define SRD_FRAGMENT_OUT_END(srd) SRD_STRUCT_END(srd)
#define SRD_FRAGMENT_OUT(srd, binding, type, name) type name;

#define SRD_RESOURCE_BEGIN(srd) SRD_CAT(SRD_DECLARE_, srd)() SRD_STRUCT_BEGIN(srd)
#define SRD_RESOURCE_END(srd) SRD_STRUCT_END(srd)

#define SRD_RESOURCE_STRUCT(srd, type, name) type name;
#define SRD_RESOURCE_SPECIALIZATION_CONSTANT(srd, type, name, default) type name;
#define SRD_RESOURCE_PUSH_CONSTANT(srd, type, name) type name;
#define SRD_RESOURCE_SAMPLER(srd, binding, type, name) HANDLE(srd, name) name;
#define SRD_RESOURCE_STORAGE_BUF(srd, binding, access, type, name, array) HANDLE(srd, name) name;
#define SRD_RESOURCE_UNIFORM_BUF(srd, binding, type, name, array) HANDLE(srd, name) name;

#define SRD_DECLARE_SAMPLER(srd, binding, type, name) \
  SRD_HANDLE_DECL(HANDLE(srd, name)) \
  SRD_CAT(SRD_SAMPLER_, SRD_IMPL(srd))(srd, binding, type, name)

#define SRD_DECLARE_STORAGE_BUF(srd, binding, access, type, name, array) \
  SRD_HANDLE_DECL(HANDLE(srd, name)) \
  SRD_CAT(SRD_STORAGE_BUF_, SRD_IMPL(srd))(srd, binding, access, type, name, array)

#define SRD_DECLARE_UNIFORM_BUF(srd, binding, type, name, array) \
  SRD_HANDLE_DECL(HANDLE(srd, name)) \
  SRD_CAT(SRD_UNIFORM_BUF_, SRD_IMPL(srd))(srd, binding, type, name, array)

/* Should only be used in code that ensures the use of this SRD. */
#define sampler_get(srd_class, srd_var, name) RESOURCE(srd_class, name)
#define buffer_get(srd_class, srd_var, name) RESOURCE(srd_class, name).data

#define SRD_GRAPHIC_PIPELINE( \
    file, pipe, vert_in, vert_out, frag_in, frag_out, vert_func, vert_res, frag_func, frag_res)
