/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_glsl_cpp_stubs.hh"

#  include "draw_object_infos_info.hh"
#  include "draw_view_info.hh"
#  include "gpu_shader_fullscreen_info.hh"

#  include "gpu_index_load_info.hh"
#  include "gpu_shader_create_info.hh"

#  include "overlay_shader_shared.hh"

#  define HAIR_SHADER
#  define DRW_HAIR_INFO

#  define POINTCLOUD_SHADER
#  define DRW_POINTCLOUD_INFO
#endif

#include "overlay_common_info.hh"

/* -------------------------------------------------------------------- */
/** \name Outline Pre-pass
 * \{ */

GPU_SHADER_NAMED_INTERFACE_INFO(overlay_outline_prepass_iface, interp)
FLAT(uint, ob_id)
GPU_SHADER_NAMED_INTERFACE_END(interp)

GPU_SHADER_CREATE_INFO(overlay_outline_prepass)
TYPEDEF_SOURCE("overlay_shader_shared.hh")
PUSH_CONSTANT(bool, is_transform)
VERTEX_OUT(overlay_outline_prepass_iface)
/* Using uint because 16bit uint can contain more ids than int. */
FRAGMENT_OUT(0, uint, out_object_id)
FRAGMENT_SOURCE("overlay_outline_prepass_frag.glsl")
ADDITIONAL_INFO(draw_globals)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(overlay_outline_prepass_mesh)
DO_STATIC_COMPILATION()
VERTEX_IN(0, float3, pos)
VERTEX_SOURCE("overlay_outline_prepass_vert.glsl")
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_modelmat)
ADDITIONAL_INFO(draw_globals)
ADDITIONAL_INFO(draw_object_infos)
ADDITIONAL_INFO(overlay_outline_prepass)
GPU_SHADER_CREATE_END()

OVERLAY_INFO_CLIP_VARIATION(overlay_outline_prepass_mesh)

GPU_SHADER_NAMED_INTERFACE_INFO(overlay_outline_prepass_wire_iface, vert)
FLAT(float3, pos)
GPU_SHADER_NAMED_INTERFACE_END(vert)

GPU_SHADER_CREATE_INFO(overlay_outline_prepass_curves)
DO_STATIC_COMPILATION()
VERTEX_SOURCE("overlay_outline_prepass_curves_vert.glsl")
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_modelmat)
ADDITIONAL_INFO(draw_globals)
ADDITIONAL_INFO(draw_hair)
ADDITIONAL_INFO(draw_object_infos)
ADDITIONAL_INFO(overlay_outline_prepass)
GPU_SHADER_CREATE_END()

OVERLAY_INFO_CLIP_VARIATION(overlay_outline_prepass_curves)

GPU_SHADER_CREATE_INFO(overlay_outline_prepass_wire)
    .do_static_compilation(true)
    .additional_info("overlay_outline_prepass",
                     "draw_object_infos",
                     "draw_mesh",
                     "draw_resource_handle")
    .vertex_in(0, Type::VEC3, "pos")
    .define("USE_GEOM")
    .vertex_out(overlay_outline_prepass_wire_iface)
    .geometry_layout(PrimitiveIn::LINES_ADJACENCY, PrimitiveOut::LINE_STRIP, 2)
    .geometry_out(overlay_outline_prepass_iface)
    .vertex_source("overlay_outline_prepass_vert.glsl")
    .geometry_source("overlay_outline_prepass_geom.glsl");

GPU_SHADER_CREATE_INFO(overlay_outline_prepass_wire_no_geom)
    .metal_backend_only(true)
    .do_static_compilation(true)
    .vertex_in(0, Type::VEC3, "pos")
    .additional_info("overlay_outline_prepass",
                     "draw_object_infos",
                     "draw_mesh",
                     "draw_resource_handle")
    .vertex_source("overlay_outline_prepass_vert_no_geom.glsl");

GPU_SHADER_CREATE_INFO(overlay_outline_prepass_wire_clipped)
    .do_static_compilation(true)
    .additional_info("overlay_outline_prepass_wire", "drw_clipped");

GPU_SHADER_INTERFACE_INFO(overlay_outline_prepass_gpencil_flat_iface, "gp_interp_flat")
    .flat(Type::VEC2, "aspect")
    .flat(Type::VEC4, "sspos1")
    .flat(Type::VEC4, "sspos2")
    .flat(Type::VEC3, "point_length");
GPU_SHADER_INTERFACE_INFO(overlay_outline_prepass_gpencil_noperspective_iface,
                          "gp_interp_noperspective")
    .no_perspective(Type::VEC2, "thickness")
    .no_perspective(Type::FLOAT, "hardness");

GPU_SHADER_CREATE_INFO(overlay_outline_prepass_gpencil)
DO_STATIC_COMPILATION()
TYPEDEF_SOURCE("overlay_shader_shared.hh")
PUSH_CONSTANT(bool, is_transform)
VERTEX_OUT(overlay_outline_prepass_iface)
VERTEX_OUT(overlay_outline_prepass_gpencil_flat_iface)
VERTEX_OUT(overlay_outline_prepass_gpencil_noperspective_iface)
VERTEX_SOURCE("overlay_outline_prepass_gpencil_vert.glsl")
PUSH_CONSTANT(bool, gp_stroke_order3d) /* TODO(fclem): Move to a GPencil object UBO. */
PUSH_CONSTANT(float4, gp_depth_plane)  /* TODO(fclem): Move to a GPencil object UBO. */
/* Using uint because 16bit uint can contain more ids than int. */
FRAGMENT_OUT(0, uint, out_object_id)
FRAGMENT_SOURCE("overlay_outline_prepass_gpencil_frag.glsl")
DEPTH_WRITE(DepthWrite::ANY)
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_modelmat)
ADDITIONAL_INFO(draw_globals)
ADDITIONAL_INFO(draw_gpencil)
ADDITIONAL_INFO(draw_object_infos)
GPU_SHADER_CREATE_END()

OVERLAY_INFO_CLIP_VARIATION(overlay_outline_prepass_gpencil)

GPU_SHADER_CREATE_INFO(overlay_outline_prepass_pointcloud)
DO_STATIC_COMPILATION()
VERTEX_SOURCE("overlay_outline_prepass_pointcloud_vert.glsl")
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_modelmat)
ADDITIONAL_INFO(draw_globals)
ADDITIONAL_INFO(draw_pointcloud)
ADDITIONAL_INFO(draw_object_infos)
ADDITIONAL_INFO(overlay_outline_prepass)
GPU_SHADER_CREATE_END()

OVERLAY_INFO_CLIP_VARIATION(overlay_outline_prepass_pointcloud)

/** \} */

/* -------------------------------------------------------------------- */
/** \name Outline Rendering
 * \{ */

GPU_SHADER_CREATE_INFO(overlay_outline_detect)
DO_STATIC_COMPILATION()
PUSH_CONSTANT(float, alpha_occlu)
PUSH_CONSTANT(bool, is_xray_wires)
PUSH_CONSTANT(bool, do_anti_aliasing)
PUSH_CONSTANT(bool, do_thick_outlines)
SAMPLER(0, usampler2D, outline_id_tx)
SAMPLER(1, sampler2DDepth, outline_depth_tx)
SAMPLER(2, sampler2DDepth, scene_depth_tx)
FRAGMENT_OUT(0, float4, frag_color)
FRAGMENT_OUT(1, float4, line_output)
FRAGMENT_SOURCE("overlay_outline_detect_frag.glsl")
ADDITIONAL_INFO(gpu_fullscreen)
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_globals)
GPU_SHADER_CREATE_END()

/** \} */
