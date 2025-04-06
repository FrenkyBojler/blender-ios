/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Must be included before the shader include. */
// #include "../inter/gpu_shader_create_info.hh"

#define NO_SHADER_CODE

#define SRD_VERTEX_IN_BEGIN(srd) \
  struct srd { \
    static void populate(ShaderCreateInfo &info) \
    {
#define SRD_VERTEX_IN_END(srd) \
  } \
  } \
  ;

#define SRD_VERTEX_IN(srd, binding, type, name) info.vertex_in(binding, Type::type##_t, #name);

#define SRD_STAGE_INOUT_BEGIN(srd) \
  struct srd { \
    static void populate(ShaderCreateInfo & /*info*/) \
    {
#define SRD_STAGE_INOUT_END(srd) \
  } \
  } \
  ;

#define SRD_STAGE_INOUT(srd, qual, type, name)  // TODO info.qual(Type::type##_t, #name);

#define SRD_FRAGMENT_OUT_BEGIN(srd) \
  struct srd { \
    static void populate(ShaderCreateInfo &info) \
    {
#define SRD_FRAGMENT_OUT_END(srd) \
  } \
  } \
  ;

#define SRD_FRAGMENT_OUT(srd, binding, type, name) \
  info.fragment_out(binding, Type::type##_t, #name);

#define SRD_RESOURCE_BEGIN(srd) \
  struct srd { \
    static void populate(ShaderCreateInfo &info) \
    {
#define SRD_RESOURCE_END(srd) \
  } \
  } \
  ;

#define SRD_RESOURCE_SPECIALIZATION_CONSTANT(srd, type, name, default) \
  info.specialization_constant(Type::type##_t, #name, default);
#define SRD_RESOURCE_PUSH_CONSTANT(srd, type, name) info.push_constant(Type::type##_t, #name);
/* TODO(fclem): Add unique names to the members and use resource accessors in shader code. */
#define SRD_RESOURCE_SAMPLER(srd, binding, type, name) \
  info.sampler(binding, ImageType::type, #name);
#define SRD_RESOURCE_STORAGE_BUF(srd, binding, access, type, name, array) \
  info.storage_buf(binding, Qualifier::access, #type, #name #array);
#define SRD_RESOURCE_UNIFORM_BUF(srd, binding, type, name, array) \
  info.uniform_buf(binding, #type, #name #array);
#define SRD_RESOURCE_STRUCT(srd, type, name) type::populate(info);

#define SRD_GRAPHIC_PIPELINE( \
    file, pipe, vert_in, state_inout, frag_out, vert_func, vert_res, frag_func, frag_res) \
  struct pipe { \
    static ShaderCreateInfo create_info() \
    { \
      ShaderCreateInfo info(#pipe); \
\
      vert_in::populate(info); \
      state_inout::populate(info); \
      frag_out::populate(info); \
\
      vert_res::populate(info); \
      frag_res::populate(info); \
\
      info.vertex_source_ = #file; \
      info.fragment_source_ = #file; \
      info.vertex_entry_point_ = #vert_func; \
      info.fragment_entry_point_ = #frag_func; \
\
      return info; \
    } \
  };
