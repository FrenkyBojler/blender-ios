/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#include "overlay_next_private.hh"

namespace blender::draw::overlay {

ShaderModule *ShaderModule::g_shader_modules[2][2] = {{nullptr}};

ShaderModule::ShaderPtr ShaderModule::shader(
    const char *create_info_name,
    const FunctionRef<void(gpu::shader::ShaderCreateInfo &info)> patch)
{
  /* Perform a copy for patching. */
  gpu::shader::ShaderCreateInfo info(create_info_name);
  GPU_shader_create_info_get_unfinalized_copy(create_info_name,
                                              reinterpret_cast<GPUShaderCreateInfo &>(info));

  patch(info);

  if (clipping_enabled_) {
    info.define("USE_WORLD_CLIP_PLANES");
  }

  return ShaderPtr(
      GPU_shader_create_from_info(reinterpret_cast<const GPUShaderCreateInfo *>(&info)));
}

ShaderModule::ShaderPtr ShaderModule::selectable_shader(const char *create_info_name)
{
  /* TODO: This is what it should be like with all variations defined with create infos. */
  // std::string create_info_name = base_create_info;
  // create_info_name += SelectEngineT::shader_suffix;
  // create_info_name += clipping_enabled_ ? "_clipped" : "";
  // this->shader_ = GPU_shader_create_from_info_name(create_info_name.c_str());

  /* WORKAROUND: ... but for now, we have to patch the create info used by the old engine. */

  /* Perform a copy for patching. */
  gpu::shader::ShaderCreateInfo info(create_info_name);
  GPU_shader_create_info_get_unfinalized_copy(create_info_name,
                                              reinterpret_cast<GPUShaderCreateInfo &>(info));

  if (selection_type_ != SelectionType::DISABLED) {
    info.define("SELECT_ENABLE");
    info.depth_write(gpu::shader::DepthWrite::UNCHANGED);
    /* Replace additional info. */
    for (StringRefNull &str : info.additional_infos_) {
      if (str == "draw_modelmat_new") {
        str = "draw_modelmat_new_with_custom_id";
      }
    }
    info.additional_info("select_id_patch");
  }

  if (clipping_enabled_) {
    info.define("USE_WORLD_CLIP_PLANES");
  }

  return ShaderPtr(
      GPU_shader_create_from_info(reinterpret_cast<const GPUShaderCreateInfo *>(&info)));
}

ShaderModule::ShaderPtr ShaderModule::selectable_shader(
    const char *create_info_name,
    const FunctionRef<void(gpu::shader::ShaderCreateInfo &info)> patch)
{
  /* Perform a copy for patching. */
  gpu::shader::ShaderCreateInfo info(create_info_name);
  GPU_shader_create_info_get_unfinalized_copy(create_info_name,
                                              reinterpret_cast<GPUShaderCreateInfo &>(info));

  patch(info);

  if (selection_type_ != SelectionType::DISABLED) {
    info.define("SELECT_ENABLE");
    info.depth_write(gpu::shader::DepthWrite::UNCHANGED);
    /* Replace additional info. */
    for (StringRefNull &str : info.additional_infos_) {
      if (str == "draw_modelmat_new") {
        str = "draw_modelmat_new_with_custom_id";
      }
    }
    info.additional_info("select_id_patch");
  }

  if (clipping_enabled_) {
    info.define("USE_WORLD_CLIP_PLANES");
  }

  return ShaderPtr(
      GPU_shader_create_from_info(reinterpret_cast<const GPUShaderCreateInfo *>(&info)));
}

ShaderModule::ShaderPtr ShaderModule::static_clippable_shader(const char *create_info_name)
{
  std::string name = create_info_name;

  if (clipping_enabled_) {
    name += "_clipped";
  }

  return ShaderPtr(GPU_shader_create_from_info_name(name.c_str()));
}

ShaderModule::ShaderPtr ShaderModule::static_selectable_shader(const char *create_info_name)
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

using namespace blender::gpu::shader;

static void shader_patch_common(gpu::shader::ShaderCreateInfo &info)
{
  info.additional_infos_.clear();
  info.additional_info(
      "draw_view", "draw_modelmat_new", "draw_resource_handle_new", "draw_globals");
}

ShaderModule::ShaderModule(const SelectionType selection_type, const bool clipping_enabled)
    : selection_type_(selection_type), clipping_enabled_(clipping_enabled)
{
  /** Shaders */
  attribute_viewer_mesh = static_clippable_shader("overlay_viewer_attribute_mesh");
  attribute_viewer_pointcloud = static_clippable_shader("overlay_viewer_attribute_pointcloud");
  attribute_viewer_curve = static_clippable_shader("overlay_viewer_attribute_curve");
  attribute_viewer_curves = static_clippable_shader("overlay_viewer_attribute_curves");

  armature_degrees_of_freedom = static_clippable_shader("overlay_armature_dof");

  curve_edit_points = static_clippable_shader("overlay_edit_curves_point");
  curve_edit_line = static_clippable_shader("overlay_edit_particle_strand");

  extra_point = static_selectable_shader("overlay_extra_point");

  grid_background = static_clippable_shader("overlay_grid_background");

  grid_image = static_clippable_shader("overlay_grid_image");

  legacy_curve_edit_wires = static_clippable_shader("overlay_edit_curve_wire");
  legacy_curve_edit_points = static_clippable_shader("overlay_edit_curve_point");

  mesh_analysis = static_clippable_shader("overlay_edit_mesh_analysis");

  mesh_edit_face = static_clippable_shader("overlay_edit_mesh_face");
  mesh_edit_vert = static_clippable_shader("overlay_edit_mesh_vert");
  mesh_edit_depth = static_clippable_shader("overlay_edit_mesh_depth");
  mesh_edit_skin_root = static_clippable_shader("overlay_edit_mesh_skin_root");

  mesh_face_normal = static_clippable_shader("overlay_mesh_face_normal");
  mesh_face_normal_subdiv = static_clippable_shader("overlay_mesh_face_normal_subdiv");
  mesh_loop_normal = static_clippable_shader("overlay_mesh_loop_normal");
  mesh_loop_normal_subdiv = static_clippable_shader("overlay_mesh_loop_normal_subdiv");
  mesh_vert_normal = static_clippable_shader("overlay_mesh_vert_normal");

  outline_prepass_mesh = static_clippable_shader("overlay_outline_prepass_mesh");
  outline_prepass_curves = static_clippable_shader("overlay_outline_prepass_curves");
  outline_prepass_pointcloud = static_clippable_shader("overlay_outline_prepass_pointcloud");
  outline_prepass_gpencil = static_clippable_shader("overlay_outline_prepass_gpencil");

  particle_edit_vert = static_clippable_shader("overlay_edit_particle_point");
  particle_edit_edge = static_clippable_shader("overlay_edit_particle_strand");

  paint_region_edge = static_clippable_shader("overlay_paint_wire");
  paint_region_face = static_clippable_shader("overlay_paint_face");
  paint_region_vert = static_clippable_shader("overlay_paint_point");
  paint_texture = static_clippable_shader("overlay_paint_texture");
  paint_weight = static_clippable_shader("overlay_paint_weight");
  paint_weight_fake_shading = static_clippable_shader("overlay_paint_weight_fake_shading");

  sculpt_mesh = static_clippable_shader("overlay_sculpt_mask");
  sculpt_curves = static_clippable_shader("overlay_sculpt_curves_selection");
  sculpt_curves_cage = static_clippable_shader("overlay_sculpt_curves_cage");

  uv_analysis_stretch_angle = static_clippable_shader("overlay_edit_uv_stretching_angle");
  uv_analysis_stretch_area = static_clippable_shader("overlay_edit_uv_stretching_area");
  uv_edit_vert = static_clippable_shader("overlay_edit_uv_verts");
  uv_edit_face = static_clippable_shader("overlay_edit_uv_faces");
  uv_edit_facedot = static_clippable_shader("overlay_edit_uv_face_dots");
  uv_image_borders = static_clippable_shader("overlay_edit_uv_tiled_image_borders");
  uv_brush_stencil = static_clippable_shader("overlay_edit_uv_stencil_image");
  uv_paint_mask = static_clippable_shader("overlay_edit_uv_mask_image");

  xray_fade = static_clippable_shader("overlay_xray_fade");

  /** Selectable Shaders */

  armature_envelope_fill = static_selectable_shader("overlay_armature_envelope_solid");
  armature_envelope_outline = static_selectable_shader("overlay_armature_envelope_outline");
  armature_shape_outline = static_selectable_shader("overlay_armature_shape_outline");
  armature_shape_fill = static_selectable_shader("overlay_armature_shape_solid");
  armature_shape_wire = static_selectable_shader("overlay_armature_shape_wire");
  armature_sphere_outline = static_selectable_shader("overlay_armature_sphere_outline");
  armature_sphere_fill = static_selectable_shader("overlay_armature_sphere_solid");
  armature_stick = static_selectable_shader("overlay_armature_stick");
  armature_wire = static_selectable_shader("overlay_armature_wire");

  facing = static_clippable_shader("overlay_facing");

  fluid_grid_lines_flags = static_selectable_shader("overlay_volume_gridlines_flags");
  fluid_grid_lines_flat = static_selectable_shader("overlay_volume_gridlines_flat");
  fluid_grid_lines_range = static_selectable_shader("overlay_volume_gridlines_range");
  fluid_velocity_streamline = static_selectable_shader("overlay_volume_velocity_streamline");
  fluid_velocity_mac = static_selectable_shader("overlay_volume_velocity_mac");
  fluid_velocity_needle = static_selectable_shader("overlay_volume_velocity_needle");

  extra_shape = static_selectable_shader("overlay_extra");
  extra_wire = static_selectable_shader("overlay_extra_wire");
  extra_wire_object = static_selectable_shader("overlay_extra_wire_object");

  extra_loose_points = static_selectable_shader("overlay_extra_loose_point");

  lattice_points = static_clippable_shader("overlay_edit_lattice_point");

  lattice_wire = static_clippable_shader("overlay_edit_lattice_wire");

  extra_grid = static_selectable_shader("overlay_extra_grid");

  extra_ground_line = static_selectable_shader("overlay_extra_groundline");

  image_plane = static_selectable_shader("overlay_image");

  image_plane_depth_bias = static_selectable_shader("overlay_image_depth_bias");

  light_spot_cone = static_clippable_shader("overlay_extra_spot_cone");

  particle_dot = static_selectable_shader("overlay_particle_dot");

  particle_shape = static_selectable_shader("overlay_particle_shape");

  particle_hair = static_selectable_shader("overlay_particle_hair");

  uniform_color = static_clippable_shader("overlay_uniform_color");

  uniform_color_batch = static_clippable_shader("overlay_uniform_color_batch");

  wireframe_mesh = static_selectable_shader("overlay_wireframe");

  wireframe_points = static_selectable_shader("overlay_wireframe_points");

  wireframe_curve = static_selectable_shader("overlay_wireframe_curve");
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
