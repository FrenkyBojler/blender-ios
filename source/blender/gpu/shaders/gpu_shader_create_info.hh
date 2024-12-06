/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "../inter/gpu_shader_create_info.hh"

#define NO_SHADER_CODE

#define SRD_VERTEX_IN_BEGIN(srd) \
  struct srd { \
    static void populate(CreateInfo &info) \
    {
#define SRD_VERTEX_IN_END(srd) \
  } \
  } \
  ;

#define SRD_VERTEX_IN(srd, binding, type, name) \
  static VertexIn name{Type::type, #name}; \
  info.vert_in.append(&name);

#define SRD_STAGE_INOUT_BEGIN(srd) \
  struct srd { \
    static void populate(CreateInfo &info) \
    {
#define SRD_STAGE_INOUT_END(srd) \
  } \
  } \
  ;

#define SRD_STAGE_INOUT(srd, qual, type, name) \
  static StageInOut name{Qualifier::qual, Type::type, #name}; \
  info.stage_inout.append(&name);

#define SRD_FRAGMENT_OUT_BEGIN(srd) \
  struct srd { \
    static void populate(CreateInfo &info) \
    {
#define SRD_FRAGMENT_OUT_END(srd) \
  } \
  } \
  ;

#define SRD_FRAGMENT_OUT(srd, binding, type, name) type name;

#define SRD_RESOURCE_BEGIN(srd) \
  struct srd { \
    static void populate(CreateInfo &info) \
    {
#define SRD_RESOURCE_END(srd) \
  } \
  } \
  ;

#define SRD_RESOURCE_PUSH_CONSTANT(srd, type, name) type name;
#define SRD_RESOURCE_SAMPLER(srd, binding, type, name) type name;

#define SRD_GRAPHIC_PIPELINE( \
    pipe, vert_in, state_inout, frag_out, vert_func, vert_res, frag_func, frag_res) \
  struct pipe : GraphicCreateInfo { \
    GraphicCreateInfo() \
    { \
      vert_in::populate(*this); \
      state_inout::populate(*this); \
      frag_out::populate(*this); \
\
      vert_res::populate(*this); \
      frag_res::populate(*this); \
\
      this->vert_func = #vert_func; \
      this->frag_func = #frag_func; \
    } \
  }
