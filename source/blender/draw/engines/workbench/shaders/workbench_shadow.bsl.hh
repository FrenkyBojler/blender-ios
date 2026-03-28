/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Extrude shadow casters along their silhouette edge.
 * Manifold meshes only generate one quad per silhouette edge.
 * Non-Manifold meshes generate one quad on their non manifold edges (border edges) and two
 * quad on their silhouette edge (non-border edges) which we consider "manifold".
 *
 * This shader uses line adjacency primitive to know the geometric normals of neighbor faces.
 * A quad is generated if the faces on each sides of the edge are not facing the light the same
 * way.
 *
 * This vertex shader emulates a geometry shader. The draw call generate enough triangle for one or
 * two quads per input primitive. Each vertex shader invocation reads the whole input primitive and
 * execute the vertex shader code on each of the input primitive's vertices.
 */

#pragma once

#include "draw_intersect_lib.glsl"
#include "draw_view_infos.hh"
#include "gpu_index_load_infos.hh"

SHADER_LIBRARY_CREATE_INFO(gpu_index_buffer_load)
SHADER_LIBRARY_CREATE_INFO(draw_view)
SHADER_LIBRARY_CREATE_INFO(draw_modelmat)

#include "draw_model_lib.glsl"
#include "draw_view_lib.glsl"
#include "gpu_shader_attribute_load_lib.glsl"
#include "gpu_shader_index_load_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"
#include "workbench_shader_shared.hh"

namespace workbench::shadow {

struct VertIn {
  /* Local position. */
  float3 lP;
};

struct VertOut {
  /* Local position. */
  float3 lP;
  /* Final NDC position. */
  float4 frontPosition;
  float4 backPosition;
};

struct GeomOut {
  float4 gpu_position;
};

struct Resources {
  [[legacy_info]] ShaderCreateInfo gpu_index_buffer_load;
  [[legacy_info]] ShaderCreateInfo draw_view;
  [[legacy_info]] ShaderCreateInfo draw_modelmat;

  /* WORKAROUND: Needed to support OpenSubdiv vertex format. Should be removed. */
  [[push_constant]] const int2 gpu_attr_3;

  [[storage(3, read), frequency(GEOMETRY)]] const float (&pos)[];
  [[uniform(1)]] const ShadowPassData &pass_data;

  [[compilation_constant]] const bool double_manifold;
  [[compilation_constant]] const bool shadow_pass; /* shadow_fail if false. */

  VertIn input_assembly(uint in_vertex_id) const
  {
    uint v_i = gpu_index_load(in_vertex_id);

    VertIn vert_in;
    vert_in.lP = gpu_attr_load_float3(this->pos, this->gpu_attr_3, v_i);
    return vert_in;
  }

  VertOut vertex_main(VertIn vert_in) const
  {
    VertOut vert_out;
    vert_out.lP = vert_in.lP;
    float3 L = this->pass_data.light_direction_ws;

    float3 ws_P = drw_point_object_to_world(vert_in.lP);
    float extrude_distance = 1e5f;
    float L_FP = dot(L, this->pass_data.far_plane.xyz);
    if (L_FP > 0.0f) {
      float signed_distance = dot(this->pass_data.far_plane.xyz, ws_P) -
                              this->pass_data.far_plane.w;
      extrude_distance = -signed_distance / L_FP;
      /* Ensure we don't overlap the far plane. */
      extrude_distance -= 1e-3f;
    }
    vert_out.backPosition = drw_point_world_to_homogenous(ws_P + L * extrude_distance);
    vert_out.frontPosition = drw_point_world_to_homogenous(drw_point_object_to_world(vert_in.lP));
    return vert_out;
  }
};

struct GeometryShaderEmulator {
  /* Result of evaluation. / */
  float4 out_pos;

  void export_vertex(GeomOut geom_out)
  {
    out_pos = geom_out.gpu_position;
#ifdef GPU_METAL
    /* Apply depth bias. Prevents Z-fighting artifacts when fast-math is enabled. */
    out_pos.z += 0.00005f;
#endif
  }

  void emit_strip_vert(const uint strip_index,
                       uint out_vertex_id,
                       uint out_primitive_id,
                       GeomOut geom_out)
  {
    bool is_odd_primitive = (out_primitive_id & 1u) != 0u;
    /* Maps triangle list primitives to triangle strip indices. */
    uint out_strip_index = (is_odd_primitive ? (2u - out_vertex_id) : out_vertex_id) +
                           out_primitive_id;

    if (out_strip_index == strip_index) {
      export_vertex(geom_out);
    }
  }

  void emit_triangle_vert(const uint tri_index, uint out_vertex_id, GeomOut geom_out)
  {
    if (out_vertex_id == tri_index) {
      export_vertex(geom_out);
    }
  }

  void extrude_edge(
      bool invert, VertOut geom_in_1, VertOut geom_in_2, uint out_vertex_id, uint out_primitive_id)
  {
    /* Reverse order if back-facing the light. */
    if (invert) {
      VertOut geom_in_tmp = geom_in_1;
      geom_in_1 = geom_in_2;
      geom_in_2 = geom_in_tmp;
    }

    GeomOut geom_out;
    geom_out.gpu_position = geom_in_2.frontPosition;
    emit_strip_vert(0, out_vertex_id, out_primitive_id, geom_out);

    geom_out.gpu_position = geom_in_1.frontPosition;
    emit_strip_vert(1, out_vertex_id, out_primitive_id, geom_out);

    geom_out.gpu_position = geom_in_2.backPosition;
    emit_strip_vert(2, out_vertex_id, out_primitive_id, geom_out);

    geom_out.gpu_position = geom_in_1.backPosition;
    emit_strip_vert(3, out_vertex_id, out_primitive_id, geom_out);
  }

  void emit_cap(bool front,
                bool invert,
                VertOut geom_in_0,
                VertOut geom_in_1,
                VertOut geom_in_2,
                uint out_vertex_id)
  {
    if (invert == front) {
      VertOut geom_in_tmp = geom_in_1;
      geom_in_1 = geom_in_2;
      geom_in_2 = geom_in_tmp;
    }

    GeomOut geom_out;
    geom_out.gpu_position = front ? geom_in_0.frontPosition : geom_in_0.backPosition;
    emit_triangle_vert(0, out_vertex_id, geom_out);

    geom_out.gpu_position = front ? geom_in_1.frontPosition : geom_in_1.backPosition;
    emit_triangle_vert(1, out_vertex_id, geom_out);

    geom_out.gpu_position = front ? geom_in_2.frontPosition : geom_in_2.backPosition;
    emit_triangle_vert(2, out_vertex_id, geom_out);
  }

  void geometry_main([[resource_table]] const Resources &srt,
                     VertOut geom_in[4],
                     uint out_vertex_id,
                     uint out_primitive_id,
                     uint out_invocation_id)
  {
    float3 v10 = geom_in[0].lP - geom_in[1].lP;
    float3 v12 = geom_in[2].lP - geom_in[1].lP;
    float3 v13 = geom_in[3].lP - geom_in[1].lP;

    float3 n1 = cross(v12, v10);
    float3 n2 = cross(v13, v12);

#ifdef DEGENERATE_TRIS_WORKAROUND
    /* Check if area is null */
    float2 faces_area = float2(length_squared(n1), length_squared(n2));
    bool2 degen_faces = lessThan(abs(faces_area), float2(DEGENERATE_TRIS_AREA_THRESHOLD));

    /* Both triangles are degenerate, abort. */
    if (all(degen_faces)) {
      return;
    }
#endif

    float3 ls_light_direction = drw_normal_world_to_object(
        float3(srt.pass_data.light_direction_ws));

    float2 facing = float2(dot(n1, ls_light_direction), dot(n2, ls_light_direction));

    bool2 backface = greaterThan(facing, float2(0.0f));

    /* WATCH: maybe unpredictable in some cases. */
    bool is_manifold = any(notEqual(geom_in[0].lP, geom_in[3].lP));

#ifdef DEGENERATE_TRIS_WORKAROUND
    if (srt.double_manifold == false) [[static_branch]] {
      /* If the mesh is known to be manifold and we don't use double count,
       * only create an quad if the we encounter a facing geom. */
      if ((degen_faces.x && backface.y) || (degen_faces.y && backface.x)) {
        return;
      }
    }

    /* If one of the 2 triangles is degenerate, replace edge by a non-manifold one. */
    backface.x = (degen_faces.x) ? !backface.y : backface.x;
    backface.y = (degen_faces.y) ? !backface.x : backface.y;
    is_manifold = (any(degen_faces)) ? false : is_manifold;
#endif

    /* If both faces face the same direction it's not an outline edge. */
    if (backface.x == backface.y) {
      return;
    }

    if (srt.double_manifold) [[static_branch]] {
      if (out_invocation_id != 0u && !is_manifold) {
        /* Only Increment/Decrement twice for manifold edges. */
        return;
      }
    }

    extrude_edge(backface.x, geom_in[1], geom_in[2], out_vertex_id, out_primitive_id);
  }

  void geometry_main_caps([[resource_table]] const Resources &srt,
                          VertOut geom_in[3],
                          uint out_vertex_id,
                          uint out_invocation_id)
  {
    float3 v10 = geom_in[0].lP - geom_in[1].lP;
    float3 v12 = geom_in[2].lP - geom_in[1].lP;

    float3 Ng = cross(v12, v10);

    float3 ls_light_direction = drw_normal_world_to_object(
        float3(srt.pass_data.light_direction_ws));

    float facing = dot(Ng, ls_light_direction);

    bool backface = facing > 0.0f;

    bool invert = false;
    bool is_manifold = true;
    if (srt.double_manifold) [[static_branch]] {
      /* In case of non manifold geom, we only increase/decrease
       * the stencil buffer by one but do every faces as they were facing the light. */
      invert = backface;
      is_manifold = false;
    }

    if (!is_manifold || !backface) {
      bool do_front = out_invocation_id == 0;
      emit_cap(do_front, invert, geom_in[0], geom_in[1], geom_in[2], out_vertex_id);
    }
  }
};

[[vertex]]
void vert_main([[resource_table]] const Resources &srt,
               [[vertex_id]] const int vert_id,
               [[position]] float4 &out_position)
{
  /* Line adjacency primitive. */
  constexpr uint input_primitive_vertex_count = 4u;
  /* Triangle list primitive. */
  constexpr uint output_primitive_vertex_count = 3u;
  constexpr uint output_primitive_count = 2u;

  uint output_invocation_count = 1u;
  if (srt.double_manifold) [[static_branch]] {
    output_invocation_count = 2u;
  }

  constexpr uint output_vertex_count_per_invocation = output_primitive_count *
                                                      output_primitive_vertex_count;
  const uint output_vertex_count_per_input_primitive = output_vertex_count_per_invocation *
                                                       output_invocation_count;

  uint in_primitive_id = uint(vert_id) / output_vertex_count_per_input_primitive;
  uint in_primitive_first_vertex = in_primitive_id * input_primitive_vertex_count;

  uint out_vertex_id = uint(vert_id) % output_primitive_vertex_count;
  uint out_primitive_id = (uint(vert_id) / output_primitive_vertex_count) % output_primitive_count;
  uint out_invocation_id = (uint(vert_id) / output_vertex_count_per_invocation) %
                           output_invocation_count;

  VertIn vert_in[input_primitive_vertex_count];
  vert_in[0] = srt.input_assembly(in_primitive_first_vertex + 0u);
  vert_in[1] = srt.input_assembly(in_primitive_first_vertex + 1u);
  vert_in[2] = srt.input_assembly(in_primitive_first_vertex + 2u);
  vert_in[3] = srt.input_assembly(in_primitive_first_vertex + 3u);

  VertOut vert_out[input_primitive_vertex_count];
  vert_out[0] = srt.vertex_main(vert_in[0]);
  vert_out[1] = srt.vertex_main(vert_in[1]);
  vert_out[2] = srt.vertex_main(vert_in[2]);
  vert_out[3] = srt.vertex_main(vert_in[3]);

  GeometryShaderEmulator gs;
  /* Discard by default. */
  gs.out_pos = float4(NAN_FLT);
  gs.geometry_main(srt, vert_out, out_vertex_id, out_primitive_id, out_invocation_id);
  out_position = gs.out_pos;
}

[[vertex]]
void vert_main_caps([[resource_table]] const Resources &srt,
                    [[vertex_id]] const int vert_id,
                    [[position]] float4 &out_position)
{
  /* Triangle list primitive. */
  constexpr uint input_primitive_vertex_count = 3u;
  /* Triangle list primitive. */
  constexpr uint output_primitive_vertex_count = 3u;
  constexpr uint output_primitive_count = 1u;
  constexpr uint output_invocation_count = 2u;

  constexpr uint output_vertex_count_per_invocation = output_primitive_count *
                                                      output_primitive_vertex_count;
  constexpr uint output_vertex_count_per_input_primitive = output_vertex_count_per_invocation *
                                                           output_invocation_count;

  uint in_primitive_id = uint(vert_id) / output_vertex_count_per_input_primitive;
  uint in_primitive_first_vertex = in_primitive_id * input_primitive_vertex_count;

  uint out_vertex_id = uint(vert_id) % output_primitive_vertex_count;
  uint out_invocation_id = (uint(vert_id) / output_vertex_count_per_invocation) %
                           output_invocation_count;

  VertIn vert_in[input_primitive_vertex_count];
  vert_in[0] = srt.input_assembly(in_primitive_first_vertex + 0u);
  vert_in[1] = srt.input_assembly(in_primitive_first_vertex + 1u);
  vert_in[2] = srt.input_assembly(in_primitive_first_vertex + 2u);

  VertOut vert_out[input_primitive_vertex_count];
  vert_out[0] = srt.vertex_main(vert_in[0]);
  vert_out[1] = srt.vertex_main(vert_in[1]);
  vert_out[2] = srt.vertex_main(vert_in[2]);

  GeometryShaderEmulator gs;
  /* Discard by default. */
  gs.out_pos = float4(NAN_FLT);
  gs.geometry_main_caps(srt, vert_out, out_vertex_id, out_invocation_id);
  out_position = gs.out_pos;
}

[[fragment]]
void frag_main()
{
  /* No color output, only depth (line below is implicit). */
  // gl_FragDepth = gl_FragCoord.z;
}

struct FragOut {
  [[frag_color(0)]] float4 color;
};

[[fragment]]
void frag_debug([[resource_table]] const Resources &srt,
                [[front_facing]] const bool front_facing,
                [[out]] FragOut &frag_out)
{
  constexpr float a = 0.1f;

  if (srt.shadow_pass) [[static_branch]] {
    frag_out.color.rgb = front_facing ? float3(a, -a, 0.0f) : float3(-a, a, 0.0f);
  }
  else {
    frag_out.color.rgb = front_facing ? float3(a, a, -a) : float3(-a, -a, a);
  }
  frag_out.color.a = a;
}

#ifndef GLSL_CPP_STUBS
/* clang-format off */
PipelineGraphic pass_manifold_no_caps(         vert_main,      frag_main, Resources{.shadow_pass = true, .double_manifold = false});
PipelineGraphic pass_no_manifold_no_caps(      vert_main,      frag_main, Resources{.shadow_pass = true, .double_manifold = true});
PipelineGraphic fail_manifold_caps(            vert_main_caps, frag_main, Resources{.shadow_pass = false, .double_manifold = false});
PipelineGraphic fail_manifold_no_caps(         vert_main,      frag_main, Resources{.shadow_pass = false, .double_manifold = false});
PipelineGraphic fail_no_manifold_caps(         vert_main_caps, frag_main, Resources{.shadow_pass = false, .double_manifold = true});
PipelineGraphic fail_no_manifold_no_caps(      vert_main,      frag_main, Resources{.shadow_pass = false, .double_manifold = true});
PipelineGraphic pass_manifold_no_caps_debug(   vert_main,      frag_debug, Resources{.shadow_pass = true, .double_manifold = false});
PipelineGraphic pass_no_manifold_no_caps_debug(vert_main,      frag_debug, Resources{.shadow_pass = true, .double_manifold = true});
PipelineGraphic fail_manifold_caps_debug(      vert_main_caps, frag_debug, Resources{.shadow_pass = false, .double_manifold = false});
PipelineGraphic fail_manifold_no_caps_debug(   vert_main,      frag_debug, Resources{.shadow_pass = false, .double_manifold = false});
PipelineGraphic fail_no_manifold_caps_debug(   vert_main_caps, frag_debug, Resources{.shadow_pass = false, .double_manifold = true});
PipelineGraphic fail_no_manifold_no_caps_debug(vert_main,      frag_debug, Resources{.shadow_pass = false, .double_manifold = true});
/* clang-format on */
#endif

namespace visibility {

struct Resources {
  [[compilation_constant]] const bool dynamic_pass_selection;

  [[legacy_info]] ShaderCreateInfo draw_view;
  [[legacy_info]] ShaderCreateInfo draw_view_culling;
  [[storage(0, read)]] const ObjectBounds (&bounds_buf)[];

  [[push_constant]] const int resource_len;
  [[push_constant]] const int view_len;

  [[push_constant]] const bool force_fail_method;
  [[push_constant]] const float3 shadow_direction;

  [[uniform(2)]] const ExtrudedFrustum &extruded_frustum;

  [[storage(1, read_write), condition(!dynamic_pass_selection)]] uint (&visibility_buf)[];

  [[storage(1, read_write), condition(dynamic_pass_selection)]] uint (&pass_visibility_buf)[];
  [[storage(2, read_write), condition(dynamic_pass_selection)]] uint (&fail_visibility_buf)[];

  bool is_visible(IsectBox box)
  {
    for (int i_plane = 0; i_plane < extruded_frustum.planes_count; i_plane++) {
      float4 plane = extruded_frustum.planes[i_plane];
      bool separating_axis = true;
      for (int i_corner = 0; i_corner < 8; i_corner++) {
        float signed_distance = dot(box.corners[i_corner], plane.xyz) - plane.w;
        if (signed_distance <= 0) {
          separating_axis = false;
          break;
        }
      }
      if (separating_axis) {
        return false;
      }
    }
    return true;
  }

  bool intersects_near_plane(IsectBox box)
  {
    float4 near_plane = drw_view_culling().frustum_planes.planes[4];
    bool on_positive_side = false;
    bool on_negative_side = false;

    for (int i_corner = 0; i_corner < 8; i_corner++) {
      for (int i_displace = 0; i_displace < 2; i_displace++) {
        float3 corner = box.corners[i_corner] + (shadow_direction * 1e5f * i_displace);
        float signed_distance = dot(corner, -near_plane.xyz) - near_plane.w;
        if (signed_distance <= 0) {
          on_negative_side = true;
        }
        else {
          on_positive_side = true;
        }
        if (on_negative_side && on_positive_side) {
          return true;
        }
      }
    }

    return false;
  }

  void set_visibility(bool pass, bool fail, uint instance_id)
  {
    if (dynamic_pass_selection) [[static_branch]] {
      if (!pass) {
        atomicAnd(pass_visibility_buf[instance_id / 32u], ~(1u << (instance_id & 31u)));
      }
      if (!fail) {
        atomicAnd(fail_visibility_buf[instance_id / 32u], ~(1u << (instance_id & 31u)));
      }
    }
  }

  void set_visibility(bool visibility, uint instance_id)
  {
    if (dynamic_pass_selection == false) [[static_branch]] {
      if (!visibility) {
        atomicAnd(visibility_buf[instance_id / 32u], ~(1u << (instance_id & 31u)));
      }
    }
  }
};

[[compute, local_size(DRW_VISIBILITY_GROUP_SIZE)]]
void culling_main([[resource_table]] Resources &srt,
                  [[global_invocation_id]] const uint3 global_id)
{
  if (int(global_id.x) >= srt.resource_len) {
    return;
  }

  ObjectBounds bounds = srt.bounds_buf[global_id.x];
  if (!drw_bounds_are_valid(bounds)) {
    /* Invalid bounding box. */
    return;
  }
  IsectBox box = isect_box_setup(bounds.bounding_corners[0].xyz,
                                 bounds.bounding_corners[1].xyz,
                                 bounds.bounding_corners[2].xyz,
                                 bounds.bounding_corners[3].xyz);

  if (srt.dynamic_pass_selection) [[static_branch]] {
    if (srt.is_visible(box)) {
      bool use_fail_pass = srt.force_fail_method || srt.intersects_near_plane(box);
      srt.set_visibility(!use_fail_pass, use_fail_pass, global_id.x);
    }
    else {
      srt.set_visibility(false, false, global_id.x);
    }
  }
  else {
    srt.set_visibility(srt.is_visible(box), global_id.x);
  }
}

#ifndef GLSL_CPP_STUBS
PipelineCompute compute_dynamic_pass_type(culling_main, Resources{.dynamic_pass_selection = true});
PipelineCompute compute_static_pass_type(culling_main, Resources{.dynamic_pass_selection = false});
#endif

}  // namespace visibility

}  // namespace workbench::shadow
