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

#include "eevee_gbuffer_lib.glsl"

namespace gbuffer {

gbuffer::ClosurePacking pack_closure(ClosureUndetermined cl)
{
  gbuffer::ClosurePacking cl_packed;
  cl_packed.mode = closure_type_to_mode(cl.type, color_is_grayscale(cl.color));

  if (cl.weight <= CLOSURE_WEIGHT_CUTOFF) {
    cl_packed.mode = GBUF_NONE;
  }
  /* Common to all configs. */
  cl_packed.N = cl.N;
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
      closure::RefractionColorless::pack_additional(cl_packed, cl);
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

/* Transient data used during packing. */
struct Packer {
  /* Packed GBuffer data in layer indexing. */
  ClosurePacking closures[GBUFFER_LAYER_MAX];
  /* Additional info to be stored inside the normal stack. */
  float additional_info;
  /* Header containing which closures are encoded and which normals are used. */
  Header header;

  /* Swap closures to avoid gap in data. Closures are then in layer order. */
  void closures_to_layer_order(const bool3 empty_bins)
  {
    if (empty_bins.y) {
#if GBUFFER_LAYER_MAX > 2
      closures[1] = closures[2];
      closures[2].mode = GBUF_NONE;
#endif
    }
    if (empty_bins.x) {
#if GBUFFER_LAYER_MAX > 1
      closures[0] = closures[1];
      closures[1].mode = GBUF_NONE;
#endif
#if GBUFFER_LAYER_MAX > 1
      closures[1] = closures[2];
      closures[2].mode = GBUF_NONE;
#endif
    }
  }

  /* Needs to happen in layer order. */
  void reuse_tangent_spaces(const bool3 empty_bins)
  {
#if GBUFFER_LAYER_MAX > 1
    if (empty_bins[1] || (all(equal(closures[0].N, closures[1].N)))) {
      this->header.tangent_space_id_set(1, 0);
#  if GBUFFER_LAYER_MAX > 2
      if (empty_bins[2] || (all(equal(closures[0].N, closures[2].N)))) {
        this->header.tangent_space_id_set(2, 0);
      }
#  endif
    }
    else {
#  if GBUFFER_LAYER_MAX > 2
      if (empty_bins[2] || (all(equal(closures[1].N, closures[2].N)))) {
        this->header.tangent_space_id_set(2, 1);
      }
#  endif
    }
#endif
  }

  UsedLayerFlag get_used_normal_layers()
  {
    uchar flag = 0;
    if (this->header.tangent_space_id(1) != 1u) {
      flag |= NORMAL_DATA_1;
    }
    if (this->header.tangent_space_id(2) != 2u) {
      flag |= NORMAL_DATA_2;
    }
    return UsedLayerFlag(flag);
  }

  Packed result_get()
  {
    Packed data;
    data.normal[0] = normal_pack(this->closures[0].N);
    data.normal[1] = normal_pack(this->closures[1].N);
    data.normal[2] = normal_pack(this->closures[2].N);
    uchar used_layers = get_used_normal_layers();

    uint closure_len = this->header.closure_len();

    /* Interleave data to simplify loading code and keep packed storage. */
    switch (closure_len) {
      case 1u:
        data.closure[0] = this->closures[0].data0;
        data.closure[1] = this->closures[0].data1;
        break;
#if GBUFFER_LAYER_MAX > 1
      case 2u:
        data.closure[0] = this->closures[0].data0;
        data.closure[1] = this->closures[1].data0;
        data.closure[2] = this->closures[0].data1;
        data.closure[3] = this->closures[1].data1;
        set_flag_from_test(used_layers, this->closures[0].use_data1(), CLOSURE_DATA_2);
        set_flag_from_test(used_layers, this->closures[1].use_data1(), CLOSURE_DATA_3);
        break;
#endif
#if GBUFFER_LAYER_MAX > 2
      case 3u:
        data.closure[0] = this->closures[0].data0;
        data.closure[1] = this->closures[1].data0;
        data.closure[2] = this->closures[2].data0;
        data.closure[3] = this->closures[0].data1;
        data.closure[4] = this->closures[1].data1;
        data.closure[5] = this->closures[2].data1;
        set_flag_from_test(used_layers, true, CLOSURE_DATA_2);
        set_flag_from_test(used_layers, this->closures[0].use_data1(), CLOSURE_DATA_3);
        set_flag_from_test(used_layers, this->closures[1].use_data1(), CLOSURE_DATA_4);
        set_flag_from_test(used_layers, this->closures[2].use_data1(), CLOSURE_DATA_5);
        break;
#endif
    }

    data.used_layers = UsedLayerFlag(used_layers);

    if (this->header.has_additional_data()) {
      data.additional_info = float2(this->additional_info);
      set_flag_from_test(used_layers, true, ADDITIONAL_DATA);
    }

    if (this->header.use_object_id()) {
      set_flag_from_test(used_layers, true, OBJECT_ID);
    }

    data.header = this->header.data();

    return data;
  }
};

struct InputClosures {
  ClosureUndetermined closure[GBUFFER_LAYER_MAX];
};

/**
  * surface_N: Fallback normal is there is no closure.
  * thickness: Additional object information if any closure needs it.
  float thickness;
  * use_object_id: True if surface uses a dedicated object id layer. Should only be turned on if
  needed. */
Packed pack(
    InputClosures cl_data, float3 Ng, packed_float3 surface_N, float thickness, bool use_object_id)
{
  Packer packer;
  packer.header = gbuffer::Header::zero();
  packer.header.use_object_id_set(use_object_id);

  for (int i = 0; i < GBUFFER_LAYER_MAX; i++) {
    packer.closures[i] = pack_closure(cl_data.closure[i]);
  }

  for (int i = 0; i < GBUFFER_LAYER_MAX; i++) {
    packer.header.closure_set(i, packer.closures[i].mode);
  }

  if (packer.header.has_additional_data()) {
    packer.additional_info = AdditionalInfo::pack(thickness).x;
  }

  bool3 empty_bins = packer.header.empty_bins();

  /* ---- Switch from Bin to Layer order. ---- */
  packer.closures_to_layer_order(empty_bins);

  /* This is correct in layer order. */
  bool has_any_closure = packer.closures[0].mode != GBUF_NONE;
  if (!has_any_closure) {
    /* Output dummy closure in the case of unlit materials for correct render passes data. */
    packer.closures[0] = gbuffer::ClosurePacking::fallback(surface_N);
    packer.header.closure_set(0, packer.closures[0].mode);
  }

  packer.reuse_tangent_spaces(empty_bins);

  /* Needs to happen in layer order and after normal fallback. */
  packer.header.geometry_normal_set(Ng, packer.closures[0].N);

  return packer.result_get();
}

/** \} */

}  // namespace gbuffer
