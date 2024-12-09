/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "overlay_common_info.hh"

GPU_SHADER_CREATE_INFO(overlay_frag_output)
FRAGMENT_OUT(0, VEC4, fragColor)
FRAGMENT_OUT(1, VEC4, lineOutput)
GPU_SHADER_CREATE_END()

GPU_SHADER_INTERFACE_INFO(overlay_armature_wire_iface)
FLAT(VEC4, finalColor)
FLAT(VEC2, edgeStart)
NO_PERSPECTIVE(VEC2, edgePos)
GPU_SHADER_INTERFACE_END()

GPU_SHADER_CREATE_INFO(overlay_armature_common)
PUSH_CONSTANT(FLOAT, alpha)
ADDITIONAL_INFO(draw_view)
GPU_SHADER_CREATE_END()

/* -------------------------------------------------------------------- */
/** \name Armature Sphere
 * \{ */

GPU_SHADER_CREATE_INFO(overlay_armature_sphere_outline)
DO_STATIC_COMPILATION()
VERTEX_IN(0, VEC2, pos)
VERTEX_OUT(overlay_armature_wire_iface)
VERTEX_SOURCE("overlay_armature_sphere_outline_vert.glsl")
FRAGMENT_SOURCE("overlay_armature_wire_frag.glsl")
ADDITIONAL_INFO(overlay_frag_output)
ADDITIONAL_INFO(overlay_armature_common)
ADDITIONAL_INFO(draw_globals)
STORAGE_BUF(0, READ, mat4, data_buf[])
DEFINE_VALUE("inst_obmat", "data_buf[gl_InstanceID]")
GPU_SHADER_CREATE_END()

OVERLAY_INFO_VARIATIONS(overlay_armature_sphere_outline)

GPU_SHADER_INTERFACE_INFO(overlay_armature_sphere_solid_iface)
FLAT(VEC3, finalStateColor)
FLAT(VEC3, finalBoneColor)
FLAT(MAT4, sphereMatrix)
SMOOTH(VEC3, viewPosition)
GPU_SHADER_INTERFACE_END()

GPU_SHADER_CREATE_INFO(overlay_armature_sphere_solid)
DO_STATIC_COMPILATION()
VERTEX_IN(0, VEC2, pos)
/* Per instance. */
VERTEX_IN(1, VEC4, color)
DEPTH_WRITE(DepthWrite::GREATER)
VERTEX_OUT(overlay_armature_sphere_solid_iface)
VERTEX_SOURCE("overlay_armature_sphere_solid_vert.glsl")
FRAGMENT_SOURCE("overlay_armature_sphere_solid_frag.glsl")
ADDITIONAL_INFO(overlay_frag_output)
ADDITIONAL_INFO(overlay_armature_common)
ADDITIONAL_INFO(draw_globals)
DEPTH_WRITE(DepthWrite::ANY)
STORAGE_BUF(0, READ, mat4, data_buf[])
DEFINE_VALUE("inst_obmat", "data_buf[gl_InstanceID]")
GPU_SHADER_CREATE_END()

OVERLAY_INFO_VARIATIONS(overlay_armature_sphere_solid)

/** \} */

/* -------------------------------------------------------------------- */
/** \name Armature Shapes
 * \{ */

GPU_SHADER_NAMED_INTERFACE_INFO(overlay_armature_shape_outline_iface, geom_in)
SMOOTH(VEC4, pPos)
SMOOTH(VEC3, vPos)
SMOOTH(VEC2, ssPos)
SMOOTH(VEC4, vColSize)
GPU_SHADER_NAMED_INTERFACE_END(geom_in)
GPU_SHADER_NAMED_INTERFACE_INFO(overlay_armature_shape_outline_flat_iface, geom_flat_in)
FLAT(INT, inverted)
GPU_SHADER_NAMED_INTERFACE_END(geom_flat_in)

GPU_SHADER_INTERFACE_INFO(overlay_armature_shape_outline_no_geom_iface)
FLAT(VEC4, finalColor)
FLAT(VEC2, edgeStart)
NO_PERSPECTIVE(VEC2, edgePos)
GPU_SHADER_INTERFACE_END()

GPU_SHADER_CREATE_INFO(overlay_armature_shape_outline)
DO_STATIC_COMPILATION()
STORAGE_BUF_FREQ(0, READ, float, pos[], GEOMETRY)
STORAGE_BUF(1, READ, mat4, data_buf[])
PUSH_CONSTANT(IVEC2, gpu_attr_0)
VERTEX_OUT(overlay_armature_shape_outline_no_geom_iface)
VERTEX_SOURCE("overlay_armature_shape_outline_next_vert.glsl")
FRAGMENT_SOURCE("overlay_armature_wire_frag.glsl")
ADDITIONAL_INFO(overlay_frag_output)
ADDITIONAL_INFO(overlay_armature_common)
ADDITIONAL_INFO(gpu_index_buffer_load)
ADDITIONAL_INFO(draw_globals)
GPU_SHADER_CREATE_END()

OVERLAY_INFO_VARIATIONS(overlay_armature_shape_outline)

GPU_SHADER_INTERFACE_INFO(overlay_armature_shape_solid_iface)
SMOOTH(VEC4, finalColor)
FLAT(INT, inverted)
GPU_SHADER_INTERFACE_END()

GPU_SHADER_CREATE_INFO(overlay_armature_shape_solid)
DO_STATIC_COMPILATION()
VERTEX_IN(0, VEC3, pos)
VERTEX_IN(1, VEC3, nor)
/* Per instance. */
DEPTH_WRITE(DepthWrite::GREATER)
VERTEX_OUT(overlay_armature_shape_solid_iface)
VERTEX_SOURCE("overlay_armature_shape_solid_vert.glsl")
FRAGMENT_SOURCE("overlay_armature_shape_solid_frag.glsl")
ADDITIONAL_INFO(overlay_frag_output)
ADDITIONAL_INFO(overlay_armature_common)
ADDITIONAL_INFO(draw_globals)
STORAGE_BUF(0, READ, mat4, data_buf[])
DEFINE_VALUE("inst_obmat", "data_buf[gl_InstanceID]")
GPU_SHADER_CREATE_END()

OVERLAY_INFO_VARIATIONS(overlay_armature_shape_solid)

GPU_SHADER_INTERFACE_INFO(overlay_armature_shape_wire_iface)
FLAT(VEC4, finalColor)
FLAT(FLOAT, wire_width)
NO_PERSPECTIVE(FLOAT, edgeCoord)
GPU_SHADER_INTERFACE_END()

GPU_SHADER_CREATE_INFO(overlay_armature_shape_wire)
DO_STATIC_COMPILATION()
/* TODO(pragma37): Remove. */
DEFINE("NO_GEOM")
PUSH_CONSTANT(BOOL, do_smooth_wire)
STORAGE_BUF_FREQ(0, READ, float, pos[], GEOMETRY)
STORAGE_BUF(1, READ, mat4, data_buf[])
PUSH_CONSTANT(IVEC2, gpu_attr_0)
DEFINE_VALUE("inst_obmat", "data_buf[gl_InstanceID]")
VERTEX_OUT(overlay_armature_shape_wire_iface)
VERTEX_SOURCE("overlay_armature_shape_wire_next_vert.glsl")
FRAGMENT_SOURCE("overlay_armature_shape_wire_frag.glsl")
TYPEDEF_SOURCE("overlay_shader_shared.h")
ADDITIONAL_INFO(overlay_frag_output)
ADDITIONAL_INFO(overlay_armature_common)
ADDITIONAL_INFO(gpu_index_buffer_load)
ADDITIONAL_INFO(draw_globals)
GPU_SHADER_CREATE_END()

OVERLAY_INFO_VARIATIONS(overlay_armature_shape_wire)

/** \} */

/* -------------------------------------------------------------------- */
/** \name Armature Envelope
 * \{ */

GPU_SHADER_CREATE_INFO(overlay_armature_envelope_outline)
DO_STATIC_COMPILATION()
TYPEDEF_SOURCE("overlay_shader_shared.h")
VERTEX_IN(0, VEC2, pos0)
VERTEX_IN(1, VEC2, pos1)
VERTEX_IN(2, VEC2, pos2)
VERTEX_OUT(overlay_armature_wire_iface)
VERTEX_SOURCE("overlay_armature_envelope_outline_vert.glsl")
FRAGMENT_SOURCE("overlay_armature_wire_frag.glsl")
ADDITIONAL_INFO(overlay_frag_output)
ADDITIONAL_INFO(overlay_armature_common)
ADDITIONAL_INFO(draw_globals)
STORAGE_BUF(0, READ, BoneEnvelopeData, data_buf[])
DEFINE_VALUE("headSphere", "data_buf[gl_InstanceID].head_sphere")
DEFINE_VALUE("tailSphere", "data_buf[gl_InstanceID].tail_sphere")
DEFINE_VALUE("outlineColorSize", "data_buf[gl_InstanceID].bone_color_and_wire_width")
DEFINE_VALUE("xAxis", "data_buf[gl_InstanceID].x_axis.xyz")
GPU_SHADER_CREATE_END()

OVERLAY_INFO_VARIATIONS(overlay_armature_envelope_outline)

GPU_SHADER_INTERFACE_INFO(overlay_armature_envelope_solid_iface)
FLAT(VEC3, finalStateColor)
FLAT(VEC3, finalBoneColor)
SMOOTH(VEC3, normalView)
GPU_SHADER_INTERFACE_END()

GPU_SHADER_CREATE_INFO(overlay_armature_envelope_solid)
DO_STATIC_COMPILATION()
TYPEDEF_SOURCE("overlay_shader_shared.h")
VERTEX_IN(0, VEC3, pos)
VERTEX_OUT(overlay_armature_envelope_solid_iface)
PUSH_CONSTANT(BOOL, isDistance)
VERTEX_SOURCE("overlay_armature_envelope_solid_vert.glsl")
FRAGMENT_SOURCE("overlay_armature_envelope_solid_frag.glsl")
ADDITIONAL_INFO(overlay_frag_output)
ADDITIONAL_INFO(overlay_armature_common)
ADDITIONAL_INFO(draw_globals)
STORAGE_BUF(0, READ, BoneEnvelopeData, data_buf[])
DEFINE_VALUE("headSphere", "data_buf[gl_InstanceID].head_sphere")
DEFINE_VALUE("tailSphere", "data_buf[gl_InstanceID].tail_sphere")
DEFINE_VALUE("xAxis", "data_buf[gl_InstanceID].x_axis.xyz")
DEFINE_VALUE("stateColor", "data_buf[gl_InstanceID].state_color.xyz")
DEFINE_VALUE("boneColor", "data_buf[gl_InstanceID].bone_color_and_wire_width.xyz")
GPU_SHADER_CREATE_END()

OVERLAY_INFO_VARIATIONS(overlay_armature_envelope_solid)

/** \} */

/* -------------------------------------------------------------------- */
/** \name Armature Stick
 * \{ */

GPU_SHADER_INTERFACE_INFO(overlay_armature_stick_iface)
NO_PERSPECTIVE(FLOAT, colorFac)
FLAT(VEC4, finalWireColor)
FLAT(VEC4, finalInnerColor)
GPU_SHADER_INTERFACE_END()

GPU_SHADER_CREATE_INFO(overlay_armature_stick_base)
TYPEDEF_SOURCE("overlay_shader_shared.h")
/* Bone aligned screen space. */
VERTEX_IN(0, VEC2, pos)
VERTEX_IN(1, INT, vclass)
DEFINE_VALUE("do_wire", "(wireColor.a > 0.0)")
VERTEX_OUT(overlay_armature_stick_iface)
VERTEX_SOURCE("overlay_armature_stick_vert.glsl")
FRAGMENT_SOURCE("overlay_armature_stick_frag.glsl")
ADDITIONAL_INFO(overlay_frag_output)
ADDITIONAL_INFO(overlay_armature_common)
ADDITIONAL_INFO(draw_resource_handle_new)
ADDITIONAL_INFO(draw_globals)
STORAGE_BUF(0, READ, BoneStickData, data_buf[])
DEFINE_VALUE("boneStart", "data_buf[gl_InstanceID].bone_start.xyz")
DEFINE_VALUE("boneEnd", "data_buf[gl_InstanceID].bone_end.xyz")
DEFINE_VALUE("wireColor", "data_buf[gl_InstanceID].wire_color")
DEFINE_VALUE("boneColor", "data_buf[gl_InstanceID].bone_color")
DEFINE_VALUE("headColor", "data_buf[gl_InstanceID].head_color")
DEFINE_VALUE("tailColor", "data_buf[gl_InstanceID].tail_color")
DEFINE_VALUE("flag", "vclass")
GPU_SHADER_CREATE_END()

OVERLAY_INFO_VARIATIONS_MODELMAT(overlay_armature_stick, overlay_armature_stick_base)

/** \} */

/* -------------------------------------------------------------------- */
/** \name Armature Degrees of Freedom
 * \{ */

GPU_SHADER_CREATE_INFO(overlay_armature_dof)
DO_STATIC_COMPILATION()
TYPEDEF_SOURCE("overlay_shader_shared.h")
VERTEX_IN(0, VEC2, pos)
VERTEX_OUT(overlay_armature_wire_iface)
VERTEX_SOURCE("overlay_armature_dof_vert.glsl")
FRAGMENT_SOURCE("overlay_armature_dof_solid_frag.glsl")
ADDITIONAL_INFO(overlay_frag_output)
ADDITIONAL_INFO(overlay_armature_common)
ADDITIONAL_INFO(draw_globals)
STORAGE_BUF(0, READ, ExtraInstanceData, data_buf[])
/*TODO(pragma37): Remove?*/
DEFINE_VALUE("inst_obmat", "data_buf[gl_InstanceID].object_to_world_")
DEFINE_VALUE("color", "data_buf[gl_InstanceID].color_")
GPU_SHADER_CREATE_END()

OVERLAY_INFO_CLIP_VARIATION(overlay_armature_dof)

/** \} */

/* -------------------------------------------------------------------- */
/** \name Armature Wire
 * \{ */

GPU_SHADER_CREATE_INFO(overlay_armature_wire_base)
TYPEDEF_SOURCE("overlay_shader_shared.h")
PUSH_CONSTANT(FLOAT, alpha)
VERTEX_OUT(overlay_armature_wire_iface)
VERTEX_SOURCE("overlay_armature_wire_vert.glsl")
FRAGMENT_SOURCE("overlay_armature_wire_frag.glsl")
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(overlay_frag_output)
ADDITIONAL_INFO(draw_resource_handle_new)
ADDITIONAL_INFO(draw_globals)
STORAGE_BUF(0, READ, VertexData, data_buf[])
DEFINE_VALUE("pos", "data_buf[gl_VertexID].pos_.xyz")
DEFINE_VALUE("color", "data_buf[gl_VertexID].color_")
GPU_SHADER_CREATE_END()

OVERLAY_INFO_VARIATIONS_MODELMAT(overlay_armature_wire, overlay_armature_wire_base)

/** \} */
