/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "BLI_math_color.h"

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
    pass.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL,
                   state.clipping_plane_count);
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
    const float4x4 transform = BKE_volume_transform_to_blender(grid_to_view->transform());
    const VolumeGridType grid_type = bke::volume_grid::get_type(grid_base);
    BKE_volume_grid_type_to_static_type(grid_type, [&](auto type_tag) {
      using GridT = typename decltype(type_tag)::type;
      using ValueType = typename GridT::ValueType;
      const GridT &grid = static_cast<const GridT &>(grid_base);
      for (typename GridT::ValueOnCIter iter = grid.cbeginValueOn(); iter; ++iter) {
        const openvdb::CoordBBox voxel = iter.getBoundingBox();
        const openvdb::Vec3d delta = voxel.dim().asVec3d() / 2.0f;
        const openvdb::Vec3d centre = voxel.getCenter() + delta;

        const float depth_factor = iter.getLevel() / grid.tree().treeDepth();

        float4 color(0.5f);
        hsv_to_rgb(depth_factor, 0.9f, 0.9f, &color.x, &color.y, &color.z);

        const float4x4 voxel_transform = transform * math::from_loc_scale<float4x4>(float3(centre.x(), centre.y(), centre.z()), float3(delta.x(), delta.y(), delta.z()));
        voxel_buf_.append({voxel_transform, color, 1.0f}, sel_id);
      }
    });
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
