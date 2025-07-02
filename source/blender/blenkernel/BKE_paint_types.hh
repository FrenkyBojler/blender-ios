/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#include "BLI_math_vector_types.hh"

namespace blender {
namespace ocio {
class ColorSpace;
}
}

namespace blender::bke {
struct StrokeRuntime : NonCopyable, NonMovable{
  blender::float2 last_rake;
  float last_rake_angle;

  int last_stroke_valid;
  blender::float3 average_stroke_accum;
  int average_stroke_counter;

  /* How much brush should be rotated in the view plane, 0 means x points right, y points up.
   * The convention is that the brush's _negative_ Y axis points in the tangent direction (of the
   * mouse curve, Bezier curve, etc.) */
  float brush_rotation;
  float brush_rotation_sec;

  /*******************************************************************************
   * all data below are used to communicate with cursor drawing and tex sampling *
   *******************************************************************************/
  int anchored_size;

  /**
   * Normalization factor due to accumulated value of curve along spacing.
   * Calculated when brush spacing changes to dampen strength of stroke
   * if space attenuation is used.
   */
  float overlap_factor;
  char draw_inverted;
  /** Check is there an ongoing stroke right now. */
  char stroke_active;

  char draw_anchored;
  char do_linear_conversion;

  /**
   * Store last location of stroke or whether the mesh was hit.
   * Valid only while stroke is active.
   */
  blender::float3 last_location;
  int last_hit;

  blender::float2 anchored_initial_mouse;

  /**
   * Radius of brush, pre-multiplied with pressure.
   * In case of anchored brushes contains the anchored radius.
   */
  float pixel_radius;
  float initial_pixel_radius;
  float start_pixel_radius;

  /** Drawing pressure. */
  float size_pressure_value;

  /** Position of mouse, used to sample the texture. */
  blender::float2 tex_mouse;

  /** Position of mouse, used to sample the mask texture. */
  blender::float2 mask_tex_mouse;

  /** ColorSpace cache to avoid locking up during sampling. */
  const blender::ocio::ColorSpace *colorspace;
};
}