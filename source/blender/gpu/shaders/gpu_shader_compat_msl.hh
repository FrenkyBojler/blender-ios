/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#define SRD_VERTEX_IN_BEGIN(srd) struct srd {
#define SRD_VERTEX_IN_END(srd) \
  } \
  ;
#define SRD_VERTEX_IN(srd, binding, type, name) type name [[attribute(binding)]];

#define SRD_INTERP_position [[position]]
#define SRD_INTERP_smooth [[center_perspective]]
#define SRD_INTERP_no_perspective [[center_perspective]]
#define SRD_INTERP_flat [[flat]]
#define SRD_INTERP_front_facing /* [[front_facing]] Doesn't compile. Need specific struct. */

#define SRD_STAGE_INOUT_BEGIN(srd) struct srd {
#define SRD_STAGE_INOUT_END(srd) \
  } \
  ;
#define SRD_STAGE_INOUT(srd, qual, type, name) type name SRD_INTERP_##qual;

#define SRD_FRAGMENT_OUT_BEGIN(srd) struct srd {
#define SRD_FRAGMENT_OUT_END(srd) \
  } \
  ;
#define SRD_FRAGMENT_OUT(srd, binding, type, name) type name [[color(binding)]];

#define SRD_RESOURCE_BEGIN(srd) struct srd {
#define SRD_RESOURCE_END(srd) \
  } \
  ;

#define SRD_READ constant
#define SRD_WRITE device
#define SRD_READ_WRITE device

#define SRD_RESOURCE_SPECIALIZATION_CONSTANT(srd, type, name, default) constant type(&name);
#define SRD_RESOURCE_PUSH_CONSTANT(srd, type, name) constant type(&name);
#define SRD_RESOURCE_SAMPLER(srd, binding, type, name) constant type(&name);
#define SRD_RESOURCE_STORAGE_BUF(srd, binding, access, type, name, array) \
  SRD_##access type(&name) array;
#define SRD_RESOURCE_UNIFORM_BUF(srd, binding, type, name, array) constant type(&name) array;
#define SRD_RESOURCE_STRUCT(srd, type, name) type name;

#define buffer_read(buffer, offset) (buffer)[offset]

#define SRD_GRAPHIC_PIPELINE( \
    file, pipe, vert_in, stage_inout, frag_out, vert_func, vert_res, frag_func, frag_res) \
  [[vertex]] stage_inout vert_func##_entry_point(vert_in in [[stage_in]], \
                                                 constant vert_res &srd [[buffer(0)]]) \
  { \
    return vert_func(in, srd); \
  } \
  [[fragment]] frag_out frag_func##_entry_point(stage_inout in [[stage_in]], \
                                                constant frag_res &srd [[buffer(1)]]) \
  { \
    return frag_func(in, srd); \
  }

/* Stub for testing. */
struct sampler2D {
  int handle;
};
struct sampler2DArray {
  int handle;
};
struct sampler1DArray {
  int handle;
};
float4 texture(constant sampler2D &a, float2 b)
{
  return float4(0.0);
}
float4 texture(constant sampler2DArray &a, float3 b)
{
  return float4(0.0);
}
float4 texelFetch(constant sampler1DArray &a, int2 b, int c)
{
  return float4(0.0);
}
int2 textureSize(constant sampler1DArray &a, int c)
{
  return a.handle;
}
