/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * \brief Simple API to draw debug shapes and log in the viewport.
 *
 * Both CPU and GPU implementation are supported and symmetrical (meaning GPU shader can use it
 * too, see common_draw_lib.glsl).
 *
 * NOTE: CPU logging will overlap GPU logging on screen as it is drawn after.
 */

#pragma once

#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

#include "DNA_object_types.h"

#include "draw_shader_shared.hh"

#include "DRW_gpu_wrapper.hh"

namespace blender::draw {

class View;

class DebugDraw {
 private:
  using DebugDrawBuf = StorageBuffer<DRWDebugDrawBuffer>;

  /** Data buffers containing all verts or chars to draw. */
  DebugDrawBuf *cpu_draw_buf_ = nullptr;
  DebugDrawBuf *gpu_draw_buf_ = nullptr;
  /** True if the gpu buffer have been requested and may contain data to draw. */
  bool gpu_draw_buf_used = false;
  /** Matrix applied to all points before drawing. Could be a stack if needed. */
  float4x4 model_mat_;
  /** Precomputed shapes verts. */
  Vector<float3> sphere_verts_;
  Vector<float3> point_verts_;

  std::mutex usage_mutex_;

  /* Reference counter used by GPUContext to allow freeing of DebugDrawBuf before the last
   * context is destroyed. */
  int ref_count_ = 0;
  std::mutex ref_count_mutex_;

 public:
  DebugDraw();
  ~DebugDraw(){};

  /**
   * Resets all buffers and reset model matrix state.
   * Not to be called by user.
   */
  void init();

  /**
   * Resets model matrix state to identity.
   */
  void modelmat_reset();
  /**
   * Sets model matrix transform to apply to any vertex passed to drawing functions.
   */
  void modelmat_set(const float modelmat[4][4]);

  /**
   * Drawing functions that will draw wire-frames with the given color.
   */
  void draw_line(float3 v1, float3 v2, float4 color = {1, 0, 0, 1});
  void draw_polygon(Span<float3> face_verts, float4 color = {1, 0, 0, 1});
  void draw_bbox(const BoundBox &bbox, const float4 color = {1, 0, 0, 1});
  void draw_sphere(const float3 center, float radius, const float4 color = {1, 0, 0, 1});
  void draw_point(const float3 center, float radius = 0.01f, const float4 color = {1, 0, 0, 1});
  /**
   * Draw a matrix transformation as 3 colored axes.
   */
  void draw_matrix(const float4x4 &m4);
  /**
   * Draw a matrix as a 2 units length bounding box, centered on origin.
   */
  void draw_matrix_as_bbox(const float4x4 &mat, const float4 color = {1, 0, 0, 1});

  /**
   * Will draw all debug shapes and text cached up until now to the current view / frame-buffer.
   * Draw buffers will be emptied and ready for new debug data.
   */
  void display_to_view(View &view);

  /**
   * Not to be called by user. Should become private.
   */
  GPUStorageBuf *gpu_draw_buf_get();

  static DebugDraw &get()
  {
    static DebugDraw module;
    return module;
  }

  void acquire()
  {
    std::scoped_lock lock(ref_count_mutex_);
    ref_count_++;
  }

  void release()
  {
    std::scoped_lock lock(ref_count_mutex_);
    ref_count_--;
    if (ref_count_ == 0) {
      clear_gpu_data();
    }
  }

  void clear_gpu_data();

 private:
  uint color_pack(float4 color);
  DRWDebugVert vert_pack(float3 pos, uint color);

  void draw_line(float3 v1, float3 v2, uint color);

  void display_lines(View &view);
};

/* Shortcuts to avoid boilerplate code and match shader API. */
template<class... Types> void drw_debug_line(Types... args)
{
  DebugDraw::get().draw_line(args...);
}
template<class... Types> void drw_debug_polygon(Types... args)
{
  DebugDraw::get().draw_polygon(args...);
}
template<class... Types> void drw_debug_bbox(Types... args)
{
  DebugDraw::get().draw_bbox(args...);
}
template<class... Types> void drw_debug_sphere(Types... args)
{
  DebugDraw::get().draw_sphere(args...);
}
template<class... Types> void drw_debug_point(Types... args)
{
  DebugDraw::get().draw_point(args...);
}
template<class... Types> void drw_debug_matrix(Types... args)
{
  DebugDraw::get().draw_matrix(args...);
}
template<class... Types> void drw_debug_matrix_as_bbox(Types... args)
{
  DebugDraw::get().draw_matrix_as_bbox(args...);
}

}  // namespace blender::draw
