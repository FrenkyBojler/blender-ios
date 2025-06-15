/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "BKE_geometry_set.hh"
#include "BKE_volume_grid.hh"
#include "BKE_volume_grid_fields.hh"
#include "BKE_volume_openvdb.hh"

#include "overlay_base.hh"

namespace blender::draw::overlay {

class VolumeTopologyGrid : Overlay {
 private:
  const SelectionType selection_type_;

  PassSimple topology_pass_ = {"topology_pass_"};

  ShapeInstanceBuf<ExtraInstanceData> voxel_buf_ = {selection_type_, "voxel_buf_"};

 public:
  VolumeTopologyGrid(const SelectionType selection_type) : selection_type_(selection_type){};

  void begin_sync(Resources &res, const State &state) final
  {
    auto &pass = topology_pass_;
    pass.init();
    pass.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL, state.clipping_plane_count);
    res.select_bind(pass);

    voxel_buf_.clear();
  }

  void object_sync(Manager &manager,
                   const ObjectRef &ob_ref,
                   Resources &res,
                   const State &state) final
  {
    const Object *eval_object = ob_ref.object;
    if (eval_object == nullptr) {
      return;
    }

    const bke::ObjectRuntime *object_runtime = eval_object->runtime;
    if (object_runtime == nullptr) {
      return;
    }

    const bke::GeometrySet *eval_geometry = object_runtime->geometry_set_eval;
    if (eval_geometry == nullptr) {
      return;
    }

    const Volume *volume = eval_geometry->get_volume();
    if (volume == nullptr) {
      return;
    }
    
    const bke::VolumeGridData *grid_to_view = BKE_volume_grid_find(volume, "density");
    if (grid_to_view == nullptr) {
      return;
    }

    ResourceHandle res_handle = manager.unique_handle(ob_ref);
    select::ID sel_id = res.select_id(ob_ref);

    bke::VolumeTreeAccessToken token;
    const openvdb::GridBase &grid_base = grid_to_view->grid(token);
    const VolumeGridType grid_type = bke::volume_grid::get_type(grid_base);
    BKE_volume_grid_type_to_static_type(grid_type, [&](auto type_tag) {
      using GridT = typename decltype(type_tag)::type;
      using ValueType = typename GridT::ValueType;
      const GridT &grid = static_cast<const GridT &>(grid_base);
      for (typename GridT::ValueOnCIter iter = grid.cbeginValueOn(); iter; ++iter) {
        const openvdb::CoordBBox voxel = iter.getBoundingBox();
        const openvdb::Vec3d centre = voxel.getCenter();
        const openvdb::Vec3d delta = voxel.dim().asVec3d() / 2.0f;

        const float4 color(1.0f, 0.0f, 1.0f, 1.0f);
        voxel_buf_.append({math::from_loc_scale<float4x4>(float3(centre.x(), centre.y(), centre.z()), float3(delta.x(), delta.y(), delta.z())), color, 1.0f}, sel_id);
      }
    });

    // 
    // PassSimple::Sub &sub = *sub_pass;
    // sub.bind_texture("velocity_x", fds->tex_velocity_x);
    // sub.bind_texture("velocity_y", fds->tex_velocity_y);
    // sub.bind_texture("velocity_z", fds->tex_velocity_z);
    // sub.push_constant("display_size", fds->vector_scale);
    // sub.push_constant("slice_position", fds->slice_depth);
    // sub.push_constant("cell_size", float3(fds->cell_size));
    // sub.push_constant("domain_origin_offset", float3(fds->p0));
    // sub.push_constant("adaptive_cell_offset", int3(fds->res_min));
    // sub.push_constant("slice_axis", slice_axis);
    // sub.push_constant("scale_with_magnitude", bool(fds->vector_scale_with_magnitude));
    // sub.push_constant("is_cell_centered", (fds->vector_field == FLUID_DOMAIN_VECTOR_FIELD_FORCE));
    // if (fds->vector_draw_type == VECTOR_DRAW_MAC) {
    //   sub.push_constant("draw_macx", (fds->vector_draw_mac_components & VECTOR_DRAW_MAC_X));
    //   sub.push_constant("draw_macy", (fds->vector_draw_mac_components & VECTOR_DRAW_MAC_Y));
    //   sub.push_constant("draw_macz", (fds->vector_draw_mac_components & VECTOR_DRAW_MAC_Z));
    // }
    // sub.push_constant("in_select_id", int(sel_id.get()));
    // sub.draw_procedural(GPU_PRIM_LINES, 1, total_lines * 2, -1, res_handle);
    // 
    // topology_pass_.
    // 
    // /* Show gridlines only for slices with no interpolation. */
    // const bool show_gridlines = fds->show_gridlines &&
    //                             (fds->axis_slice_method == AXIS_SLICE_SINGLE) &&
    //                             (fds->interp_method == FLUID_DISPLAY_INTERP_CLOSEST ||
    //                              fds->coba_field == FLUID_DOMAIN_FIELD_FLAGS);
    // if (show_gridlines) {
    //   PassSimple::Sub *sub_pass = nullptr;
    //   switch (fds->gridlines_color_field) {
    //     default:
    //     case FLUID_GRIDLINE_COLOR_TYPE_FLAGS:
    //       DRW_fluid_ensure_flags(fmd);
    // 
    //       sub_pass = grid_lines_flags_ps_;
    //       sub_pass->bind_texture("flag_tx", fds->tex_flags);
    //       break;
    //     case FLUID_GRIDLINE_COLOR_TYPE_RANGE:
    //       if (fds->use_coba && (fds->coba_field != FLUID_DOMAIN_FIELD_FLAGS)) {
    //         DRW_fluid_ensure_flags(fmd);
    //         DRW_fluid_ensure_range_field(fmd);
    // 
    //         sub_pass = grid_lines_range_ps_;
    //         sub_pass->bind_texture("flag_tx", fds->tex_flags);
    //         sub_pass->bind_texture("field_tx", fds->tex_range_field);
    //         sub_pass->push_constant("lower_bound", fds->gridlines_lower_bound);
    //         sub_pass->push_constant("upper_bound", fds->gridlines_upper_bound);
    //         sub_pass->push_constant("range_color", float4(fds->gridlines_range_color));
    //         sub_pass->push_constant("cell_filter", int(fds->gridlines_cell_filter));
    //         break;
    //       }
    //       /* Otherwise, fall back to none color type. */
    //       ATTR_FALLTHROUGH;
    //     case FLUID_GRIDLINE_COLOR_TYPE_NONE:
    //       sub_pass = grid_lines_flat_ps_;
    //       break;
    //   }
    // 
    //   PassSimple::Sub &sub = *sub_pass;
    //   sub.push_constant("volume_size", int3(fds->res));
    //   sub.push_constant("slice_position", fds->slice_depth);
    //   sub.push_constant("cell_size", float3(fds->cell_size));
    //   sub.push_constant("domain_origin_offset", float3(fds->p0));
    //   sub.push_constant("adaptive_cell_offset", int3(fds->res_min));
    //   sub.push_constant("slice_axis", slice_axis);
    //   sub.push_constant("in_select_id", int(sel_id.get()));
    // 
    //   BLI_assert(slice_axis != -1);
    //   int lines_per_voxel = 4;
    //   int total_lines = lines_per_voxel * math::reduce_mul(int3(fds->res)) / fds->res[slice_axis];
    //   sub.draw_procedural(GPU_PRIM_LINES, 1, total_lines * 2, -1, res_handle);
    // }
  }

  void end_sync(Resources &res, const State & /*state*/) final
  {
    topology_pass_.shader_set(res.shaders->extra_shape.get());
    topology_pass_.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
    topology_pass_.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);

    voxel_buf_.end_sync(topology_pass_, res.shapes.cube.get());
  }

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final
  {
    GPU_framebuffer_bind(framebuffer);
    manager.submit(topology_pass_, view);
  }
};

}  // namespace blender::draw::overlay
