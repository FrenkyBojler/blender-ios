/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_glsl_cpp_stubs.hh"

/* Temp. Avoid issue with GLSL keyword. */
#undef in
#undef out
#undef inout

#define SRD_ENABLED(srd) 1

#define SRD_VERTEX_IN_BEGIN(srd) struct srd {
#define SRD_VERTEX_IN_END(srd) \
  } \
  ;
#define SRD_VERTEX_IN(srd, binding, type, name) type name;

#define SRD_STAGE_INOUT_BEGIN(srd) struct srd {
#define SRD_STAGE_INOUT_END(srd) \
  } \
  ;
#define SRD_STAGE_INOUT(srd, qual, type, name) type name;

#define SRD_FRAGMENT_OUT_BEGIN(srd) struct srd {
#define SRD_FRAGMENT_OUT_END(srd) \
  } \
  ;
#define SRD_FRAGMENT_OUT(srd, binding, type, name) type name;

#define SRD_RESOURCE_BEGIN(srd) struct srd {
#define SRD_RESOURCE_END(srd) \
  } \
  ;

#define SRD_RESOURCE_SPECIALIZATION_CONSTANT(srd, type, name, default) type name;
#define SRD_RESOURCE_PUSH_CONSTANT(srd, type, name) type name;
#define SRD_RESOURCE_SAMPLER(srd, binding, type, name) type name;
#define SRD_RESOURCE_STORAGE_BUF(srd, binding, access, type, name, array) type(*name) array;
#define SRD_RESOURCE_UNIFORM_BUF(srd, binding, type, name, array) type(*name) array;
#define SRD_RESOURCE_STRUCT(srd, type, name) type name;

#define buffer_read(buffer, offset) (*buffer)[offset]

/* Create pseudo shader program for type checking. */
#define SRD_GRAPHIC_PIPELINE( \
    file, pipe, vert_in, stage_inout, frag_out, vert_func, vert_res, frag_func, frag_res) \
  frag_out pipe() \
  { \
    stage_inout inout = vert_func(vert_in{}, vert_res{}); \
    return frag_func(inout, frag_res{}); \
  }
