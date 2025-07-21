/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_material_info.hh"

VERTEX_SHADER_CREATE_INFO(eevee_clip_plane)
VERTEX_SHADER_CREATE_INFO(eevee_geom_curves)

#include "draw_curves_lib.glsl"
#include "draw_model_lib.glsl"
#include "eevee_attributes_curves_lib.glsl"
#include "eevee_nodetree_vert_lib.glsl"
#include "eevee_reverse_z_lib.glsl"
#include "eevee_surf_lib.glsl"
#include "eevee_velocity_lib.glsl"

void main()
{
  DRW_VIEW_FROM_RESOURCE_ID;
#ifdef MAT_SHADOW
  shadow_viewport_layer_set(int(drw_view_id), int(render_view_buf[drw_view_id].viewport_index));
#endif

  init_interface();

  const curves::Point ls_pt = curves::point_get(uint(gl_VertexID));
  const curves::Point ws_pt = curves::object_to_world(ls_pt, drw_modelmat());
  interp.P = curves::shape_point_get(
      ws_pt, drw_world_incident_vector(ws_pt.P), curve_interp.binormal);
  interp.N = cross(ws_pt.T, curve_interp.binormal);
  curve_interp.tangent = ws_pt.T;
  curve_interp.time = ws_pt.time;
  curve_interp.thickness = ws_pt.radius;
  curve_interp.time_width = ws_pt.cylinder_time;
  curve_interp.point_id = ws_pt.point_id;
  curve_interp_flat.strand_id = ws_pt.curve_id;

#ifdef MAT_VELOCITY
  /* Due to the screen space nature of the vertex positioning, we compute only the motion of curve
   * strand, not its cylinder. Otherwise we would add the rotation velocity. */
  int vert_idx = ws_pt.point_id;
  float3 prv, nxt;
  float3 pos = ws_pt.P;
  velocity_local_pos_get(pos, vert_idx, prv, nxt);
  /* FIXME(fclem): Evaluating before displacement avoid displacement being treated as motion but
   * ignores motion from animated displacement. Supporting animated displacement motion vectors
   * would require evaluating the node-tree multiple time with different node-tree UBOs evaluated
   * at different times, but also with different attributes (maybe we could assume static attribute
   * at least). */
  velocity_vertex(prv, pos, nxt, motion.prev, motion.next);
#endif

  init_globals();
  attrib_load();

  interp.P += nodetree_displacement();

#ifdef MAT_CLIP_PLANE
  clip_interp.clip_distance = dot(clip_plane.plane, float4(interp.P, 1.0f));
#endif

#ifdef MAT_SHADOW
  float3 vs_P = drw_point_world_to_view(interp.P);
  ShadowRenderView view = render_view_buf[drw_view_id];
  shadow_clip.position = shadow_position_vector_get(vs_P, view);
  shadow_clip.vector = shadow_clip_vector_get(vs_P, view.clip_distance_inv);
#endif

  gl_Position = reverse_z::transform(drw_point_world_to_homogenous(interp.P));
}
