/* SPDX-FileCopyrightText: 2009 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blf
 *
 * Glyph conversion, from FreeType to curves.
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <ft2build.h>

#include FT_OUTLINE_H

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math_geom.h"

#include "BLF_api.hh"

#include "DNA_curve_types.h"

#include "BKE_curve.hh"

#include "blf_internal.hh"

#ifdef WITH_FONT_FORGE
/* FontForge wrapper for overlap removal (C++ compatible). */
extern "C" {
#  include "fontforge/splineoverlap_blender.h"
}
#endif
#include "blf_internal_types.hh"

#include "BLI_math_vector.h"

#include "BLI_strict_flags.h" /* IWYU pragma: keep. Keep last. */

namespace blender {

#ifdef WITH_FONT_FORGE

/* -------------------------------------------------------------------- */
/** \name Overlap Removal (FontForge Integration)
 * \{ */

/**
 * Round a coordinate value to a specified precision.
 * This matches the rounding used in the legacy Blender Nurbs path.
 */
static double round_coord(double value, double round_factor)
{
  /* Round to specified precision, then pass through float to introduce
   * small variations similar to the legacy Blender Nurbs path. This prevents
   * exact equality matches in FontForge's SplineRefigure3 that incorrectly
   * detect curves as lines. */
  double rounded = round(value * round_factor) / round_factor;
  return double(float(rounded));
}

/**
 * Convert FreeType outline directly to FontForge SplineSets.
 *
 * This function parses FreeType's outline format and creates FontForge SplineSets
 * directly, avoiding the intermediate Blender Nurbs representation. This is more
 * efficient and preserves the original coordinate precision better.
 *
 * FreeType provides glyph contours using TrueType convention (CW outer, CCW holes).
 * FontForge's overlap removal handles both winding conventions.
 *
 * \param ftoutline: The FreeType outline to convert.
 * \param round_factor: Factor for coordinate rounding (higher = more precision).
 * \return The FontForge SplineSets, or nullptr on failure.
 */
static FFSplineSet *ftoutline_to_splinesets(const FT_Outline &ftoutline, double round_factor)
{
  FFSplineSet *ss_head = nullptr;
  FFSplineSet *ss_tail = nullptr;

  int contour_prev = -1;

  for (int j = 0; j < ftoutline.n_contours; j++) {
    const int contour_end = ftoutline.contours[j];
    const int contour_start = contour_prev + 1;
    const int n = contour_end - contour_prev; /* Number of points in this contour. */
    contour_prev = contour_end;

    if (n < 2) {
      continue;
    }

    FFSplinePoint *first_sp = nullptr;
    FFSplinePoint *prev_sp = nullptr;

    /* Find the first on-curve point (or create virtual one) starting from the original first. */
    int first_on_idx = -1;
    for (int k = 0; k < n; k++) {
      int idx = contour_start + k;
      if (ftoutline.tags[idx] == FT_Curve_Tag_On) {
        first_on_idx = k;
        break;
      }
    }

    /* If no on-curve point, check for virtual point between two conics. */
    if (first_on_idx == -1) {
      /* All points are off-curve (conic). First virtual on-point is between last and first. */
      first_on_idx = 0; /* We'll handle this as a virtual point. */
    }

    /* Count on-curve points (including virtual ones from consecutive conics). */
    int on_count = 0;
    for (int k = 0; k < n; k++) {
      int idx = contour_start + k;
      if (ftoutline.tags[idx] == FT_Curve_Tag_On) {
        on_count++;
      }
      else if (ftoutline.tags[idx] == FT_Curve_Tag_Conic) {
        int k_next = (k + 1) % n;
        int idx_next = contour_start + k_next;
        if (ftoutline.tags[idx_next] == FT_Curve_Tag_Conic) {
          on_count++; /* Virtual on-point between consecutive conics. */
        }
      }
    }

    if (on_count < 2) {
      continue;
    }

    /* Build arrays of on-curve point positions and their incoming/outgoing control points.
     * We process forward first, then reverse to get PostScript winding. */

    struct OnCurvePoint {
      double x, y;                 /* On-curve point position. */
      double prev_cp_x, prev_cp_y; /* Incoming control point (from previous segment). */
      double next_cp_x, next_cp_y; /* Outgoing control point (to next segment). */
      bool prev_is_line;           /* Previous segment is a line. */
      bool next_is_line;           /* Next segment is a line. */
    };

    OnCurvePoint *on_points = static_cast<OnCurvePoint *>(
        MEM_callocN(sizeof(OnCurvePoint) * size_t(on_count), "on_curve_points"));
    if (on_points == nullptr) {
      FF_FreeSplineSets(ss_head);
      return nullptr;
    }

    int on_idx = 0;

    /* Process the contour forward to extract on-curve points. */
    for (int k = 0; k < n; k++) {
      int idx = contour_start + k;
      int k_prev = (k - 1 + n) % n;
      int k_next = (k + 1) % n;
      int idx_prev = contour_start + k_prev;
      int idx_next = contour_start + k_next;

      char tag = ftoutline.tags[idx];
      char tag_prev = ftoutline.tags[idx_prev];
      char tag_next = ftoutline.tags[idx_next];

      if (tag == FT_Curve_Tag_On) {
        /* Real on-curve point. */
        double px = round_coord(double(ftoutline.points[idx].x), round_factor);
        double py = round_coord(double(ftoutline.points[idx].y), round_factor);

        on_points[on_idx].x = px;
        on_points[on_idx].y = py;

        /* Determine incoming control point (prev_cp). */
        if (tag_prev == FT_Curve_Tag_On) {
          /* Line segment from previous on-curve point. */
          on_points[on_idx].prev_is_line = true;
          on_points[on_idx].prev_cp_x = px;
          on_points[on_idx].prev_cp_y = py;
        }
        else if (tag_prev == FT_Curve_Tag_Conic) {
          /* Conic arc: control point is (P + 2*C) / 3 where P is on-curve, C is conic. */
          double cx = round_coord(double(ftoutline.points[idx_prev].x), round_factor);
          double cy = round_coord(double(ftoutline.points[idx_prev].y), round_factor);
          on_points[on_idx].prev_is_line = false;
          on_points[on_idx].prev_cp_x = (px + 2.0 * cx) / 3.0;
          on_points[on_idx].prev_cp_y = (py + 2.0 * cy) / 3.0;
        }
        else if (tag_prev == FT_Curve_Tag_Cubic) {
          /* Cubic arc: control point is the cubic point itself. */
          on_points[on_idx].prev_is_line = false;
          on_points[on_idx].prev_cp_x = round_coord(double(ftoutline.points[idx_prev].x),
                                                    round_factor);
          on_points[on_idx].prev_cp_y = round_coord(double(ftoutline.points[idx_prev].y),
                                                    round_factor);
        }

        /* Determine outgoing control point (next_cp). */
        if (tag_next == FT_Curve_Tag_On) {
          /* Line segment to next on-curve point. */
          on_points[on_idx].next_is_line = true;
          on_points[on_idx].next_cp_x = px;
          on_points[on_idx].next_cp_y = py;
        }
        else if (tag_next == FT_Curve_Tag_Conic) {
          /* Conic arc: control point is (P + 2*C) / 3. */
          double cx = round_coord(double(ftoutline.points[idx_next].x), round_factor);
          double cy = round_coord(double(ftoutline.points[idx_next].y), round_factor);
          on_points[on_idx].next_is_line = false;
          on_points[on_idx].next_cp_x = (px + 2.0 * cx) / 3.0;
          on_points[on_idx].next_cp_y = (py + 2.0 * cy) / 3.0;
        }
        else if (tag_next == FT_Curve_Tag_Cubic) {
          /* Cubic arc: control point is the cubic point itself. */
          on_points[on_idx].next_is_line = false;
          on_points[on_idx].next_cp_x = round_coord(double(ftoutline.points[idx_next].x),
                                                    round_factor);
          on_points[on_idx].next_cp_y = round_coord(double(ftoutline.points[idx_next].y),
                                                    round_factor);
        }

        on_idx++;
      }
      else if (tag == FT_Curve_Tag_Conic && tag_next == FT_Curve_Tag_Conic) {
        /* Virtual on-curve point between two consecutive conic points. */
        double c1x = round_coord(double(ftoutline.points[idx].x), round_factor);
        double c1y = round_coord(double(ftoutline.points[idx].y), round_factor);
        double c2x = round_coord(double(ftoutline.points[idx_next].x), round_factor);
        double c2y = round_coord(double(ftoutline.points[idx_next].y), round_factor);

        /* Virtual point is midpoint of the two conic points. */
        double vx = (c1x + c2x) / 2.0;
        double vy = (c1y + c2y) / 2.0;

        on_points[on_idx].x = vx;
        on_points[on_idx].y = vy;

        /* Incoming control point from c1: (V + 2*C1) / 3. */
        on_points[on_idx].prev_is_line = false;
        on_points[on_idx].prev_cp_x = (vx + 2.0 * c1x) / 3.0;
        on_points[on_idx].prev_cp_y = (vy + 2.0 * c1y) / 3.0;

        /* Outgoing control point to c2: (V + 2*C2) / 3. */
        on_points[on_idx].next_is_line = false;
        on_points[on_idx].next_cp_x = (vx + 2.0 * c2x) / 3.0;
        on_points[on_idx].next_cp_y = (vy + 2.0 * c2y) / 3.0;

        on_idx++;
      }
    }

    /* Create FontForge SplineSet from on_points in REVERSE order to convert
     * from TrueType winding (CW outer) to PostScript winding (CCW outer).
     * Order: [0, n-1, n-2, ..., 1] - first point stays at index 0. */

    for (int i = 0; i < on_count; i++) {
      /* Map to reversed index: 0 stays at 0, then count down from n-1. */
      int src_idx = (i == 0) ? 0 : (on_count - i);
      OnCurvePoint *op = &on_points[src_idx];

      FFSplinePoint *sp = FF_CreatePoint(op->x, op->y);
      if (sp == nullptr) {
        MEM_freeN(on_points);
        FF_FreeSplineSets(ss_head);
        return nullptr;
      }

      if (first_sp == nullptr) {
        first_sp = sp;
      }

      if (prev_sp != nullptr) {
        /* Connect previous point to this one in reversed direction.
         * prev_sp corresponds to prev_src_idx, sp corresponds to src_idx.
         * In reversed direction: prev_sp's outgoing handle = its original incoming (prev_cp)
         *                        sp's incoming handle = its original outgoing (next_cp) */
        int prev_src_idx = (i == 1) ? 0 : (on_count - (i - 1));
        OnCurvePoint *prev_op = &on_points[prev_src_idx];

        bool is_line = prev_op->prev_is_line && op->next_is_line;

        if (is_line) {
          FF_SetPointNextCP(prev_sp, FF_PointGetX(prev_sp), FF_PointGetY(prev_sp));
          FF_SetPointPrevCP(sp, FF_PointGetX(sp), FF_PointGetY(sp));
        }
        else {
          /* Swap: prev_op's prev_cp becomes outgoing, op's next_cp becomes incoming. */
          FF_SetPointNextCP(prev_sp, prev_op->prev_cp_x, prev_op->prev_cp_y);
          FF_SetPointPrevCP(sp, op->next_cp_x, op->next_cp_y);
        }

        if (!FF_ConnectPoints(prev_sp, sp)) {
          MEM_freeN(on_points);
          FF_FreeSplineSets(ss_head);
          return nullptr;
        }
      }

      prev_sp = sp;
    }

    /* Close the contour: connect last created point (on_points[1]) back to first (on_points[0]).
     * In reversed order, last created is on_points[1], first created is on_points[0]. */
    if (prev_sp != nullptr && first_sp != nullptr && prev_sp != first_sp) {
      OnCurvePoint *last_op = &on_points[1];  /* Last point in reversed order. */
      OnCurvePoint *first_op = &on_points[0]; /* First point (stays at 0). */

      bool is_line = last_op->prev_is_line && first_op->next_is_line;

      if (is_line) {
        FF_SetPointNextCP(prev_sp, FF_PointGetX(prev_sp), FF_PointGetY(prev_sp));
        FF_SetPointPrevCP(first_sp, FF_PointGetX(first_sp), FF_PointGetY(first_sp));
      }
      else {
        /* Swap for reversed direction. */
        FF_SetPointNextCP(prev_sp, last_op->prev_cp_x, last_op->prev_cp_y);
        FF_SetPointPrevCP(first_sp, first_op->next_cp_x, first_op->next_cp_y);
      }

      if (!FF_ConnectPoints(prev_sp, first_sp)) {
        MEM_freeN(on_points);
        FF_FreeSplineSets(ss_head);
        return nullptr;
      }
    }

    MEM_freeN(on_points);

    /* Create SplineSet from the linked points.
     * For cyclic contours, FontForge requires first == last (same pointer). */
    FFSplineSet *ss = FF_CreateSplineSet(first_sp, first_sp);
    if (ss == nullptr) {
      FF_FreeSplineSets(ss_head);
      return nullptr;
    }

    FF_AppendSplineSet(&ss_head, &ss_tail, ss);
  }

  return ss_head;
}

/**
 * Check if all contours in the result are closed (cyclic).
 * Open contours indicate a failure in the overlap removal algorithm.
 */
static bool all_contours_cyclic(FFSplineSet *result)
{
  for (FFSplineSet *ss = result; ss != nullptr; ss = FF_SplineSetGetNext(ss)) {
    if (FF_SplineSetIsCyclic(ss) == 0) {
      return false;
    }
  }
  return true;
}

/**
 * Try overlap removal with direct FreeType to FontForge conversion.
 * Returns nullptr on failure, or the result SplineSets on success.
 */
static FFSplineSet *try_overlap_removal_direct(const FT_Outline &ftoutline,
                                               double round_factor,
                                               bool require_all_cyclic)
{
  FFSplineSet *ss_head = ftoutline_to_splinesets(ftoutline, round_factor);
  if (ss_head == nullptr) {
    return nullptr;
  }

  FFSplineSet *result = FF_RemoveOverlap(ss_head);
  if (result == nullptr) {
    return nullptr;
  }

  if (require_all_cyclic && !all_contours_cyclic(result)) {
    FF_FreeSplineSets(result);
    return nullptr;
  }

  return result;
}

/**
 * Reverse the direction of a bezier curve while preserving the first point.
 *
 * FreeType provides glyph contours using TrueType convention:
 *   - Outer contours: clockwise (CW)
 *   - Holes: counter-clockwise (CCW)
 * FontForge's overlap removal expects PostScript convention:
 *   - Outer contours: counter-clockwise (CCW)
 *   - Holes: clockwise (CW)
 *
 * Unlike BKE_nurb_direction_switch which changes the first point,
 * this function preserves the first point for cyclic curves (matching
 * FontForge's SplineSetReverse behavior). This is important because
 * the overlap removal algorithm can be sensitive to the starting point.
 */
static void nurb_reverse_direction(Nurb *nu)
{
  if (nu->bezt == nullptr || nu->pntsu < 2) {
    return;
  }

  const int n = nu->pntsu;
  const bool is_cyclic = (nu->flagu & CU_NURB_CYCLIC) != 0;

  /* Use Blender's standard reversal function. */
  BKE_nurb_direction_switch(nu);

  if (is_cyclic && n > 1) {
    /* BKE_nurb_direction_switch reverses the array, making the original last point
     * the new first point. To match FontForge's SplineSetReverse behavior (which
     * preserves the first point for cyclic contours), rotate the array so the
     * original first point (now at index n-1) is back at index 0.
     *
     * Original: [A, B, C, D] (first=A)
     * After BKE_nurb_direction_switch: [D, C, B, A] (first=D, handles swapped)
     * After rotation: [A, D, C, B] (first=A, direction reversed) */

    /* Save the last element (originally the first). */
    BezTriple saved = nu->bezt[n - 1];

    /* Shift all elements right by one. */
    for (int i = n - 1; i > 0; i--) {
      nu->bezt[i] = nu->bezt[i - 1];
    }

    /* Put the saved element at the front. */
    nu->bezt[0] = saved;
  }
}

/**
 * Convert Blender Nurbs to FontForge SplineSets with specified coordinate rounding.
 * Returns nullptr on failure.
 */
static FFSplineSet *nurbs_to_splinesets(ListBaseT<Nurb> *nurbsbase,
                                        double inv_scale,
                                        double round_factor)
{
  FFSplineSet *ss_head = nullptr;
  FFSplineSet *ss_tail = nullptr;

  for (Nurb *nu = static_cast<Nurb *>(nurbsbase->first); nu != nullptr; nu = nu->next) {
    if (nu->bezt == nullptr || nu->pntsu < 2) {
      continue;
    }

    /* For a cyclic curve with N points, we have N bezier segments.
     * For a non-cyclic curve with N points, we have N-1 segments. */
    bool is_cyclic = (nu->flagu & CU_NURB_CYCLIC) != 0;
    int num_segments = is_cyclic ? nu->pntsu : (nu->pntsu - 1);

    if (num_segments < 1) {
      continue;
    }

    FFSplinePoint *first_sp = nullptr;
    FFSplinePoint *prev_sp = nullptr;

    /* Create SplinePoints and connect them */
    for (int i = 0; i < num_segments; i++) {
      int i_next = (i + 1) % nu->pntsu;
      BezTriple *bezt_from = &nu->bezt[i];
      BezTriple *bezt_to = &nu->bezt[i_next];

      /* Convert coordinates to raw font units with specified rounding. */
      double p0x = round(double(bezt_from->vec[1][0]) * inv_scale * round_factor) / round_factor;
      double p0y = round(double(bezt_from->vec[1][1]) * inv_scale * round_factor) / round_factor;
      double p1x = round(double(bezt_from->vec[2][0]) * inv_scale * round_factor) / round_factor;
      double p1y = round(double(bezt_from->vec[2][1]) * inv_scale * round_factor) / round_factor;
      double p2x = round(double(bezt_to->vec[0][0]) * inv_scale * round_factor) / round_factor;
      double p2y = round(double(bezt_to->vec[0][1]) * inv_scale * round_factor) / round_factor;
      double p3x = round(double(bezt_to->vec[1][0]) * inv_scale * round_factor) / round_factor;
      double p3y = round(double(bezt_to->vec[1][1]) * inv_scale * round_factor) / round_factor;

      /* Create the start point (or reuse first point for later segments) */
      FFSplinePoint *sp_from;
      if (i == 0) {
        sp_from = FF_CreatePoint(p0x, p0y);
        first_sp = sp_from;
      }
      else {
        sp_from = prev_sp;
      }

      /* Create the end point (reuse first point for last segment of cyclic contour) */
      FFSplinePoint *sp_to;
      if (is_cyclic && i == num_segments - 1) {
        sp_to = first_sp;
      }
      else {
        sp_to = FF_CreatePoint(p3x, p3y);
      }

      if (sp_from == nullptr || sp_to == nullptr) {
        FF_FreeSplineSets(ss_head);
        return nullptr;
      }

      /* Detect if this is a line segment using Blender's handle types.
       * HD_VECT handles indicate a line segment. FontForge expects line segments
       * to have control points at endpoints (with nonextcp/noprevcp flags set). */
      bool is_line = (bezt_from->h2 == HD_VECT && bezt_to->h1 == HD_VECT);

      if (is_line) {
        /* For lines, set control points to endpoints */
        FF_SetPointNextCP(sp_from, p0x, p0y);
        FF_SetPointPrevCP(sp_to, p3x, p3y);
      }
      else {
        FF_SetPointNextCP(sp_from, p1x, p1y);
        FF_SetPointPrevCP(sp_to, p2x, p2y);
      }

      /* Connect points with a cubic spline (computes polynomial coefficients) */
      if (!FF_ConnectPoints(sp_from, sp_to)) {
        FF_FreeSplineSets(ss_head);
        return nullptr;
      }

      prev_sp = sp_to;
    }

    /* Create SplineSet from the linked points */
    FFSplineSet *ss = FF_CreateSplineSet(first_sp, prev_sp);
    if (ss == nullptr) {
      FF_FreeSplineSets(ss_head);
      return nullptr;
    }

    FF_AppendSplineSet(&ss_head, &ss_tail, ss);
  }

  return ss_head;
}
/**
 * Try overlap removal with specified precision (legacy path via Blender Nurbs).
 * Returns nullptr on failure, or the result SplineSets on success.
 * If require_all_cyclic is true, returns nullptr if any result contour is open.
 */
static FFSplineSet *try_overlap_removal(ListBaseT<Nurb> *nurbsbase,
                                        double inv_scale,
                                        double round_factor,
                                        bool require_all_cyclic)
{
  FFSplineSet *ss_head = nurbs_to_splinesets(nurbsbase, inv_scale, round_factor);
  if (ss_head == nullptr) {
    return nullptr;
  }

  FFSplineSet *result = FF_RemoveOverlap(ss_head);
  if (result == nullptr) {
    return nullptr;
  }

  if (require_all_cyclic && !all_contours_cyclic(result)) {
    FF_FreeSplineSets(result);
    return nullptr;
  }

  return result;
}

/**
 * Convert FontForge SplineSets back to Blender Nurbs.
 */
static void splinesets_to_nurbs(FFSplineSet *result, ListBaseT<Nurb> *nurbsbase, float scale)
{
  for (FFSplineSet *ss = result; ss != nullptr; ss = FF_SplineSetGetNext(ss)) {
    int num_splines = FF_SplineSetCountSplines(ss);
    if (num_splines < 1) {
      continue;
    }

    /* Allocate new Nurb. */
    Nurb *nu = static_cast<Nurb *>(MEM_callocN(sizeof(Nurb), "overlap_nurb"));
    if (nu == nullptr) {
      continue;
    }

    /* For a cyclic contour with N bezier segments, we have N control points.
     * For a non-cyclic contour with N bezier segments, we have N+1 control points. */
    bool is_cyclic = FF_SplineSetIsCyclic(ss) != 0;
    int num_points = is_cyclic ? num_splines : (num_splines + 1);

    nu->pntsu = num_points;
    nu->pntsv = 1;
    nu->type = CU_BEZIER;
    nu->resolu = 12;
    nu->resolv = 12;
    if (is_cyclic) {
      nu->flagu |= CU_NURB_CYCLIC;
    }

    nu->bezt = static_cast<BezTriple *>(
        MEM_callocN(sizeof(BezTriple) * size_t(num_points), "overlap_bezt"));
    if (nu->bezt == nullptr) {
      MEM_freeN(nu);
      continue;
    }

    /* Fill in control points from SplineSet.
     * Iterate through spline segments and extract bezier data. */
    FFSplinePoint *first = FF_SplineSetGetFirst(ss);
    FFSplinePoint *sp = first;
    int idx = 0;

    do {
      if (sp == nullptr || idx >= num_splines) {
        break;
      }

      BezTriple *bezt = &nu->bezt[idx];

      /* Get coordinates from FontForge SplinePoint */
      double p0x = FF_PointGetX(sp);
      double p0y = FF_PointGetY(sp);
      double p1x = FF_PointGetNextCPX(sp);
      double p1y = FF_PointGetNextCPY(sp);

      /* Get next point for p2 and p3 */
      FFSplinePoint *sp_next = FF_PointGetNext(sp, first);
      if (sp_next == nullptr && is_cyclic) {
        /* For cyclic, the last segment connects back to first */
        sp_next = first;
      }

      double p2x, p2y, p3x, p3y;
      if (sp_next != nullptr) {
        p2x = FF_PointGetPrevCPX(sp_next);
        p2y = FF_PointGetPrevCPY(sp_next);
        p3x = FF_PointGetX(sp_next);
        p3y = FF_PointGetY(sp_next);
      }
      else {
        /* Non-cyclic endpoint - use same point */
        p2x = p0x;
        p2y = p0y;
        p3x = p0x;
        p3y = p0y;
      }

      /* Set control point (scaled back to Blender coords). */
      bezt->vec[1][0] = float(p0x * double(scale));
      bezt->vec[1][1] = float(p0y * double(scale));
      bezt->vec[1][2] = 0.0f;

      /* Detect if handles are at their control points (line segment).
       * Use epsilon comparison to handle floating point precision. */
      constexpr double LINE_EPSILON = 1e-6;
      bool p1_at_p0 = (fabs(p1x - p0x) < LINE_EPSILON && fabs(p1y - p0y) < LINE_EPSILON);
      bool p2_at_p3 = (fabs(p2x - p3x) < LINE_EPSILON && fabs(p2y - p3y) < LINE_EPSILON);

      /* Set right handle. For vector handles, position at 1/3 toward next point. */
      if (p1_at_p0) {
        bezt->vec[2][0] = float((p0x + (p3x - p0x) / 3.0) * double(scale));
        bezt->vec[2][1] = float((p0y + (p3y - p0y) / 3.0) * double(scale));
        bezt->h2 = HD_VECT;
      }
      else {
        bezt->vec[2][0] = float(p1x * double(scale));
        bezt->vec[2][1] = float(p1y * double(scale));
        bezt->h2 = HD_FREE;
      }
      bezt->vec[2][2] = 0.0f;

      /* Set the left handle of the next point. */
      int i_next = (idx + 1) % num_points;
      BezTriple *bezt_next = &nu->bezt[i_next];

      if (p2_at_p3) {
        bezt_next->vec[0][0] = float((p0x + (p3x - p0x) * 2.0 / 3.0) * double(scale));
        bezt_next->vec[0][1] = float((p0y + (p3y - p0y) * 2.0 / 3.0) * double(scale));
        bezt_next->h1 = HD_VECT;
      }
      else {
        bezt_next->vec[0][0] = float(p2x * double(scale));
        bezt_next->vec[0][1] = float(p2y * double(scale));
        bezt_next->h1 = HD_FREE;
      }
      bezt_next->vec[0][2] = 0.0f;

      /* For non-cyclic, set the last point's position from p3 */
      if (!is_cyclic && idx == num_splines - 1) {
        bezt_next->vec[1][0] = float(p3x * double(scale));
        bezt_next->vec[1][1] = float(p3y * double(scale));
        bezt_next->vec[1][2] = 0.0f;
      }

      /* Move to next point */
      sp = FF_PointGetNext(sp, first);
      if (sp == nullptr && is_cyclic && idx < num_splines - 1) {
        /* Should not happen for cyclic, but handle it */
        break;
      }
      idx++;
    } while (sp != nullptr && sp != first);

    /* For non-cyclic, set first point's left handle and last point's right handle */
    if (!is_cyclic && num_points > 0) {
      /* First point: left handle = control point (vector handle). */
      copy_v3_v3(nu->bezt[0].vec[0], nu->bezt[0].vec[1]);
      nu->bezt[0].h1 = HD_VECT;
      /* Last point: right handle = control point (vector handle). */
      int last = num_points - 1;
      copy_v3_v3(nu->bezt[last].vec[2], nu->bezt[last].vec[1]);
      nu->bezt[last].h2 = HD_VECT;
    }

    BLI_addtail(nurbsbase, nu);
  }
}

/**
 * Clear all Nurbs from the list and free their memory.
 */
static void clear_nurbs(ListBaseT<Nurb> *nurbsbase)
{
  Nurb *nu_next;
  for (Nurb *nu = static_cast<Nurb *>(nurbsbase->first); nu != nullptr; nu = nu_next) {
    nu_next = nu->next;
    BLI_remlink(nurbsbase, nu);
    if (nu->bezt) {
      MEM_freeN(nu->bezt);
    }
    if (nu->bp) {
      MEM_freeN(nu->bp);
    }
    if (nu->knotsu) {
      MEM_freeN(nu->knotsu);
    }
    if (nu->knotsv) {
      MEM_freeN(nu->knotsv);
    }
    MEM_freeN(nu);
  }
}

/**
 * Remove overlapping regions from font glyph curves.
 *
 * This function takes a list of Nurb curves representing a font glyph
 * and removes any self-intersecting or overlapping regions, producing
 * clean non-overlapping outlines suitable for tessellation.
 *
 * \param nurbsbase: List of Nurb curves to process (modified in place)
 * \param scale: The scale factor that was applied to the coordinates (typically 1.0/em_size).
 *               Used to convert to FontForge's expected coordinate range.
 */
void blf_glyph_remove_overlaps(ListBaseT<Nurb> *nurbsbase, float scale)
{
  if (BLI_listbase_is_empty(nurbsbase)) {
    return;
  }

  /* Reverse all contour directions before overlap removal.
   * FreeType provides glyphs using TrueType convention (CW outer, CCW holes),
   * but FontForge's overlap removal expects PostScript convention (CCW outer, CW holes).
   * Reversing all contours converts between these conventions. */
  for (Nurb *nu = static_cast<Nurb *>(nurbsbase->first); nu != nullptr; nu = nu->next) {
    nurb_reverse_direction(nu);
  }

  const double inv_scale = (scale != 0.0f) ? (1.0 / double(scale)) : 1.0;

  constexpr double round_factor = 10000000.0; /* 7 decimals - highest precision */

  FFSplineSet *result = try_overlap_removal(nurbsbase, inv_scale, round_factor, false);

  if (result == nullptr) {
    /* All attempts failed - leave original contours unchanged. */
    return;
  }

  /* Clear the original nurbs list and replace with result. */
  clear_nurbs(nurbsbase);
  splinesets_to_nurbs(result, nurbsbase, scale);

  /* Free the FontForge result. */
  FF_FreeSplineSets(result);
}

/**
 * Convert FreeType outline directly to Blender Nurbs with overlap removal.
 *
 * This is the optimized path that converts FreeType outline data directly to
 * FontForge SplineSets for overlap removal, then to Blender Nurbs. This avoids
 * the intermediate Blender Nurbs representation and preserves coordinate precision.
 *
 * \param ftoutline: The FreeType outline to convert.
 * \param nurbsbase: Output list of Nurb curves.
 * \param scale: Scale factor to apply to coordinates.
 * \return true on success, false if overlap removal failed.
 */
static bool blf_ftoutline_to_curves_with_overlap_removal(const FT_Outline &ftoutline,
                                                         ListBaseT<Nurb> *nurbsbase,
                                                         float scale)
{
  /* Handle empty outlines (e.g., space character) - this is not a failure. */
  if (ftoutline.n_contours == 0) {
    return true;
  }

  constexpr double round_factor = 10000000.0; /* 7 decimals - highest precision */

  FFSplineSet *result = try_overlap_removal_direct(ftoutline, round_factor, false);

  if (result == nullptr) {
    return false;
  }

  splinesets_to_nurbs(result, nurbsbase, scale);
  FF_FreeSplineSets(result);
  return true;
}

/** \} */

#endif /* WITH_FONT_FORGE */

/* -------------------------------------------------------------------- */
/** \name Internal Utilities
 * \{ */

/**
 * Convert a floating point value to a FreeType 16.16 fixed point value.
 */
static FT_Fixed to_16dot16(const double val)
{
  return FT_Fixed(lround(val * 65536.0));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Convert Glyph to Curves
 * \{ */

/**
 * from: http://www.freetype.org/freetype2/docs/glyphs/glyphs-6.html#section-1
 *
 * Vectorial representation of Freetype glyphs
 *
 * The source format of outlines is a collection of closed paths called "contours". Each contour is
 * made of a series of line segments and bezier arcs. Depending on the file format, these can be
 * second-order or third-order polynomials. The former are also called quadratic or conic arcs, and
 * they come from the TrueType format. The latter are called cubic arcs and mostly come from the
 * Type1 format.
 *
 * Each arc is described through a series of start, end and control points.
 * Each point of the outline has a specific tag which indicates whether it is
 * used to describe a line segment or an arc.
 * The following rules are applied to decompose the contour's points into segments and arcs :
 *
 * # two successive "on" points indicate a line segment joining them.
 *
 * # one conic "off" point midst two "on" points indicates a conic bezier arc,
 *   the "off" point being the control point, and the "on" ones the start and end points.
 *
 * # Two successive cubic "off" points midst two "on" points indicate a cubic bezier arc.
 *   There must be exactly two cubic control points and two on points for each cubic arc
 *   (using a single cubic "off" point between two "on" points is forbidden, for example).
 *
 * # finally, two successive conic "off" points forces the rasterizer to create
 *   (during the scan-line conversion process exclusively) a virtual "on" point midst them,
 *   at their exact middle.
 *   This greatly facilitates the definition of successive conic bezier arcs.
 *   Moreover, it's the way outlines are described in the TrueType specification.
 *
 * Note that it is possible to mix conic and cubic arcs in a single contour, even though no current
 * font driver produces such outlines.
 *
 * <pre>
 *                                   *            # on
 *                                                * off
 *                                __---__
 *   #-__                      _--       -_
 *       --__                _-            -
 *           --__           #               \
 *               --__                        #
 *                   -#
 *                            Two "on" points
 *    Two "on" points       and one "conic" point
 *                             between them
 *                 *
 *   #            __      Two "on" points with two "conic"
 *    \          -  -     points between them. The point
 *     \        /    \    marked '0' is the middle of the
 *      -      0      \   "off" points, and is a 'virtual'
 *       -_  _-       #   "on" point where the curve passes.
 *         --             It does not appear in the point
 *                        list.
 *         *
 *         *                # on
 *                    *     * off
 *          __---__
 *       _--       -_
 *     _-            -
 *    #               \
 *                     #
 *
 *      Two "on" points
 *    and two "cubic" point
 *       between them
 * </pre>
 *
 * Each glyphs original outline points are located on a grid of indivisible units.
 * The points are stored in the font file as 16-bit integer grid coordinates,
 * with the grid origin's being at (0, 0); they thus range from -16384 to 16383.
 *
 * Convert conic to bezier arcs:
 * Conic P0 P1 P2
 * Bezier B0 B1 B2 B3
 * B0=P0
 * B1=(P0+2*P1)/3
 * B2=(P2+2*P1)/3
 * B3=P2
 */

static void blf_glyph_to_curves(const FT_Outline &ftoutline,
                                ListBaseT<Nurb> *nurbsbase,
                                const float scale)
{
  const float eps = 0.0001f;
  const float eps_sq = eps * eps;
  Nurb *nu;
  BezTriple *bezt;
  float dx, dy;
  int j, k, l, l_first = 0;

  /* initialize as -1 to add 1 on first loop each time */
  int contour_prev;

  /* Start converting the FT data */
  int *onpoints = MEM_calloc_arrayN<int>(size_t(ftoutline.n_contours), "onpoints");

  /* Get number of on-curve points for bezier-triples (including conic virtual on-points). */
  for (j = 0, contour_prev = -1; j < ftoutline.n_contours; j++) {
    const int n = ftoutline.contours[j] - contour_prev;
    contour_prev = ftoutline.contours[j];

    for (k = 0; k < n; k++) {
      l = (j > 0) ? (k + ftoutline.contours[j - 1] + 1) : k;
      if (k == 0) {
        l_first = l;
      }

      if (ftoutline.tags[l] == FT_Curve_Tag_On) {
        onpoints[j]++;
      }

      {
        const int l_next = (k < n - 1) ? (l + 1) : l_first;
        if (ftoutline.tags[l] == FT_Curve_Tag_Conic &&
            ftoutline.tags[l_next] == FT_Curve_Tag_Conic)
        {
          onpoints[j]++;
        }
      }
    }
  }

  /* contour loop, bezier & conic styles merged */
  for (j = 0, contour_prev = -1; j < ftoutline.n_contours; j++) {
    const int n = ftoutline.contours[j] - contour_prev;
    contour_prev = ftoutline.contours[j];

    /* add new curve */
    nu = MEM_new_for_free<Nurb>("objfnt_nurb");
    bezt = MEM_calloc_arrayN<BezTriple>(size_t(onpoints[j]), "objfnt_bezt");
    BLI_addtail(nurbsbase, nu);

    nu->type = CU_BEZIER;
    nu->pntsu = onpoints[j];
    nu->resolu = 8;
    nu->flagu = CU_NURB_CYCLIC;
    nu->bezt = bezt;

    /* individual curve loop, start-end */
    for (k = 0; k < n; k++) {
      l = (j > 0) ? (k + ftoutline.contours[j - 1] + 1) : k;
      if (k == 0) {
        l_first = l;
      }

      /* virtual conic on-curve points */
      {
        const int l_next = (k < n - 1) ? (l + 1) : l_first;
        if (ftoutline.tags[l] == FT_Curve_Tag_Conic &&
            ftoutline.tags[l_next] == FT_Curve_Tag_Conic)
        {
          dx = float(ftoutline.points[l].x + ftoutline.points[l_next].x) * scale / 2.0f;
          dy = float(ftoutline.points[l].y + ftoutline.points[l_next].y) * scale / 2.0f;

          /* left handle */
          bezt->vec[0][0] = (dx + (2.0f * float(ftoutline.points[l].x)) * scale) / 3.0f;
          bezt->vec[0][1] = (dy + (2.0f * float(ftoutline.points[l].y)) * scale) / 3.0f;

          /* midpoint (virtual on-curve point) */
          bezt->vec[1][0] = dx;
          bezt->vec[1][1] = dy;

          /* right handle */
          bezt->vec[2][0] = (dx + (2.0f * float(ftoutline.points[l_next].x)) * scale) / 3.0f;
          bezt->vec[2][1] = (dy + (2.0f * float(ftoutline.points[l_next].y)) * scale) / 3.0f;

          bezt->h1 = bezt->h2 = HD_ALIGN;
          bezt->radius = 1.0f;
          bezt++;
        }
      }

      /* on-curve points */
      if (ftoutline.tags[l] == FT_Curve_Tag_On) {
        const int l_prev = (k > 0) ? (l - 1) : ftoutline.contours[j];
        const int l_next = (k < n - 1) ? (l + 1) : l_first;

        /* left handle */
        if (ftoutline.tags[l_prev] == FT_Curve_Tag_Cubic) {
          bezt->vec[0][0] = float(ftoutline.points[l_prev].x) * scale;
          bezt->vec[0][1] = float(ftoutline.points[l_prev].y) * scale;
          bezt->h1 = HD_FREE;
        }
        else if (ftoutline.tags[l_prev] == FT_Curve_Tag_Conic) {
          bezt->vec[0][0] = (float(ftoutline.points[l].x) +
                             (2.0f * float(ftoutline.points[l_prev].x))) *
                            scale / 3.0f;
          bezt->vec[0][1] = (float(ftoutline.points[l].y) +
                             (2.0f * float(ftoutline.points[l_prev].y))) *
                            scale / 3.0f;
          bezt->h1 = HD_FREE;
        }
        else {
          bezt->vec[0][0] = float(ftoutline.points[l].x) * scale -
                            (float(ftoutline.points[l].x) - float(ftoutline.points[l_prev].x)) *
                                scale / 3.0f;
          bezt->vec[0][1] = float(ftoutline.points[l].y) * scale -
                            (float(ftoutline.points[l].y) - float(ftoutline.points[l_prev].y)) *
                                scale / 3.0f;
          bezt->h1 = HD_VECT;
        }

        /* midpoint (on-curve point) */
        bezt->vec[1][0] = float(ftoutline.points[l].x) * scale;
        bezt->vec[1][1] = float(ftoutline.points[l].y) * scale;

        /* right handle */
        if (ftoutline.tags[l_next] == FT_Curve_Tag_Cubic) {
          bezt->vec[2][0] = float(ftoutline.points[l_next].x) * scale;
          bezt->vec[2][1] = float(ftoutline.points[l_next].y) * scale;
          bezt->h2 = HD_FREE;
        }
        else if (ftoutline.tags[l_next] == FT_Curve_Tag_Conic) {
          bezt->vec[2][0] = (float(ftoutline.points[l].x) +
                             (2.0f * float(ftoutline.points[l_next].x))) *
                            scale / 3.0f;
          bezt->vec[2][1] = (float(ftoutline.points[l].y) +
                             (2.0f * float(ftoutline.points[l_next].y))) *
                            scale / 3.0f;
          bezt->h2 = HD_FREE;
        }
        else {
          bezt->vec[2][0] = float(ftoutline.points[l].x) * scale -
                            (float(ftoutline.points[l].x) - float(ftoutline.points[l_next].x)) *
                                scale / 3.0f;
          bezt->vec[2][1] = float(ftoutline.points[l].y) * scale -
                            (float(ftoutline.points[l].y) - float(ftoutline.points[l_next].y)) *
                                scale / 3.0f;
          bezt->h2 = HD_VECT;
        }

        /* get the handles that are aligned, tricky...
         * - check if one of them is a vector handle.
         * - dist_squared_to_line_v2, check if the three beztriple points are on one line
         * - len_squared_v2v2, see if there's a distance between the three points
         * - len_squared_v2v2 again, to check the angle between the handles
         */
        if ((bezt->h1 != HD_VECT && bezt->h2 != HD_VECT) &&
            (dist_squared_to_line_v2(bezt->vec[0], bezt->vec[1], bezt->vec[2]) <
             (0.001f * 0.001f)) &&
            (len_squared_v2v2(bezt->vec[0], bezt->vec[1]) > eps_sq) &&
            (len_squared_v2v2(bezt->vec[1], bezt->vec[2]) > eps_sq) &&
            (len_squared_v2v2(bezt->vec[0], bezt->vec[2]) > eps_sq) &&
            (len_squared_v2v2(bezt->vec[0], bezt->vec[2]) >
             max_ff(len_squared_v2v2(bezt->vec[0], bezt->vec[1]),
                    len_squared_v2v2(bezt->vec[1], bezt->vec[2]))))
        {
          bezt->h1 = bezt->h2 = HD_ALIGN;
        }
        bezt->radius = 1.0f;
        bezt++;
      }
    }
  }

  MEM_freeN(onpoints);
}

static FT_GlyphSlot blf_glyphslot_ensure_outline(FontBLF *font, uint charcode, bool use_fallback)
{
  if (charcode < 32) {
    if (ELEM(charcode, 0x10, 0x13)) {
      /* Do not render line feed or carriage return. #134972. */
      return nullptr;
    }
    /* Other C0 controls (U+0000 - U+001F) can show as space. #135421. */
    /* TODO: Return all but TAB as ".notdef" character when we have our own. */
    charcode = ' ';
  }

  /* Glyph might not come from the initial font. */
  FontBLF *font_with_glyph = font;
  FT_UInt glyph_index = use_fallback ? blf_glyph_index_from_charcode(&font_with_glyph, charcode) :
                                       blf_get_char_index(font_with_glyph, charcode);

  if (!glyph_index) {
    return nullptr;
  }

  if (!blf_ensure_face(font_with_glyph)) {
    return nullptr;
  }

  FT_GlyphSlot glyph = blf_glyph_render_outline(font, font_with_glyph, glyph_index, charcode, 0);

  if (font != font_with_glyph) {
    if (!blf_ensure_face(font)) {
      return nullptr;
    }
    double ratio = float(font->face->units_per_EM) / float(font_with_glyph->face->units_per_EM);
    FT_Matrix transform = {to_16dot16(ratio), 0, 0, to_16dot16(ratio)};
    FT_Outline_Transform(&glyph->outline, &transform);
    glyph->advance.x = int(float(glyph->advance.x) * ratio);
    glyph->metrics.horiAdvance = int(float(glyph->metrics.horiAdvance) * ratio);
  }

  return glyph;
}

bool blf_character_to_curves(FontBLF *font,
                             uint unicode,
                             ListBaseT<Nurb> *nurbsbase,
                             const float scale,
                             bool use_fallback,
                             bool use_sanitize,
                             float *r_advance)
{
  FT_GlyphSlot glyph = blf_glyphslot_ensure_outline(font, unicode, use_fallback);
  if (!glyph) {
    *r_advance = 0.0f;
    return false;
  }

#ifdef WITH_FONT_FORGE
  if (use_sanitize) {
    if (!blf_ftoutline_to_curves_with_overlap_removal(glyph->outline, nurbsbase, scale)) {
      *r_advance = 0.0f;
      return false;
    }
  }
  else {
    blf_glyph_to_curves(glyph->outline, nurbsbase, scale);
  }
#else
  UNUSED_VARS(use_sanitize);
  blf_glyph_to_curves(glyph->outline, nurbsbase, scale);
#endif

  *r_advance = float(glyph->advance.x) * scale;
  return true;
}

/** \} */

}  // namespace blender
