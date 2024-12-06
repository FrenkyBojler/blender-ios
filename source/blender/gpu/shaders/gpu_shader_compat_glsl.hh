/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#define SRD_RESOURCE_STRUCT_DECL(typename) \
  struct typename \
  { \
    uint _pad; \
  };

#define HANDLE(srd, name) srd##_##name##_t
#define RESOURCE(srd, name) srd##_##name##_r

#ifdef GPU_GLSL
// #define SRD_RESOURCE_PUSH_CONSTANT(srd, type, name) type name;
// #define SRD_RESOURCE_SAMPLER(srd, type, name) HANDLE(srd, name) name;
#endif

#define sampler2D_DataVec_t float4
#define sampler2D_SizeVec_t int2
#define sampler2D_FltCoord_t float2

#define SRD_SAMPLER_DECLARE(srd, _binding, type, name) \
  layout(binding = _binding) uniform type RESOURCE(srd, name); \
\
  SRD_RESOURCE_STRUCT_DECL(HANDLE(srd, name)) \
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

#define SRD_SAMPLER_DECLARE_DUMMY(srd, binding, type, name) \
  SRD_RESOURCE_STRUCT_DECL(HANDLE(srd, name)) \
\
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
