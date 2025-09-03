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

#include "infos/eevee_common_info.hh"

#include "gpu_shader_codegen_lib.glsl"
#include "gpu_shader_math_vector_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

namespace gbuffer {

/* NOTE: Only specialized for the gbuffer pass. */
#ifndef GBUFFER_LAYER_MAX
#  define GBUFFER_LAYER_MAX 3
#endif

/* TODO(fclem): This should save some compile time per material. */
#define GBUFFER_HAS_REFLECTION
#define GBUFFER_HAS_REFRACTION
#define GBUFFER_HAS_SUBSURFACE
#define GBUFFER_HAS_TRANSLUCENT

/* -------------------------------------------------------------------- */
/** \name Utilities
 *
 * \{ */

GBufferMode closure_type_to_mode(ClosureType type, bool is_grayscale)
{
  switch (type) {
    case CLOSURE_BSDF_DIFFUSE_ID:
      return GBUF_DIFFUSE;
    case CLOSURE_BSDF_TRANSLUCENT_ID:
      return GBUF_TRANSLUCENT;
    case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID:
      return is_grayscale ? GBUF_REFLECTION_COLORLESS : GBUF_REFLECTION;
    case CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID:
      return is_grayscale ? GBUF_REFRACTION_COLORLESS : GBUF_REFRACTION;
    case CLOSURE_BSSRDF_BURLEY_ID:
      return GBUF_SUBSURFACE;
    default:
      return GBUF_NONE;
  }
}

ClosureType mode_to_closure_type(uint mode)
{
  switch (mode) {
    case GBUF_DIFFUSE:
      return ClosureType(CLOSURE_BSDF_DIFFUSE_ID);
    case GBUF_TRANSLUCENT:
      return ClosureType(CLOSURE_BSDF_TRANSLUCENT_ID);
    case GBUF_SUBSURFACE:
      return ClosureType(CLOSURE_BSSRDF_BURLEY_ID);
    case GBUF_REFLECTION_COLORLESS:
    case GBUF_REFLECTION:
      return ClosureType(CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID);
    case GBUF_REFRACTION_COLORLESS:
    case GBUF_REFRACTION:
      return ClosureType(CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID);
    default:
      return ClosureType(CLOSURE_NONE_ID);
  }
}

bool color_is_grayscale(float3 color)
{
  /* This tests is R == G == B. */
  return all(equal(color.rgb, color.gbr));
}

bool closure_is_empty(ClosureUndetermined cl)
{
  return cl.weight <= CLOSURE_WEIGHT_CUTOFF || cl.type == CLOSURE_NONE_ID;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pack / Unpack Utils
 *
 * \{ */

float2 normal_pack(float3 N)
{
  N /= length_manhattan(N);
  float2 _sign = sign(N.xy);
  _sign.x = _sign.x == 0.0f ? 1.0f : _sign.x;
  _sign.y = _sign.y == 0.0f ? 1.0f : _sign.y;
  N.xy = (N.z >= 0.0f) ? N.xy : ((1.0f - abs(N.yx)) * _sign);
  N.xy = N.xy * 0.5f + 0.5f;
  return N.xy;
}
float3 normal_unpack(float2 N_packed)
{
  N_packed = N_packed * 2.0f - 1.0f;
  float3 N = float3(N_packed.x, N_packed.y, 1.0f - abs(N_packed.x) - abs(N_packed.y));
  float t = clamp(-N.z, 0.0f, 1.0f);
  N.x += (N.x >= 0.0f) ? -t : t;
  N.y += (N.y >= 0.0f) ? -t : t;
  return normalize(N);
}

float ior_pack(float ior)
{
  return (ior > 1.0f) ? (1.0f - 0.5f / ior) : (0.5f * ior);
}
float ior_unpack(float ior_packed)
{
  return (ior_packed > 0.5f) ? (0.5f / (1.0f - ior_packed)) : (2.0f * ior_packed);
}

float thickness_pack(float thickness)
{
  /* TODO(fclem): If needed, we could increase precision by defining a ceiling value like the view
   * distance and remap to it. Or tweak the hyperbole equality. */
  /* NOTE: Sign encodes the thickness mode. */
  /* Remap [0..+inf) to [0..1/2]. */
  float thickness_packed = abs(thickness) / (1.0f + 2.0f * abs(thickness));
  /* Mirror the negative from [0..1/2] to [1..1/2]. O is mapped to 0 for precision. */
  return (thickness < 0.0f) ? 1.0f - thickness_packed : thickness_packed;
}
float thickness_unpack(float thickness_packed)
{
  /* Undo mirroring. */
  float thickness = (thickness_packed > 0.5f) ? 1.0f - thickness_packed : thickness_packed;
  /* Remap [0..1/2] to [0..+inf). */
  thickness = thickness / (1.0f - 2.0f * thickness);
  /* Retrieve sign. */
  return (thickness_packed > 0.5f) ? -thickness : thickness;
}

/**
 * Pack color with values in the range of [0..8] using a 2 bit shared exponent.
 * This allows values up to 8 with some color degradation.
 * Above 8, the result will be clamped when writing the data to the output buffer.
 * This is supposed to be stored in a 10_10_10_2_unorm format with exponent in alpha.
 */
float4 closure_color_pack(float3 color)
{
  float max_comp = max(color.x, max(color.y, color.z));
  float exponent = (max_comp > 1) ? ((max_comp > 2) ? ((max_comp > 4) ? 3.0f : 2.0f) : 1.0f) :
                                    0.0f;
  /* TODO(fclem): Could try dithering to avoid banding artifacts on higher exponents. */
  return float4(color / exp2(exponent), exponent / 3.0f);
}
float3 closure_color_unpack(float4 color_packed)
{
  float exponent = color_packed.a * 3.0f;
  return color_packed.rgb * exp2(exponent);
}

float4 sss_radii_pack(float3 sss_radii)
{
  /* TODO(fclem): Something better. */
  return closure_color_pack(
      float3(ior_pack(sss_radii.x), ior_pack(sss_radii.y), ior_pack(sss_radii.z)));
}
float3 sss_radii_unpack(float4 sss_radii_packed)
{
  /* TODO(fclem): Something better. */
  float3 radii_packed = closure_color_unpack(sss_radii_packed);
  return float3(
      ior_unpack(radii_packed.x), ior_unpack(radii_packed.y), ior_unpack(radii_packed.z));
}

float object_id_f16_pack(uint object_id)
{
  /* TODO(fclem): Make use of all the 16 bits in a half float.
   * This here only correctly represent values up to 1024. */
  return float(object_id);
}

uint object_id_f16_unpack(float object_id_packed)
{
  return uint(object_id_packed);
}

/* Quantize geometric normal to 6 bits. */
uint geometry_normal_pack(float3 Ng, float3 N)
{
  /* This is a threshold that minimizes the error over the sphere. */
  constexpr float quantization_multiplier = 1.360f;
  /* Normalize for comparison. */
  float3 Ng_quantize = normalize(round(quantization_multiplier * Ng));
  /* Note: Comparing the error using cosines. The greater the cosine value, the lower the error. */
  if (dot(N, Ng) > dot(Ng, Ng_quantize)) {
    /* If the error between the default shading normal and the geometric normal is smaller than the
     * compression error, we do not encode the geometric normal and use the shading normal for
     * biasing the shadow rays. This avoid precision issues that comes with the quantization. */
    return 0;
  }
  uint data;
  data = (Ng_quantize.x > 0.0f) ? (1u << 0u) : 0u;
  data |= (Ng_quantize.y > 0.0f) ? (1u << 1u) : 0u;
  data |= (Ng_quantize.z > 0.0f) ? (1u << 2u) : 0u;
  data |= (Ng_quantize.x < 0.0f) ? (1u << 3u) : 0u;
  data |= (Ng_quantize.y < 0.0f) ? (1u << 4u) : 0u;
  data |= (Ng_quantize.z < 0.0f) ? (1u << 5u) : 0u;
  return data;
}

float3 geometry_normal_unpack(uint data, float3 N)
{
  /* If data is 0 it means the shading normal is representative enough. */
  if ((data & (63u << 20u)) == 0u) {
    return N;
  }
  float3 Ng = float3((uint3(data) >> (uint3(0, 1, 2) + 20u)) & 1u) -
              float3((uint3(data) >> (uint3(3, 4, 5) + 20u)) & 1u);
  return normalize(Ng);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Header
 *
 * \{ */

/**
 * The GBuffer is polymorphic and its content varies from pixel to pixel.
 * The header contains some common informations and the layout of the GBuffer content.
 */
struct Header {
#define GBUFFER_NORMAL_BITS_SHIFT 12u
#define GBUFFER_GEOMETRIC_NORMAL_BITS_SHIFT 20u
#define GBUFFER_HEADER_BITS_PER_LAYER 4

 private:
  /**
   * Bit packed header.
   *
   *  Use Object ID
   *    |
   *    |
   * |  v |                        |       Geometric normal      |
   * |....|....|....|....|....|....|....|....|....|....|....|....|....|....|....|....|
   *   31   30   29   28   27   26   25   24   23   22   21   20   19   18   17   16
   *
   *
   * | Tangent Space ID  |                       Closure Types                       |
   * |                   |                   |                   |                   |
   * |- Bin 2 -|- Bin 1 -|------ Bin 2 ------|------ Bin 1 ------|------ Bin 0 ------|
   * |....|....|....|....|....|....|....|....|....|....|....|....|....|....|....|....|
   *   15   14   13   12   11   10    9    8    7    6    5    4    3    2    1    0
   */
  uint header_;

 public:
  METAL_CONSTRUCTOR_1(Header, uint, header_)

  void closure_set(uint bin, GBufferMode mode)
  {
    this->header_ |= (mode << (GBUFFER_HEADER_BITS_PER_LAYER * bin));
  }
  GBufferMode closure(uint bin) const
  {
    return GBufferMode((this->header_ >> (GBUFFER_HEADER_BITS_PER_LAYER * bin)) & 15u);
  }

  /* If this flag is set, the header texture has a second layer containing the ObjectIDs. */
  bool use_object_id() const
  {
    return flag_test(this->header_, 1u << 31u);
  }
  void use_object_id_set(bool value)
  {
    set_flag_from_test(this->header_, value, 1u << 31u);
  }

  /**
   * Set the dedicated normal bit for the specified bin.
   * Expects `bin_id` to be in [0..2].
   * Expects `normal_id` to be in [0..2] (packed in 2bits).
   */
  void tangent_space_id_set(uint bin, uint normal_id)
  {
    /* Layer 0 will always have normal id 0. It doesn't have to be encoded. Skip it. */
    if (bin != 0u) {
      /* Note: Keep this in the if statement as it compiles faster somehow. */
      /* -2 is to skip the bin 0 and start encoding for bin 1. This keeps the FMA. */
      this->header_ |= normal_id << ((GBUFFER_NORMAL_BITS_SHIFT - 2u) + bin * 2u);
    }
  }
  uint tangent_space_id(uint bin) const
  {
    /* Layer 0 will always have normal id 0. */
    if (bin == 0u) {
      return 0u;
    }
    /* -2 is to skip the bin 0 and start encoding for bin 1. This keeps the FMA. */
    return (3u & (this->header_ >> ((GBUFFER_NORMAL_BITS_SHIFT - 2u) + bin * 2u)));
  }

  /* Pack geometric normal into the header if needed. */
  void geometry_normal_set(float3 Ng, float3 N)
  {
    this->header_ |= geometry_normal_pack(Ng, N);
  }
  float3 geometry_normal(float3 surface_N) const
  {
    return geometry_normal_unpack(this->header_, surface_N);
  }

  uint closure_len() const
  {
    /* NOTE: Need to be adjusted for different global GBUFFER_LAYER_MAX. */
    constexpr uint bits_per_layer = uint(GBUFFER_HEADER_BITS_PER_LAYER);
    uint3 closure_types = (uint3(this->header_) >> (uint3(0u, 1u, 2u) * bits_per_layer)) &
                          ((1u << bits_per_layer) - 1);
    return reduce_add(int3(not(equal(closure_types, uint3(0u)))));
  }

  uint normal_len() const
  {
    if (this->header_ == 0u) {
      return 0;
    }
    /* Count implicit first layer. */
    uint count = 1u;
    count += uint(((this->header_ >> 12u) & 3u) != 0);
    count += uint(((this->header_ >> 14u) & 3u) != 0);
    return int(count);
  }

  uint data() const
  {
    return this->header_;
  }

  bool has_transmission() const
  {
    /* NOTE: Need to be adjusted for different global GBUFFER_LAYER_MAX. */
    constexpr uint bits_per_layer = uint(GBUFFER_HEADER_BITS_PER_LAYER);
    constexpr uint header_mask = (GBUF_TRANSMISSION_BIT << (bits_per_layer * 0)) |
                                 (GBUF_TRANSMISSION_BIT << (bits_per_layer * 1)) |
                                 (GBUF_TRANSMISSION_BIT << (bits_per_layer * 2));
    return (this->header_ & header_mask) != 0;
  }

  bool has_additional_data() const
  {
    /* For now, this is true. Only the transmission closures use the thickness data. */
    return has_transmission();
  }
};

/* Added data inside the Tangent Space layers. */
struct AdditionalInfo {
  float thickness;

  METAL_CONSTRUCTOR_1(AdditionalInfo, float, thickness)

  static float2 pack(float thickness)
  {
    return float2(thickness_pack(thickness), 0.0f /* UNUSED */);
  }

  static AdditionalInfo unpack(float2 data)
  {
    return AdditionalInfo(thickness_unpack(data.x));
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Writer
 *
 * \{ */


/** \} */

/* -------------------------------------------------------------------- */
/** \name Reader
 *
 * \{ */

/* Result of loading the GBuffer. */
struct Reader {
  Header header;
  /* Closures indexed by bins. */
  ClosureUndetermined closures[GBUFFER_LAYER_MAX];
  /* First world normal stored in the gbuffer. Only valid if `has_any_surface` is true. */
  packed_float3 surface_N;
  /* Additional object information if any closure needs it. */
  float thickness;

  /* Used for regular fetching. */
  Header fetch_header(usampler2DArray tx, int2 texel, uchar layer)
  {
    return Header(texelFetch(tx, int3(texel, layer), 0).r);
  }
  float4 fetch_data(sampler2DArray tx, int2 texel, uchar layer)
  {
    return texelFetch(tx, int3(texel, layer), 0);
  }
  float4 fetch_normal(sampler2DArray tx, int2 texel, uchar layer)
  {
    return texelFetch(tx, int3(texel, layer), 0);
  }

  /* Used for unit tests. */
  Header fetch_header(Writer writer, int2 texel, uchar layer)
  {
    return writer.header;
  }
  float4 fetch_data(Writer writer, int2 texel, uchar layer)
  {
    return writer.closures[layer].data0;
  }
  float4 fetch_normal(Writer writer, int2 texel, uchar layer)
  {
    return writer.closures[layer].xyyy;
  }
};

template<typename T> AdditionalInfo additional_info_load(T loader, int2 texel, uint closure_count)
{
  float2 data_packed = fetch_normal(loader, texel, int(closure_count)).rg;
  return AdditionalInfo::unpack(data_packed);
}

/** \} */

}  // namespace gbuffer

/* -------------------------------------------------------------------- */
/** \name Pack / Unpack Closures
 *
 * \{ */

namespace gbuffer::closure {

struct Subsurface {
  static void pack_additional(ClosurePacked &cl_packed, ClosureUndetermined cl)
  {
    cl_packed.data1 = sss_radii_pack(cl.data.xyz);
  }

  static void unpack_additional(ClosureUndetermined &cl, float4 data1)
  {
    cl.data.rgb = sss_radii_unpack(data1);
  }
};

struct Reflection {
  static void pack_additional(ClosurePacked &cl_packed, ClosureUndetermined cl)
  {
    cl_packed.data1 = float4(cl.data.x, 0.0f, 0.0f, 0.0f);
  }

  static void unpack_additional(ClosureUndetermined &cl, float4 data1)
  {
    cl.data.x = data1.x; /* Roughness. */
  }
};

struct Refraction {
  static void pack_additional(ClosurePacked &cl_packed, ClosureUndetermined cl)
  {
    cl_packed.data1 = float4(cl.data.x, ior_pack(cl.data.y), 0.0f, 0.0f);
  }

  static void unpack_additional(ClosureUndetermined &cl, float4 data1)
  {
    cl.data.x = data1.x; /* Roughness. */
    cl.data.y = ior_unpack(data1.y);
  }
};

/* Special case where we can save 1 data layers per closure. */
struct ReflectionColorless {
  static void pack_additional(ClosurePacked &cl_packed, ClosureUndetermined cl)
  {
    cl_packed.data0 = float4(cl.data.x, 0.0f, cl_packed.data0.zw);
  }

  static void unpack_additional(ClosureUndetermined &cl, float4 data1)
  {
    cl.data.x = data1.x; /* Roughness. */
  }
};

/* Special case where we can save 1 data layers per closure. */
struct RefractionColorless {
  static void pack_additional(ClosurePacked &cl_packed, ClosureUndetermined cl)
  {
    cl_packed.data0 = float4(cl.data.x, ior_pack(cl.data.y), cl_packed.data0.zw);
  }

  static void unpack_additional(ClosureUndetermined &cl, float4 data1)
  {
    cl.data.x = data1.x; /* Roughness. */
    cl.data.y = ior_unpack(data1.y);
  }
};

}  // namespace gbuffer::closure

/** \} */

/* -------------------------------------------------------------------- */
/** \name Gbuffer Read / Write
 *
 * \{ */

namespace gbuffer {

ClosurePacked pack_closure(ClosureUndetermined cl)
{
  ClosurePacked cl_packed;
  cl_packed.mode = closure_type_to_mode(cl.type, color_is_grayscale(cl.color));

  if (cl.weight <= CLOSURE_WEIGHT_CUTOFF) {
    cl_packed.mode = GBUF_NONE;
  }
  /* Common to all configs. */
  cl_packed.data0 = closure_color_pack(cl.color);
  /* Some closures require additional packing. */
  switch (cl_packed.mode) {
#ifdef GBUFFER_HAS_REFLECTION
    case GBUF_REFLECTION:
      closure::Reflection::pack_additional(cl_packed, cl);
      break;
    case GBUF_REFLECTION_COLORLESS:
      closure::ReflectionColorless::pack_additional(cl_packed, cl);
      break;
#endif
#ifdef GBUFFER_HAS_REFRACTION
    case GBUF_REFRACTION:
      closure::Refraction::pack_additional(cl_packed, cl);
      break;
    case GBUF_REFRACTION_COLORLESS:
      closure::RefractionColorLess::pack_additional(cl_packed, cl);
      break;
#endif
#ifdef GBUFFER_HAS_SUBSURFACE
    case GBUF_SUBSURFACE:
      closure::Subsurface::pack_additional(cl_packed, cl);
      break;
#endif
    default:
      break;
  }
  return cl_packed;
}

ClosureUndetermined unpack_closure(GBufferMode mode, float4 data0, float4 data1)
{
  ClosureUndetermined cl;
  cl.type = mode_to_closure_type(cl.type);
  /* Common to all configs. */
  cl.color = closure_color_unpack(cl.color);
  /* Some closures require additional unpacking. */
  switch (mode) {
#ifdef GBUFFER_HAS_REFLECTION
    case GBUF_REFLECTION:
      closure::Reflection::unpack_additional(cl, data1);
      break;
    case GBUF_REFLECTION_COLORLESS:
      closure::ReflectionColorless::unpack_additional(cl, data0, cl.color);
      break;
#endif
#ifdef GBUFFER_HAS_REFRACTION
    case GBUF_REFRACTION:
      closure::Refraction::unpack_additional(cl, data1);
      break;
    case GBUF_REFRACTION_COLORLESS:
      closure::RefractionColorless::unpack_additional(cl, data0, cl.color);
      break;
#endif
#ifdef GBUFFER_HAS_SUBSURFACE
    case GBUF_SUBSURFACE:
      closure::Subsurface::unpack_additional(cl, data1);
      break;
#endif
    default:
      break;
  }
  return mode;
}

void reuse_normals(Writer &gbuf, bool3 empty_closures)
{
#if GBUFFER_LAYER_MAX > 1
  if (empty_closures[1] || all(equal(gbuf.N[0], gbuf.N[1]))) {
    header_normal_layer_id_set(gbuf.header, 1, 0);
#  if GBUFFER_LAYER_MAX > 2
    if (empty_closures[2] || all(equal(gbuf.N[0], gbuf.N[2]))) {
      header_normal_layer_id_set(gbuf.header, 2, 0);
    }
#  endif
  }
#  if GBUFFER_LAYER_MAX > 2
  else if (empty_closures[2] || all(equal(gbuf.N[1], gbuf.N[2]))) {
    header_normal_layer_id_set(gbuf.header, 2, 1);
  }
#  endif
#endif
}

void reorder_input_closures(InputData &data_in, bool3 empty_closures)
{
  /* Swap closures to avoid gap in data. */
  if (empty_closures[0]) {
    if (empty_closures[1]) {
      if (empty_closures[2]) {
        /* Output dummy closure in the case of unlit materials for correct render passes data. */
        data_in.closure[0].type = CLOSURE_BSDF_DIFFUSE_ID;
        data_in.closure[0].color = float3(0);
        data_in.closure[0].weight = 1.0f;
        data_in.closure[0].N = data_in.surface_N;
      }
      else {
#if GBUFFER_LAYER_MAX > 2
        data_in.closure[0] = data_in.closure[2];
        data_in.closure[1].type = CLOSURE_NONE_ID;
        data_in.closure[2].type = CLOSURE_NONE_ID;
#endif
      }
    }
    else {
#if GBUFFER_LAYER_MAX > 1
      data_in.closure[0] = data_in.closure[1];
      data_in.closure[1].type = CLOSURE_NONE_ID;
#endif
#if GBUFFER_LAYER_MAX > 2
      data_in.closure[1] = data_in.closure[2];
      data_in.closure[2].type = CLOSURE_NONE_ID;
#endif
    }
  }
  else if (empty_closures[1]) {
#if GBUFFER_LAYER_MAX > 2
    data_in.closure[1] = data_in.closure[2];
    data_in.closure[2].type = CLOSURE_NONE_ID;
#endif
  }
}

Writer gbuffer_pack(InputData data_in, float3 Ng)
{
  Writer gbuf;
  gbuf.header = 0u;
  gbuf.data_len = 0;
  gbuf.normal_len = 0;

  bool3 empty_closures = bool3(true);
  for (int i = 0; i < GBUFFER_LAYER_MAX; i++) {
    empty_closures[i] = closure_is_empty(data_in.closure[i]);
  }

  gbuffer_reorder_input_closures(data_in, empty_closures);

  for (int i = 0; i < GBUFFER_LAYER_MAX; i++) {
    gbuf.N[i] = gbuffer_normal_pack(data_in.closure[i].N);
  }

  gbuffer_reuse_normals(gbuf, empty_closures);

  uint3 gbuf_modes = uint3(GBUF_NONE);
  for (int i = 0; i < GBUFFER_LAYER_MAX; i++) {
    gbuf_modes[0] = pack_closure(
        data_in.closure[i], gbuf.data[i], gbuf.data[i + GBUFFER_LAYER_MAX]);
    append_closure(gbuf, GBufferMode(gbuf_modes[i]), uint(i));
  }

#if 0 /* TODO(fclem): Have to make reading code aware of this transformation. */
bool use_interleaved_data = false;
#  if GBUFFER_LAYER_MAX > 1
  if (gbuf_modes[1] != GBUF_NONE) {
    use_interleaved_data = true;
  }
#  endif
#  if GBUFFER_LAYER_MAX > 2
  if (gbuf_modes[2] != GBUF_NONE) {
    use_interleaved_data = true;
  }
#  endif

  if (!use_interleaved_data) {
    gbuf.data[1] = gbuf.data[3];
  }
#endif

  /* Pack geometric normal into the header if needed. */
  gbuf.header |= geometry_normal_pack(Ng, data_in.closure[0].N);
  gbuf.header |= use_object_id_pack(data_in.use_object_id);

  if (gbuffer_has_transmission(gbuf.header)) {
    additional_info_pack(gbuf, data_in.thickness);
  }

  return gbuf;
}

/* Return the number of closure as encoded in the given header value. */
int closure_count(uint header) {}

/* Return the number of normal layer as encoded in the given header value. */
int normal_count(uint header) {}

/* Return the type of a closure using its bin index. */
ClosureType closure_type_get_by_bin(uint header, uchar bin_index)
{
  constexpr int bits_per_layer = GBUFFER_HEADER_BITS_PER_LAYER;
  uint mode = (header >> (bin_index * bits_per_layer)) & ((1u << bits_per_layer) - 1);
  return mode_to_closure_type(mode);
}

/* Return the bin index of a closure using its layer index. */
uchar closure_get_bin_index(gbuffer::Reader gbuf, uchar layer_index)
{
  uchar layer = 0u;
  for (uchar bin = 0u; bin < GBUFFER_LAYER_MAX; bin++) {
    GBufferMode mode = header_unpack(gbuf.header, bin);
    /* Gbuffer header can have holes. Skip GBUF_NONE. */
    if (mode != GBUF_NONE) {
      if (layer == layer_index) {
        return bin;
      }
      layer++;
    }
  }
  /* Should never happen. But avoid out of bound access. */
  return 0u;
}

ClosureUndetermined closure_get_by_bin(gbuffer::Reader gbuf, uchar bin_index)
{
  int layer_index = 0;
  for (uchar bin = 0; bin < GBUFFER_LAYER_MAX; bin++) {
    GBufferMode mode = header_unpack(gbuf.header, bin);
    if (bin == bin_index) {
      return closure_get(gbuf, layer_index);
    }
    else {
      if (mode != GBUF_NONE) {
        layer_index++;
      }
    }
  }
  /* Should never happen. */
  return closure_new(CLOSURE_NONE_ID);
}

/* Read the entirety of the GBuffer. */
gbuffer::Reader read(samplerGBufferHeader header_tx,
                     samplerGBufferClosure closure_tx,
                     samplerGBufferNormal normal_tx,
                     int2 texel)
{
  gbuffer::Reader gbuf;
  gbuf.texel = texel;
  gbuf.thickness = 0.0f;
  gbuf.closure_count = 0;
  gbuf.data_len = 0;
  gbuf.normal_len = 0;
  gbuf.surface_N = float3(0.0f);
  for (uchar bin = 0; bin < GBUFFER_LAYER_MAX; bin++) {
    register_closure(gbuf, closure_new(CLOSURE_NONE_ID), bin);
  }

  gbuf.header = fetch(header_tx, texel, 0);

  if (gbuf.header == 0u) {
    return gbuf;
  }

  /* First closure is always written. */
  gbuf.surface_N = normal_unpack(fetch(normal_tx, texel, 0).xy);

  bool has_additional_data = false;
  for (uchar bin = 0; bin < GBUFFER_LAYER_MAX; bin++) {
    GBufferMode mode = header_unpack(gbuf.header, bin);
    switch (mode) {
      default:
      case GBUF_NONE:
        break;
      case GBUF_DIFFUSE:
        closure_diffuse_load(gbuf, gbuf.closure_count, bin, closure_tx, normal_tx);
        gbuf.closure_count++;
        break;
      case GBUF_TRANSLUCENT:
        closure_translucent_load(gbuf, gbuf.closure_count, bin, closure_tx, normal_tx);
        gbuf.closure_count++;
        has_additional_data = true;
        break;
      case GBUF_SUBSURFACE:
        closure_subsurface_load(gbuf, gbuf.closure_count, bin, closure_tx, normal_tx);
        gbuf.closure_count++;
        has_additional_data = true;
        break;
      case GBUF_REFLECTION:
        closure_reflection_load(gbuf, gbuf.closure_count, bin, closure_tx, normal_tx);
        gbuf.closure_count++;
        break;
      case GBUF_REFRACTION:
        closure_refraction_load(gbuf, gbuf.closure_count, bin, closure_tx, normal_tx);
        gbuf.closure_count++;
        has_additional_data = true;
        break;
      case GBUF_REFLECTION_COLORLESS:
        closure_reflection_colorless_load(gbuf, gbuf.closure_count, bin, closure_tx, normal_tx);
        gbuf.closure_count++;
        break;
      case GBUF_REFRACTION_COLORLESS:
        closure_refraction_colorless_load(gbuf, gbuf.closure_count, bin, closure_tx, normal_tx);
        gbuf.closure_count++;
        has_additional_data = true;
        break;
    }
  }

  if (has_additional_data) {
    additional_info_load(gbuf, normal_tx);
  }

  return gbuf;
}

/* Read only one bin from the GBuffer. */
ClosureUndetermined read_bin(uint header,
                             samplerGBufferClosure closure_tx,
                             samplerGBufferNormal normal_tx,
                             int2 texel,
                             uchar bin_index)
{
  gbuffer::Reader gbuf;
  gbuf.texel = texel;
  gbuf.closure_count = 0;
  gbuf.data_len = 0;
  gbuf.normal_len = 0;
  gbuf.header = header;

  if (gbuf.header == 0u) {
    return closure_new(CLOSURE_NONE_ID);
  }

  GBufferMode mode;
  for (uchar bin = 0; bin < GBUFFER_LAYER_MAX; bin++) {
    mode = header_unpack(gbuf.header, bin);

    if (bin >= bin_index) {
      break;
    }

    switch (mode) {
      default:
      case GBUF_NONE:
        break;
      case GBUF_DIFFUSE:
        closure_diffuse_skip(gbuf);
        break;
      case GBUF_TRANSLUCENT:
        closure_translucent_skip(gbuf);
        break;
      case GBUF_SUBSURFACE:
        closure_subsurface_skip(gbuf);
        break;
      case GBUF_REFLECTION:
        closure_reflection_skip(gbuf);
        break;
      case GBUF_REFRACTION:
        closure_refraction_skip(gbuf);
        break;
      case GBUF_REFLECTION_COLORLESS:
        closure_reflection_colorless_skip(gbuf);
        break;
      case GBUF_REFRACTION_COLORLESS:
        closure_refraction_colorless_skip(gbuf);
        break;
    }
  }

  switch (mode) {
    default:
    case GBUF_NONE:
      register_closure(gbuf, closure_new(CLOSURE_NONE_ID), gbuf.closure_count);
      break;
    case GBUF_DIFFUSE:
      closure_diffuse_load(gbuf, gbuf.closure_count, bin_index, closure_tx, normal_tx);
      break;
    case GBUF_TRANSLUCENT:
      closure_translucent_load(gbuf, gbuf.closure_count, bin_index, closure_tx, normal_tx);
      break;
    case GBUF_SUBSURFACE:
      closure_subsurface_load(gbuf, gbuf.closure_count, bin_index, closure_tx, normal_tx);
      break;
    case GBUF_REFLECTION:
      closure_reflection_load(gbuf, gbuf.closure_count, bin_index, closure_tx, normal_tx);
      break;
    case GBUF_REFRACTION:
      closure_refraction_load(gbuf, gbuf.closure_count, bin_index, closure_tx, normal_tx);
      break;
    case GBUF_REFLECTION_COLORLESS:
      closure_reflection_colorless_load(
          gbuf, gbuf.closure_count, bin_index, closure_tx, normal_tx);
      break;
    case GBUF_REFRACTION_COLORLESS:
      closure_refraction_colorless_load(
          gbuf, gbuf.closure_count, bin_index, closure_tx, normal_tx);
      break;
  }

  return closure_get(gbuf, gbuf.closure_count);
}
ClosureUndetermined read_bin(samplerGBufferHeader header_tx,
                             samplerGBufferClosure closure_tx,
                             samplerGBufferNormal normal_tx,
                             int2 texel,
                             uchar bin_index)
{
  return read_bin(fetch(header_tx, texel, 0), closure_tx, normal_tx, texel, bin_index);
}

/* Load thickness data only if available. Return 0 otherwise. */
float read_thickness(uint header, samplerGBufferNormal normal_tx, int2 texel)
{
  /* WATCH: Assumes all closures needing additional data are in first bin. */
  switch (gbuffer_closure_type_get_by_bin(header, 0)) {
    case CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID:
    case CLOSURE_BSDF_TRANSLUCENT_ID:
    case CLOSURE_BSSRDF_BURLEY_ID: {
      int normal_len = normal_count(header);
      float2 data_packed = fetch(normal_tx, texel, normal_len).rg;
      return thickness_unpack(data_packed.x);
    }
    default:
      return 0.0f;
  }
}

/* Returns the first world normal stored in the gbuffer. Assume gbuffer header is non-null. */
float3 read_normal(samplerGBufferNormal normal_tx, int2 texel)
{
  float2 normal_packed = fetch(normal_tx, texel, 0).rg;
  return normal_unpack(normal_packed);
}

/** \} */

}  // namespace gbuffer
