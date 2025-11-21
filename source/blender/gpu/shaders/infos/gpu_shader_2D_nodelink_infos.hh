/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"

#  include "GPU_shader_shared.hh"
#endif

#include "gpu_shader_create_info.hh"

struct [[vertex_in]] NodeLinkVertIn {
  [[attribute(0)]] float2 uv;
  [[attribute(1)]] float2 pos;
  [[attribute(2)]] float2 expand;
};

struct [[vertex_out]] NodeLinkVertOut {
  [[smooth]] float4 final_color;
  [[smooth]] float2 line_uv;
  [[flat]] float line_length;
  [[flat]] float line_thickness;
  [[flat]] float dash_length;
  [[flat]] float dash_factor;
  [[flat]] float dash_alpha;
  [[flat]] float aspect;
  [[flat]] int has_back_link;
  [[flat]] int is_main_line;
};

struct [[fragment_out]] NodeLinkFragOut {
  [[color(0)]] float4 color;
};

struct [[resource_table]] NodeLinkSRT {
  [[push_constant]] float4x4 ModelViewProjectionMatrix;
  [[storage(0, read)]] NodeLinkData (&link_data_buf)[];
  [[uniform(0)]] NodeLinkUniformData &link_uniforms;
};

GPU_SHADER_CREATE_INFO(gpu_shader_2D_nodelink)
GRAPHIC_SOURCE("gpu_shader_2D_nodelink.glsl")
VERTEX_FUNCTION("nodelink_vertex")
FRAGMENT_FUNCTION("nodelink_fragment")
VERTEX_OUT_SRT(NodeLinkVertOut)
SRT_DATA(NodeLinkVertIn)
SRT_DATA(NodeLinkFragOut)
SRT_DATA(NodeLinkSRT)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
