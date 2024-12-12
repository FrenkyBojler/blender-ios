/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#include "overlay_next_private.hh"

namespace blender::draw::overlay {

ShaderModule *ShaderModule::g_shader_modules[2][2] = {{nullptr}};

ShaderModule::ShaderPtr ShaderModule::shader_clippable(const char *create_info_name)
{
  std::string name = create_info_name;

  if (clipping_enabled_) {
    name += "_clipped";
  }

  return ShaderPtr(GPU_shader_create_from_info_name(name.c_str()));
}

ShaderModule::ShaderPtr ShaderModule::shader_selectable(const char *create_info_name)
{
  std::string name = create_info_name;

  if (selection_type_ != SelectionType::DISABLED) {
    name += "_selectable";
  }

  if (clipping_enabled_) {
    name += "_clipped";
  }

  return ShaderPtr(GPU_shader_create_from_info_name(name.c_str()));
}

ShaderModule::ShaderPtr ShaderModule::shader_selectable_no_clip(const char *create_info_name)
{
  std::string name = create_info_name;

  if (selection_type_ != SelectionType::DISABLED) {
    name += "_selectable";
  }

  return ShaderPtr(GPU_shader_create_from_info_name(name.c_str()));
}

using namespace blender::gpu::shader;

ShaderModule::ShaderModule(const SelectionType selection_type, const bool clipping_enabled)
    : selection_type_(selection_type), clipping_enabled_(clipping_enabled)
{
  /** Shaders */
  attribute_viewer_mesh = shader_clippable("overlay_viewer_attribute_mesh");
  attribute_viewer_pointcloud = shader_clippable("overlay_viewer_attribute_pointcloud");
  attribute_viewer_curve = shader_clippable("overlay_viewer_attribute_curve");
  attribute_viewer_curves = shader_clippable("overlay_viewer_attribute_curves");

  armature_degrees_of_freedom = shader_clippable("overlay_armature_dof");

  curve_edit_points = shader_clippable("overlay_edit_curves_point");
  curve_edit_line = shader_clippable("overlay_edit_particle_strand");

  extra_point = shader_selectable("overlay_extra_point");

  grid_background = shader("overlay_grid_background");
  grid_image = shader("overlay_grid_image");

  legacy_curve_edit_wires = shader_clippable("overlay_edit_curve_wire");
  legacy_curve_edit_points = shader_clippable("overlay_edit_curve_point");

  mesh_analysis = shader_clippable("overlay_edit_mesh_analysis");
  mesh_edit_face = shader_clippable("overlay_edit_mesh_face");
  mesh_edit_vert = shader_clippable("overlay_edit_mesh_vert");
  mesh_edit_depth = shader_clippable("overlay_edit_mesh_depth");
  mesh_edit_skin_root = shader_clippable("overlay_edit_mesh_skin_root");

  mesh_face_normal = shader_clippable("overlay_mesh_face_normal");
  mesh_face_normal_subdiv = shader_clippable("overlay_mesh_face_normal_subdiv");
  mesh_loop_normal = shader_clippable("overlay_mesh_loop_normal");
  mesh_loop_normal_subdiv = shader_clippable("overlay_mesh_loop_normal_subdiv");
  mesh_vert_normal = shader_clippable("overlay_mesh_vert_normal");

  outline_prepass_mesh = shader_clippable("overlay_outline_prepass_mesh");
  outline_prepass_curves = shader_clippable("overlay_outline_prepass_curves");
  outline_prepass_pointcloud = shader_clippable("overlay_outline_prepass_pointcloud");
  outline_prepass_gpencil = shader_clippable("overlay_outline_prepass_gpencil");

  particle_edit_vert = shader_clippable("overlay_edit_particle_point");
  particle_edit_edge = shader_clippable("overlay_edit_particle_strand");

  paint_region_edge = shader_clippable("overlay_paint_wire");
  paint_region_face = shader_clippable("overlay_paint_face");
  paint_region_vert = shader_clippable("overlay_paint_point");
  paint_texture = shader_clippable("overlay_paint_texture");
  paint_weight = shader_clippable("overlay_paint_weight");
  paint_weight_fake_shading = shader_clippable("overlay_paint_weight_fake_shading");

  sculpt_mesh = shader_clippable("overlay_sculpt_mask");
  sculpt_curves = shader_clippable("overlay_sculpt_curves_selection");
  sculpt_curves_cage = shader_clippable("overlay_sculpt_curves_cage");

  uv_analysis_stretch_angle = shader("overlay_edit_uv_stretching_angle");
  uv_analysis_stretch_area = shader("overlay_edit_uv_stretching_area");
  uv_edit_vert = shader("overlay_edit_uv_verts");
  uv_edit_face = shader("overlay_edit_uv_faces");
  uv_edit_facedot = shader("overlay_edit_uv_face_dots");
  uv_image_borders = shader("overlay_edit_uv_tiled_image_borders");
  uv_brush_stencil = shader("overlay_edit_uv_stencil_image");
  uv_paint_mask = shader("overlay_edit_uv_mask_image");

  xray_fade = shader("overlay_xray_fade");

  /** Selectable Shaders */

  armature_envelope_fill = shader_selectable("overlay_armature_envelope_solid");
  armature_envelope_outline = shader_selectable("overlay_armature_envelope_outline");
  armature_shape_outline = shader_selectable("overlay_armature_shape_outline");
  armature_shape_fill = shader_selectable("overlay_armature_shape_solid");
  armature_shape_wire = shader_selectable("overlay_armature_shape_wire");
  armature_sphere_outline = shader_selectable("overlay_armature_sphere_outline");
  armature_sphere_fill = shader_selectable("overlay_armature_sphere_solid");
  armature_stick = shader_selectable("overlay_armature_stick");
  armature_wire = shader_selectable("overlay_armature_wire");

  facing = shader_clippable("overlay_facing");

  fluid_grid_lines_flags = shader_selectable_no_clip("overlay_volume_gridlines_flags");
  fluid_grid_lines_flat = shader_selectable_no_clip("overlay_volume_gridlines_flat");
  fluid_grid_lines_range = shader_selectable_no_clip("overlay_volume_gridlines_range");
  fluid_velocity_streamline = shader_selectable_no_clip("overlay_volume_velocity_streamline");
  fluid_velocity_mac = shader_selectable_no_clip("overlay_volume_velocity_mac");
  fluid_velocity_needle = shader_selectable_no_clip("overlay_volume_velocity_needle");

  extra_shape = shader_selectable("overlay_extra");
  extra_wire = shader_selectable("overlay_extra_wire");
  extra_wire_object = shader_selectable("overlay_extra_wire_object");

  extra_loose_points = shader_selectable("overlay_extra_loose_point");

  lattice_points = shader_clippable("overlay_edit_lattice_point");

  lattice_wire = shader_clippable("overlay_edit_lattice_wire");

  extra_grid = shader_selectable("overlay_extra_grid");

  extra_ground_line = shader_selectable("overlay_extra_groundline");

  image_plane = shader_selectable("overlay_image");

  image_plane_depth_bias = shader_selectable("overlay_image_depth_bias");

  light_spot_cone = shader_clippable("overlay_extra_spot_cone");

  particle_dot = shader_selectable("overlay_particle_dot");

  particle_shape = shader_selectable("overlay_particle_shape");

  particle_hair = shader_selectable("overlay_particle_hair");

  uniform_color = shader_clippable("overlay_uniform_color");

  wireframe_mesh = shader_selectable("overlay_wireframe");

  wireframe_points = shader_selectable("overlay_wireframe_points");

  wireframe_curve = shader_selectable("overlay_wireframe_curve");
}

ShaderModule &ShaderModule::module_get(SelectionType selection_type, bool clipping_enabled)
{
  int selection_index = selection_type == SelectionType::DISABLED ? 0 : 1;
  ShaderModule *&g_shader_module = g_shader_modules[selection_index][clipping_enabled];
  if (g_shader_module == nullptr) {
    /* TODO(@fclem) thread-safety. */
    g_shader_module = new ShaderModule(selection_type, clipping_enabled);
  }
  return *g_shader_module;
}

void ShaderModule::module_free()
{
  for (int i : IndexRange(2)) {
    for (int j : IndexRange(2)) {
      if (g_shader_modules[i][j] != nullptr) {
        /* TODO(@fclem) thread-safety. */
        delete g_shader_modules[i][j];
        g_shader_modules[i][j] = nullptr;
      }
    }
  }
}

}  // namespace blender::draw::overlay
