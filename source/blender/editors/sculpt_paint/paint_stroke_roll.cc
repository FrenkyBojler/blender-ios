/* SPDX-FileCopyrightText: 2009 by Nicholas Bishop. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Roll texture mapping for paint strokes.
 *
 * This file implements the arc-length-parameterized spline (RollSpline),
 * virtual extension points, the surface-interpolation grid (CC subdivision,
 * Laplacian smoothing, LUT rasterization), and the UV lookup used by
 * sculpt_apply_texture() when the brush map mode is MTEX_MAP_MODE_ROLL.
 */

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_length_parameterize.hh"
#include "BLI_utildefines.h"

#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_userdef_types.h"

#include "RNA_access.hh"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"

#include "WM_api.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "ED_view3d.hh"

#include "paint_intern.hh"

#include "mesh/sculpt_intern.hh"

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
  knots_3d.clear();
  resolution = kRollResolution;
}

void RollSpline::smooth_evaluate_3d(int seg, float t, float3 &r_pos, float3 &r_tan) const
{
  /* Map polyline segment + t back to Catmull-Rom knot span + parameter.
   * Each knot span has `resolution` polyline segments, so:
   *   span = seg / resolution
   *   u    = (seg % resolution + t) / resolution
   */
  const int n_knots = int(knots_3d.size());
  if (n_knots < 2) {
    r_pos = poly_3d.is_empty() ? float3(0) : poly_3d[std::min(seg, int(poly_3d.size()) - 1)];
    r_tan = tangents_3d.is_empty() ? float3(1, 0, 0) : tangents_3d[std::min(seg, int(tangents_3d.size()) - 1)];
    return;
  }

  const int resolution = this->resolution;
  const int span = std::min(seg / resolution, n_knots - 2);
  const float u = std::clamp(
      (float(seg % resolution) + t) / float(resolution), 0.0f, 1.0f);

  /* Catmull-Rom control points (clamped at boundaries). */
  const int i0 = std::max(span - 1, 0);
  const int i1 = span;
  const int i2 = std::min(span + 1, n_knots - 1);
  const int i3 = std::min(span + 2, n_knots - 1);

  r_pos = bke::curves::catmull_rom::interpolate(
      knots_3d[i0], knots_3d[i1], knots_3d[i2], knots_3d[i3], u);

  /* Tangent = derivative of Catmull-Rom at u.
   * d/du CR(p0,p1,p2,p3, u) can be computed via the derivative of the
   * basis functions, or by finite difference at a small epsilon. */
  constexpr float eps = 1e-3f;
  const float u_lo = std::max(0.0f, u - eps);
  const float u_hi = std::min(1.0f, u + eps);
  const float3 pos_lo = bke::curves::catmull_rom::interpolate(
      knots_3d[i0], knots_3d[i1], knots_3d[i2], knots_3d[i3], u_lo);
  const float3 pos_hi = bke::curves::catmull_rom::interpolate(
      knots_3d[i0], knots_3d[i1], knots_3d[i2], knots_3d[i3], u_hi);
  r_tan = math::normalize(pos_hi - pos_lo);
}

void RollSpline::refine_closest_smooth(const float3 &query,
                                       int poly_seg,
                                       float poly_t,
                                       float3 &r_pos,
                                       float3 &r_tan,
                                       float &r_arc_len) const
{
  const int n_knots = int(knots_3d.size());
  if (n_knots < 2) {
    /* Fall back to polyline interpolation when no smooth curve is available. */
    r_pos = math::interpolate(poly_3d[poly_seg], poly_3d[poly_seg + 1], poly_t);
    r_tan = math::normalize(
        math::interpolate(tangents_3d[poly_seg], tangents_3d[poly_seg + 1], poly_t));
    const float s0 = (poly_seg > 0) ? lengths_3d[poly_seg - 1] : 0.0f;
    const float s1 = lengths_3d[poly_seg];
    r_arc_len = s0 + poly_t * (s1 - s0);
    return;
  }

  const int res = this->resolution;

  /* Map polyline segment to Catmull-Rom span + parameter u in [0,1]. */
  int span = std::min(poly_seg / res, n_knots - 2);
  float u = std::clamp(
      (float(poly_seg % res) + poly_t) / float(res), 0.0f, 1.0f);

  /* Catmull-Rom control points (clamped at boundaries). */
  const int i0 = std::max(span - 1, 0);
  const int i1 = span;
  const int i2 = std::min(span + 1, n_knots - 1);
  const int i3 = std::min(span + 2, n_knots - 1);
  const float3 &k0 = knots_3d[i0];
  const float3 &k1 = knots_3d[i1];
  const float3 &k2 = knots_3d[i2];
  const float3 &k3 = knots_3d[i3];

  /* Catmull-Rom derivative coefficients:
   *   C(u) = 0.5 * ((2*k1) + (-k0+k2)*u + (2*k0-5*k1+4*k2-k3)*u^2
   *                 + (-k0+3*k1-3*k2+k3)*u^3)
   *   C'(u) = 0.5 * (a + 2*b*u + 3*c*u^2)
   *   C''(u) = 0.5 * (2*b + 6*c*u) = b + 3*c*u
   */
  const float3 a = -k0 + k2;
  const float3 b = 2.0f * k0 - 5.0f * k1 + 4.0f * k2 - k3;
  const float3 c = -k0 + 3.0f * k1 - 3.0f * k2 + k3;

  /* Newton iterations: find u* where dot(Q - C(u*), C'(u*)) = 0
   * i.e. the foot point is perpendicular to the curve tangent. */
  for (int iter = 0; iter < 4; iter++) {
    const float3 pos = bke::curves::catmull_rom::interpolate(k0, k1, k2, k3, u);
    const float3 deriv = 0.5f * (a + (2.0f * b + 3.0f * c * u) * u);
    const float3 diff = query - pos;

    const float f = math::dot(diff, deriv);
    /* f'(u) = -|C'|^2 + dot(Q - C, C'') */
    const float3 deriv2 = b + 3.0f * c * u;
    const float fp = -math::dot(deriv, deriv) + math::dot(diff, deriv2);

    if (std::abs(fp) < 1e-12f) {
      break;
    }
    const float du = f / fp;
    u = std::clamp(u + du, 0.0f, 1.0f);

    if (std::abs(du) < 1e-6f) {
      break; /* Converged. */
    }
  }

  /* Evaluate position and tangent at the refined parameter. */
  r_pos = bke::curves::catmull_rom::interpolate(k0, k1, k2, k3, u);
  const float3 deriv = 0.5f * (a + (2.0f * b + 3.0f * c * u) * u);
  r_tan = math::normalize(deriv);

  /* Map the refined u back to polyline arc length. The polyline has
   * `res` segments per knot span, so the continuous polyline
   * "index" is span * res + u * res. */
  const float seg_f = float(span * res) + u * float(res);
  const int seg_i = std::clamp(int(seg_f), 0, int(poly_3d.size()) - 2);
  const float seg_t = seg_f - float(seg_i);
  const float s0 = (seg_i > 0) ? lengths_3d[seg_i - 1] : 0.0f;
  const float s1 = lengths_3d[seg_i];
  r_arc_len = s0 + seg_t * (s1 - s0);
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

  /* Use smooth Catmull-Rom evaluation for the tangent if knots are available,
   * otherwise fall back to linear interpolation of polyline tangents. */
  if (!knots_3d.is_empty()) {
    float3 smooth_pos;
    smooth_evaluate_3d(best_seg, best_t, smooth_pos, r_tan);
  }
  else {
    r_tan = math::normalize(
        math::interpolate(tangents_3d[best_seg], tangents_3d[best_seg + 1], best_t));
  }
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
  /* 5× the point budget: keep a long tail for curve history, give more
   * lookahead for self-intersection detection on the inner side of turns,
   * and provide extra deferral so dabs are painted on stable spline data. */
  tot *= 5;
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
        const int res = roll_spline_.resolution;
        if (res - 1 < int(roll_spline_.lengths_3d.size())) {
          stroke_distance_world_ += roll_spline_.lengths_3d[res - 1];
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

/**
 * Generate virtual extension points that continue the stroke's curvature
 * beyond its start or end. Used for both backward (pre-stroke) and forward
 * (post-stroke) extensions so the spline has coverage for the full brush
 * footprint at the stroke boundaries.
 *
 * \param anchor_2d, anchor_3d: The point to extend from.
 * \param dir_2d, dir_3d: Unit direction of the stroke at the anchor.
 * \param seg1_2d, seg2_2d, seg1_3d, seg2_3d: Two consecutive segments at the
 *   anchor end, used to measure curvature. May be zero-length if < 3 points.
 * \param n_points: Number of extension points to generate.
 * \param step_2d, step_3d: Spacing between consecutive extension points.
 * \param sign: +1.0f for forward, -1.0f for backward.
 * \param r_ext_2d, r_ext_3d: Output arrays (appended in walk order, i.e.
 *   nearest-to-anchor first for forward, farthest-from-anchor first for backward).
 */
static void generate_virtual_extension(const float2 &anchor_2d,
                                       const float3 &anchor_3d,
                                       const float2 &dir_2d,
                                       const float3 &dir_3d,
                                       const float2 &seg1_2d,
                                       const float2 &seg2_2d,
                                       const float3 &seg1_3d,
                                       const float3 &seg2_3d,
                                       const int n_points,
                                       const float step_2d,
                                       const float step_3d,
                                       const float sign,
                                       Vector<float2> &r_ext_2d,
                                       Vector<float3> &r_ext_3d)
{
  /* Measure curvature from the angle between the two segments.
   * Each virtual step rotates the direction by a proportional amount so the
   * extension follows the same arc the user started drawing. */
  float angle_2d = 0.0f;
  float angle_3d = 0.0f;
  float3 rot_axis = float3(0);
  bool has_rot_3d = false;

  {
    const float cross2 = seg1_2d.x * seg2_2d.y - seg1_2d.y * seg2_2d.x;
    const float dot2 = math::dot(seg1_2d, seg2_2d);
    const float total_a2 = atan2f(cross2, dot2);
    const float ref_len2 = math::length(seg1_2d);
    if (ref_len2 > 1e-7f) {
      angle_2d = total_a2 * (step_2d / ref_len2);
    }

    const float3 n1 = math::normalize(seg1_3d);
    const float3 n2 = math::normalize(seg2_3d);
    const float3 c3 = math::cross(n1, n2);
    const float c3_len = math::length(c3);
    const float d3 = math::dot(n1, n2);
    if (c3_len > 1e-7f) {
      rot_axis = c3 / c3_len;
      const float total_a3 = atan2f(c3_len, d3);
      const float ref_len3 = math::length(seg1_3d);
      if (ref_len3 > 1e-7f) {
        angle_3d = total_a3 * (step_3d / ref_len3);
        has_rot_3d = true;
      }
    }
  }

  /* Clamp per-step angle so total curvature over all extension points
   * stays within 90°.  Without this, tight turns can spiral into loops. */
  const float max_per_step = float(M_PI_2) / float(std::max(1, n_points));
  angle_2d = std::clamp(angle_2d, -max_per_step, max_per_step);
  if (has_rot_3d) {
    angle_3d = std::clamp(angle_3d, -max_per_step, max_per_step);
  }

  /* Walk from anchor, rotating direction each step. */
  float2 cur_2d = dir_2d;
  float3 cur_3d = dir_3d;
  float2 prev_2d = anchor_2d;
  float3 prev_3d = anchor_3d;

  /* For backward extensions we build into a temporary array and reverse,
   * so the output is ordered farthest→nearest (matching the expected
   * prepend order where index 0 is the farthest virtual knot). */
  const bool backward = (sign < 0.0f);
  const int old_size = int(r_ext_2d.size());

  for (int i = 0; i < n_points; i++) {
    /* Rotate direction before stepping. */
    const float a2 = sign * angle_2d;
    if (a2 != 0.0f) {
      const float c = cosf(a2);
      const float s = sinf(a2);
      cur_2d = float2(cur_2d.x * c - cur_2d.y * s,
                      cur_2d.x * s + cur_2d.y * c);
    }
    if (has_rot_3d) {
      const float a3 = sign * angle_3d;
      const float c = cosf(a3);
      const float s = sinf(a3);
      const float3 kxv = math::cross(rot_axis, cur_3d);
      const float kdv = math::dot(rot_axis, cur_3d);
      cur_3d = cur_3d * c + kxv * s + rot_axis * kdv * (1.0f - c);
    }

    prev_2d = prev_2d + cur_2d * (sign * step_2d);
    prev_3d = prev_3d + cur_3d * (sign * step_3d);
    r_ext_2d.append(prev_2d);
    r_ext_3d.append(prev_3d);
  }

  if (backward) {
    /* Reverse so output goes farthest-from-anchor → nearest. */
    std::reverse(r_ext_2d.begin() + old_size, r_ext_2d.end());
    std::reverse(r_ext_3d.begin() + old_size, r_ext_3d.end());
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

  float3 dir3 = points_[next_oldest].location - p_start;
  float2 dir2 = points_[next_oldest].mouse_out - m_start;
  const float len3 = math::length(dir3);
  const float len2 = math::length(dir2);
  if (len3 < 1e-7f || len2 < 1e-7f) {
    return;
  }
  dir3 /= len3;
  dir2 /= len2;

  const float r_world = paint_calc_object_space_radius(vc, p_start, points_[oldest].size);
  const float r_screen = points_[oldest].size;

  constexpr int n_backward = 6;
  const float step_3d = (r_world * 1.5f) / float(n_backward);
  const float step_2d = (r_screen * 1.5f) / float(n_backward);

  /* Curvature segments (zero-length if < 3 points). */
  float2 seg1_2d(0), seg2_2d(0);
  float3 seg1_3d(0), seg2_3d(0);
  if (num_points_ >= 3) {
    const int third = (oldest + 2) % cap;
    seg1_2d = points_[next_oldest].mouse_out - m_start;
    seg2_2d = points_[third].mouse_out - points_[next_oldest].mouse_out;
    seg1_3d = points_[next_oldest].location - p_start;
    seg2_3d = points_[third].location - points_[next_oldest].location;
  }

  backward_ext_2d_.clear();
  backward_ext_3d_.clear();
  generate_virtual_extension(m_start,
                             p_start,
                             dir2,
                             dir3,
                             seg1_2d,
                             seg2_2d,
                             seg1_3d,
                             seg2_3d,
                             n_backward,
                             step_2d,
                             step_3d,
                             -1.0f,
                             backward_ext_2d_,
                             backward_ext_3d_);
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

  /* Adaptive resolution: cap total polyline segments to keep per-vertex
   * search in spline_uv() fast. With low spacing the knots are very close
   * together and each span needs only a few subdivisions.
   * Resolution is computed once (on first call) from the expected maximum
   * knot count and kept fixed for the entire stroke — changing resolution
   * mid-stroke would invalidate polyline indices and accumulated arc lengths. */
  constexpr int kMaxPolySegments = 512;
  const int n_spans = n_total - 1;
  if (roll_prev_n_total_ == 0) {
    /* First call: base resolution on expected peak knot count. */
    const int max_knots = roll_max_points() + initial_backward_ext_count_ + 2;
    const int max_spans = std::max(1, max_knots - 1);
    roll_spline_.resolution = std::max(2, std::min(int(kRollResolution),
                                                   kMaxPolySegments / max_spans));
  }
  const int resolution = roll_spline_.resolution;
  const int n_back_ext = int(backward_ext_2d_.size());

  /* Check whether we can do an incremental update: only the last 2 spans
   * need recomputation when a single knot was appended and the front
   * (backward extension) didn't change. */
  const bool can_incremental =
      roll_prev_n_total_ > 0 &&
      n_back_ext == roll_prev_n_back_ext_ &&
      n_total == roll_prev_n_total_ + 1;

  const int n_poly = n_spans * resolution + 1;

  if (can_incremental) {
    /* Incremental: keep the stable polyline prefix, only recompute dirty tail.
     * Adding knot N affects span N-2 (its clamped i3 now sees the real knot)
     * and creates the new span N-1.  Everything before span N-2 is unchanged. */
    const int old_n_spans = roll_prev_n_total_ - 1;
    const int first_dirty_span = std::max(0, old_n_spans - 1);

    /* Grow vectors to the new size (preserves existing elements). */
    roll_spline_.poly_2d.resize(n_poly);
    roll_spline_.poly_3d.resize(n_poly);
    roll_spline_.knots_3d.resize(n_total);
    roll_spline_.knots_3d[n_total - 1] = all_3d[n_total - 1];

    /* Overwrite dirty spans + new span. */
    for (int i = first_dirty_span; i < n_spans; i++) {
      const int i0 = std::max(i - 1, 0);
      const int i3 = std::min(i + 2, n_total - 1);

      for (int s = 0; s < resolution; s++) {
        const int poly_idx = i * resolution + s;
        const float t = float(s) / float(resolution);
        roll_spline_.poly_2d[poly_idx] = bke::curves::catmull_rom::interpolate(
            all_2d[i0], all_2d[i], all_2d[i + 1], all_2d[i3], t);
        roll_spline_.poly_3d[poly_idx] = bke::curves::catmull_rom::interpolate(
            all_3d[i0], all_3d[i], all_3d[i + 1], all_3d[i3], t);
      }
    }
    roll_spline_.poly_2d[n_poly - 1] = all_2d.last();
    roll_spline_.poly_3d[n_poly - 1] = all_3d.last();
  }
  else {
    /* Full rebuild. */
    roll_spline_.clear();
    roll_spline_.resolution = resolution;

    roll_spline_.knots_3d.reinitialize(n_total);
    for (int i = 0; i < n_total; i++) {
      roll_spline_.knots_3d[i] = all_3d[i];
    }

    roll_spline_.poly_2d.reinitialize(n_poly);
    roll_spline_.poly_3d.reinitialize(n_poly);

    int idx = 0;
    for (int i = 0; i < n_spans; i++) {
      const int i0 = std::max(i - 1, 0);
      const int i3 = std::min(i + 2, n_total - 1);

      for (int s = 0; s < resolution; s++) {
        const float t = float(s) / float(resolution);
        roll_spline_.poly_2d[idx] = bke::curves::catmull_rom::interpolate(
            all_2d[i0], all_2d[i], all_2d[i + 1], all_2d[i3], t);
        roll_spline_.poly_3d[idx] = bke::curves::catmull_rom::interpolate(
            all_3d[i0], all_3d[i], all_3d[i + 1], all_3d[i3], t);
        idx++;
      }
    }
    roll_spline_.poly_2d[idx] = all_2d.last();
    roll_spline_.poly_3d[idx] = all_3d.last();
  }

  roll_prev_n_total_ = n_total;
  roll_prev_n_back_ext_ = n_back_ext;

  /* Virtual boundary: backward extension has backward_ext_2d_.size() knots,
   * each knot transition = resolution polyline points. */
  n_virtual_poly_points_ = n_back_ext * resolution;

  roll_spline_.update_lengths();

  /* Arc length of virtual extension — subtracted from V so V=0 at mouse-down.
   * Computed only once (when the full virtual extension is first available).
   * As virtual points are consumed, stroke_distance_world_ compensates;
   * updating roll_virtual_length_ here would cause double-counting. */
  if (roll_virtual_length_ == 0.0f && n_virtual_poly_points_ > 0 &&
      n_virtual_poly_points_ - 1 < int(roll_spline_.lengths_3d.size()))
  {
    roll_virtual_length_ = roll_spline_.lengths_3d[n_virtual_poly_points_ - 1];
  }
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
   *    Uses the same curvature-following logic as prepend_virtual_roll_points()
   *    but in the forward direction, appending directly to the polyline. */
  if (num_points_ >= 2 && !roll_spline_.is_empty()) {
    constexpr int cap = PAINT_MAX_INPUT_SAMPLES;
    const int newest = (cur_point_ - 1 + cap) % cap;
    const int prev_newest = (newest - 1 + cap) % cap;

    const float3 p_end = points_[newest].location;
    const float2 m_end = points_[newest].mouse_out;

    float3 dir3 = p_end - points_[prev_newest].location;
    float2 dir2 = m_end - points_[prev_newest].mouse_out;
    const float len3 = math::length(dir3);
    const float len2 = math::length(dir2);

    if (len3 > 1e-7f && len2 > 1e-7f) {
      dir3 /= len3;
      dir2 /= len2;

      const float r_world = paint_calc_object_space_radius(vc, p_end, points_[newest].size);
      const float r_screen = points_[newest].size;

      constexpr int n_forward = 6;
      const float step_3d = (r_world * 1.5f) / float(n_forward);
      const float step_2d = (r_screen * 1.5f) / float(n_forward);

      float2 seg1_2d(0), seg2_2d(0);
      float3 seg1_3d(0), seg2_3d(0);
      if (num_points_ >= 3) {
        const int prev2 = (prev_newest - 1 + cap) % cap;
        seg1_2d = points_[prev_newest].mouse_out - points_[prev2].mouse_out;
        seg2_2d = m_end - points_[prev_newest].mouse_out;
        seg1_3d = points_[prev_newest].location - points_[prev2].location;
        seg2_3d = p_end - points_[prev_newest].location;
      }

      generate_virtual_extension(m_end,
                                 p_end,
                                 dir2,
                                 dir3,
                                 seg1_2d,
                                 seg2_2d,
                                 seg1_3d,
                                 seg2_3d,
                                 n_forward,
                                 step_2d,
                                 step_3d,
                                 +1.0f,
                                 roll_spline_.poly_2d,
                                 roll_spline_.poly_3d);

      roll_spline_.update_lengths();
    }
  }

  /* 4. Flush deferred dabs: advance look_back from its current position
   *    to the newest recorded point, placing a dab at each step. */
  constexpr int buf_cap = PAINT_MAX_INPUT_SAMPLES;
  const int half = (roll_max_points() * 3) / 5 + 2;
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

/* ---------------------------------------------------------------------------
 * Closest point on a 3D triangle (Ericson, Real-Time Collision Detection §5.1.5).
 *
 * Returns the squared distance from `query` to the closest point on the
 * triangle (A, B, C).  The barycentric coordinates of the closest point are
 * stored in `r_bary` such that  closest = r_bary.x*A + r_bary.y*B + r_bary.z*C.
 * Degenerate (zero-area) triangles return FLT_MAX so they are skipped. */
static float closest_point_on_triangle(const float3 &query,
                                        const float3 &A,
                                        const float3 &B,
                                        const float3 &C,
                                        float3 &r_bary)
{
  const float3 ab = B - A, ac = C - A;
  if (math::length_squared(math::cross(ab, ac)) < 1e-12f) {
    r_bary = float3(1, 0, 0);
    return FLT_MAX;
  }

  const float3 ap = query - A;
  const float d1 = math::dot(ab, ap);
  const float d2 = math::dot(ac, ap);
  if (d1 <= 0.0f && d2 <= 0.0f) {
    r_bary = float3(1, 0, 0);
    return math::distance_squared(query, A);
  }

  const float3 bp = query - B;
  const float d3 = math::dot(ab, bp);
  const float d4 = math::dot(ac, bp);
  if (d3 >= 0.0f && d4 <= d3) {
    r_bary = float3(0, 1, 0);
    return math::distance_squared(query, B);
  }

  const float vc = d1 * d4 - d3 * d2;
  if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
    const float v = d1 / (d1 - d3);
    r_bary = float3(1.0f - v, v, 0.0f);
    return math::distance_squared(query, A + v * ab);
  }

  const float3 cp = query - C;
  const float d5 = math::dot(ab, cp);
  const float d6 = math::dot(ac, cp);
  if (d6 >= 0.0f && d5 <= d6) {
    r_bary = float3(0, 0, 1);
    return math::distance_squared(query, C);
  }

  const float vb = d5 * d2 - d1 * d6;
  if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
    const float w = d2 / (d2 - d6);
    r_bary = float3(1.0f - w, 0.0f, w);
    return math::distance_squared(query, A + w * ac);
  }

  const float va = d3 * d6 - d5 * d4;
  if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
    const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    r_bary = float3(0.0f, 1.0f - w, w);
    return math::distance_squared(query, B + w * (C - B));
  }

  const float denom = 1.0f / (va + vb + vc);
  const float bv = vb * denom;
  const float bw = vc * denom;
  r_bary = float3(1.0f - bv - bw, bv, bw);
  return math::distance_squared(query, A + bv * ab + bw * ac);
}

/* ---------------------------------------------------------------------------
 * Catmull-Clark subdivision for a regular grid.
 *
 * Input:  rows × cols grid of (float3 pos, float2 uv), flat row-major.
 * Output: (2*rows-1) × (2*cols-1) grid, same layout.
 * Boundary rule: cubic B-spline (V' = (Vleft + 6V + Vright)/8).
 * Interior rule: standard CC  (V' = (F + 2R + V) / 4). */
static void catmull_clark_grid(const float3 *in_pos,
                                const float2 *in_uv,
                                int rows,
                                int cols,
                                float3 *out_pos,
                                float2 *out_uv,
                                int &out_rows,
                                int &out_cols)
{
  out_rows = 2 * rows - 1;
  out_cols = 2 * cols - 1;

  auto I = [cols](int r, int c) { return r * cols + c; };
  auto O = [&out_cols](int r, int c) { return r * out_cols + c; };

  /* 1. Face points (odd row, odd col). */
  for (int r = 0; r < rows - 1; r++) {
    for (int c = 0; c < cols - 1; c++) {
      const int o = O(2 * r + 1, 2 * c + 1);
      out_pos[o] = 0.25f * (in_pos[I(r, c)] + in_pos[I(r, c + 1)] +
                             in_pos[I(r + 1, c)] + in_pos[I(r + 1, c + 1)]);
      out_uv[o] = 0.25f * (in_uv[I(r, c)] + in_uv[I(r, c + 1)] +
                            in_uv[I(r + 1, c)] + in_uv[I(r + 1, c + 1)]);
    }
  }

  /* 2. Edge points. */
  /* Horizontal edges (even row, odd col). */
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols - 1; c++) {
      const int o = O(2 * r, 2 * c + 1);
      const bool bnd = (r == 0 || r == rows - 1);
      if (bnd) {
        out_pos[o] = 0.5f * (in_pos[I(r, c)] + in_pos[I(r, c + 1)]);
        out_uv[o] = 0.5f * (in_uv[I(r, c)] + in_uv[I(r, c + 1)]);
      }
      else {
        const float3 &fp_a = out_pos[O(2 * r - 1, 2 * c + 1)];
        const float3 &fp_b = out_pos[O(2 * r + 1, 2 * c + 1)];
        out_pos[o] = 0.25f * (in_pos[I(r, c)] + in_pos[I(r, c + 1)] + fp_a + fp_b);
        const float2 &fu_a = out_uv[O(2 * r - 1, 2 * c + 1)];
        const float2 &fu_b = out_uv[O(2 * r + 1, 2 * c + 1)];
        out_uv[o] = 0.25f * (in_uv[I(r, c)] + in_uv[I(r, c + 1)] + fu_a + fu_b);
      }
    }
  }
  /* Vertical edges (odd row, even col). */
  for (int r = 0; r < rows - 1; r++) {
    for (int c = 0; c < cols; c++) {
      const int o = O(2 * r + 1, 2 * c);
      const bool bnd = (c == 0 || c == cols - 1);
      if (bnd) {
        out_pos[o] = 0.5f * (in_pos[I(r, c)] + in_pos[I(r + 1, c)]);
        out_uv[o] = 0.5f * (in_uv[I(r, c)] + in_uv[I(r + 1, c)]);
      }
      else {
        const float3 &fp_l = out_pos[O(2 * r + 1, 2 * c - 1)];
        const float3 &fp_r = out_pos[O(2 * r + 1, 2 * c + 1)];
        out_pos[o] = 0.25f * (in_pos[I(r, c)] + in_pos[I(r + 1, c)] + fp_l + fp_r);
        const float2 &fu_l = out_uv[O(2 * r + 1, 2 * c - 1)];
        const float2 &fu_r = out_uv[O(2 * r + 1, 2 * c + 1)];
        out_uv[o] = 0.25f * (in_uv[I(r, c)] + in_uv[I(r + 1, c)] + fu_l + fu_r);
      }
    }
  }

  /* 3. Vertex points (even row, even col). */
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      const int o = O(2 * r, 2 * c);
      const bool rb = (r == 0 || r == rows - 1);
      const bool cb = (c == 0 || c == cols - 1);

      if (rb && cb) {
        /* Corner: unchanged. */
        out_pos[o] = in_pos[I(r, c)];
        out_uv[o] = in_uv[I(r, c)];
      }
      else if (rb) {
        /* Top/bottom boundary. */
        out_pos[o] = (in_pos[I(r, c - 1)] + 6.0f * in_pos[I(r, c)] +
                      in_pos[I(r, c + 1)]) /
                     8.0f;
        out_uv[o] = (in_uv[I(r, c - 1)] + 6.0f * in_uv[I(r, c)] +
                     in_uv[I(r, c + 1)]) /
                    8.0f;
      }
      else if (cb) {
        /* Left/right boundary. */
        out_pos[o] = (in_pos[I(r - 1, c)] + 6.0f * in_pos[I(r, c)] +
                      in_pos[I(r + 1, c)]) /
                     8.0f;
        out_uv[o] = (in_uv[I(r - 1, c)] + 6.0f * in_uv[I(r, c)] +
                     in_uv[I(r + 1, c)]) /
                    8.0f;
      }
      else {
        /* Interior: V' = (avg_F + 2V + avg_N) / 4. */
        const float3 avg_F = 0.25f * (out_pos[O(2 * r - 1, 2 * c - 1)] +
                                       out_pos[O(2 * r - 1, 2 * c + 1)] +
                                       out_pos[O(2 * r + 1, 2 * c - 1)] +
                                       out_pos[O(2 * r + 1, 2 * c + 1)]);
        const float3 avg_N = 0.25f * (in_pos[I(r - 1, c)] + in_pos[I(r + 1, c)] +
                                       in_pos[I(r, c - 1)] + in_pos[I(r, c + 1)]);
        out_pos[o] = (avg_F + 2.0f * in_pos[I(r, c)] + avg_N) / 4.0f;

        const float2 avg_Fu = 0.25f * (out_uv[O(2 * r - 1, 2 * c - 1)] +
                                        out_uv[O(2 * r - 1, 2 * c + 1)] +
                                        out_uv[O(2 * r + 1, 2 * c - 1)] +
                                        out_uv[O(2 * r + 1, 2 * c + 1)]);
        const float2 avg_Nu = 0.25f * (in_uv[I(r - 1, c)] + in_uv[I(r + 1, c)] +
                                        in_uv[I(r, c - 1)] + in_uv[I(r, c + 1)]);
        out_uv[o] = (avg_Fu + 2.0f * in_uv[I(r, c)] + avg_Nu) / 4.0f;
      }
    }
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

  /* Precompute polyline segment search range for per-vertex UV lookups. */
  const Span<float> lengths = roll_spline_.lengths_3d.as_span();
  const int n_segs = int(roll_spline_.poly_3d.size()) - 1;

  const float search_r = cache.initial_radius * 3.0f;
  const float s_lo = std::max(0.0f, raw_s - search_r);
  /* Extend forward to include the newest stroke points (trailing segment).
   * This ensures the grid covers the area between the dab and the mouse,
   * which is needed for proper self-intersection detection and UV mapping
   * near the stroke tip. */
  const float s_hi = std::min(lengths.last(), raw_s + search_r * 2.0f);

  float fac;
  length_parameterize::sample_at_length(lengths, s_lo, cache.roll_seg_lo, fac);
  length_parameterize::sample_at_length(lengths, s_hi, cache.roll_seg_hi, fac);
  cache.roll_seg_hi = std::min(cache.roll_seg_hi, n_segs - 1);

  length_parameterize::sample_at_length(lengths, raw_s, cache.roll_center_seg, fac);

  /* Surface interpolation: precompute border curves for the search range.
   * Each polyline vertex gets a binormal (perpendicular to tangent in the
   * view plane) and left/right border points at ±brush_radius. */
  cache.roll_surface_ready = false;
  if (U.experimental.use_roll_surface_interp) {
    const Span<float3> poly = roll_spline_.poly_3d.as_span();
    const Span<float3> tangents = roll_spline_.tangents_3d.as_span();
    /* Build the grid over the FULL stored polyline so that self-intersections
     * anywhere on the stroke are properly detected.  The per-vertex search
     * in spline_uv() is limited to rows near the dab for performance. */
    const int lo = 0;
    const int count = int(poly.size());
    const float R = cache.initial_radius;

    /* Use the mesh surface normal (sculpt_normal) for binormal computation
     * and 2D projection. This aligns the poly-strip to the mesh surface
     * so the texture "stamps" from the normal direction, giving consistent
     * results regardless of viewing angle.
     * Fall back to view_normal on the first dab (sculpt_normal not yet set). */
    const float3 proj_normal = (math::length_squared(cache.sculpt_normal) > 1e-8f) ?
                                   math::normalize(cache.sculpt_normal) :
                                   math::normalize(cache.view_normal);
    cache.roll_proj_normal = proj_normal;

    cache.roll_binormals.reinitialize(count);
    cache.roll_border_left.reinitialize(count);
    cache.roll_border_right.reinitialize(count);

    for (int k = 0; k < count; k++) {
      const int vi = lo + k; /* polyline vertex index */
      const float3 &T = tangents[vi];
      /* Binormal = cross(tangent, proj_normal), lies in the mesh tangent plane. */
      float3 B = math::cross(T, proj_normal);
      const float blen = math::length(B);
      if (blen > 1e-7f) {
        B /= blen;
      }
      else {
        /* Degenerate: tangent parallel to view — pick an arbitrary perpendicular. */
        B = math::normalize(math::cross(T, float3(0, 0, 1)));
        if (math::length_squared(B) < 1e-12f) {
          B = math::normalize(math::cross(T, float3(1, 0, 0)));
        }
      }
      cache.roll_binormals[k] = B;
      cache.roll_border_left[k] = poly[vi] + B * R;
      cache.roll_border_right[k] = poly[vi] - B * R;
    }

    /* Fix inner-border self-intersections at sharp turns.
     *
     * The border curves (at distance R from center) may self-intersect on the
     * inner side of tight turns where the radius of curvature < R.
     * We find where the border polyline actually crosses itself (segment vs
     * segment test), then collapse all loop vertices to that crossing point.
     * This turns overlapping quads into degenerate triangles that fan from
     * the crossing point — the cross-lines rotate to point toward it. */

    /* Find border polyline self-intersections via exact 2D segment tests.
     *
     * Self-intersection is a 2D (screen-space) phenomenon: the border
     * polyline crosses itself when viewed from the camera.  Using exact
     * 2D intersection (not 3D closest-approach with a tolerance) ensures
     * the grid collapse matches the 2D debug overlay perfectly and never
     * incorrectly collapses the outer border at a turn. */
    float3 view_x = math::cross(proj_normal, float3(0, 0, 1));
    if (math::length_squared(view_x) < 1e-6f) {
      view_x = math::cross(proj_normal, float3(1, 0, 0));
    }
    view_x = math::normalize(view_x);
    const float3 view_y = math::normalize(math::cross(proj_normal, view_x));

    auto fix_border_self_intersections = [count, &view_x, &view_y, &poly, R](
                                             Vector<float3> &border) {
      if (count < 4) {
        return;
      }

      /* Project to 2D view plane for exact intersection tests. */
      Vector<float2> b2d(count);
      for (int k = 0; k < count; k++) {
        b2d[k] = float2(math::dot(border[k], view_x), math::dot(border[k], view_y));
      }

      /* Maximum loop size for self-intersection search.
       * Use count-1 to check all segment pairs — tight turns may produce
       * large loops and limiting the range causes missed intersections. */
      const int max_loop = count - 1;

      Vector<float3> orig(border);
      int skip_until = -1;

      for (int i = 0; i < count - 1; i++) {
        if (i <= skip_until) {
          continue;
        }
        const float2 d1 = b2d[i + 1] - b2d[i];
        if (math::dot(d1, d1) < 1e-8f) {
          continue;
        }

        /* Search within the max_loop neighborhood, far to near. */
        const int j_max = std::min(count - 2, i + max_loop);
        for (int j = j_max; j >= i + 2; j--) {
          const float2 d2 = b2d[j + 1] - b2d[j];
          if (math::dot(d2, d2) < 1e-8f) {
            continue;
          }

          /* Exact 2D segment intersection. */
          const float2 ac = b2d[j] - b2d[i];
          const float denom = d1.x * d2.y - d1.y * d2.x;
          if (std::abs(denom) < 1e-8f) {
            continue;
          }
          const float s = (ac.x * d2.y - ac.y * d2.x) / denom;
          const float t = (ac.x * d1.y - ac.y * d1.x) / denom;

          if (s >= 0.0f && s <= 1.0f && t >= 0.0f && t <= 1.0f) {
            /* Only collapse if all intermediate border vertices are closer
             * to the stroke center line than the brush radius R.  This
             * ensures we only merge genuine inner-side folds from tight
             * curvature, not spurious crossings where the stroke doubles
             * back or forms complex shapes (loops, S-curves). */
            bool all_inside = true;
            const int m_lo = std::max(0, i - 2);
            const int m_hi = std::min(count - 2, j + 2);
            const float R_sq = R * R;
            for (int k = i + 1; k <= j; k++) {
              float min_dist_sq = FLT_MAX;
              for (int m = m_lo; m <= m_hi; m++) {
                const float3 ab = poly[m + 1] - poly[m];
                const float ab_dot = math::dot(ab, ab);
                const float tp = (ab_dot > 1e-12f) ?
                                     std::clamp(math::dot(orig[k] - poly[m], ab) / ab_dot,
                                                0.0f,
                                                1.0f) :
                                     0.0f;
                const float3 proj = math::interpolate(poly[m], poly[m + 1], tp);
                min_dist_sq = std::min(min_dist_sq,
                                       math::distance_squared(orig[k], proj));
              }
              if (min_dist_sq >= R_sq) {
                all_inside = false;
                break;
              }
            }
            if (!all_inside) {
              continue;
            }

            /* Compute 3D crossing point from the 2D parameters. */
            const float3 pa = orig[i] + (orig[i + 1] - orig[i]) * s;
            const float3 pb = orig[j] + (orig[j + 1] - orig[j]) * t;
            const float3 cross_pt = (pa + pb) * 0.5f;
            for (int k = i + 1; k <= j; k++) {
              border[k] = cross_pt;
            }
            skip_until = j;
            break;
          }
        }
      }
    };

    fix_border_self_intersections(cache.roll_border_left);
    fix_border_self_intersections(cache.roll_border_right);

    /* Build subdivided poly-strip for the dab neighborhood.
     *
     * Borders were collapsed over the full stroke range (above) to detect
     * all self-intersections.  The grid itself only needs to cover the
     * area near the current dab — this keeps CC subdivision and Laplacian
     * smoothing fast by operating on ~60 rows instead of ~350. */
    {
      /* Crop range: seg_lo/seg_hi (±3R from dab) + margin for smoothing. */
      const int grid_lo = std::max(0, cache.roll_seg_lo - 8);
      const int grid_hi = std::min(count - 1, cache.roll_seg_hi + 8);
      const int init_rows = grid_hi - grid_lo + 1;
      const int init_cols = 3;
      Vector<float3> grid_pos(init_rows * init_cols);
      Vector<float2> grid_uv(init_rows * init_cols);

      for (int k = 0; k < init_rows; k++) {
        const int bi = grid_lo + k; /* index into border arrays */
        const int vi = bi;          /* polyline vertex index (lo=0) */
        const float v_len = (vi > 0) ? lengths[vi - 1] : 0.0f;
        /* Col 0 = right border, col 1 = center, col 2 = left border. */
        grid_pos[k * 3 + 0] = cache.roll_border_right[bi];
        grid_pos[k * 3 + 1] = poly[vi];
        grid_pos[k * 3 + 2] = cache.roll_border_left[bi];
        grid_uv[k * 3 + 0] = float2(-R, v_len);
        grid_uv[k * 3 + 1] = float2(0.0f, v_len);
        grid_uv[k * 3 + 2] = float2(R, v_len);
      }

      /* Catmull-Clark subdivision: N rows × 3 cols → (2N-1) rows × 5 cols.
       *
       * Uses CC face/edge point rules so that cross-stroke segments curve
       * smoothly near collapse points (subsurf effect).  All original
       * vertices are PINNED (no vertex rule) so the borders are not
       * smoothed along the stroke direction — only the NEW intermediate
       * positions get CC-averaged.  The center column is inherently
       * preserved because all its original vertices are pinned and its
       * vertical edge points use simple midpoints. */
      int cur_rows, cur_cols;
      {
        const int oR = init_rows;
        const int oC = init_cols; /* 3 */
        const int nR = 2 * oR - 1;
        const int nC = 2 * oC - 1; /* 5 */
        const int fR = oR - 1; /* face-point row count */
        const int fC = oC - 1; /* face-point col count = 2 */

        auto oi = [oC](int r, int c) { return r * oC + c; };
        auto ni = [nC](int r, int c) { return r * nC + c; };

        /* Step 1: Face points (odd row, odd col in new grid).
         * F = average of the 4 corners of each quad. */
        Vector<float3> fp(fR * fC);
        Vector<float2> fu(fR * fC);
        for (int r = 0; r < fR; r++) {
          for (int c = 0; c < fC; c++) {
            fp[r * fC + c] = 0.25f * (grid_pos[oi(r, c)] + grid_pos[oi(r, c + 1)] +
                                       grid_pos[oi(r + 1, c)] + grid_pos[oi(r + 1, c + 1)]);
            fu[r * fC + c] = 0.25f * (grid_uv[oi(r, c)] + grid_uv[oi(r, c + 1)] +
                                       grid_uv[oi(r + 1, c)] + grid_uv[oi(r + 1, c + 1)]);
          }
        }

        Vector<float3> np(nR * nC);
        Vector<float2> nu(nR * nC);

        /* Step 2: Fill face points. */
        for (int r = 0; r < fR; r++) {
          for (int c = 0; c < fC; c++) {
            np[ni(2 * r + 1, 2 * c + 1)] = fp[r * fC + c];
            nu[ni(2 * r + 1, 2 * c + 1)] = fu[r * fC + c];
          }
        }

        /* Step 3: Horizontal edge points (even row, odd col).
         * Interior: average of endpoints + adjacent face points.
         * Boundary (first/last row): simple midpoint. */
        for (int r = 0; r < oR; r++) {
          for (int c = 0; c < fC; c++) {
            if (r == 0 || r == oR - 1) {
              np[ni(2 * r, 2 * c + 1)] = 0.5f * (grid_pos[oi(r, c)] +
                                                    grid_pos[oi(r, c + 1)]);
              nu[ni(2 * r, 2 * c + 1)] = 0.5f * (grid_uv[oi(r, c)] +
                                                    grid_uv[oi(r, c + 1)]);
            }
            else {
              np[ni(2 * r, 2 * c + 1)] = 0.25f * (grid_pos[oi(r, c)] +
                                                     grid_pos[oi(r, c + 1)] +
                                                     fp[(r - 1) * fC + c] + fp[r * fC + c]);
              nu[ni(2 * r, 2 * c + 1)] = 0.25f * (grid_uv[oi(r, c)] +
                                                     grid_uv[oi(r, c + 1)] +
                                                     fu[(r - 1) * fC + c] + fu[r * fC + c]);
            }
          }
        }

        /* Step 4: Vertical edge points (odd row, even col).
         * Border columns (C=0,2) and center column (C=1): simple midpoint.
         * (Center vertical edges stay on the stroke polyline.) */
        for (int r = 0; r < fR; r++) {
          for (int c = 0; c < oC; c++) {
            np[ni(2 * r + 1, 2 * c)] = 0.5f * (grid_pos[oi(r, c)] +
                                                  grid_pos[oi(r + 1, c)]);
            nu[ni(2 * r + 1, 2 * c)] = 0.5f * (grid_uv[oi(r, c)] +
                                                  grid_uv[oi(r + 1, c)]);
          }
        }

        /* Step 5: Original vertex points (even row, even col).
         * Center column (C=1): CC boundary rule (1D smoothing along stroke)
         * so cross-stroke segments can curve near merge points.
         * Border columns + first/last rows: pinned. */
        for (int r = 0; r < oR; r++) {
          for (int c = 0; c < oC; c++) {
            if (c == 1 && r > 0 && r < oR - 1) {
              /* Center column interior: smooth along the stroke direction.
               * V_new = (1/8)(V_above + 6V + V_below) — the CC boundary
               * vertex rule.  This shifts the center along the stroke,
               * allowing cross-segments to curve at merge points. */
              np[ni(2 * r, 2 * c)] = (1.0f / 8.0f) *
                                      (grid_pos[oi(r - 1, c)] +
                                       6.0f * grid_pos[oi(r, c)] +
                                       grid_pos[oi(r + 1, c)]);
              nu[ni(2 * r, 2 * c)] = (1.0f / 8.0f) *
                                      (grid_uv[oi(r - 1, c)] +
                                       6.0f * grid_uv[oi(r, c)] +
                                       grid_uv[oi(r + 1, c)]);
            }
            else {
              /* Corners, boundary rows, border columns: pinned. */
              np[ni(2 * r, 2 * c)] = grid_pos[oi(r, c)];
              nu[ni(2 * r, 2 * c)] = grid_uv[oi(r, c)];
            }
          }
        }

        grid_pos = std::move(np);
        grid_uv = std::move(nu);
        cur_rows = nR;
        cur_cols = nC;
      }

      /* 2D Laplacian smoothing: pin border columns, smooth everything
       * between them (including center).  This curves the cross-stroke
       * segments near collapse points — the perpendicular lines bow
       * toward the merge point like a subdivision surface.
       * Uses double-buffering to avoid per-iteration allocation. */
      {
        constexpr int smooth_iters = 10;
        constexpr float mix = 0.5f;
        const int grid_total = cur_rows * cur_cols;
        Vector<float3> buf_p(grid_total);
        Vector<float2> buf_u(grid_total);
        float3 *src_p = grid_pos.data(), *dst_p = buf_p.data();
        float2 *src_u = grid_uv.data(), *dst_u = buf_u.data();
        for (int iter = 0; iter < smooth_iters; iter++) {
          memcpy(dst_p, src_p, sizeof(float3) * grid_total);
          memcpy(dst_u, src_u, sizeof(float2) * grid_total);
          for (int r = 1; r < cur_rows - 1; r++) {
            for (int c = 1; c < cur_cols - 1; c++) {
              const int idx = r * cur_cols + c;
              float3 avg_p = 0.25f * (src_p[(r - 1) * cur_cols + c] +
                                       src_p[(r + 1) * cur_cols + c] +
                                       src_p[r * cur_cols + c - 1] +
                                       src_p[r * cur_cols + c + 1]);
              float2 avg_u = 0.25f * (src_u[(r - 1) * cur_cols + c] +
                                       src_u[(r + 1) * cur_cols + c] +
                                       src_u[r * cur_cols + c - 1] +
                                       src_u[r * cur_cols + c + 1]);
              dst_p[idx] = (1.0f - mix) * src_p[idx] + mix * avg_p;
              dst_u[idx] = (1.0f - mix) * src_u[idx] + mix * avg_u;
            }
          }
          std::swap(src_p, dst_p);
          std::swap(src_u, dst_u);
        }
        /* Ensure results end up in grid_pos/grid_uv. */
        if (src_p != grid_pos.data()) {
          memcpy(grid_pos.data(), src_p, sizeof(float3) * grid_total);
          memcpy(grid_uv.data(), src_u, sizeof(float2) * grid_total);
        }
      }

      /* Pre-project grid to 2D projection-plane coordinates.
       * Both the grid and per-vertex query points are projected onto the
       * same 2D plane (perpendicular to the projection normal).  This:
       * 1. Eliminates off-plane 3D projection bias (V-shape artifact)
       * 2. Reduces the per-vertex search to 2D (faster, less memory)
       * The projection is done once per dab, not per vertex. */
      const int total = cur_rows * cur_cols;
      Vector<float2> grid_pos_2d(total);
      {
        for (int i = 0; i < total; i++) {
          grid_pos_2d[i] = float2(math::dot(grid_pos[i], view_x),
                                  math::dot(grid_pos[i], view_y));
        }
      }

      /* Compute eval row range: only rows within 2R of the dab center
       * are searched per-vertex in spline_uv().  The full grid is still
       * stored for debug draw and correct self-intersection handling. */
      {
        const float2 dab_2d = float2(math::dot(cache.location, view_x),
                                     math::dot(cache.location, view_y));
        const float eval_r_sq = (R * 2.0f) * (R * 2.0f);
        const int center_c = cur_cols / 2;
        int eval_lo = cur_rows - 1, eval_hi = 0;
        for (int r = 0; r < cur_rows; r++) {
          if (math::distance_squared(dab_2d, grid_pos_2d[r * cur_cols + center_c]) <=
              eval_r_sq)
          {
            eval_lo = std::min(eval_lo, r);
            eval_hi = std::max(eval_hi, r);
          }
        }
        /* Small margin for quads that straddle the boundary. */
        cache.roll_eval_row_lo = std::max(0, eval_lo - 3);
        cache.roll_eval_row_hi = std::min(cur_rows - 2, eval_hi + 3);
      }

      cache.roll_subdiv_rows = cur_rows;
      cache.roll_subdiv_cols = cur_cols;

      /* Build UV lookup table: rasterize eval-range grid quads into a 2D
       * texture so per-vertex UV lookup is O(1) instead of O(rows×cols).
       * On high-poly meshes this is the dominant cost — the LUT amortizes
       * the bilinear inverse across a fixed-size image. */
      {
        constexpr int RES = StrokeCache::ROLL_LUT_RES;
        const int el = cache.roll_eval_row_lo;
        const int eh = cache.roll_eval_row_hi;

        /* 2D bounding box of eval-range grid. */
        float2 bb_min(FLT_MAX), bb_max(-FLT_MAX);
        for (int r = el; r <= std::min(eh + 1, cur_rows - 1); r++) {
          for (int c = 0; c < cur_cols; c++) {
            bb_min = math::min(bb_min, grid_pos_2d[r * cur_cols + c]);
            bb_max = math::max(bb_max, grid_pos_2d[r * cur_cols + c]);
          }
        }
        const float2 extent = bb_max - bb_min;
        const float2 margin = extent * 0.1f;
        bb_min -= margin;
        bb_max += margin;
        const float2 ext = bb_max - bb_min;
        const float2 inv_ext(ext.x > 1e-10f ? float(RES) / ext.x : 0.0f,
                             ext.y > 1e-10f ? float(RES) / ext.y : 0.0f);

        const int lut_total = RES * RES;
        cache.roll_lut_uv.reinitialize(lut_total);
        cache.roll_lut_dist_sq.reinitialize(lut_total);
        cache.roll_lut_tan.reinitialize(lut_total);
        for (int i = 0; i < lut_total; i++) {
          cache.roll_lut_dist_sq[i] = FLT_MAX;
        }

        auto cross2d = [](float2 a, float2 b) { return a.x * b.y - a.y * b.x; };

        /* Rasterize each eval-range quad into the LUT. */
        for (int r = el; r <= eh; r++) {
          for (int c = 0; c < cur_cols - 1; c++) {
            const float2 P00 = grid_pos_2d[r * cur_cols + c];
            const float2 P10 = grid_pos_2d[r * cur_cols + c + 1];
            const float2 P01 = grid_pos_2d[(r + 1) * cur_cols + c];
            const float2 P11 = grid_pos_2d[(r + 1) * cur_cols + c + 1];

            /* Pixel bounding box of this quad. */
            float2 qmin = math::min(math::min(P00, P10), math::min(P01, P11));
            float2 qmax = math::max(math::max(P00, P10), math::max(P01, P11));
            int px_lo = std::max(0, int((qmin.x - bb_min.x) * inv_ext.x));
            int px_hi = std::min(RES - 1, int((qmax.x - bb_min.x) * inv_ext.x) + 1);
            int py_lo = std::max(0, int((qmin.y - bb_min.y) * inv_ext.y));
            int py_hi = std::min(RES - 1, int((qmax.y - bb_min.y) * inv_ext.y) + 1);

            const float2 a = P10 - P00;
            const float2 b = P01 - P00;
            const float2 tw = P11 - P10 - P01 + P00;
            const float A_coeff = cross2d(b, tw);

            for (int py = py_lo; py <= py_hi; py++) {
              for (int px = px_lo; px <= px_hi; px++) {
                const float2 query = bb_min + float2(px + 0.5f, py + 0.5f) / float(RES) * ext;
                const float2 d = query - P00;

                const float B_coeff = cross2d(b, a) - cross2d(d, tw);
                const float C_coeff = -cross2d(d, a);

                float v;
                if (std::abs(A_coeff) < 1e-8f) {
                  v = (std::abs(B_coeff) > 1e-8f) ? -C_coeff / B_coeff : 0.5f;
                }
                else {
                  const float disc = B_coeff * B_coeff - 4.0f * A_coeff * C_coeff;
                  if (disc < 0.0f) {
                    continue;
                  }
                  const float sq = sqrtf(disc);
                  const float v1 = (-B_coeff + sq) / (2.0f * A_coeff);
                  const float v2 = (-B_coeff - sq) / (2.0f * A_coeff);
                  const float e1 = std::max(0.0f, std::max(-v1, v1 - 1.0f));
                  const float e2 = std::max(0.0f, std::max(-v2, v2 - 1.0f));
                  v = (e1 <= e2) ? v1 : v2;
                }
                if (v < -0.01f || v > 1.01f) {
                  continue;
                }
                v = std::clamp(v, 0.0f, 1.0f);

                const float2 dv = a + v * tw;
                float u;
                if (std::abs(dv.x) > std::abs(dv.y)) {
                  u = (std::abs(dv.x) > 1e-8f) ? (d.x - v * b.x) / dv.x : 0.5f;
                }
                else {
                  u = (std::abs(dv.y) > 1e-8f) ? (d.y - v * b.y) / dv.y : 0.5f;
                }
                if (u < -0.01f || u > 1.01f) {
                  continue;
                }
                u = std::clamp(u, 0.0f, 1.0f);

                const float2 pnt = (1 - u) * (1 - v) * P00 + u * (1 - v) * P10 +
                                   (1 - u) * v * P01 + u * v * P11;
                const float dsq = math::distance_squared(query, pnt);

                const int li = py * RES + px;
                if (dsq < cache.roll_lut_dist_sq[li]) {
                  cache.roll_lut_dist_sq[li] = dsq;
                  /* Interpolate UV. */
                  const float2 &UV00 = grid_uv[r * cur_cols + c];
                  const float2 &UV10 = grid_uv[r * cur_cols + c + 1];
                  const float2 &UV01 = grid_uv[(r + 1) * cur_cols + c];
                  const float2 &UV11 = grid_uv[(r + 1) * cur_cols + c + 1];
                  cache.roll_lut_uv[li] = (1 - u) * (1 - v) * UV00 + u * (1 - v) * UV10 +
                                          (1 - u) * v * UV01 + u * v * UV11;
                  /* Interpolate tangent from 3D grid. */
                  const float3 &Q00 = grid_pos[r * cur_cols + c];
                  const float3 &Q10 = grid_pos[r * cur_cols + c + 1];
                  const float3 &Q01 = grid_pos[(r + 1) * cur_cols + c];
                  const float3 &Q11 = grid_pos[(r + 1) * cur_cols + c + 1];
                  const float3 dPdv = (1 - u) * (Q01 - Q00) + u * (Q11 - Q10);
                  cache.roll_lut_tan[li] = math::normalize(dPdv);
                }
              }
            }
          }
        }

        cache.roll_lut_min = bb_min;
        cache.roll_lut_inv_extent = inv_ext;
        cache.roll_lut_ready = true;
      }

      cache.roll_subdiv_pos = std::move(grid_pos);
      cache.roll_subdiv_pos_2d = std::move(grid_pos_2d);
      cache.roll_subdiv_uv = std::move(grid_uv);
      cache.roll_view_x = view_x;
      cache.roll_view_y = view_y;
    }

    cache.roll_surface_ready = true;
  }
}

void PaintStroke::spline_uv(const StrokeCache &cache,
                             const float co[3],
                             float r_out[3],
                             float r_tan[3]) const
{
  float3 tan;
  float3 p;
  bool used_lut = false;

  if (cache.roll_surface_ready && cache.roll_lut_ready) {
    /* --- LUT-based UV lookup (O(1) per vertex) ---
     *
     * The grid's bilinear inverse is pre-rasterized into a 2D lookup table
     * once per dab.  Per-vertex evaluation is a simple 2D projection +
     * bilinear sample of the LUT — no per-quad search needed. */

    constexpr int RES = StrokeCache::ROLL_LUT_RES;
    const float2 query = float2(math::dot(float3(co), cache.roll_view_x),
                                math::dot(float3(co), cache.roll_view_y));

    /* Map query to LUT coordinates.  Guard against NaN (from degenerate
     * grids where inv_extent is very large and query == min, giving 0*inf). */
    const float2 fc = (query - cache.roll_lut_min) * cache.roll_lut_inv_extent;
    if (LIKELY(std::isfinite(fc.x) && std::isfinite(fc.y))) {
      const float fx = std::clamp(fc.x - 0.5f, 0.0f, float(RES - 2));
      const float fy = std::clamp(fc.y - 0.5f, 0.0f, float(RES - 2));
      const int ix = std::min(int(fx), RES - 2);
      const int iy = std::min(int(fy), RES - 2);
      const float tx = fx - float(ix);
      const float ty = fy - float(iy);

      /* Bilinear interpolation of UV from 4 nearest LUT cells. */
      const float2 &uv00 = cache.roll_lut_uv[iy * RES + ix];
      const float2 &uv10 = cache.roll_lut_uv[iy * RES + ix + 1];
      const float2 &uv01 = cache.roll_lut_uv[(iy + 1) * RES + ix];
      const float2 &uv11 = cache.roll_lut_uv[(iy + 1) * RES + ix + 1];
      const float2 uv_result = (1 - tx) * (1 - ty) * uv00 + tx * (1 - ty) * uv10 +
                                (1 - tx) * ty * uv01 + tx * ty * uv11;

      r_out[0] = -uv_result.x;
      r_out[1] = uv_result.y;

      /* Bilinear interpolation of tangent. */
      const float3 &t00 = cache.roll_lut_tan[iy * RES + ix];
      const float3 &t10 = cache.roll_lut_tan[iy * RES + ix + 1];
      const float3 &t01 = cache.roll_lut_tan[(iy + 1) * RES + ix];
      const float3 &t11 = cache.roll_lut_tan[(iy + 1) * RES + ix + 1];
      tan = math::normalize((1 - tx) * (1 - ty) * t00 + tx * (1 - ty) * t10 +
                            (1 - tx) * ty * t01 + tx * ty * t11);
      p = float3(co); /* Not used for texture mapping in this path. */
      used_lut = true;
    }
  }

  if (!used_lut && cache.roll_center_s >= 0.0f) {
    /* --- Original closest-point projection with wedge-based V ---
     *
     * 1. Coarse search: find the nearest polyline segment (line projection).
     * 2. V via perpendicular-plane wedge: at each polyline vertex the tangent
     *    defines a perpendicular plane.  A vertex Q between two consecutive
     *    planes has signed distances d_lo, d_hi to them.  Interpolating by
     *    the ratio  t = d_lo / (d_lo - d_hi)  gives iso-V contours that are
     *    smooth planes fanning between the two perpendiculars — circular arcs
     *    in 2-D, exactly what curved texture mapping needs.
     * 3. U = raw closest-point distance (stable, no parameterization needed). */
    const Span<float3> poly = roll_spline_.poly_3d.as_span();
    const Span<float> lengths = roll_spline_.lengths_3d.as_span();
    const Span<float3> tangents = roll_spline_.tangents_3d.as_span();

    /* Use precomputed segment range from compute_roll_center. */
    const int seg_lo = cache.roll_seg_lo;
    const int seg_hi = cache.roll_seg_hi;

    const float3 query = float3(co);
    float best_dist_sq = FLT_MAX;
    int best_seg = cache.roll_center_seg;
    float best_t = 0.0f;

    for (int i = seg_lo; i <= seg_hi; i++) {
      const float3 a = poly[i];
      const float3 ab = poly[i + 1] - a;
      const float ab_dot = math::dot(ab, ab);
      const float t = (ab_dot > 1e-12f) ?
                          std::clamp(math::dot(query - a, ab) / ab_dot, 0.0f, 1.0f) :
                          0.0f;
      const float3 proj = math::interpolate(a, poly[i + 1], t);
      const float dist_sq = math::distance_squared(query, proj);
      if (dist_sq < best_dist_sq) {
        best_dist_sq = dist_sq;
        best_seg = i;
        best_t = t;
      }
    }

    /* --- V via perpendicular-plane wedge ---
     *
     * Signed distance from Q to the perpendicular plane at each endpoint:
     *   d = dot(Q - P_i, T_i)
     * where T_i is the (central-difference) tangent at vertex i.
     * d > 0 means Q is "ahead" of the plane, d < 0 means "behind". */
    const float d_lo = math::dot(query - poly[best_seg], tangents[best_seg]);
    const float d_hi = math::dot(query - poly[best_seg + 1], tangents[best_seg + 1]);

    const float seg_start = (best_seg > 0) ? lengths[best_seg - 1] : 0.0f;
    const float seg_end = lengths[best_seg];

    /* Wedge interpolation: when d_lo > 0 and d_hi < 0 the vertex is inside
     * the wedge. The ratio gives a smooth arc-length parameter. Falls back
     * to the closest-point t when the denominator is degenerate. */
    const float denom = d_lo - d_hi;
    const float wedge_t = (std::abs(denom) > 1e-12f) ?
                              std::clamp(d_lo / denom, 0.0f, 1.0f) :
                              best_t;
    r_out[1] = seg_start + wedge_t * (seg_end - seg_start);

    /* U = raw closest-point distance (robust, no parameterization artifacts). */
    r_out[0] = sqrtf(best_dist_sq);

    /* Tangent for signing: interpolate tangents at the wedge parameter. */
    tan = math::normalize(
        math::interpolate(tangents[best_seg], tangents[best_seg + 1], wedge_t));
    p = math::interpolate(poly[best_seg], poly[best_seg + 1], wedge_t);

    /* Sign the perpendicular distance (left/right of stroke). */
    const float3 diff2 = p - query;
    float3 cross_vec;
    cross_v3_v3v3(cross_vec, diff2, tan);
    if (math::dot(cross_vec, cache.view_normal) < 0.0f) {
      r_out[0] = -r_out[0];
    }
  }
  else if (!used_lut) {
    /* Fallback: full closest-point search (used before center is precomputed). */
    roll_spline_.closest_point_3d(float3(co), r_out[1], tan, r_out[0]);
    p = roll_spline_.evaluate_3d(r_out[1]);

    /* Sign the perpendicular distance (left/right of stroke). */
    const float3 diff = p - float3(co);
    float3 cross_vec;
    cross_v3_v3v3(cross_vec, diff, tan);
    if (math::dot(cross_vec, cache.view_normal) < 0.0f) {
      r_out[0] = -r_out[0];
    }
  }

  copy_v3_v3(r_tan, tan);
  r_out[2] = 0.0f;

  r_out[1] += stroke_distance_world_ - roll_virtual_length_;
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

  /* --- 2D overlay: center polyline + tick marks --- */
  {
    const uint pos_attr = GPU_vertformat_attr_add(
        immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

    const int n_pts = int(roll_spline_.poly_2d.size());

    /* Polyline: red = virtual extension, green = real. */
    GPU_line_width(3.0f);
    if (n_virtual_poly_points_ > 1) {
      immUniformColor4ub(255, 50, 50, 200);
      const int cnt = std::min(n_virtual_poly_points_ + 1, n_pts);
      immBegin(GPU_PRIM_LINE_STRIP, cnt);
      for (int i = 0; i < cnt; i++) {
        immVertex2f(pos_attr, roll_spline_.poly_2d[i].x + ox, roll_spline_.poly_2d[i].y + oy);
      }
      immEnd();
    }
    if (n_virtual_poly_points_ < n_pts) {
      immUniformColor4ub(50, 255, 50, 200);
      const int cnt = n_pts - n_virtual_poly_points_;
      immBegin(GPU_PRIM_LINE_STRIP, cnt);
      for (int i = n_virtual_poly_points_; i < n_pts; i++) {
        immVertex2f(pos_attr, roll_spline_.poly_2d[i].x + ox, roll_spline_.poly_2d[i].y + oy);
      }
      immEnd();
    }

    /* Tick marks at knot boundaries. */
    const int roll_res = roll_spline_.resolution;
    GPU_line_width(1.5f);
    const float tick_len = 10.0f;
    for (int i = 0; i < n_pts; i += roll_res) {
      const float2 tan = roll_spline_.tangent_2d_at_index(std::min(i, n_pts - 2));
      const float2 perp(-tan.y, tan.x);
      const float2 &pt = roll_spline_.poly_2d[i];
      immUniformColor4ub(200, 200, 200, 140);
      immBegin(GPU_PRIM_LINES, 2);
      immVertex2f(pos_attr, pt.x + perp.x * tick_len + ox, pt.y + perp.y * tick_len + oy);
      immVertex2f(pos_attr, pt.x - perp.x * tick_len + ox, pt.y - perp.y * tick_len + oy);
      immEnd();
    }

    /* Yellow tick at virtual/real boundary. */
    if (n_virtual_poly_points_ > 0 && n_virtual_poly_points_ < n_pts) {
      GPU_line_width(2.5f);
      immUniformColor4ub(255, 255, 0, 255);
      const float2 tan = roll_spline_.tangent_2d_at_index(n_virtual_poly_points_);
      const float2 perp(-tan.y, tan.x);
      const float2 &pt = roll_spline_.poly_2d[n_virtual_poly_points_];
      immBegin(GPU_PRIM_LINES, 2);
      immVertex2f(pos_attr, pt.x + perp.x * 25.0f + ox, pt.y + perp.y * 25.0f + oy);
      immVertex2f(pos_attr, pt.x - perp.x * 25.0f + ox, pt.y - perp.y * 25.0f + oy);
      immEnd();
    }

    immUnbindProgram();
  }

  /* --- 3D debug draw: poly-strip grid wireframe --- */

  Object *ob = CTX_data_active_object(C);
  RegionView3D *rv3d = static_cast<RegionView3D *>(region->regiondata);
  if (ob && ob->runtime && ob->runtime->sculpt_session &&
      ob->runtime->sculpt_session->cache && rv3d)
  {
    const StrokeCache &cache = *ob->runtime->sculpt_session->cache;
    if (cache.roll_surface_ready && cache.roll_subdiv_rows > 1) {
      const int rows = cache.roll_subdiv_rows;
      const int cols = cache.roll_subdiv_cols;
      const float3 *gpos = cache.roll_subdiv_pos.data();
      const int center_col = cols / 2;

      /* Save viewport state and set up 3D matrices. */
      int saved_viewport[4];
      GPU_viewport_size_get_i(saved_viewport);

      GPU_matrix_push();
      GPU_matrix_push_projection();
      wmViewport(&region->winrct);
      GPU_matrix_projection_set(rv3d->winmat);
      const float4x4 mv = float4x4(rv3d->viewmat) * ob->object_to_world();
      GPU_matrix_set(mv.ptr());

      const uint pos3d_attr = GPU_vertformat_attr_add(
          immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32_32);
      immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

      const int eval_lo = cache.roll_eval_row_lo;
      const int eval_hi = cache.roll_eval_row_hi;

      /* Full-grid column lines (dim) — shows the entire stored strip. */
      for (int c = 0; c < cols; c++) {
        GPU_line_width(1.0f);
        immUniformColor4ub(100, 100, 100, 60);
        immBegin(GPU_PRIM_LINE_STRIP, rows);
        for (int r = 0; r < rows; r++) {
          immVertex3fv(pos3d_attr, gpos[r * cols + c]);
        }
        immEnd();
      }

      /* Eval-range column lines (bright) — the portion used for this dab. */
      const int eval_rows = std::min(eval_hi + 2, rows) - eval_lo;
      if (eval_rows > 1) {
        for (int c = 0; c < cols; c++) {
          if (c == center_col) {
            GPU_line_width(2.5f);
            immUniformColor4ub(50, 255, 50, 220);
          }
          else if (c == 0 || c == cols - 1) {
            GPU_line_width(2.0f);
            immUniformColor4ub(255, 160, 0, 220);
          }
          else {
            GPU_line_width(1.0f);
            immUniformColor4ub(180, 180, 180, 140);
          }
          immBegin(GPU_PRIM_LINE_STRIP, eval_rows);
          for (int r = eval_lo; r < eval_lo + eval_rows; r++) {
            immVertex3fv(pos3d_attr, gpos[r * cols + c]);
          }
          immEnd();
        }
      }

      /* Row lines (across stroke) in eval range only. */
      GPU_line_width(1.0f);
      immUniformColor4ub(200, 200, 200, 80);
      const int row_stride = std::max(1, eval_rows / 30);
      for (int r = eval_lo; r < eval_lo + eval_rows; r += row_stride) {
        immBegin(GPU_PRIM_LINE_STRIP, cols);
        for (int c = 0; c < cols; c++) {
          immVertex3fv(pos3d_attr, gpos[r * cols + c]);
        }
        immEnd();
      }

      /* Cyan markers at eval range boundaries. */
      GPU_point_size(10.0f);
      immUniformColor4ub(0, 255, 255, 255);
      for (int boundary_r : {eval_lo, std::min(eval_hi + 1, rows - 1)}) {
        immBegin(GPU_PRIM_POINTS, 1);
        immVertex3fv(pos3d_attr, gpos[boundary_r * cols + center_col]);
        immEnd();
      }

      /* Red dots at collapsed border vertices (where consecutive border
       * vertices share the same position = self-intersection collapse). */
      GPU_point_size(8.0f);
      immUniformColor4ub(255, 0, 0, 255);
      for (int side = 0; side < 2; side++) {
        const int bc = (side == 0) ? 0 : cols - 1;
        for (int r = 2; r < rows - 1; r++) {
          const float3 &prev = gpos[(r - 1) * cols + bc];
          const float3 &cur = gpos[r * cols + bc];
          const float3 &next = gpos[(r + 1) * cols + bc];
          if (math::distance_squared(prev, cur) < 1e-10f &&
              math::distance_squared(cur, next) < 1e-10f)
          {
            const float3 &before = gpos[(r - 2) * cols + bc];
            if (math::distance_squared(before, prev) > 1e-10f) {
              immBegin(GPU_PRIM_POINTS, 1);
              immVertex3fv(pos3d_attr, cur);
              immEnd();
            }
          }
        }
      }

      immUnbindProgram();
      GPU_matrix_pop_projection();
      GPU_matrix_pop();

      /* Restore viewport. */
      GPU_viewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);
    }
  }

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
  const int res = roll_spline_.resolution;

  /* Determine which polyline index corresponds to the last-painted dab.
   * Points from painted_poly onward are "unflushed" and shown as preview. */
  constexpr int buf_cap = PAINT_MAX_INPUT_SAMPLES;
  const int half = (roll_max_points() * 3) / 5 + 2;

  int painted_poly;
  if (last_painted_roll_idx_ < 0 || num_points_ < half) {
    painted_poly = n_virtual_poly_points_;
  }
  else {
    const int oldest_idx = (cur_point_ - num_points_ + buf_cap) % buf_cap;
    const int dist = (last_painted_roll_idx_ - oldest_idx + buf_cap) % buf_cap;
    painted_poly = n_virtual_poly_points_ + dist * res;
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

void PaintStroke::init_roll_cursors()
{
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

}  // namespace blender::ed::sculpt_paint
