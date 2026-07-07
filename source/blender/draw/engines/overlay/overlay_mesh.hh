/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "BKE_editmesh.hh"
#include "BKE_mesh.hh"
#include "BKE_paint.hh"
#include "BKE_subdiv_modifier.hh"

#include "DNA_mask_types.h"

#include "ED_view3d.hh"

#include "draw_cache.hh"
#include "draw_cache_impl.hh"
#include "draw_manager_text.hh"
#include "overlay_base.hh"

namespace blender::draw::overlay {

/**
 * Draw edit mesh overlays.
 */
class Meshes : Overlay {
 private:
  PassSimple edit_mesh_normals_ps_ = {"Normals"};
  PassSimple::Sub *face_normals_ = nullptr;
  PassSimple::Sub *face_normals_subdiv_ = nullptr;
  PassSimple::Sub *loop_normals_ = nullptr;
  PassSimple::Sub *loop_normals_subdiv_ = nullptr;
  PassSimple::Sub *vert_normals_ = nullptr;
  PassSimple::Sub *vert_normals_subdiv_ = nullptr;

  PassSimple edit_mesh_analysis_ps_ = {"Mesh Analysis"};
  PassSimple edit_mesh_weight_ps_ = {"Edit Weight"};

  PassSimple edit_mesh_edges_ps_ = {"Edges"};
  PassSimple edit_mesh_faces_ps_ = {"Faces"};
  PassSimple edit_mesh_cages_ps_ = {"Cages"}; /* Same as faces but with a different offset. */
  PassSimple edit_mesh_verts_ps_ = {"Verts"};
  PassSimple edit_mesh_facedots_ps_ = {"FaceDots"};
  PassSimple edit_mesh_skin_roots_ps_ = {"SkinRoots"};

  /* Depth pre-pass to cull edit cage in case the object is not opaque. */
  PassSimple edit_mesh_prepass_ps_ = {"Prepass"};

  bool xray_enabled_ = false;
  bool xray_flag_enabled_ = false;

  bool show_retopology_ = false;
  bool show_mesh_analysis_ = false;
  bool show_face_overlay_ = false;
  bool show_weight_ = false;

  bool select_vert_ = false;
  bool select_edge_ = false;
  bool select_face_ = false;
  bool select_face_dots_ = false;

  /**
   * Depth offsets applied in screen space to different edit overlay components.
   * This is multiplied by a factor based on zoom level computed by `GPU_polygon_offset_calc`.
   */
  static constexpr float cage_ndc_offset_ = 0.5f;
  static constexpr float edge_ndc_offset_ = 1.0f;
  static constexpr float vert_ndc_offset_ = 1.5f;

  /* TODO(fclem): This is quite wasteful and expensive, prefer in shader Z modification like the
   * retopology offset. */
  View view_edit_cage_ = {"view_edit_cage"};
  View::OffsetData offset_data_;

 public:
  void begin_sync(Resources &res, const State &state) final;

  void edit_object_sync(Manager &manager,
                        const ObjectRef &ob_ref,
                        Resources & /*res*/,
                        const State &state) final;

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final;

  void draw_color_only(Framebuffer &framebuffer, Manager &manager, View &view) final;

  static bool mesh_has_edit_cage(const Object *ob);

 private:
  uint4 data_mask_get(const int flag);

  static bool mesh_has_skin_roots(const Object *ob);
};

/**
 * Draw edit uv overlays.
 */
class MeshUVs : Overlay {
 private:
  PassSimple analysis_ps_ = {"Mesh Analysis"};

  /* TODO(fclem): Should be its own Overlay?. */
  PassSimple wireframe_ps_ = {"Wireframe"};

  PassSimple edges_ps_ = {"Edges"};
  PassSimple faces_ps_ = {"Faces"};
  PassSimple verts_ps_ = {"Verts"};
  PassSimple facedots_ps_ = {"FaceDots"};

  /* TODO(fclem): Should be its own Overlay?. */
  PassSimple image_border_ps_ = {"ImageBorder"};

  /* TODO(fclem): Should be its own Overlay?. */
  PassSimple brush_stencil_ps_ = {"BrushStencil"};

  /* TODO(fclem): Should be its own Overlay?. */
  PassSimple paint_mask_ps_ = {"PaintMask"};

  bool select_vert_ = false;
  bool select_edge_ = false;
  bool select_face_ = false;
  bool select_face_dots_ = false;

  bool show_face_overlay_ = false;

  bool show_uv_edit_ = false;

  /** Wireframe Overlay */
  /* Draw final evaluated UVs (modifier stack applied) as grayed out wire-frame. */
  /* TODO(fclem): Maybe should be its own Overlay?. */
  bool show_wireframe_ = false;

  /** Brush stencil. */
  /* TODO(fclem): Maybe should be its own Overlay?. */
  bool show_stencil_ = false;

  /** Paint Mask overlay. */
  /* TODO(fclem): Maybe should be its own Overlay?. */
  bool show_mask_ = false;
  MaskOverlayMode mask_mode_ = MASK_OVERLAY_ALPHACHANNEL;
  Mask *mask_id_ = nullptr;
  Texture mask_texture_ = {"mask_texture_"};

  /** Stretching Overlay. */
  bool show_mesh_analysis_ = false;
  eSpaceImage_UVDT_Stretch mesh_analysis_type_;
  /**
   * In order to display the stretching relative to all objects in edit mode, we have to sum the
   * area ***AFTER*** extraction and before drawing. To that end, we get a pointer to the resulting
   * total per mesh area location to dereference after extraction.
   */
  Vector<float *> per_mesh_area_3d_;
  Vector<float *> per_mesh_area_2d_;
  float total_area_ratio_;

  /** UDIM border overlay. */
  bool show_tiled_image_active_ = false;
  bool show_tiled_image_border_ = false;
  bool show_tiled_image_label_ = false;

 public:
  void begin_sync(Resources &res, const State &state) final;

  void object_sync(Manager &manager,
                   const ObjectRef &ob_ref,
                   Resources & /*res*/,
                   const State &state) final;

  void edit_object_sync(Manager &manager,
                        const ObjectRef &ob_ref,
                        Resources & /*res*/,
                        const State &state) final;

  void end_sync(Resources &res, const State &state) final;

  void draw(Framebuffer &framebuffer, Manager &manager, View &view) final;

  void draw_on_render(gpu::FrameBuffer *framebuffer, Manager &manager, View &view) final;

 private:
  /* TODO(jbakker): the GPU texture should be cached with the mask. */
  void paint_mask_texture_ensure(Mask *mask, const int2 &resolution, const float2 &aspect);
};

}  // namespace blender::draw::overlay
