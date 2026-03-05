/* SPDX-FileCopyrightText: 2009 by Nicholas Bishop. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <fmt/format.h>

#include "MEM_guardedalloc.h"

#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_length_parameterize.hh"
#include "BLI_rand.hh"
#include "BLI_utildefines.h"

#include "DNA_brush_types.h"
#include "DNA_curve_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "RNA_access.hh"

#include "BKE_curves.hh"

#include "BKE_brush.hh"
#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_curve.hh"
#include "BKE_global.hh"
#include "BKE_image.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "GPU_immediate.hh"
#include "GPU_state.hh"

#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "IMB_imbuf_types.hh"

#include "paint_intern.hh"

#include "mesh/sculpt_cloth.hh"
#include "mesh/sculpt_intern.hh"

// #define DEBUG_TIME

#ifdef DEBUG_TIME
#  include "BLI_time_utildefines.h"
#endif

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name RollSpline — polyline-based arc-length parameterized spline
 * \{ */

void RollSpline::clear()
{
  poly_2d.clear();
  poly_3d.clear();
  lengths_2d.clear();
  lengths_3d.clear();
  tangents_3d.clear();
}

bool RollSpline::is_empty() const
{
  return poly_3d.size() < 2;
}

float RollSpline::total_length_2d() const
{
  return lengths_2d.is_empty() ? 0.0f : lengths_2d.last();
}

float RollSpline::total_length_3d() const
{
  return lengths_3d.is_empty() ? 0.0f : lengths_3d.last();
}

void RollSpline::update_lengths()
{
  const int n2d = int(poly_2d.size());
  const int n3d = int(poly_3d.size());
  if (n2d >= 2) {
    lengths_2d.reinitialize(length_parameterize::segments_num(n2d, false));
    length_parameterize::accumulate_lengths(poly_2d.as_span(), false, lengths_2d);
  }
  else {
    lengths_2d.clear();
  }
  if (n3d >= 2) {
    lengths_3d.reinitialize(length_parameterize::segments_num(n3d, false));
    length_parameterize::accumulate_lengths(poly_3d.as_span(), false, lengths_3d);

    /* Smooth tangents via central differences — gives C0 continuous tangent
     * field instead of the piecewise-constant direction per segment. */
    tangents_3d.reinitialize(n3d);
    tangents_3d[0] = math::normalize(poly_3d[1] - poly_3d[0]);
    for (int i = 1; i < n3d - 1; i++) {
      tangents_3d[i] = math::normalize(poly_3d[i + 1] - poly_3d[i - 1]);
    }
    tangents_3d[n3d - 1] = math::normalize(poly_3d[n3d - 1] - poly_3d[n3d - 2]);
  }
  else {
    lengths_3d.clear();
    tangents_3d.clear();
  }
}

float2 RollSpline::evaluate_2d(float s) const
{
  int seg_idx;
  float factor;
  length_parameterize::sample_at_length(lengths_2d, s, seg_idx, factor);
  return math::interpolate(poly_2d[seg_idx], poly_2d[seg_idx + 1], factor);
}

float3 RollSpline::evaluate_3d(float s) const
{
  int seg_idx;
  float factor;
  length_parameterize::sample_at_length(lengths_3d, s, seg_idx, factor);
  return math::interpolate(poly_3d[seg_idx], poly_3d[seg_idx + 1], factor);
}

float3 RollSpline::tangent_3d(float s) const
{
  int seg_idx;
  float factor;
  length_parameterize::sample_at_length(lengths_3d, s, seg_idx, factor);
  return math::normalize(math::interpolate(tangents_3d[seg_idx], tangents_3d[seg_idx + 1], factor));
}

float2 RollSpline::tangent_2d_at_index(int poly_idx) const
{
  poly_idx = std::clamp(poly_idx, 0, int(poly_2d.size()) - 2);
  float2 dir = poly_2d[poly_idx + 1] - poly_2d[poly_idx];
  const float len = math::length(dir);
  return (len > 1e-7f) ? dir / len : float2(1, 0);
}

void RollSpline::closest_point_3d(const float3 &query,
                                  float &r_s,
                                  float3 &r_tan,
                                  float &r_dis) const
{
  float best_dist_sq = FLT_MAX;
  int best_seg = 0;
  float best_t = 0.0f;

  for (int i = 0; i < int(poly_3d.size()) - 1; i++) {
    const float3 ab = poly_3d[i + 1] - poly_3d[i];
    const float ab_dot = math::dot(ab, ab);
    const float t = (ab_dot > 1e-12f) ?
                        std::clamp(math::dot(query - poly_3d[i], ab) / ab_dot, 0.0f, 1.0f) :
                        0.0f;
    const float3 proj = math::interpolate(poly_3d[i], poly_3d[i + 1], t);
    const float dist_sq = math::distance_squared(query, proj);
    if (dist_sq < best_dist_sq) {
      best_dist_sq = dist_sq;
      best_seg = i;
      best_t = t;
    }
  }

  const float seg_start = (best_seg > 0) ? lengths_3d[best_seg - 1] : 0.0f;
  const float seg_end = lengths_3d[best_seg];
  r_s = seg_start + best_t * (seg_end - seg_start);
  r_dis = sqrtf(best_dist_sq);
  r_tan = math::normalize(
      math::interpolate(tangents_3d[best_seg], tangents_3d[best_seg + 1], best_t));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Roll Texture Mapping Stroke Helpers
 * \{ */

int PaintStroke::roll_max_points() const
{
  if (!need_roll_mapping_) {
    return 1;
  }

  float s = std::max(spacing_raw_, 0.05f);
  int tot = int(std::ceil(1.0f / s)) + 2;
  tot = std::max(tot, 5);
  return tot;
}

void PaintStroke::add_roll_point(const float2 &mouse_in,
                                 const float2 &mouse_out,
                                 const float3 &loc,
                                 float size,
                                 float pressure,
                                 bool pen_flip,
                                 float x_tilt,
                                 float y_tilt)
{
  constexpr int buf_cap = PAINT_MAX_INPUT_SAMPLES;

  if (need_roll_mapping_) {
    /* Budget: total knots (virtual + real) capped at roll_max_points() + initial
     * backward extension count. Virtual knots are dropped first from the oldest
     * end; only after all virtual knots are consumed does the ring buffer wrap. */
    const int budget = roll_max_points() + initial_backward_ext_count_;
    const int total_knots = int(backward_ext_2d_.size()) + num_points_;
    bool wrap_ring = false;

    if (total_knots >= budget) {
      if (!backward_ext_2d_.is_empty()) {
        /* Consume the oldest virtual knot. The ring buffer keeps growing;
         * no real stroke data is lost yet.
         * Accumulate the arc-length of the removed span so that
         * stroke_distance_world_ compensates and the texture stays put. */
        if (kRollResolution - 1 < int(roll_spline_.lengths_3d.size())) {
          stroke_distance_world_ += roll_spline_.lengths_3d[kRollResolution - 1];
        }
        else if (backward_ext_3d_.size() >= 2) {
          stroke_distance_world_ += math::distance(backward_ext_3d_[0], backward_ext_3d_[1]);
        }
        backward_ext_2d_.remove(0);
        backward_ext_3d_.remove(0);
      }
      else {
        /* All virtual knots consumed — ring buffer wraps, oldest real point
         * is abandoned. Accumulate its distance so stroke_distance_world_
         * stays correct. */
        const int oldest = (cur_point_ - num_points_ + buf_cap) % buf_cap;
        const int next = (oldest + 1) % buf_cap;
        stroke_distance_world_ += math::distance(points_[oldest].location,
                                                 points_[next].location);
        wrap_ring = true;
      }
    }

    PaintStrokePoint *point = &points_[cur_point_];
    point->size = size;
    point->mouse_in = mouse_in;
    point->mouse_out = mouse_out;
    point->x_tilt = x_tilt;
    point->y_tilt = y_tilt;
    point->pen_flip = pen_flip;
    point->location = loc;
    point->pressure = pressure;

    cur_point_ = (cur_point_ + 1) % buf_cap;
    if (!wrap_ring) {
      num_points_++;
    }
  }
  else {
    /* Non-roll: simple ring buffer wrapping at PAINT_MAX_INPUT_SAMPLES. */
    PaintStrokePoint *point = &points_[cur_point_];
    point->size = size;
    point->mouse_in = mouse_in;
    point->mouse_out = mouse_out;
    point->x_tilt = x_tilt;
    point->y_tilt = y_tilt;
    point->pen_flip = pen_flip;
    point->location = loc;
    point->pressure = pressure;

    cur_point_ = (cur_point_ + 1) % buf_cap;
    if (num_points_ < buf_cap) {
      num_points_++;
    }
  }
}

void PaintStroke::prepend_virtual_roll_points()
{
  if (num_points_ < 2) {
    return;
  }

  constexpr int cap = PAINT_MAX_INPUT_SAMPLES;
  const int oldest = (cur_point_ - num_points_ + cap) % cap;
  const int next_oldest = (oldest + 1) % cap;

  const float3 p_start = points_[oldest].location;
  const float2 m_start = points_[oldest].mouse_out;

  /* Direction from 1st to 2nd point (local tangent at stroke start). */
  float3 dir3 = points_[next_oldest].location - p_start;
  float2 dir2 = points_[next_oldest].mouse_out - m_start;

  const float len3 = math::length(dir3);
  const float len2 = math::length(dir2);

  if (len3 < 1e-7f || len2 < 1e-7f) {
    return;
  }

  dir3 /= len3;
  dir2 /= len2;

  /* Brush radius in world and screen space. */
  const float r_world = paint_calc_object_space_radius(vc, p_start, points_[oldest].size);
  const float r_screen = points_[oldest].size;

  /* Extend backward from the stroke start by 3 brush radii so that early
   * dabs have generous spline coverage behind them.
   * 12 knots subdivide this extension; with curvature they form an arc. */
  constexpr int n_backward = 12;
  const float total_backward_3d = r_world * 3.0f;
  const float total_backward_2d = r_screen * 3.0f;
  const float step_3d = total_backward_3d / float(n_backward);
  const float step_2d = total_backward_2d / float(n_backward);

  /* Measure curvature from the angle between 1st→2nd and 2nd→3rd segments.
   * Each virtual step rotates the direction by a proportional amount so the
   * extension follows the same arc the user started drawing. */
  float angle_2d = 0.0f;
  float angle_3d = 0.0f;
  float3 rot_axis = float3(0);
  bool has_rot_3d = false;

  if (num_points_ >= 3) {
    const int third = (oldest + 2) % cap;

    /* 2D curvature. */
    const float2 seg1 = points_[next_oldest].mouse_out - m_start;
    const float2 seg2 = points_[third].mouse_out - points_[next_oldest].mouse_out;
    const float cross2 = seg1.x * seg2.y - seg1.y * seg2.x;
    const float dot2 = math::dot(seg1, seg2);
    const float total_a2 = atan2f(cross2, dot2);
    const float seg_len2 = math::length(seg1);
    if (seg_len2 > 1e-7f) {
      angle_2d = total_a2 * (step_2d / seg_len2);
    }

    /* 3D curvature (Rodrigues rotation). */
    const float3 s1 = points_[next_oldest].location - p_start;
    const float3 s2 = points_[third].location - points_[next_oldest].location;
    const float3 n1 = math::normalize(s1);
    const float3 n2 = math::normalize(s2);
    const float3 c3 = math::cross(n1, n2);
    const float c3_len = math::length(c3);
    const float d3 = math::dot(n1, n2);
    if (c3_len > 1e-7f) {
      rot_axis = c3 / c3_len;
      const float total_a3 = atan2f(c3_len, d3);
      const float seg_len3 = math::length(s1);
      if (seg_len3 > 1e-7f) {
        angle_3d = total_a3 * (step_3d / seg_len3);
        has_rot_3d = true;
      }
    }
  }

  /* Build extension points walking backward from p_start.
   * v[n_backward] = p_start, v[0] = farthest backward.
   * Rotation is applied before each step so that curvature is active
   * from the very first virtual segment. */
  float3 v3d[n_backward + 1];
  float2 v2d[n_backward + 1];
  v3d[n_backward] = p_start;
  v2d[n_backward] = m_start;

  float2 cur_dir_2d = dir2;
  float3 cur_dir_3d = dir3;

  for (int i = n_backward - 1; i >= 0; i--) {
    /* Rotate direction by -angle before stepping (continue arc backward). */
    if (angle_2d != 0.0f) {
      const float c = cosf(-angle_2d);
      const float s = sinf(-angle_2d);
      cur_dir_2d = float2(cur_dir_2d.x * c - cur_dir_2d.y * s,
                          cur_dir_2d.x * s + cur_dir_2d.y * c);
    }
    if (has_rot_3d) {
      /* Rodrigues: v' = v*cos + (k x v)*sin + k*(k.v)*(1-cos). */
      const float c = cosf(-angle_3d);
      const float s = sinf(-angle_3d);
      const float3 kxv = math::cross(rot_axis, cur_dir_3d);
      const float kdv = math::dot(rot_axis, cur_dir_3d);
      cur_dir_3d = cur_dir_3d * c + kxv * s + rot_axis * kdv * (1.0f - c);
    }

    v2d[i] = v2d[i + 1] - cur_dir_2d * step_2d;
    v3d[i] = v3d[i + 1] - cur_dir_3d * step_3d;
  }

  /* Store backward extension knots (not including p_start which comes from real knots).
   * These are prepended to the knot array during each polyline rebuild. */
  backward_ext_2d_.clear();
  backward_ext_3d_.clear();
  for (int i = 0; i < n_backward; i++) {
    backward_ext_2d_.append(v2d[i]);
    backward_ext_3d_.append(v3d[i]);
  }
  initial_backward_ext_count_ = n_backward;
}

void PaintStroke::make_roll_spline(bContext * /*C*/)
{
  if (num_points_ < 4) {
    return;
  }

  if (!roll_virtual_prepended_) {
    roll_virtual_prepended_ = true;
    prepend_virtual_roll_points();
  }

  /* Collect real knots from ring buffer. */
  constexpr int buf_cap = PAINT_MAX_INPUT_SAMPLES;
  Vector<float2> knots_2d;
  Vector<float3> knots_3d;

  const int oldest = (cur_point_ - num_points_ + buf_cap) % buf_cap;
  for (int i = 0; i < num_points_; i++) {
    const int idx = (oldest + i) % buf_cap;
    knots_2d.append(points_[idx].mouse_out);
    knots_3d.append(points_[idx].location);
  }

  /* Combine backward extension knots + real knots.
   * backward_ext ends just before the first real knot (oldest stroke point),
   * so Catmull-Rom through them gives a smooth transition. */
  Vector<float2> all_2d;
  Vector<float3> all_3d;
  all_2d.extend(backward_ext_2d_);
  all_3d.extend(backward_ext_3d_);
  all_2d.extend(knots_2d);
  all_3d.extend(knots_3d);

  const int n_total = int(all_2d.size());
  if (n_total < 2) {
    return;
  }

  /* Rebuild polyline via Catmull-Rom evaluation. */
  roll_spline_.clear();
  const int resolution = kRollResolution;

  for (int i = 0; i < n_total - 1; i++) {
    const int i0 = std::max(i - 1, 0);
    const int i3 = std::min(i + 2, n_total - 1);

    for (int s = 0; s < resolution; s++) {
      const float t = float(s) / float(resolution);
      roll_spline_.poly_2d.append(bke::curves::catmull_rom::interpolate(
          all_2d[i0], all_2d[i], all_2d[i + 1], all_2d[i3], t));
      roll_spline_.poly_3d.append(bke::curves::catmull_rom::interpolate(
          all_3d[i0], all_3d[i], all_3d[i + 1], all_3d[i3], t));
    }
  }
  /* Add the last endpoint. */
  roll_spline_.poly_2d.append(all_2d.last());
  roll_spline_.poly_3d.append(all_3d.last());

  /* Virtual boundary: backward extension has backward_ext_2d_.size() knots,
   * each knot transition = resolution polyline points. */
  n_virtual_poly_points_ = int(backward_ext_2d_.size()) * resolution;

  roll_spline_.update_lengths();
}

void PaintStroke::finish_roll_stroke(bContext *C,
                                     wmOperator *op,
                                     const float2 &mouse_up,
                                     float pressure)
{
  if (!need_roll_mapping_ || num_points_ < 4) {
    return;
  }

  const PaintMode mode = BKE_paintmode_get_active_from_context(C);
  const Brush &brush = *BKE_paint_brush_for_read(this->paint);
  bke::PaintRuntime *paint_runtime = this->paint->runtime;

  /* 1. Process any remaining spacing steps up to the mouse-up position. */
  if (paint_space_stroke_enabled(brush, mode)) {
    space_stroke(C, op, mouse_up, pressure);
  }

  /* 2. Force-record the exact mouse-up position as a roll point
   *    (even if it doesn't fall on a spacing boundary). */
  {
    float2 mouse_out = paint_stroke_jitter_pos(
        this->paint, mode, brush, pressure, stroke_mode_, zoom_2d_, mouse_up);
    float3 location;
    bool is_location_is_set;
    update(C, brush, mode, mouse_up, mouse_out, pressure, location, &is_location_is_set);
    if (is_location_is_set) {
      add_roll_point(
          mouse_up, mouse_out, location, paint_runtime->pixel_radius, pressure, pen_flip_,
          tilt_.x, tilt_.y);
    }
    else if (num_points_ > 0) {
      /* Fallback: use last known location so the spline still extends. */
      const int last = (cur_point_ - 1 + PAINT_MAX_INPUT_SAMPLES) % PAINT_MAX_INPUT_SAMPLES;
      add_roll_point(mouse_up,
                     mouse_out,
                     points_[last].location,
                     points_[last].size,
                     pressure,
                     pen_flip_,
                     tilt_.x,
                     tilt_.y);
    }
    make_roll_spline(C);
  }

  /* 3. Append a virtual forward extension so the last dab has spline
   *    coverage for the brush half that extends beyond the stroke end.
   *    Mirrors prepend_virtual_roll_points() but in the forward direction.
   *    Points are appended directly to the polyline (no Catmull-Rom needed). */
  if (num_points_ >= 2 && !roll_spline_.is_empty()) {
    constexpr int cap = PAINT_MAX_INPUT_SAMPLES;
    const int newest = (cur_point_ - 1 + cap) % cap;
    const int prev_newest = (newest - 1 + cap) % cap;

    const float3 p_end = points_[newest].location;
    const float2 m_end = points_[newest].mouse_out;

    /* Direction from 2nd-to-last to last point (local tangent at stroke end). */
    float3 dir3 = p_end - points_[prev_newest].location;
    float2 dir2 = m_end - points_[prev_newest].mouse_out;
    const float len3 = math::length(dir3);
    const float len2 = math::length(dir2);

    if (len3 > 1e-7f && len2 > 1e-7f) {
      dir3 /= len3;
      dir2 /= len2;

      const float r_world = paint_calc_object_space_radius(vc, p_end, points_[newest].size);
      const float r_screen = points_[newest].size;

      constexpr int n_forward = 12;
      const float step_3d = (r_world * 3.0f) / float(n_forward);
      const float step_2d = (r_screen * 3.0f) / float(n_forward);

      /* Measure curvature from the angle between the last two segments. */
      float angle_2d = 0.0f;
      float angle_3d = 0.0f;
      float3 rot_axis = float3(0);
      bool has_rot_3d = false;

      if (num_points_ >= 3) {
        const int prev2 = (prev_newest - 1 + cap) % cap;

        const float2 seg1 = points_[prev_newest].mouse_out - points_[prev2].mouse_out;
        const float2 seg2 = m_end - points_[prev_newest].mouse_out;
        const float cross2 = seg1.x * seg2.y - seg1.y * seg2.x;
        const float dot2 = math::dot(seg1, seg2);
        const float total_a2 = atan2f(cross2, dot2);
        const float seg_len2 = math::length(seg2);
        if (seg_len2 > 1e-7f) {
          angle_2d = total_a2 * (step_2d / seg_len2);
        }

        const float3 s1 = points_[prev_newest].location - points_[prev2].location;
        const float3 s2 = p_end - points_[prev_newest].location;
        const float3 n1 = math::normalize(s1);
        const float3 n2 = math::normalize(s2);
        const float3 c3 = math::cross(n1, n2);
        const float c3_len = math::length(c3);
        const float d3 = math::dot(n1, n2);
        if (c3_len > 1e-7f) {
          rot_axis = c3 / c3_len;
          const float total_a3 = atan2f(c3_len, d3);
          const float seg_len3 = math::length(s2);
          if (seg_len3 > 1e-7f) {
            angle_3d = total_a3 * (step_3d / seg_len3);
            has_rot_3d = true;
          }
        }
      }

      /* Build forward extension points, walking from p_end. */
      float2 cur_dir_2d = dir2;
      float3 cur_dir_3d = dir3;
      float2 prev_2d = m_end;
      float3 prev_3d = p_end;

      for (int i = 0; i < n_forward; i++) {
        if (angle_2d != 0.0f) {
          const float c = cosf(angle_2d);
          const float s = sinf(angle_2d);
          cur_dir_2d = float2(cur_dir_2d.x * c - cur_dir_2d.y * s,
                              cur_dir_2d.x * s + cur_dir_2d.y * c);
        }
        if (has_rot_3d) {
          const float c = cosf(angle_3d);
          const float s = sinf(angle_3d);
          const float3 kxv = math::cross(rot_axis, cur_dir_3d);
          const float kdv = math::dot(rot_axis, cur_dir_3d);
          cur_dir_3d = cur_dir_3d * c + kxv * s + rot_axis * kdv * (1.0f - c);
        }

        prev_2d = prev_2d + cur_dir_2d * step_2d;
        prev_3d = prev_3d + cur_dir_3d * step_3d;
        roll_spline_.poly_2d.append(prev_2d);
        roll_spline_.poly_3d.append(prev_3d);
      }

      /* Re-accumulate lengths after appending forward extension. */
      roll_spline_.update_lengths();
    }
  }

  /* 4. Flush deferred dabs: advance look_back from its current position
   *    to the newest recorded point, placing a dab at each step. */
  constexpr int buf_cap = PAINT_MAX_INPUT_SAMPLES;
  const int half = (roll_max_points() >> 1) + 2;
  const int oldest_idx = (cur_point_ - num_points_ + buf_cap) % buf_cap;

  /* Where the last placed dab was. Use the explicitly tracked index
   * rather than recalculating, since space_stroke() and add_roll_point()
   * in steps 1-2 above may have advanced cur_point_. */
  int flush_start;
  if (last_painted_roll_idx_ >= 0) {
    flush_start = last_painted_roll_idx_;
  }
  else if (num_points_ <= half) {
    flush_start = oldest_idx;
  }
  else {
    flush_start = (cur_point_ - half + buf_cap) % buf_cap;
  }

  /* Advance one step at a time toward (but not including) the newest point.
   * The newest entry is the force-recorded mouse-up position which may sit
   * arbitrarily close to the previous point — including it would create an
   * overlapping dab at the stroke end. */
  const int newest = (cur_point_ - 1 + buf_cap) % buf_cap;
  int idx = (flush_start + 1) % buf_cap;

  while (idx != newest) {
    PaintStrokePoint *point = &points_[idx];

    PointerRNA itemptr;
    RNA_collection_add(op->ptr, "stroke", &itemptr);
    RNA_float_set(&itemptr, "size", point->size);
    RNA_float_set_array(&itemptr, "location", point->location);
    RNA_float_set_array(&itemptr, "mouse", point->mouse_out);
    RNA_float_set_array(&itemptr, "mouse_event", point->mouse_in);
    RNA_float_set(&itemptr, "pressure", point->pressure);
    RNA_float_set(&itemptr, "x_tilt", point->x_tilt);
    RNA_float_set(&itemptr, "y_tilt", point->y_tilt);

    this->update_step(op, &itemptr);
    RNA_collection_clear(op->ptr, "stroke");

    tot_samples_++;
    idx = (idx + 1) % buf_cap;
  }
}

void PaintStroke::compute_roll_center(StrokeCache &cache) const
{
  if (roll_spline_.is_empty()) {
    cache.roll_center_s = -1.0f;
    return;
  }
  float raw_s, dis;
  float3 tan;
  /* Full closest-point search, called once per dab on the main thread. */
  roll_spline_.closest_point_3d(cache.location, raw_s, tan, dis);
  cache.roll_center_s = raw_s;
  cache.roll_center_pos = roll_spline_.evaluate_3d(raw_s);
  cache.roll_tangent = tan;
}

void PaintStroke::spline_uv(const StrokeCache &cache,
                             const float co[3],
                             float r_out[3],
                             float r_tan[3]) const
{
  float3 tan;
  float3 p;
  const float total_len = roll_spline_.total_length_3d();

  if (cache.roll_center_s >= 0.0f) {
    /* Fast path: project vertex onto stroke tangent to get an initial
     * arc-length estimate, then refine with Newton iterations. */
    float s = cache.roll_center_s +
              math::dot(float3(co) - cache.roll_center_pos, cache.roll_tangent);
    s = std::clamp(s, 0.0f, total_len);

    /* Newton refinement: minimize dot(spline(s) - co, tangent(s)) = 0. */
    for (int iter = 0; iter < 3; iter++) {
      p = roll_spline_.evaluate_3d(s);
      tan = roll_spline_.tangent_3d(s);
      const float err = math::dot(p - float3(co), tan);
      if (std::abs(err) < 1e-5f) {
        break;
      }
      s = std::clamp(s - err, 0.0f, total_len);
    }
    p = roll_spline_.evaluate_3d(s);
    tan = roll_spline_.tangent_3d(s);
    r_out[1] = s;
  }
  else {
    /* Fallback: full closest-point search (used before center is precomputed). */
    roll_spline_.closest_point_3d(float3(co), r_out[1], tan, r_out[0]);
    p = roll_spline_.evaluate_3d(r_out[1]);
  }

  copy_v3_v3(r_tan, tan);
  r_out[0] = math::distance(p, float3(co));
  r_out[2] = 0.0f;

  float3 vec = p - float3(co);
  float3 vec2;
  cross_v3_v3v3(vec2, vec, tan);

  if (math::dot(vec2, cache.view_normal) < 0.0f) {
    r_out[0] = -r_out[0];
  }

  r_out[1] += stroke_distance_world_;
}

float PaintStroke::spline_length() const
{
  return roll_spline_.total_length_3d();
}

void PaintStroke::draw_debug_roll(bContext *C) const
{
  if (!need_roll_mapping_ || roll_spline_.is_empty()) {
    return;
  }

  ARegion *region = CTX_wm_region(C);
  if (!region) {
    return;
  }

  const float ox = float(region->winrct.xmin);
  const float oy = float(region->winrct.ymin);

  GPU_line_smooth(true);
  GPU_blend(GPU_BLEND_ALPHA);

  const uint pos_attr = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  const int n_pts = int(roll_spline_.poly_2d.size());

  /* Draw polyline: red = virtual backward extension, green = real. */
  GPU_line_width(3.0f);

  /* Virtual portion. */
  if (n_virtual_poly_points_ > 1) {
    immUniformColor4ub(255, 50, 50, 200);
    const int count = std::min(n_virtual_poly_points_ + 1, n_pts);
    immBegin(GPU_PRIM_LINE_STRIP, count);
    for (int i = 0; i < count; i++) {
      immVertex2f(pos_attr, roll_spline_.poly_2d[i].x + ox, roll_spline_.poly_2d[i].y + oy);
    }
    immEnd();
  }

  /* Real portion. */
  if (n_virtual_poly_points_ < n_pts) {
    immUniformColor4ub(50, 255, 50, 200);
    const int count = n_pts - n_virtual_poly_points_;
    immBegin(GPU_PRIM_LINE_STRIP, count);
    for (int i = n_virtual_poly_points_; i < n_pts; i++) {
      immVertex2f(pos_attr, roll_spline_.poly_2d[i].x + ox, roll_spline_.poly_2d[i].y + oy);
    }
    immEnd();
  }

  /* Perpendicular tick marks at each knot boundary (every kRollResolution points). */
  {
    GPU_line_width(1.5f);
    const float tick_len = 10.0f;
    for (int i = 0; i < n_pts; i += kRollResolution) {
      const float2 tan = roll_spline_.tangent_2d_at_index(std::min(i, n_pts - 2));
      const float2 perp(-tan.y, tan.x);
      const float2 &pt = roll_spline_.poly_2d[i];
      if (i < n_virtual_poly_points_) {
        immUniformColor4ub(180, 80, 80, 140);
      }
      else {
        immUniformColor4ub(200, 200, 200, 180);
      }
      immBegin(GPU_PRIM_LINES, 2);
      immVertex2f(pos_attr, pt.x + perp.x * tick_len + ox, pt.y + perp.y * tick_len + oy);
      immVertex2f(pos_attr, pt.x - perp.x * tick_len + ox, pt.y - perp.y * tick_len + oy);
      immEnd();
    }
  }

  /* Mark the boundary between virtual and real: yellow tick. */
  if (n_virtual_poly_points_ > 0 && n_virtual_poly_points_ < n_pts) {
    GPU_line_width(2.5f);
    immUniformColor4ub(255, 255, 0, 255);
    const float2 tan = roll_spline_.tangent_2d_at_index(n_virtual_poly_points_);
    const float2 perp(-tan.y, tan.x);
    const float big_tick = 25.0f;
    const float2 &pt = roll_spline_.poly_2d[n_virtual_poly_points_];
    immBegin(GPU_PRIM_LINES, 2);
    immVertex2f(pos_attr, pt.x + perp.x * big_tick + ox, pt.y + perp.y * big_tick + oy);
    immVertex2f(pos_attr, pt.x - perp.x * big_tick + ox, pt.y - perp.y * big_tick + oy);
    immEnd();
  }

  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
  GPU_line_smooth(false);
}

static void paint_draw_roll_debug(bContext *C,
                                  const int2 & /*xy*/,
                                  const float2 & /*tilt*/,
                                  void *customdata)
{
  PaintStroke *stroke = static_cast<PaintStroke *>(customdata);
  stroke->draw_debug_roll(C);
}

void PaintStroke::draw_roll_preview(bContext *C) const
{
  if (!need_roll_mapping_ || roll_spline_.is_empty()) {
    return;
  }

  ARegion *region = CTX_wm_region(C);
  if (!region) {
    return;
  }

  const int n_pts = int(roll_spline_.poly_2d.size());
  const int resolution = kRollResolution;

  /* Determine which polyline index corresponds to the last-painted dab.
   * Points from painted_poly onward are "unflushed" and shown as preview. */
  constexpr int buf_cap = PAINT_MAX_INPUT_SAMPLES;
  const int half = (roll_max_points() >> 1) + 2;

  int painted_poly;
  if (last_painted_roll_idx_ < 0 || num_points_ < half) {
    painted_poly = n_virtual_poly_points_;
  }
  else {
    const int oldest_idx = (cur_point_ - num_points_ + buf_cap) % buf_cap;
    const int dist = (last_painted_roll_idx_ - oldest_idx + buf_cap) % buf_cap;
    painted_poly = n_virtual_poly_points_ + dist * resolution;
    painted_poly = std::min(painted_poly, n_pts - 1);
  }

  if (painted_poly >= n_pts) {
    return;
  }

  const float ox = float(region->winrct.xmin);
  const float oy = float(region->winrct.ymin);

  GPU_line_smooth(true);
  GPU_blend(GPU_BLEND_ALPHA);

  const uint pos_attr = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  /* Same color as the stabilize stroke line. */
  immUniformColor4ub(255, 100, 100, 128);

  GPU_line_width(2.0f);
  const int count = n_pts - painted_poly;
  if (count >= 2) {
    immBegin(GPU_PRIM_LINE_STRIP, count);
    for (int i = painted_poly; i < n_pts; i++) {
      immVertex2f(pos_attr, roll_spline_.poly_2d[i].x + ox, roll_spline_.poly_2d[i].y + oy);
    }
    immEnd();
  }

  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
  GPU_line_smooth(false);
}

static void paint_draw_roll_preview(bContext *C,
                                    const int2 & /*xy*/,
                                    const float2 & /*tilt*/,
                                    void *customdata)
{
  PaintStroke *stroke = static_cast<PaintStroke *>(customdata);
  stroke->draw_roll_preview(C);
}

/** \} */

/*** Cursors ***/
static void paint_draw_smooth_cursor(bContext *C,
                                     const int2 &xy,
                                     const float2 & /*tilt*/,
                                     void *customdata)
{
  PaintStroke *data = static_cast<PaintStroke *>(customdata);

  const Paint *paint = BKE_paint_get_active_from_context(C);
  const Brush *brush = BKE_paint_brush_for_read(paint);
  const PaintMode mode = BKE_paintmode_get_active_from_context(C);
  ARegion *region = CTX_wm_region(C);

  if ((mode == PaintMode::GPencil) && (paint->flags & PAINT_SHOW_BRUSH) == 0) {
    return;
  }

  if (data && brush) {
    GPU_line_smooth(true);
    GPU_blend(GPU_BLEND_ALPHA);

    const uint pos = GPU_vertformat_attr_add(
        immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
    const uchar4 color = uchar4(255, 100, 100, 128);
    immUniformColor4ubv(color);

    immBegin(GPU_PRIM_LINES, 2);
    immVertex2fv(pos, float2(xy));
    immVertex2f(pos,
                data->last_mouse_position[0] + region->winrct.xmin,
                data->last_mouse_position[1] + region->winrct.ymin);

    immEnd();

    immUnbindProgram();

    GPU_blend(GPU_BLEND_NONE);
    GPU_line_smooth(false);
  }
}

static void paint_draw_line_cursor(bContext * /*C*/,
                                   const int2 &xy,
                                   const float2 & /*tilt*/,
                                   void *customdata)
{
  PaintStroke *stroke = static_cast<PaintStroke *>(customdata);

  GPU_line_smooth(true);

  const uint shdr_pos = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);

  immBindBuiltinProgram(GPU_SHADER_3D_LINE_DASHED_UNIFORM_COLOR);

  float4 viewport_size;
  GPU_viewport_size_get_f(viewport_size);
  immUniform2f("viewport_size", viewport_size[2], viewport_size[3]);

  immUniform1i("colors_len", 2); /* "advanced" mode */
  immUniform4f("color", 0.0f, 0.0f, 0.0f, 0.5);
  immUniform4f("color2", 1.0f, 1.0f, 1.0f, 0.5);
  immUniform1f("dash_width", 6.0f);
  immUniform1f("udash_factor", 0.5f);

  immBegin(GPU_PRIM_LINES, 2);

  const ARegion *region = stroke->vc.region;

  if (stroke->constrain_line) {
    immVertex2f(shdr_pos,
                stroke->last_mouse_position[0] + region->winrct.xmin,
                stroke->last_mouse_position[1] + region->winrct.ymin);

    immVertex2f(shdr_pos,
                stroke->constrained_pos[0] + region->winrct.xmin,
                stroke->constrained_pos[1] + region->winrct.ymin);
  }
  else {
    immVertex2f(shdr_pos,
                stroke->last_mouse_position[0] + region->winrct.xmin,
                stroke->last_mouse_position[1] + region->winrct.ymin);

    immVertex2fv(shdr_pos, float2(xy));
  }

  immEnd();

  immUnbindProgram();

  GPU_line_smooth(false);
}

static bool paint_brush_type_require_location(const Brush &brush, const PaintMode mode)
{
  switch (mode) {
    case PaintMode::Sculpt:
      if (ELEM(brush.sculpt_brush_type,
               SCULPT_BRUSH_TYPE_GRAB,
               SCULPT_BRUSH_TYPE_ELASTIC_DEFORM,
               SCULPT_BRUSH_TYPE_POSE,
               SCULPT_BRUSH_TYPE_BOUNDARY,
               SCULPT_BRUSH_TYPE_ROTATE,
               SCULPT_BRUSH_TYPE_SNAKE_HOOK,
               SCULPT_BRUSH_TYPE_THUMB))
      {
        return false;
      }
      else if (cloth::is_cloth_deform_brush(brush)) {
        return false;
      }
      else {
        return true;
      }
    default:
      break;
  }

  return true;
}

static bool paint_stroke_use_scene_spacing(const Brush &brush, const PaintMode mode)
{
  switch (mode) {
    case PaintMode::Sculpt:
      return brush.flag & BRUSH_SCENE_SPACING;
    default:
      break;
  }
  return false;
}

static bool paint_brush_type_raycast_original(const Brush &brush, PaintMode /*mode*/)
{
  return ELEM(brush.stroke_method, BRUSH_STROKE_ANCHORED, BRUSH_STROKE_DRAG_DOT);
}

static bool paint_brush_type_require_inbetween_mouse_events(const Brush &brush,
                                                            const PaintMode mode)
{
  if (brush.stroke_method == BRUSH_STROKE_ANCHORED) {
    return false;
  }

  switch (mode) {
    case PaintMode::Sculpt:
      if (ELEM(brush.sculpt_brush_type,
               SCULPT_BRUSH_TYPE_GRAB,
               SCULPT_BRUSH_TYPE_ROTATE,
               SCULPT_BRUSH_TYPE_THUMB,
               SCULPT_BRUSH_TYPE_SNAKE_HOOK,
               SCULPT_BRUSH_TYPE_ELASTIC_DEFORM,
               SCULPT_BRUSH_TYPE_CLOTH,
               SCULPT_BRUSH_TYPE_BOUNDARY,
               SCULPT_BRUSH_TYPE_POSE))
      {
        return false;
      }
      else {
        return true;
      }
    default:
      break;
  }

  return true;
}

bool PaintStroke::update(bContext *C,
                         const Brush &brush,
                         const PaintMode mode,
                         const float mouse_init[2],
                         float mouse[2],
                         const float pressure,
                         float r_location[3],
                         bool *r_location_is_set)
{
  Scene *scene = CTX_data_scene(C);
  Paint *paint = BKE_paint_get_active_from_paintmode(scene, mode);
  bke::PaintRuntime &paint_runtime = *paint->runtime;
  bool location_sampled = false;
  bool location_success = false;
  /* Use to perform all operations except applying the stroke,
   * needed for operations that require cursor motion (rake). */
  bool is_dry_run = false;
  bool do_random = false;
  bool do_random_mask = false;
  *r_location_is_set = false;
  /* XXX: Use pressure value from first brush step for brushes which don't
   *      support strokes (grab, thumb). They depends on initial state and
   *      brush coord/pressure/etc.
   *      It's more an events design issue, which doesn't split coordinate/pressure/angle
   *      changing events. We should avoid this after events system re-design */
  if (!input_init_) {
    copy_v2_v2(initial_mouse_, mouse);
    copy_v2_v2(paint_runtime.last_rake, mouse);
    copy_v2_v2(paint_runtime.tex_mouse, mouse);
    copy_v2_v2(paint_runtime.mask_tex_mouse, mouse);
    cached_size_pressure_ = pressure;

    input_init_ = true;
  }

  if (paint_supports_dynamic_size(brush, mode)) {
    copy_v2_v2(paint_runtime.tex_mouse, mouse);
    copy_v2_v2(paint_runtime.mask_tex_mouse, mouse);
  }

  /* Truly temporary data that isn't stored in properties */

  paint_runtime.stroke_active = true;
  const float pressure_to_evaluate = paint_supports_dynamic_size(brush, mode) ?
                                         pressure :
                                         cached_size_pressure_;
  paint_runtime.size_pressure_value = BKE_brush_use_size_pressure(&brush) ?
                                          BKE_curvemapping_evaluateF(
                                              brush.curve_size, 0, pressure_to_evaluate) :
                                          1.0f;

  paint_runtime.pixel_radius = BKE_brush_radius_get(paint, &brush) *
                               paint_runtime.size_pressure_value;
  paint_runtime.initial_pixel_radius = BKE_brush_radius_get(paint, &brush);

  if (paint_supports_dynamic_tex_coords(brush, mode)) {

    if (ELEM(brush.mtex.brush_map_mode,
             MTEX_MAP_MODE_VIEW,
             MTEX_MAP_MODE_AREA,
             MTEX_MAP_MODE_RANDOM))
    {
      do_random = true;
    }

    if (brush.mtex.brush_map_mode == MTEX_MAP_MODE_RANDOM) {
      BKE_brush_randomize_texture_coords(paint, false);
    }
    else {
      copy_v2_v2(paint_runtime.tex_mouse, mouse);
    }

    /* take care of mask texture, if any */
    if (brush.mask_mtex.tex) {

      if (ELEM(brush.mask_mtex.brush_map_mode,
               MTEX_MAP_MODE_VIEW,
               MTEX_MAP_MODE_AREA,
               MTEX_MAP_MODE_RANDOM))
      {
        do_random_mask = true;
      }

      if (brush.mask_mtex.brush_map_mode == MTEX_MAP_MODE_RANDOM) {
        BKE_brush_randomize_texture_coords(paint, true);
      }
      else {
        copy_v2_v2(paint_runtime.mask_tex_mouse, mouse);
      }
    }
  }

  if (brush.stroke_method == BRUSH_STROKE_ANCHORED) {
    bool hit = false;
    float2 halfway;

    const float dx = mouse[0] - initial_mouse_[0];
    const float dy = mouse[1] - initial_mouse_[1];

    paint_runtime.anchored_size = paint_runtime.pixel_radius = sqrtf(dx * dx + dy * dy);

    paint_runtime.brush_rotation = paint_runtime.brush_rotation_sec = atan2f(dy, dx) +
                                                                      float(0.5f * M_PI);

    if (brush.flag & BRUSH_EDGE_TO_EDGE) {
      halfway[0] = dx * 0.5f + initial_mouse_[0];
      halfway[1] = dy * 0.5f + initial_mouse_[1];

      if (mode != PaintMode::Texture2D) {
        if (this->get_location(r_location, halfway, original_)) {
          hit = true;
          location_sampled = true;
          location_success = true;
          *r_location_is_set = true;
        }
        else if (!paint_brush_type_require_location(brush, mode)) {
          hit = true;
        }
      }
      else {
        hit = true;
      }
    }
    if (hit) {
      copy_v2_v2(paint_runtime.anchored_initial_mouse, halfway);
      copy_v2_v2(paint_runtime.tex_mouse, halfway);
      copy_v2_v2(paint_runtime.mask_tex_mouse, halfway);
      copy_v2_v2(mouse, halfway);
      paint_runtime.anchored_size /= 2.0f;
      paint_runtime.pixel_radius /= 2.0f;
      stroke_distance_ = paint_runtime.pixel_radius;
    }
    else {
      copy_v2_v2(paint_runtime.anchored_initial_mouse, initial_mouse_);
      copy_v2_v2(mouse, initial_mouse_);
      stroke_distance_ = paint_runtime.pixel_radius;
    }
    paint_runtime.pixel_radius /= zoom_2d_;
    paint_runtime.draw_anchored = true;
  }
  else {
    /* curve strokes do their own rake calculation */
    if (brush.stroke_method != BRUSH_STROKE_CURVE) {
      if (!paint_calculate_rake_rotation(*paint, brush, mouse_init, mode, rake_started_)) {
        /* Not enough motion to define an angle. */
        if (!rake_started_) {
          is_dry_run = true;
        }
      }
      else {
        rake_started_ = true;
      }
    }
  }

  if ((do_random || do_random_mask) && !rng_) {
    /* Lazy initialization. */
    rng_ = RandomNumberGenerator::from_random_seed();
  }

  if (do_random) {
    if (brush.mtex.brush_angle_mode & MTEX_ANGLE_RANDOM) {
      paint_runtime.brush_rotation += -brush.mtex.random_angle / 2.0f +
                                      brush.mtex.random_angle * rng_->get_float();
    }
  }

  if (do_random_mask) {
    if (brush.mask_mtex.brush_angle_mode & MTEX_ANGLE_RANDOM) {
      paint_runtime.brush_rotation_sec += -brush.mask_mtex.random_angle / 2.0f +
                                          brush.mask_mtex.random_angle * rng_->get_float();
    }
  }

  if (!location_sampled) {
    if (mode != PaintMode::Texture2D) {
      if (this->get_location(r_location, mouse, original_)) {
        location_success = true;
        *r_location_is_set = true;
      }
      else if (!paint_brush_type_require_location(brush, mode)) {
        location_success = true;
      }
    }
    else {
      zero_v3(r_location);
      location_success = true;
      /* don't set 'r_location_is_set', since we don't want to use the value. */
    }
  }

  return location_success && !is_dry_run;
}

static bool paint_stroke_use_dash(const Brush &brush)
{
  /* Only these stroke modes support dash lines */
  return ELEM(brush.stroke_method, BRUSH_STROKE_SPACE, BRUSH_STROKE_LINE, BRUSH_STROKE_CURVE);
}

static bool paint_stroke_use_jitter(const PaintMode mode, const Brush &brush, const bool invert)
{
  bool use_jitter = brush.flag & BRUSH_ABSOLUTE_JITTER ? brush.jitter_absolute != 0 :
                                                         brush.jitter != 0;

  /* jitter-ed brush gives weird and unpredictable result for this
   * kinds of stroke, so manually disable jitter usage (sergey) */
  use_jitter &= (ELEM(brush.stroke_method, BRUSH_STROKE_DRAG_DOT, BRUSH_STROKE_ANCHORED)) == 0;
  use_jitter &= !ELEM(mode, PaintMode::Texture2D, PaintMode::Texture3D) ||
                !(invert && brush.image_brush_type == IMAGE_PAINT_BRUSH_TYPE_CLONE);

  return use_jitter;
}

float2 paint_stroke_jitter_pos(Paint *paint,
                               PaintMode mode,
                               const Brush &brush,
                               float pressure,
                               BrushStrokeMode stroke_mode,
                               float zoom_2d,
                               const float2 &mval)
{
  if (paint_stroke_use_jitter(mode, brush, stroke_mode == BrushStrokeMode::Invert)) {
    float factor = zoom_2d;

    if (brush.flag & BRUSH_JITTER_PRESSURE) {
      factor *= BKE_curvemapping_evaluateF(brush.curve_jitter, 0, pressure);
    }

    float2 jittered_position = BKE_brush_jitter_pos(*paint, brush, mval);

    /* XXX: meh, this is round about because
     * BKE_brush_jitter_pos isn't written in the best way to
     * be reused here */
    if (factor != 1.0f) {
      const float2 delta = (jittered_position - mval) * factor;
      return mval + delta;
    }
    return jittered_position;
  }

  return mval;
}

/* Put the location of the next stroke dot into the stroke RNA and apply it to the mesh */
void PaintStroke::add_step(bContext *C, wmOperator *op, const float2 mval, float pressure)
{
  const PaintMode mode = BKE_paintmode_get_active_from_context(C);
  const Brush &brush = *BKE_paint_brush_for_read(this->paint);
  bke::PaintRuntime *paint_runtime = this->paint->runtime;

/* the following code is adapted from texture paint. It may not be needed but leaving here
 * just in case for reference (code in texpaint removed as part of refactoring).
 * It's strange that only texpaint had these guards. */
#if 0
  /* special exception here for too high pressure values on first touch in
   * windows for some tablets, then we just skip first touch. */
  if (tablet && (pressure >= 0.99f) &&
      ((pop->s.brush.flag & BRUSH_SPACING_PRESSURE) ||
       BKE_brush_use_alpha_pressure(pop->s.brush) || BKE_brush_use_size_pressure(pop->s.brush)))
  {
    return;
  }

  /* This can be removed once fixed properly in
   * BKE_brush_painter_paint(
   *     BrushPainter *painter, BrushFunc func,
   *     float *pos, double time, float pressure, void *user);
   * at zero pressure we should do nothing 1/2^12 is 0.0002
   * which is the sensitivity of the most sensitive pen tablet available */
  if (tablet && (pressure < 0.0002f) &&
      ((pop->s.brush.flag & BRUSH_SPACING_PRESSURE) ||
       BKE_brush_use_alpha_pressure(pop->s.brush) || BKE_brush_use_size_pressure(pop->s.brush)))
  {
    return;
  }
#endif

  /* copy last position -before- jittering, or space fill code
   * will create too many dabs */
  this->last_mouse_position = mval;
  last_pressure_ = pressure;

  if (paint_stroke_use_scene_spacing(brush, mode)) {
    BLI_assert(mode != PaintMode::Texture2D);
    float3 world_space_position;

    if (this->get_location(world_space_position, this->last_mouse_position, original_)) {
      last_world_space_position_ = math::transform_point(this->vc.obact->object_to_world(),
                                                         world_space_position);
    }
    else {
      last_world_space_position_ += last_scene_spacing_delta_;
    }
  }

  /* Get jitter position (same as mval if no jitter is used). */
  float2 mouse_out = paint_stroke_jitter_pos(
      this->paint, mode, brush, pressure, stroke_mode_, zoom_2d_, mval);

  float3 location;
  bool is_location_is_set;
  paint_runtime->last_hit = update(
      C, brush, mode, mval, mouse_out, pressure, location, &is_location_is_set);
  if (is_location_is_set) {
    copy_v3_v3(paint_runtime->last_location, location);
  }
  if (!paint_runtime->last_hit) {
    /* For roll mapping, record the press position even during a "dry run"
     * (e.g. rake angle not yet established).  update() sets is_location_is_set
     * when the mesh was actually hit, so the 3D location is valid. Without
     * this, the first recorded roll point ends up one spacing step away from
     * the real mouse-down position. */
    if (need_roll_mapping_ && is_location_is_set) {
      add_roll_point(mval,
                     mouse_out,
                     location,
                     paint_runtime->pixel_radius,
                     pressure,
                     pen_flip_,
                     tilt_.x,
                     tilt_.y);
    }
    return;
  }

  add_roll_point(mval,
                 mouse_out,
                 location,
                 paint_runtime->pixel_radius,
                 pressure,
                 pen_flip_,
                 tilt_.x,
                 tilt_.y);

  if (need_roll_mapping_) {
    make_roll_spline(C);
  }

  /* Dash */
  bool add_step = true;
  if (paint_stroke_use_dash(brush)) {
    const int dash_samples = tot_samples_ % brush.dash_samples;
    const float dash = float(dash_samples) / float(brush.dash_samples);
    if (dash > brush.dash_ratio) {
      add_step = false;
    }
  }

  if (!add_step) {
    ARegion *region = CTX_wm_region(C);
    if (region) {
      ED_region_tag_redraw(region);
    }
    tot_samples_++;
    return;
  }

  /* When roll mapping is active, wait until we have at least one spline segment
   * (requires 4 recorded points). Virtual backward segments are prepended at that
   * point so the first dab already has full spline coverage. */
  PaintStrokePoint *point;

  if (need_roll_mapping_) {
    constexpr int buf_cap = PAINT_MAX_INPUT_SAMPLES;
    const int half = (roll_max_points() >> 1) + 2;

    /* Wait until there are enough real spline segments ahead of the dab
     * position to cover the full brush footprint (≈1 brush radius forward).
     * At num_points_ == half, the real segments ahead of oldest_idx span
     * approximately one brush radius, preventing forward-clamp artifacts.
     * This also handles the num_points_ < 4 case (before any real segment
     * exists), since half >= 5 for all spacing values. */
    if (num_points_ < half) {
      ARegion *region = CTX_wm_region(C);
      if (region) {
        ED_region_tag_redraw(region);
      }
      tot_samples_++;
      return;
    }

    /* Look back half the rolling window to keep the current position centered in the
     * spline for smooth UV mapping. When there is not enough history (early in a stroke),
     * fall back to the oldest available point so painting begins immediately. */
    const int oldest_idx = (cur_point_ - num_points_ + buf_cap) % buf_cap;
    int look_back;
    if (num_points_ <= half) {
      look_back = oldest_idx;
    }
    else {
      look_back = (cur_point_ - half + buf_cap) % buf_cap;
    }

    last_painted_roll_idx_ = look_back;

    /* Place the dab exactly at the look_back position.  Earlier code
     * interpolated halfway to the previous point for smoothing, but that
     * introduced a systematic half-spacing offset at the start of every
     * stroke and a matching gap before finalization. */
    point = &points_[look_back];
  }
  else {
    constexpr int cap = PAINT_MAX_INPUT_SAMPLES;
    point = &points_[(cur_point_ - 1 + cap) % cap];
  }

  /* Add to stroke */
  {
    PointerRNA itemptr;
    RNA_collection_add(op->ptr, "stroke", &itemptr);
    RNA_float_set(&itemptr, "size", point->size);
    RNA_float_set_array(&itemptr, "location", point->location);
    /* Mouse coordinates modified by the stroke type options. */
    RNA_float_set_array(&itemptr, "mouse", point->mouse_out);
    /* Original mouse coordinates. */
    RNA_float_set_array(&itemptr, "mouse_event", point->mouse_in);
    RNA_float_set(&itemptr, "pressure", point->pressure);
    RNA_float_set(&itemptr, "x_tilt", point->x_tilt);
    RNA_float_set(&itemptr, "y_tilt", point->y_tilt);

    this->update_step(op, &itemptr);

    /* don't record this for now, it takes up a lot of memory when doing long
     * strokes with small brush size, and operators have register disabled */
    RNA_collection_clear(op->ptr, "stroke");
  }

  tot_samples_++;
}

/* Returns zero if no sculpt changes should be made, non-zero otherwise */
static bool paint_smooth_stroke(const Brush &brush,
                                const PaintSample *sample,
                                const PaintMode mode,
                                const BrushSwitchMode brush_switch_mode,
                                float zoom_2d,
                                float2 last_mouse_position,
                                float last_pressure,
                                float2 &r_mouse,
                                float &r_pressure)
{
  if (paint_supports_smooth_stroke(brush, mode, brush_switch_mode)) {
    const float radius = brush.smooth_stroke_radius * zoom_2d;
    const float u = brush.smooth_stroke_factor;

    /* If the mouse is moving within the radius of the last move,
     * don't update the mouse position. This allows sharp turns. */
    if (math::distance_squared(last_mouse_position, sample->mouse) < square_f(radius)) {
      return false;
    }

    r_mouse = math::interpolate(sample->mouse, last_mouse_position, u);
    r_pressure = math::interpolate(sample->pressure, last_pressure, u);
  }
  else {
    r_mouse = sample->mouse;
    r_pressure = sample->pressure;
  }

  return true;
}

static float paint_space_stroke_spacing(const ViewContext &vc,
                                        const Paint *paint,
                                        const Brush *brush,
                                        float3 last_world_space_position,
                                        float zoom_2d,
                                        const float size_factor,
                                        const float pressure)
{
  const PaintMode mode = paint->runtime->paint_mode;

  float size_clamp = 0.0f;
  if (paint_stroke_use_scene_spacing(*brush, mode)) {
    const float3 last_object_space_position = math::transform_point(vc.obact->world_to_object(),
                                                                    last_world_space_position);
    size_clamp = object_space_radius_get(
        vc, *paint, *brush, last_object_space_position, size_factor);
  }
  else {
    /* brushes can have a minimum size of 1.0 but with pressure it can be smaller than a pixel
     * causing very high step sizes, hanging blender #32381. */
    size_clamp = max_ff(1.0f, BKE_brush_radius_get(paint, brush) * size_factor);
  }

  float spacing = brush->spacing;

  /* apply spacing pressure */
  if (brush->stroke_method == BRUSH_STROKE_SPACE && brush->flag & BRUSH_SPACING_PRESSURE) {
    spacing = spacing * (1.5f - pressure);
  }

  if (cloth::is_cloth_deform_brush(*brush)) {
    /* The spacing in tools that use the cloth solver should not be affected by the brush radius to
     * avoid affecting the simulation update rate when changing the radius of the brush.
     * With a value of 100 and the brush default of 10 for spacing, a simulation step runs every 2
     * pixels movement of the cursor. */
    size_clamp = 100.0f;
  }

  /* stroke system is used for 2d paint too, so we need to account for
   * the fact that brush can be scaled there. */
  spacing *= zoom_2d;

  if (paint_stroke_use_scene_spacing(*brush, mode)) {
    /* Low pressure on size (with tablets) can cause infinite recursion in paint_space_stroke(),
     * see #129853. */
    return max_ff(FLT_EPSILON, size_clamp * spacing / 50.0f);
  }
  return max_ff(zoom_2d, size_clamp * spacing / 50.0f);
}

static float paint_space_stroke_spacing_no_pressure(const ViewContext &vc,
                                                    const Paint *paint,
                                                    const Brush *brush,
                                                    float3 last_world_space_position,
                                                    float zoom_2d)
{
  /* Unlike many paint pressure curves, spacing assumes that a stroke without pressure (e.g. with
   * the mouse, or with the setting turned off) represents an input of 0.5, not 1.0. */
  return paint_space_stroke_spacing(
      vc, paint, brush, last_world_space_position, zoom_2d, 1.0f, 0.5f);
}

static float paint_stroke_overlapped_curve(const Brush &br, const float x, const float spacing)
{
  /* Avoid division by small numbers, can happen
   * on some pen setups. See #105341.
   */

  const float clamped_spacing = max_ff(spacing, 0.1f);

  const int n = 100 / clamped_spacing;
  const float h = clamped_spacing / 50.0f;
  const float x0 = x - 1;

  float sum = 0;
  for (int i = 0; i < n; i++) {
    float xx = fabsf(x0 + i * h);

    if (xx < 1.0f) {
      sum += BKE_brush_curve_strength(&br, xx, 1);
    }
  }

  return sum;
}

static float paint_stroke_integrate_overlap(const Brush &br, const float factor)
{
  const float spacing = br.spacing * factor;

  if (!(br.flag & BRUSH_SPACE_ATTEN && (br.spacing < 100))) {
    return 1.0;
  }

  constexpr int m = 10;
  float g = 1.0f / m;
  float max = 0;
  for (int i = 0; i < m; i++) {
    const float overlap = fabs(paint_stroke_overlapped_curve(br, i * g, spacing));

    max = std::max(overlap, max);
  }

  if (max == 0.0f) {
    return 1.0f;
  }
  return 1.0f / max;
}

static float paint_space_stroke_spacing_variable(ViewContext &vc,
                                                 const Paint *paint,
                                                 const Brush *brush,
                                                 float3 last_world_space_position,
                                                 float zoom_2d,
                                                 const float last_pressure,
                                                 const float pressure,
                                                 const float pressure_delta,
                                                 const float length)
{
  if (BKE_brush_use_size_pressure(brush)) {
    const float max_size_factor = BKE_curvemapping_evaluateF(brush->curve_size, 0, 1.0f);
    /* use pressure to modify size. set spacing so that at 100%, the circles
     * are aligned nicely with no overlap. for this the spacing needs to be
     * the average of the previous and next size. */
    const float s = paint_space_stroke_spacing(
        vc, paint, brush, last_world_space_position, zoom_2d, max_size_factor, pressure);
    const float q = s * pressure_delta / (2.0f * length);
    const float pressure_fac = (1.0f + q) / (1.0f - q);

    const float last_size_factor = BKE_curvemapping_evaluateF(brush->curve_size, 0, last_pressure);
    const float new_size_factor = BKE_curvemapping_evaluateF(
        brush->curve_size, 0, last_pressure * pressure_fac);

    /* average spacing */
    const float last_spacing = paint_space_stroke_spacing(
        vc, paint, brush, last_world_space_position, zoom_2d, last_size_factor, pressure);
    const float new_spacing = paint_space_stroke_spacing(
        vc, paint, brush, last_world_space_position, zoom_2d, new_size_factor, pressure);

    return 0.5f * (last_spacing + new_spacing);
  }

  /* no size pressure */
  return paint_space_stroke_spacing(
      vc, paint, brush, last_world_space_position, zoom_2d, 1.0f, pressure);
}

/* For brushes with stroke spacing enabled, moves mouse in steps
 * towards the final mouse location. */
int PaintStroke::space_stroke(bContext *C,
                              wmOperator *op,
                              const float2 final_mouse,
                              const float final_pressure)
{
  const ARegion *region = CTX_wm_region(C);
  bke::PaintRuntime *paint_runtime = this->paint->runtime;
  const Paint &paint = *BKE_paint_get_active_from_context(C);
  const PaintMode mode = BKE_paintmode_get_active_from_context(C);
  const Brush &brush = *BKE_paint_brush_for_read(&paint);

  float2 mouse_delta = final_mouse - this->last_mouse_position;
  float length = normalize_v2(mouse_delta);

  float3 world_space_position_delta;
  const bool use_scene_spacing = paint_stroke_use_scene_spacing(brush, mode);
  if (use_scene_spacing) {
    BLI_assert(mode != PaintMode::Texture2D);
    float3 world_space_position;
    const bool hit = this->get_location(world_space_position, final_mouse, original_);
    world_space_position = math::transform_point(this->vc.obact->object_to_world(),
                                                 world_space_position);
    if (hit && stroke_over_mesh_) {
      world_space_position_delta = world_space_position - last_world_space_position_;
      length = math::length(world_space_position_delta);
      stroke_over_mesh_ = true;
    }
    else {
      length = 0.0f;
      world_space_position_delta = {0.0f, 0.0f, 0.0f};
      stroke_over_mesh_ = hit;
      if (stroke_over_mesh_) {
        last_world_space_position_ = world_space_position;
      }
    }
  }

  spacing_raw_ = brush.spacing * 0.01f;

  float pressure = last_pressure_;
  float pressure_delta = final_pressure - last_pressure_;
  const float no_pressure_spacing = paint_space_stroke_spacing_no_pressure(
      this->vc, &paint, &brush, last_world_space_position_, zoom_2d_);
  int count = 0;
  while (length > 0.0f) {
    const float spacing = paint_space_stroke_spacing_variable(this->vc,
                                                              &paint,
                                                              &brush,
                                                              last_world_space_position_,
                                                              zoom_2d_,
                                                              last_pressure_,
                                                              pressure,
                                                              pressure_delta,
                                                              length);
    BLI_assert(spacing >= 0.0f);

    if (length >= spacing) {
      float2 mouse;
      if (use_scene_spacing) {
        float3 final_world_space_position;
        world_space_position_delta = math::normalize(world_space_position_delta);
        final_world_space_position = world_space_position_delta * spacing +
                                     last_world_space_position_;
        ED_view3d_project_v2(region, final_world_space_position, mouse);

        last_scene_spacing_delta_ = world_space_position_delta * spacing;
      }
      else {
        mouse = this->last_mouse_position + mouse_delta * spacing;
      }
      pressure = last_pressure_ + (spacing / length) * pressure_delta;

      paint_runtime->overlap_factor = paint_stroke_integrate_overlap(
          brush, spacing / no_pressure_spacing);

      stroke_distance_ += spacing / zoom_2d_;
      this->add_step(C, op, mouse, pressure);

      length -= spacing;
      pressure = last_pressure_;
      pressure_delta = final_pressure - last_pressure_;

      count++;
    }
    else {
      break;
    }
  }

  return count;
}

static bool print_pressure_status_enabled()
{
  return U.tablet_flag & USER_TABLET_SHOW_DEBUG_VALUES;
}

/**** Public API ****/

PaintStroke::PaintStroke(bContext *C, wmOperator *op, int event_type) : event_type_(event_type)
{
  this->depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  this->paint = BKE_paint_get_active_from_context(C);
  this->ups = &paint->unified_paint_settings;
  bke::PaintRuntime *paint_runtime = this->paint->runtime;
  this->brush = BKE_paint_brush(this->paint);
  RegionView3D *rv3d = CTX_wm_region_view3d(C);

  this->evil_C = C;
  this->vc = ED_view3d_viewcontext_init(C, this->depsgraph);
  this->object = CTX_data_active_object(C);
  this->scene = CTX_data_scene(C);

  stroke_mode_ = BrushStrokeMode(RNA_enum_get(op->ptr, "mode"));
  brush_switch_mode_ = BrushSwitchMode(RNA_enum_get(op->ptr, "brush_toggle"));

  original_ = paint_brush_type_raycast_original(*this->brush,
                                                BKE_paintmode_get_active_from_context(C));

  float zoomx;
  float zoomy;
  get_imapaint_zoom(C, &zoomx, &zoomy);
  zoom_2d_ = std::max(zoomx, zoomy);

  const Brush *br = this->brush;
  if (br->mtex.tex && br->mtex.brush_map_mode == MTEX_MAP_MODE_ROLL) {
    need_roll_mapping_ = true;
  }
  if (br->mask_mtex.tex && br->mask_mtex.brush_map_mode == MTEX_MAP_MODE_ROLL) {
    need_roll_mapping_ = true;
  }
  if (need_roll_mapping_) {
    /* Always-on preview of the unflushed portion of the roll spline. */
    roll_cursor_ = WM_paint_cursor_activate(
        SPACE_TYPE_ANY, RGN_TYPE_ANY, paint_brush_cursor_poll, paint_draw_roll_preview, this);
    /* Register the roll spline debug overlay only when the developer
     * "Paint Debug" option is enabled (Preferences → Developer Extras). */
    if (U.experimental.use_paint_debug) {
      debug_cursor_ = WM_paint_cursor_activate(
          SPACE_TYPE_ANY, RGN_TYPE_ANY, paint_brush_cursor_poll, paint_draw_roll_debug, this);
    }
  }

  /* Check here if color sampling the main brush should do color conversion. This is done here
   * to avoid locking up to get the image buffer during sampling. */
  paint_runtime->do_linear_conversion = false;
  paint_runtime->colorspace = nullptr;

  if (this->brush->mtex.tex && this->brush->mtex.tex->type == TEX_IMAGE &&
      this->brush->mtex.tex->ima)
  {
    ImBuf *tex_ibuf = BKE_image_pool_acquire_ibuf(
        this->brush->mtex.tex->ima, &this->brush->mtex.tex->iuser, nullptr);
    if (tex_ibuf && tex_ibuf->float_buffer.data == nullptr) {
      paint_runtime->do_linear_conversion = true;
      paint_runtime->colorspace = tex_ibuf->byte_buffer.colorspace;
    }
    BKE_image_pool_release_ibuf(this->brush->mtex.tex->ima, tex_ibuf, nullptr);
  }

  if (stroke_mode_ == BrushStrokeMode::Invert) {
    if (this->brush->stroke_method == BRUSH_STROKE_CURVE) {
      RNA_enum_set(op->ptr, "mode", int(BrushStrokeMode::Normal));
    }
  }
  /* initialize here */
  paint_runtime->overlap_factor = 1.0;
  paint_runtime->stroke_active = true;

  if (rv3d) {
    rv3d->rflag |= RV3D_PAINTING;
  }

  /* Preserve location from last stroke while applying and resetting
   * ups->average_stroke_counter to 1.
   */
  if (paint_runtime->average_stroke_counter) {
    mul_v3_fl(paint_runtime->average_stroke_accum,
              1.0f / float(paint_runtime->average_stroke_counter));
    paint_runtime->average_stroke_counter = 1;
  }

  /* initialize here to avoid initialization conflict with threaded strokes */
  bke::brush::common_pressure_curves_init(*this->brush);
  if (this->paint->flags & PAINT_USE_CAVITY_MASK) {
    BKE_curvemapping_init(this->paint->cavity_curve);
  }

  BKE_paint_set_overlay_override(eOverlayFlags(this->brush->overlay_flags));

  paint_runtime->start_pixel_radius = BKE_brush_radius_get(this->paint, this->brush);
}

void PaintStroke::free(bContext *C, wmOperator * /*op*/)
{
  if (RegionView3D *rv3d = CTX_wm_region_view3d(C)) {
    rv3d->rflag &= ~RV3D_PAINTING;
  }

  BKE_paint_set_overlay_override(eOverlayFlags(0));

  bke::PaintRuntime *paint_runtime = this->paint->runtime;
  paint_runtime->draw_anchored = false;
  paint_runtime->stroke_active = false;

  if (timer_) {
    WM_event_timer_remove(CTX_wm_manager(C), CTX_wm_window(C), timer_);
  }

  if (stroke_cursor_) {
    WM_paint_cursor_end(static_cast<wmPaintCursor *>(stroke_cursor_));
  }
  if (roll_cursor_) {
    WM_paint_cursor_end(static_cast<wmPaintCursor *>(roll_cursor_));
  }
  if (debug_cursor_) {
    WM_paint_cursor_end(static_cast<wmPaintCursor *>(debug_cursor_));
  }
}

void PaintStroke::stroke_done(bContext *C, wmOperator *op, const bool is_cancel)
{
  if (print_pressure_status_enabled()) {
    ED_workspace_status_text(C, nullptr);
  }
  bke::PaintRuntime *paint_runtime = this->paint->runtime;

  /* reset rotation here to avoid doing so in cursor display */
  if (this->brush) {
    if (!(this->brush->mtex.brush_angle_mode & MTEX_ANGLE_RAKE)) {
      paint_runtime->brush_rotation = 0.0f;
    }

    if (!(this->brush->mask_mtex.brush_angle_mode & MTEX_ANGLE_RAKE)) {
      paint_runtime->brush_rotation_sec = 0.0f;
    }
  }

  if (stroke_started_) {
    this->redraw(true);

    this->done(is_cancel);
  }

  this->free(C, op);
}

static bool curves_sculpt_brush_uses_spacing(const eBrushCurvesSculptType tool)
{
  return ELEM(tool, CURVES_SCULPT_BRUSH_TYPE_ADD, CURVES_SCULPT_BRUSH_TYPE_DENSITY);
}

bool paint_space_stroke_enabled(const Brush &br, const PaintMode mode)
{
  if (br.stroke_method != BRUSH_STROKE_SPACE) {
    return false;
  }

  if (br.sculpt_brush_type == SCULPT_BRUSH_TYPE_CLOTH || cloth::is_cloth_deform_brush(br)) {
    /* The Cloth Brush is a special case for stroke spacing. Even if it has grab modes which do
     * not support dynamic size, stroke spacing needs to be enabled so it is possible to control
     * whether the simulation runs constantly or only when the brush moves when using the cloth
     * grab brushes. */
    return true;
  }

  if (mode == PaintMode::SculptCurves &&
      !curves_sculpt_brush_uses_spacing(eBrushCurvesSculptType(br.curves_sculpt_brush_type)))
  {
    return false;
  }

  if (ELEM(mode, PaintMode::GPencil, PaintMode::SculptGPencil)) {
    /* No spacing needed for now. */
    return false;
  }

  return paint_supports_dynamic_size(br, mode);
}

static bool sculpt_is_grab_tool(const Brush &br)
{
  if (br.sculpt_brush_type == SCULPT_BRUSH_TYPE_CLOTH &&
      br.cloth_deform_type == BRUSH_CLOTH_DEFORM_GRAB)
  {
    return true;
  }
  return ELEM(br.sculpt_brush_type,
              SCULPT_BRUSH_TYPE_GRAB,
              SCULPT_BRUSH_TYPE_ELASTIC_DEFORM,
              SCULPT_BRUSH_TYPE_POSE,
              SCULPT_BRUSH_TYPE_BOUNDARY,
              SCULPT_BRUSH_TYPE_THUMB,
              SCULPT_BRUSH_TYPE_ROTATE,
              SCULPT_BRUSH_TYPE_SNAKE_HOOK);
}

bool paint_supports_dynamic_size(const Brush &br, const PaintMode mode)
{
  if (br.stroke_method == BRUSH_STROKE_ANCHORED) {
    return false;
  }

  switch (mode) {
    case PaintMode::Sculpt:
      return bke::brush::supports_size_pressure(br);
      break;

    case PaintMode::Texture2D: /* fall through */
    case PaintMode::Texture3D:
      if ((br.image_brush_type == IMAGE_PAINT_BRUSH_TYPE_FILL) && (br.flag & BRUSH_USE_GRADIENT)) {
        return false;
      }
      break;

    default:
      break;
  }
  return true;
}

bool paint_supports_smooth_stroke(const Brush &brush,
                                  const PaintMode mode,
                                  const BrushSwitchMode brush_switch_mode)
{
  /* The grease pencil draw tool needs to enable this when the `stroke_mode` is set to
   * `BrushSwitchMode::Smooth`. */
  if (mode == PaintMode::GPencil &&
      eBrushGPaintType(brush.gpencil_brush_type) == GPAINT_BRUSH_TYPE_DRAW &&
      brush_switch_mode == BrushSwitchMode::Smooth)
  {
    return true;
  }
  if (!(brush.flag & BRUSH_SMOOTH_STROKE) ||
      ELEM(brush.stroke_method, BRUSH_STROKE_ANCHORED, BRUSH_STROKE_DRAG_DOT, BRUSH_STROKE_LINE))
  {
    return false;
  }

  switch (mode) {
    case PaintMode::Sculpt:
      if (sculpt_is_grab_tool(brush)) {
        return false;
      }
      break;
    default:
      break;
  }
  return true;
}

bool paint_supports_texture(const PaintMode mode)
{
  /* omit: PAINT_WEIGHT, PAINT_SCULPT_UV, PAINT_INVALID */
  return ELEM(
      mode, PaintMode::Sculpt, PaintMode::Vertex, PaintMode::Texture3D, PaintMode::Texture2D);
}

bool paint_supports_dynamic_tex_coords(const Brush &br, const PaintMode mode)
{
  if (br.stroke_method == BRUSH_STROKE_ANCHORED) {
    return false;
  }

  switch (mode) {
    case PaintMode::Sculpt:
      if (sculpt_is_grab_tool(br)) {
        return false;
      }
      break;
    default:
      break;
  }
  return true;
}

#define PAINT_STROKE_MODAL_CANCEL 1

wmKeyMap *paint_stroke_modal_keymap(wmKeyConfig *keyconf)
{
  static const EnumPropertyItem modal_items[] = {
      {PAINT_STROKE_MODAL_CANCEL, "CANCEL", 0, "Cancel", "Cancel and undo a stroke in progress"},
      {0}};

  static const char *name = "Paint Stroke Modal";

  wmKeyMap *keymap = WM_modalkeymap_find(keyconf, name);

  /* This function is called for each space-type, only needs to add map once. */
  if (!keymap) {
    keymap = WM_modalkeymap_ensure(keyconf, name, modal_items);
  }

  return keymap;
}

void PaintStroke::add_sample(const int input_samples,
                             const float x,
                             const float y,
                             const float pressure)
{
  PaintSample *sample = &samples_[cur_sample_];
  const int max_samples = std::clamp(input_samples, 1, PAINT_MAX_INPUT_SAMPLES);

  sample->mouse[0] = x;
  sample->mouse[1] = y;
  sample->pressure = pressure;

  cur_sample_++;
  if (cur_sample_ >= max_samples) {
    cur_sample_ = 0;
  }
  if (num_samples_ < max_samples) {
    num_samples_++;
  }
}

void PaintStroke::calc_average_sample(PaintSample *average)
{
  BLI_assert(num_samples_ > 0);

  for (int i = 0; i < num_samples_; i++) {
    average->mouse += samples_[i].mouse;
    average->pressure += samples_[i].pressure;
  }

  average->mouse /= num_samples_;
  average->pressure /= num_samples_;

  // printf("avg=(%f, %f), num=%d\n", average->mouse[0], average->mouse[1], stroke->num_samples);
}

/**
 * Slightly different version of spacing for line/curve strokes,
 * makes sure the dabs stay on the line path.
 */
void PaintStroke::lines_spacing(bContext *C,
                                wmOperator *op,
                                const float spacing,
                                float *length_residue,
                                const float2 old_pos,
                                const float2 new_pos)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  bke::PaintRuntime *paint_runtime = paint->runtime;
  const Brush &brush = *BKE_paint_brush(paint);
  const PaintMode mode = BKE_paintmode_get_active_from_context(C);
  const ARegion *region = CTX_wm_region(C);

  const bool use_scene_spacing = paint_stroke_use_scene_spacing(brush, mode);

  float2 mouse_delta;
  float length;
  float3 world_space_position_delta;
  float3 world_space_position_old;

  this->last_mouse_position = old_pos;

  if (use_scene_spacing) {
    BLI_assert(mode != PaintMode::Texture2D);
    const bool hit_old = this->get_location(world_space_position_old, old_pos, original_);

    float3 world_space_position_new;
    const bool hit_new = this->get_location(world_space_position_new, new_pos, original_);

    world_space_position_old = math::transform_point(this->vc.obact->object_to_world(),
                                                     world_space_position_old);
    world_space_position_new = math::transform_point(this->vc.obact->object_to_world(),
                                                     world_space_position_new);
    if (hit_old && hit_new && stroke_over_mesh_) {
      world_space_position_delta = world_space_position_new - world_space_position_old;
      length = math::length(world_space_position_delta);
      stroke_over_mesh_ = true;
    }
    else {
      length = 0.0f;
      world_space_position_delta = {0.0f, 0.0f, 0.0f};
      stroke_over_mesh_ = hit_new;
      if (stroke_over_mesh_) {
        last_world_space_position_ = world_space_position_old;
      }
    }
  }
  else {
    mouse_delta = new_pos - old_pos;
    mouse_delta = math::normalize_and_get_length(mouse_delta, length);
  }

  BLI_assert(length >= 0.0f);

  if (length == 0.0f) {
    return;
  }

  float2 mouse;
  while (length > 0.0f) {
    float spacing_final = spacing - *length_residue;
    length += *length_residue;
    *length_residue = 0.0;

    if (length >= spacing) {
      if (use_scene_spacing) {
        world_space_position_delta = math::normalize(world_space_position_delta);
        const float3 final_world_space_position = world_space_position_delta * spacing_final +
                                                  world_space_position_old;
        ED_view3d_project_v2(region, final_world_space_position, mouse);
      }
      else {
        mouse = this->last_mouse_position + mouse_delta * spacing_final;
      }

      paint_runtime->overlap_factor = paint_stroke_integrate_overlap(brush, 1.0);

      stroke_distance_ += spacing / zoom_2d_;
      this->add_step(C, op, mouse, 1.0);

      length -= spacing;
      spacing_final = spacing;
    }
    else {
      break;
    }
  }

  *length_residue = length;
}

void PaintStroke::line_end(bContext *C, wmOperator *op, const float2 mouse)
{
  Brush *br = this->brush;
  bke::PaintRuntime *paint_runtime = this->paint->runtime;
  if (stroke_started_ && br->stroke_method == BRUSH_STROKE_LINE) {
    paint_runtime->overlap_factor = paint_stroke_integrate_overlap(*br, 1.0);

    this->add_step(C, op, this->last_mouse_position, 1.0);
    this->space_stroke(C, op, mouse, 1.0);
  }
}

bool PaintStroke::curve_end(bContext *C, wmOperator *op)
{
  const Brush &br = *this->brush;
  if (br.stroke_method != BRUSH_STROKE_CURVE) {
    return false;
  }

  Paint *paint = BKE_paint_get_active_from_context(C);
  bke::PaintRuntime *paint_runtime = paint->runtime;
  const PaintMode mode = paint_runtime->paint_mode;
  const float no_pressure_spacing = paint_space_stroke_spacing_no_pressure(
      this->vc, paint, this->brush, last_world_space_position_, zoom_2d_);
  const PaintCurve *pc = br.paint_curve;

  if (!pc) {
    return true;
  }

#ifdef DEBUG_TIME
  TIMEIT_START_AVERAGED(whole_stroke);
#endif

  const PaintCurvePoint *pcp = pc->points;
  paint_runtime->overlap_factor = paint_stroke_integrate_overlap(br, 1.0);

  float length_residue = 0.0f;
  for (int i = 0; i < pc->tot_points - 1; i++, pcp++) {
    float data[(PAINT_CURVE_NUM_SEGMENTS + 1) * 2];
    float tangents[(PAINT_CURVE_NUM_SEGMENTS + 1) * 2];
    const PaintCurvePoint *pcp_next = pcp + 1;
    bool do_rake = false;

    for (int j = 0; j < 2; j++) {
      BKE_curve_forward_diff_bezier(pcp->bez.vec[1][j],
                                    pcp->bez.vec[2][j],
                                    pcp_next->bez.vec[0][j],
                                    pcp_next->bez.vec[1][j],
                                    data + j,
                                    PAINT_CURVE_NUM_SEGMENTS,
                                    sizeof(float[2]));
    }

    if ((br.mtex.brush_angle_mode & MTEX_ANGLE_RAKE) ||
        (br.mask_mtex.brush_angle_mode & MTEX_ANGLE_RAKE))
    {
      do_rake = true;
      for (int j = 0; j < 2; j++) {
        BKE_curve_forward_diff_tangent_bezier(pcp->bez.vec[1][j],
                                              pcp->bez.vec[2][j],
                                              pcp_next->bez.vec[0][j],
                                              pcp_next->bez.vec[1][j],
                                              tangents + j,
                                              PAINT_CURVE_NUM_SEGMENTS,
                                              sizeof(float[2]));
      }
    }

    for (int j = 0; j < PAINT_CURVE_NUM_SEGMENTS; j++) {
      if (do_rake) {
        const float rotation = atan2f(tangents[2 * j + 1], tangents[2 * j]) + float(0.5f * M_PI);
        paint_update_brush_rake_rotation(*paint, br, rotation);
      }

      if (!stroke_started_) {
        last_pressure_ = 1.0;
        copy_v2_v2(this->last_mouse_position, data + 2 * j);

        if (paint_stroke_use_scene_spacing(br, mode)) {
          BLI_assert(mode != PaintMode::Texture2D);
          stroke_over_mesh_ = this->get_location(
              last_world_space_position_, data + 2 * j, original_);
          mul_m4_v3(this->vc.obact->object_to_world().ptr(), last_world_space_position_);
        }

        stroke_started_ = this->test_start(op, this->last_mouse_position);

        if (stroke_started_) {
          this->add_step(C, op, data + 2 * j, 1.0);
          this->lines_spacing(
              C, op, no_pressure_spacing, &length_residue, data + 2 * j, data + 2 * (j + 1));
        }
      }
      else {
        this->lines_spacing(
            C, op, no_pressure_spacing, &length_residue, data + 2 * j, data + 2 * (j + 1));
      }
    }
  }

  /* Flush remaining deferred roll dabs before finishing the curve stroke. */
  if (need_roll_mapping_) {
    this->finish_roll_stroke(C, op, this->last_mouse_position, 1.0f);
  }

  this->stroke_done(C, op, false);

#ifdef DEBUG_TIME
  TIMEIT_END_AVERAGED(whole_stroke);
#endif

  return true;
}

static void paint_stroke_line_constrain(float2 last_mouse_position,
                                        float2 constrained_pos,
                                        float2 &mouse)
{
  float2 line = mouse - last_mouse_position;
  float angle = atan2f(line[1], line[0]);
  const float len = math::length(line);

  /* divide angle by PI/4 */
  angle = 4.0f * angle / float(M_PI);

  /* now take residue */
  const float res = angle - floorf(angle);

  /* residue decides how close we are at a certain angle */
  if (res <= 0.5f) {
    angle = floorf(angle) * float(M_PI_4);
  }
  else {
    angle = (floorf(angle) + 1.0f) * float(M_PI_4);
  }

  mouse[0] = constrained_pos[0] = len * cosf(angle) + last_mouse_position[0];
  mouse[1] = constrained_pos[1] = len * sinf(angle) + last_mouse_position[1];
}

wmOperatorStatus PaintStroke::modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  /* TODO: Temporary, used to facilitate removing bContext usage in subclasses */
  this->evil_C = C;

  Paint *paint = BKE_paint_get_active_from_context(C);
  const Brush *br = this->brush = BKE_paint_brush(paint);
  if (paint == nullptr || br == nullptr) {
    /* In some circumstances, the context may change during modal execution. In this case,
     * we need to cancel the operator. See #147544 and related issues for further information. */
    this->stroke_done(C, op, true);
    return OPERATOR_CANCELLED;
  }
  const PaintMode mode = BKE_paintmode_get_active_from_context(C);
  bke::PaintRuntime &paint_runtime = *paint->runtime;
  bool first_dab = false;
  bool first_modal = false;
  bool needs_redraw = false;

  if (event->type == INBETWEEN_MOUSEMOVE &&
      !paint_brush_type_require_inbetween_mouse_events(*br, mode))
  {
    return OPERATOR_RUNNING_MODAL;
  }

  /* see if tablet affects event. Line, anchored and drag dot strokes do not support pressure */
  const float tablet_pressure = WM_event_tablet_data(event, &pen_flip_, nullptr);
  float pressure =
      ELEM(br->stroke_method, BRUSH_STROKE_LINE, BRUSH_STROKE_ANCHORED, BRUSH_STROKE_DRAG_DOT) ?
          1.0f :
          tablet_pressure;

  if (print_pressure_status_enabled() && WM_event_is_tablet(event)) {
    std::string msg = fmt::format("Tablet Pressure: {:.4f}", pressure);
    ED_workspace_status_text(C, msg.c_str());
  }

  /* When processing a timer event the pressure from the event is 0, so use the last valid
   * pressure. */
  if (event->type == TIMER) {
    pressure = last_tablet_event_pressure_;
  }
  else {
    last_tablet_event_pressure_ = pressure;
  }

  PaintSample sample_average;
  if (!need_roll_mapping_) {
    const int input_samples = BKE_brush_input_samples_get(paint, br);
    this->add_sample(input_samples, event->mval[0], event->mval[1], pressure);
    this->calc_average_sample(&sample_average);
  }
  else {
    sample_average.mouse[0] = float(event->mval[0]);
    sample_average.mouse[1] = float(event->mval[1]);
    sample_average.pressure = pressure;
  }

  if (stroke_sample_index_ == 0) {
    this->last_mouse_position[0] = event->mval[0];
    this->last_mouse_position[1] = event->mval[1];
  }
  stroke_sample_index_++;

  /* Tilt. */
  if (WM_event_is_tablet(event)) {
    tilt_ = event->tablet.tilt;
  }

#ifdef WITH_INPUT_NDOF
  /* let NDOF motion pass through to the 3D view so we can paint and rotate simultaneously!
   * this isn't perfect... even when an extra MOUSEMOVE is spoofed, the stroke discards it
   * since the 2D deltas are zero -- code in this file needs to be updated to use the
   * post-NDOF_MOTION MOUSEMOVE */
  if (event->type == NDOF_MOTION) {
    return OPERATOR_PASS_THROUGH;
  }
#endif

  /* one time initialization */
  if (!stroke_init_) {
    if (this->curve_end(C, op)) {
      return OPERATOR_FINISHED;
    }

    stroke_init_ = true;
    first_modal = true;
  }

  /* one time stroke initialization */
  if (!stroke_started_) {
    RNA_boolean_set(op->ptr, "pen_flip", pen_flip_);

    last_pressure_ = sample_average.pressure;
    this->last_mouse_position = sample_average.mouse;
    if (paint_stroke_use_scene_spacing(*br, mode)) {
      BLI_assert(mode != PaintMode::Texture2D);
      stroke_over_mesh_ = this->get_location(
          last_world_space_position_, sample_average.mouse, original_);
      last_world_space_position_ = math::transform_point(this->vc.obact->object_to_world(),
                                                         last_world_space_position_);
    }
    stroke_started_ = this->test_start(op, sample_average.mouse);

    if (stroke_started_) {
      /* StrokeTestStart often updates the currently active brush so we need to re-retrieve it
       * here. */
      br = BKE_paint_brush(paint);

      if (paint_supports_smooth_stroke(*br, mode, brush_switch_mode_)) {

        stroke_cursor_ = WM_paint_cursor_activate(
            SPACE_TYPE_ANY, RGN_TYPE_ANY, paint_brush_cursor_poll, paint_draw_smooth_cursor, this);
      }

      if (br->stroke_method == BRUSH_STROKE_AIRBRUSH) {
        timer_ = WM_event_timer_add(CTX_wm_manager(C), CTX_wm_window(C), TIMER, this->brush->rate);
      }

      if (br->stroke_method == BRUSH_STROKE_LINE) {
        stroke_cursor_ = WM_paint_cursor_activate(
            SPACE_TYPE_ANY, RGN_TYPE_ANY, paint_brush_cursor_poll, paint_draw_line_cursor, this);
      }

      first_dab = true;
    }
  }

  /* Cancel */
  if (event->type == EVT_MODAL_MAP && event->val == PAINT_STROKE_MODAL_CANCEL) {
    if (op->type->cancel) {
      if (this->test_cancel()) {
        op->type->cancel(C, op);
        return OPERATOR_CANCELLED;
      }
    }
    BKE_report(op->reports, RPT_WARNING, "Cancelling this stroke is unsupported");
  }

  /* Handles shift-key active smooth toggling during a grease pencil stroke. */
  if (mode == PaintMode::GPencil) {
    if (event->modifier & KM_SHIFT) {
      brush_switch_mode_ = BrushSwitchMode::Smooth;
      if (!stroke_cursor_) {
        stroke_cursor_ = WM_paint_cursor_activate(
            SPACE_TYPE_ANY, RGN_TYPE_ANY, paint_brush_cursor_poll, paint_draw_smooth_cursor, this);
      }
    }
    else {
      stroke_mode_ = BrushStrokeMode::Normal;
      if (stroke_cursor_ != nullptr) {
        WM_paint_cursor_end(static_cast<wmPaintCursor *>(stroke_cursor_));
        stroke_cursor_ = nullptr;
      }
    }
  }

  float2 mouse;
  if (event->type == event_type_ && !first_modal) {
    if (event->val == KM_RELEASE) {
      mouse = {float(event->mval[0]), float(event->mval[1])};
      if (this->constrain_line) {
        paint_stroke_line_constrain(this->last_mouse_position, this->constrained_pos, mouse);
      }
      this->line_end(C, op, mouse);
      /* For smooth stroke, finalize at the last smoothed position,
       * not the raw cursor position. */
      const float2 roll_mouse =
          paint_supports_smooth_stroke(*br, mode, brush_switch_mode_) ?
              last_smoothed_mouse_ : mouse;
      this->finish_roll_stroke(C, op, roll_mouse, pressure);
      this->stroke_done(C, op, false);
      return OPERATOR_FINISHED;
    }
  }
  else if (ELEM(event->type, EVT_RETKEY, EVT_SPACEKEY)) {
    this->line_end(C, op, sample_average.mouse);
    const float2 roll_mouse2 =
        paint_supports_smooth_stroke(*br, mode, brush_switch_mode_) ?
            last_smoothed_mouse_ : sample_average.mouse;
    this->finish_roll_stroke(C, op, roll_mouse2, pressure);
    this->stroke_done(C, op, false);
    return OPERATOR_FINISHED;
  }
  else if (br->stroke_method == BRUSH_STROKE_LINE) {
    if (event->modifier & KM_ALT) {
      this->constrain_line = true;
    }
    else {
      this->constrain_line = false;
    }

    mouse = {float(event->mval[0]), float(event->mval[1])};
    paint_stroke_line_constrain(this->last_mouse_position, this->constrained_pos, mouse);

    if (stroke_started_ && (first_modal || ISMOUSE_MOTION(event->type))) {
      if ((br->mtex.brush_angle_mode & MTEX_ANGLE_RAKE) ||
          (br->mask_mtex.brush_angle_mode & MTEX_ANGLE_RAKE))
      {
        copy_v2_v2(paint_runtime.last_rake, this->last_mouse_position);
      }
      paint_calculate_rake_rotation(*paint, *br, mouse, mode, true);
    }
  }
  else if (first_modal ||
           /* regular dabs */
           (!(br->stroke_method == BRUSH_STROKE_AIRBRUSH) && ISMOUSE_MOTION(event->type)) ||
           /* airbrush */
           ((br->stroke_method == BRUSH_STROKE_AIRBRUSH) && event->type == TIMER &&
            event->customdata == timer_))
  {
    if (paint_smooth_stroke(*this->brush,
                            &sample_average,
                            mode,
                            brush_switch_mode_,
                            zoom_2d_,
                            this->last_mouse_position,
                            last_pressure_,
                            mouse,
                            pressure))
    {
      last_smoothed_mouse_ = mouse;
      if (stroke_started_) {
        if (paint_space_stroke_enabled(*br, mode)) {
          if (this->space_stroke(C, op, mouse, pressure)) {
            needs_redraw = true;
          }
        }
        else {
          const float2 mouse_delta = mouse - this->last_mouse_position;
          stroke_distance_ += math::length(mouse_delta);
          this->add_step(C, op, mouse, pressure);
          needs_redraw = true;
        }
      }
    }
  }

  /* we want the stroke to have the first daub at the start location
   * instead of waiting till we have moved the space distance */
  if (first_dab && paint_space_stroke_enabled(*br, mode) && !(br->flag & BRUSH_SMOOTH_STROKE)) {
    paint_runtime.overlap_factor = paint_stroke_integrate_overlap(*br, 1.0);
    this->add_step(C, op, sample_average.mouse, sample_average.pressure);
    needs_redraw = true;
  }

  /* Don't update the paint cursor in #INBETWEEN_MOUSEMOVE events. */
  if (event->type != INBETWEEN_MOUSEMOVE) {
    wmWindow *window = CTX_wm_window(C);
    ARegion *region = CTX_wm_region(C);

    if (region && (paint->flags & PAINT_SHOW_BRUSH)) {
      WM_paint_cursor_tag_redraw(window, region);
    }
  }

  /* Draw for all events (even in between) otherwise updating the brush
   * display is noticeably delayed.
   */
  if (needs_redraw) {
    this->redraw(false);
  }

  return OPERATOR_RUNNING_MODAL;
}

wmOperatorStatus PaintStroke::exec(bContext *C, wmOperator *op)
{
  /* TODO: Temporary, used to facilitate removing bContext usage in subclasses */
  this->evil_C = C;

  const PaintMode mode = BKE_paintmode_get_active_from_context(C);
  PropertyRNA *prop = RNA_struct_find_property(op->ptr, "override_location");
  const bool override_location = prop && RNA_property_boolean_get(op->ptr, prop) &&
                                 mode != PaintMode::Texture2D;

  RNA_BEGIN (op->ptr, itemptr, "stroke") {
    float2 mval;
    RNA_float_get_array(&itemptr, "mouse_event", mval);

    if (!stroke_started_) {
      stroke_started_ = this->test_start(op, mval);
    }

    if (!stroke_started_) {
      continue;
    }

    /* This mimics `add_step` to update various properties on PaintRuntime. */
    const float pressure = RNA_float_get(&itemptr, "pressure");
    float2 mouse_out = paint_stroke_jitter_pos(
        this->paint, mode, *this->brush, pressure, stroke_mode_, zoom_2d_, mval);

    /* TODO: This process misses updating some values at the moment, see `add_step` */
    float3 dummy_location;
    bool dummy_is_set;
    this->update(C, *this->brush, mode, mval, mouse_out, pressure, dummy_location, &dummy_is_set);
    RNA_float_set_array(&itemptr, "mouse", mouse_out);

    if (override_location) {
      float3 location;
      if (this->get_location(location, mouse_out, false)) {
        RNA_float_set_array(&itemptr, "location", location);
        this->update_step(op, &itemptr);
      }
    }
    else {
      this->update_step(op, &itemptr);
    }
  }
  RNA_END;

  const bool ok = stroke_started_;

  this->stroke_done(C, op, !ok);

  return ok ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

void PaintStroke::cancel(bContext *C, wmOperator *op)
{
  this->stroke_done(C, op, true);
}

static const bToolRef *brush_tool_get(const ScrArea *area, const ARegion *region)
{
  if ((area && ELEM(area->spacetype, SPACE_VIEW3D, SPACE_IMAGE)) &&
      (region && region->regiontype == RGN_TYPE_WINDOW))
  {
    if (area->runtime.tool && area->runtime.tool->runtime &&
        (area->runtime.tool->runtime->flag & TOOLREF_FLAG_USE_BRUSHES))
    {
      return area->runtime.tool;
    }
  }
  return nullptr;
}

bool paint_brush_tool_poll(bContext *C)
{
  const Paint *paint = BKE_paint_get_active_from_context(C);
  const Object *ob = CTX_data_active_object(C);
  const ScrArea *area = CTX_wm_area(C);
  const ARegion *region = CTX_wm_region(C);
  return paint_brush_tool_poll(area, region, paint, ob);
}

bool paint_brush_tool_poll(const ScrArea *area,
                           const ARegion *region,
                           const Paint *paint,
                           const Object *ob)
{
  if (!paint) {
    return false;
  }

  if (!BKE_paint_brush_for_read(paint)) {
    return false;
  }

  const bToolRef *tref = brush_tool_get(area, region);
  if (!tref) {
    return false;
  }

  if (ob) {
    return true;
  }

  /* Be permissive painting in the Image Editor without an active object. */
  return BKE_paintmode_get_from_tool(tref) == PaintMode::Texture2D;
}

bool paint_brush_cursor_poll(bContext *C)
{
  Paint *paint = BKE_paint_get_active_from_context(C);

  if (!paint) {
    return false;
  }

  if (!BKE_paint_brush_for_read(paint)) {
    return false;
  }

  const ScrArea *area = CTX_wm_area(C);
  const ARegion *region = CTX_wm_region(C);
  const bToolRef *tref = brush_tool_get(area, region);

  if (!tref) {
    return false;
  }

  /* Don't use brush cursor when the tool sets its own cursor. */
  if (tref->runtime->cursor != WM_CURSOR_DEFAULT) {
    return false;
  }

  if (CTX_data_active_object(C)) {
    return true;
  }

  /* Be permissive painting in the Image Editor without an active object. */
  return BKE_paintmode_get_from_tool(tref) == PaintMode::Texture2D;
}

}  // namespace blender::ed::sculpt_paint
