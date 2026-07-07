/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "overlay_base.hh"

namespace blender::draw::overlay {

/**
 * Draw grease pencil overlays.
 * Also contains grease pencil helper functions for other overlays.
 */
class GreasePencil : Overlay {
 private:
  PassSimple edit_grease_pencil_ps_ = {"GPencil Edit"};
  PassSimple::Sub *edit_handles_ = nullptr;
  PassSimple::Sub *edit_points_ = nullptr;
  PassSimple::Sub *edit_lines_ = nullptr;

  PassSimple grid_ps_ = {"GPencil Grid"};

  bool show_handles_ = false;
  bool show_points_ = false;
  bool show_lines_ = false;
  bool show_grid_ = false;
  bool show_weight_ = false;
  bool show_material_name_ = false;

  /* TODO(fclem): This is quite wasteful and expensive, prefer in shader Z modification like the
   * retopology offset. */
  View view_edit_cage_ = {"view_edit_cage"};
  View::OffsetData offset_data_;

 public:
  void begin_sync(Resources &res, const State &state) final;

  void edit_object_sync(Manager &manager,
                        const ObjectRef &ob_ref,
                        Resources &res,
                        const State &state) final;

  void paint_object_sync(Manager &manager,
                         const ObjectRef &ob_ref,
                         Resources &res,
                         const State &state)
  {
    /* Reuse same logic as edit mode. */
    edit_object_sync(manager, ob_ref, res, state);
  }

  void sculpt_object_sync(Manager &manager,
                          const ObjectRef &ob_ref,
                          Resources &res,
                          const State &state)
  {
    /* Reuse same logic as edit mode. */
    edit_object_sync(manager, ob_ref, res, state);
  }

  void object_sync(Manager & /*manager*/,
                   const ObjectRef &ob_ref,
                   Resources & /*res*/,
                   const State &state) final;

  static void compute_depth_planes(Manager &manager,
                                   View &view,
                                   Resources &res,
                                   const State & /*state*/);

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final;

  void draw_color_only(Framebuffer &framebuffer, Manager &manager, View &view) final;

  static void draw_grease_pencil(Resources &res,
                                 PassMain::Sub &pass,
                                 const Scene *scene,
                                 Object *ob,
                                 ResourceHandleRange res_handle,
                                 select::ID select_id = select::SelectMap::select_invalid_id());

 private:
  float4x4 grid_matrix_get(const Object &object, const Scene *scene);
  void draw_material_names(const ObjectRef &ob_ref, const State &state, Resources &res);
};

}  // namespace blender::draw::overlay
