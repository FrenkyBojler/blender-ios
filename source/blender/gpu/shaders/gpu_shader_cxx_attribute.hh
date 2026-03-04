/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * BSL attributes.
 *
 * Define them as standard attribute with similar placement to trigger compiler warning about typos
 * and error for misplacement.
 *
 * Actual implementation is done through the shader_tool and the attributes are not present in
 * final shader code.
 */

#pragma once

#ifdef _MSC_VER
/* MSVC doesn't support the same attribute multiple time (fixed in later versions). */
#  define cxx_symbol_attr
#else
/* Placeholder attribute for symbols. */
#  define cxx_symbol_attr maybe_unused
#endif

/* Placeholder attribute for control flow. */
#define cxx_flow_attr likely

/* Specify a function is a compute shader entry point. */
#define compute cxx_symbol_attr
/* Specify a function is a vertex shader entry point. */
#define vertex cxx_symbol_attr
/* Specify a function is a fragment shader entry point. */
#define fragment cxx_symbol_attr
/* Set compute shader workgroup size. */
#define local_size(...) cxx_symbol_attr
/* Request performing fragment tests before the fragment function executes. */
#define early_fragment_tests cxx_symbol_attr

/* In a compute function, specify an input variable containing the 3-dimensional index of the local
 * work invocation within the work group that the current shader is executing in. */
#define local_invocation_id cxx_symbol_attr
/* In a compute function, specify a derived input variable containing the 3-dimensional index of
 * the work invocation within the global work group that the current shader is executing on. The
 * value is equal to work_group_id * work_group_size + local_invocation_id.  */
#define global_invocation_id cxx_symbol_attr
/* In a compute function, specify an 1-dimensional linearized index of the work invocation within
 * the work group that the current shader is executing on. */
#define local_invocation_index cxx_symbol_attr
/* In a compute function, specify an input variable containing the 3-dimensional index of the
 * global work group that the current compute shader invocation is executing within. */
#define work_group_id cxx_symbol_attr
/* In a compute function, specify an input variable containing the total number of work groups that
 * will execute for the current compute shader dispatch. */
#define num_work_groups cxx_symbol_attr

/* Specify a vertex attribute. */
#define attribute(slot) cxx_symbol_attr
/* Vertex attribute interpolation modes. */
#define flat cxx_symbol_attr
#define smooth cxx_symbol_attr
#define no_perspective cxx_symbol_attr

/* Vertex shader output position. */
#define position cxx_symbol_attr
/* Vertex shader output point size. */
#define point_size cxx_symbol_attr
/* Vertex shader output, distance from vertex to clipping plane. */
#define clip_distance cxx_symbol_attr
/* The render target array index. */
#define layer cxx_symbol_attr
/* The viewport (and scissor rectangle) index value of the primitive. */
#define viewport_index cxx_symbol_attr

/* Vertex shader input vertex index, which includes the base vertex if one is specified. */
#define vertex_id cxx_symbol_attr
/* Vertex shader input instance index, which includes the base instance if one is specified. */
#define instance_id cxx_symbol_attr
/* Vertex shader input base instance value added to each instance identifier before reading
 * per-instance data. */
#define base_instance cxx_symbol_attr

#define frag_coord cxx_symbol_attr
#define point_coord cxx_symbol_attr
#define front_facing cxx_symbol_attr

/* Fragment shader output. */
#define frag_color(slot) cxx_symbol_attr
/* Fragment shader output. */
#define frag_depth(mode) cxx_symbol_attr
/* Fragment shader output. Set stencil reference value per pixel.
 * Only supported on some platform. Check for compatibility first. */
#define frag_stencil_ref cxx_symbol_attr

/* Graphic pipeline stage in/out. */
#define in cxx_symbol_attr
#define out cxx_symbol_attr

/* Declare a dependency to a legacy create info whose name is the struct member name. */
#define legacy_info cxx_symbol_attr
/* Declare a sampler at the given slot. */
#define sampler(slot) cxx_symbol_attr
/* Declare a uniform buffer at the given slot. */
#define uniform(slot) cxx_symbol_attr
/* Declare a storage buffer at the given slot. */
#define storage(slot, qualifiers) cxx_symbol_attr
/* Declare a storage buffer at the given slot. */
#define image(slot, qualifiers, format) cxx_symbol_attr
#define compilation_constant cxx_symbol_attr
#define specialization_constant cxx_symbol_attr
#define push_constant cxx_symbol_attr
/* Declare a nested resource table member. */
#define resource_table cxx_symbol_attr
/* Only declare the member if cond evaluates to true. */
#define condition(cond) cxx_symbol_attr

/* Make a structure layout or enum shared between CPU and GPU code.
 * Required for structs defining storage and uniform buffer layout. */
#define host_shared

/** Make function callable thought the node-tree code-generating system. */
#define node cxx_symbol_attr

/* Make the branch condition evaluate at compile time. */
#define static_branch cxx_flow_attr
/* Unroll the loop at compile time. */
#define unroll cxx_flow_attr
/**
 * Unroll the loop N time at compile time.
 * IMPORTANT: Will discard any iteration above N.
 */
#define unroll_n(N) cxx_flow_attr
