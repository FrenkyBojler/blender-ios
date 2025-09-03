/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/**
 * G-buffer: Packing and unpacking of G-buffer data.
 *
 * See #GBuffer for a breakdown of the G-buffer layout.
 *
 * There is two way of indexing closure data from the GBuffer:
 * - per "bin": same closure indices as during the material evaluation pass.
 *              Can have none-closures.
 * - per "layer": gbuffer internal storage order. Tightly packed, will only have none-closures at
 *                the end of the array.
 *
 * Indexing per bin is better to avoid parameter discontinuity for a given closure
 * (i.e: for denoising), whereas indexing per layer is better for iterating through the closure
 * without dealing with none-closures.
 */

#include "eevee_gbuffer_lib.glsl"

/* NOTE: Only specialized for the gbuffer pass. */
#ifndef GBUFFER_LAYER_MAX
#  define GBUFFER_LAYER_MAX 3
#endif

/* TODO(fclem): This should save some compile time per material. */
#define GBUFFER_HAS_REFLECTION
#define GBUFFER_HAS_REFRACTION
#define GBUFFER_HAS_SUBSURFACE
#define GBUFFER_HAS_TRANSLUCENT

namespace gbuffer::detail {

uint fetch_object_id(int2 texel)
{
  usampler2DArray tx = sampler_get(eevee_gbuffer_data, gbuf_header_tx);
  return texelFetch(tx, int3(texel, 1), 0).r;
}

float4 fetch_data(int2 texel, uchar layer)
{
  sampler2DArray tx = sampler_get(eevee_gbuffer_data, gbuf_closure_tx);
  return texelFetch(tx, int3(texel, layer), 0);
}

float4 fetch_normal(int2 texel, uchar layer)
{
  sampler2DArray tx = sampler_get(eevee_gbuffer_data, gbuf_normal_tx);
  return texelFetch(tx, int3(texel, layer), 0);
}

ClosureUndetermined unpack_closure(gbuffer::ClosurePacking cl_in)
{
  ClosureUndetermined cl;
  cl.type = mode_to_closure_type(cl_in.mode);
  /* Common to all configs. */
  cl.color = closure_color_unpack(cl_in.data0);
  cl.N = cl_in.N;
  /* Some closures require additional unpacking. */
  switch (cl_in.mode) {
#ifdef GBUFFER_HAS_REFLECTION
    case GBUF_REFLECTION:
      closure::Reflection::unpack_additional(cl, cl_in.data1);
      break;
    case GBUF_REFLECTION_COLORLESS:
      closure::ReflectionColorless::unpack_additional(cl, cl_in.data0);
      break;
#endif
#ifdef GBUFFER_HAS_REFRACTION
    case GBUF_REFRACTION:
      closure::Refraction::unpack_additional(cl, cl_in.data1);
      break;
    case GBUF_REFRACTION_COLORLESS:
      closure::RefractionColorless::unpack_additional(cl, cl_in.data0);
      break;
#endif
#ifdef GBUFFER_HAS_SUBSURFACE
    case GBUF_SUBSURFACE:
      closure::Subsurface::unpack_additional(cl, cl_in.data1);
      break;
#endif
    default:
      break;
  }
  return cl;
}

/* Read only one layer from the GBuffer. */
ClosureUndetermined read_layer(
    uchar normal_id, uchar closure_len, GBufferMode bin_mode, int2 texel, uchar layer_id)
{
  if (bin_mode == GBUF_NONE) {
    return closure_new(ClosureType(CLOSURE_NONE_ID));
  }

  gbuffer::ClosurePacking cl_in;
  cl_in.mode = bin_mode;

  float2 packed_N = detail::fetch_normal(texel, normal_id).xy;
  cl_in.N = normal_unpack(packed_N);

  cl_in.data0 = detail::fetch_data(texel, layer_id);
  if (cl_in.use_data1()) {
    cl_in.data1 = detail::fetch_data(texel, layer_id + closure_len);
  }
  return unpack_closure(cl_in);
}

}  // namespace gbuffer::detail

namespace gbuffer {

gbuffer::Header read_header(int2 texel)
{
  usampler2DArray tx = sampler_get(eevee_gbuffer_data, gbuf_header_tx);
  return Header::from_data(texelFetch(tx, int3(texel, 0), 0).r);
}

/* Read the entirety of the GBuffer by layer. */
gbuffer::Layers read_layers(int2 texel)
{
  gbuffer::Layers layers;

  layers.header = gbuffer::read_header(texel);
  uint3 layer_types = layers.header.bin_types_per_layer();
  uchar closure_len = layers.header.closure_len();

  layers.layer[0] = gbuffer::detail::read_layer(
      layers.header.tangent_space_id(0), closure_len, GBufferMode(layer_types[0]), texel, 0);

#if GBUFFER_LAYER_MAX > 1
  layers.layer[1] = gbuffer::detail::read_layer(
      layers.header.tangent_space_id(1), closure_len, GBufferMode(layer_types[1]), texel, 1);
#endif

#if GBUFFER_LAYER_MAX > 2
  layers.layer[2] = gbuffer::detail::read_layer(
      layers.header.tangent_space_id(2), closure_len, GBufferMode(layer_types[2]), texel, 2);
#endif
  return layers;
}

/* Read only one bin from the GBuffer. */
ClosureUndetermined read_bin(Header header, int2 texel, uchar bin_index)
{
  GBufferMode bin_mode = header.bin_type(bin_index);

  uchar layer_id = header.bin_to_layer(bin_index);
  uchar normal_id = header.tangent_space_id(layer_id);
  uchar closure_len = header.closure_len();

  return gbuffer::detail::read_layer(normal_id, closure_len, bin_mode, texel, layer_id);
}
ClosureUndetermined read_bin(int2 texel, uchar bin_index)
{
  return read_bin(gbuffer::read_header(texel), texel, bin_index);
}

/* Load thickness data only if available. Return 0 otherwise. */
float read_thickness(Header header, int2 texel)
{
  if (!header.has_additional_data()) {
    return 0.0f;
  }
  uint closure_len = header.closure_len();
  float2 data_packed = gbuffer::detail::fetch_normal(texel, closure_len).rg;
  return AdditionalInfo::unpack(data_packed).thickness;
}

/* Returns the first world normal stored in the gbuffer. Assume gbuffer header is non-null. */
float3 read_normal(int2 texel)
{
  return normal_unpack(gbuffer::detail::fetch_normal(texel, 0).rg);
}

/* Returns the object id stored in the gbuffer. Assume gbuffer header is non-null and object id is
 * stored. */
uint read_object_id(int2 texel)
{
  return gbuffer::detail::fetch_object_id(texel);
}

/** \} */

}  // namespace gbuffer
