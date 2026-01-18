/* SPDX-FileCopyrightText: 2009 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blf
 *
 * Glyph conversion, from FreeType to curves.
 */

#include <cmath>

#include <ft2build.h>

#include FT_OUTLINE_H

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math_geom.h"

#include "BLF_api.hh"

#include "DNA_curve_types.h"
#include "DNA_vfont_types.h"

#include "BKE_curve.hh"

#include "blf_internal.hh"

#ifdef WITH_FONT_FORGE
/* FontForge wrapper for overlap removal (C++ compatible). */
extern "C" {
#  include "fontforge/splineoverlap_blender.h"
}
#endif

#ifdef WITH_FONT_SKIA
#  include "skpathops_blender.h"
#endif

#include "blf_internal_types.hh"

#include "BLI_math_base.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"

#include "BLI_strict_flags.h" /* IWYU pragma: keep. Keep last. */

namespace blender {

/* -------------------------------------------------------------------- */
/** \name FreeType Outline Iterator
 *
 * Template-based iterator for parsing FreeType outlines. Handles all FreeType
 * curve types (on-curve, conic/quadratic, cubic) and virtual on-curve points
 * between consecutive conics.
 *
 * The Handler must implement:
 * - begin_contour(const double2 &p) - Called at start of each contour with first point
 * - line_to(const double2 &p) - Called for line segments
 * - cubic_to(const double2 &cp1, const double2 &cp2, const double2 &p) - Called for cubic bezier
 * - end_contour() - Called at end of each contour
 * \{ */

/**
 * Iterate over FreeType outline, calling handler methods for each segment.
 * Converts conic (quadratic) curves to cubic bezier curves.
 */
template<typename Handler>
static void iterate_ft_outline(const FT_Outline &ftoutline, Handler &handler)
{
  int contour_prev = -1;

  for (int j = 0; j < ftoutline.n_contours; j++) {
    const int contour_end = ftoutline.contours[j];
    const int contour_start = contour_prev + 1;
    const int n = contour_end - contour_prev;
    contour_prev = contour_end;

    if (n < 2) {
      continue;
    }

    /* Helper to get point at index within this contour (wraps around). */
    auto point_at_index = [&](int k) -> double2 {
      int index = contour_start + math::mod_periodic(k, n);
      return double2(double(ftoutline.points[index].x), double(ftoutline.points[index].y));
    };

    /* Helper to get curve type tag at index (masks out non-curve-type bits). */
    auto tag_at_index = [&](int k) -> char {
      int index = contour_start + math::mod_periodic(k, n);
      return FT_CURVE_TAG(ftoutline.tags[index]);
    };

    /* Find starting point. Could be on-curve, or virtual between two conics. */
    double2 start;
    int start_k = 0;

    if (tag_at_index(0) == FT_Curve_Tag_On) {
      start = point_at_index(0);
      start_k = 0;
    }
    else if (tag_at_index(0) == FT_Curve_Tag_Conic) {
      if (tag_at_index(n - 1) == FT_Curve_Tag_On) {
        /* Start from the last on-curve point. */
        start = point_at_index(n - 1);
        start_k = n - 1;
      }
      else {
        /* Both first and last are conic - start at virtual point between them. */
        start = (point_at_index(0) + point_at_index(n - 1)) / 2.0;
        start_k = 0;
      }
    }
    else {
      /* Cubic - find an on-curve point. */
      bool found = false;
      for (int k = 0; k < n; k++) {
        if (tag_at_index(k) == FT_Curve_Tag_On) {
          start = point_at_index(k);
          start_k = k;
          found = true;
          break;
        }
      }
      if (!found) {
        /* No on-curve points - skip this contour. */
        continue;
      }
    }

    handler.begin_contour(start);

    /* Current position. */
    double2 curr = start;

    /* Process the contour starting from start_k. */
    int k = start_k;
    int steps = 0;
    while (steps < n) {
      char tag = tag_at_index(k);

      if (tag == FT_Curve_Tag_On) {
        /* On-curve point - look at what follows. */
        char next_tag = tag_at_index(k + 1);

        if (next_tag == FT_Curve_Tag_On) {
          /* Line to next on-curve point. */
          double2 p = point_at_index(k + 1);
          handler.line_to(p);
          curr = p;
          k++;
          steps++;
        }
        else if (next_tag == FT_Curve_Tag_Conic) {
          /* Conic curve - may have consecutive conics with virtual on-points. */
          double2 c = point_at_index(k + 1);
          char after_tag = tag_at_index(k + 2);
          double2 end;

          if (after_tag == FT_Curve_Tag_On) {
            end = point_at_index(k + 2);
            k += 2;
            steps += 2;
          }
          else if (after_tag == FT_Curve_Tag_Conic) {
            /* Virtual on-curve point between two conics. */
            end = (c + point_at_index(k + 2)) / 2.0;
            k += 1;
            steps += 1;
          }
          else {
            k++;
            steps++;
            continue;
          }

          /* Convert conic (quadratic) to cubic bezier. */
          double2 cp1 = curr + (2.0 / 3.0) * (c - curr);
          double2 cp2 = end + (2.0 / 3.0) * (c - end);
          handler.cubic_to(cp1, cp2, end);
          curr = end;
        }
        else if (next_tag == FT_Curve_Tag_Cubic) {
          /* Cubic curve - need exactly 2 cubic off-curve points. */
          double2 cp1 = point_at_index(k + 1);
          double2 cp2 = point_at_index(k + 2);
          double2 end = point_at_index(k + 3);
          handler.cubic_to(cp1, cp2, end);
          curr = end;
          k += 3;
          steps += 3;
        }
      }
      else if (tag == FT_Curve_Tag_Conic) {
        /* At a conic point (came from virtual on-point or special case). */
        double2 c = point_at_index(k);
        char next_tag = tag_at_index(k + 1);
        double2 end;

        if (next_tag == FT_Curve_Tag_On) {
          end = point_at_index(k + 1);
          k += 1;
          steps += 1;
        }
        else if (next_tag == FT_Curve_Tag_Conic) {
          end = (c + point_at_index(k + 1)) / 2.0;
          k += 1;
          steps += 1;
        }
        else {
          k++;
          steps++;
          continue;
        }

        double2 cp1 = curr + (2.0 / 3.0) * (c - curr);
        double2 cp2 = end + (2.0 / 3.0) * (c - end);
        handler.cubic_to(cp1, cp2, end);
        curr = end;
      }
      else {
        /* Cubic off-curve - should only appear in pairs after on-curve. Skip. */
        k++;
        steps++;
      }
    }

    handler.end_contour();
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Common Types
 * \{ */

/** Segment data for bezier curve conversion. */
struct SegmentData {
  double2 start; /* Start point of segment. */
  double2 cp1;   /* First control point (from start). */
  double2 cp2;   /* Second control point (toward end). */
  double2 end;   /* End point of segment. */
  bool is_line;  /* True if line segment. */
};

/** \} */

#ifdef WITH_FONT_FORGE

/* -------------------------------------------------------------------- */
/** \name Overlap Removal (FontForge Integration)
 *
 * Note: This section does NOT use the unified `iterate_ft_outline` iterator because
 * FontForge requires points in reversed order to convert from TrueType winding
 * (CW outer contours) to PostScript winding (CCW outer contours). The iterator
 * outputs segments in forward order, so the FontForge conversion handles FT outline
 * parsing directly to enable efficient point reversal during construction.
 * \{ */

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
 * \return The FontForge SplineSets, or nullptr on failure.
 */
static FFSplineSet *ftoutline_to_splinesets(const FT_Outline &ftoutline)
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

    /* Count on-curve points (including virtual ones from consecutive conics). */
    int on_count = 0;
    for (int k = 0; k < n; k++) {
      int index = contour_start + k;
      if (FT_CURVE_TAG(ftoutline.tags[index]) == FT_Curve_Tag_On) {
        on_count++;
      }
      else if (FT_CURVE_TAG(ftoutline.tags[index]) == FT_Curve_Tag_Conic) {
        int index_next = contour_start + math::mod_periodic(k + 1, n);
        if (FT_CURVE_TAG(ftoutline.tags[index_next]) == FT_Curve_Tag_Conic) {
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
      double2 pos;       /* On-curve point position. */
      double2 prev_cp;   /* Incoming control point (from previous segment). */
      double2 next_cp;   /* Outgoing control point (to next segment). */
      bool prev_is_line; /* Previous segment is a line. */
      bool next_is_line; /* Next segment is a line. */
    };

    OnCurvePoint *on_points = static_cast<OnCurvePoint *>(
        MEM_callocN(sizeof(OnCurvePoint) * size_t(on_count), "on_curve_points"));
    if (on_points == nullptr) {
      FF_FreeSplineSets(ss_head);
      return nullptr;
    }

    int on_index = 0;

    /* Helper to get point from FreeType outline. */
    auto ft_point = [&](int index) -> double2 {
      return double2(double(ftoutline.points[index].x), double(ftoutline.points[index].y));
    };

    /* Process the contour forward to extract on-curve points. */
    for (int k = 0; k < n; k++) {
      int index = contour_start + k;
      int index_prev = contour_start + math::mod_periodic(k - 1, n);
      int index_next = contour_start + math::mod_periodic(k + 1, n);

      char tag = FT_CURVE_TAG(ftoutline.tags[index]);
      char tag_prev = FT_CURVE_TAG(ftoutline.tags[index_prev]);
      char tag_next = FT_CURVE_TAG(ftoutline.tags[index_next]);

      if (tag == FT_Curve_Tag_On) {
        /* Real on-curve point. */
        double2 p = ft_point(index);

        on_points[on_index].pos = p;

        /* Determine incoming control point (prev_cp). */
        if (tag_prev == FT_Curve_Tag_On) {
          /* Line segment from previous on-curve point. */
          on_points[on_index].prev_is_line = true;
          on_points[on_index].prev_cp = p;
        }
        else if (tag_prev == FT_Curve_Tag_Conic) {
          /* Conic arc: control point is (P + 2*C) / 3 where P is on-curve, C is conic. */
          double2 c = ft_point(index_prev);
          on_points[on_index].prev_is_line = false;
          on_points[on_index].prev_cp = (p + 2.0 * c) / 3.0;
        }
        else if (tag_prev == FT_Curve_Tag_Cubic) {
          /* Cubic arc: control point is the cubic point itself. */
          on_points[on_index].prev_is_line = false;
          on_points[on_index].prev_cp = ft_point(index_prev);
        }

        /* Determine outgoing control point (next_cp). */
        if (tag_next == FT_Curve_Tag_On) {
          /* Line segment to next on-curve point. */
          on_points[on_index].next_is_line = true;
          on_points[on_index].next_cp = p;
        }
        else if (tag_next == FT_Curve_Tag_Conic) {
          /* Conic arc: control point is (P + 2*C) / 3. */
          double2 c = ft_point(index_next);
          on_points[on_index].next_is_line = false;
          on_points[on_index].next_cp = (p + 2.0 * c) / 3.0;
        }
        else if (tag_next == FT_Curve_Tag_Cubic) {
          /* Cubic arc: control point is the cubic point itself. */
          on_points[on_index].next_is_line = false;
          on_points[on_index].next_cp = ft_point(index_next);
        }

        on_index++;
      }
      else if (tag == FT_Curve_Tag_Conic && tag_next == FT_Curve_Tag_Conic) {
        /* Virtual on-curve point between two consecutive conic points. */
        double2 c1 = ft_point(index);
        double2 c2 = ft_point(index_next);

        /* Virtual point is midpoint of the two conic points. */
        double2 v = (c1 + c2) / 2.0;

        on_points[on_index].pos = v;

        /* Incoming control point from c1: (V + 2*C1) / 3. */
        on_points[on_index].prev_is_line = false;
        on_points[on_index].prev_cp = (v + 2.0 * c1) / 3.0;

        /* Outgoing control point to c2: (V + 2*C2) / 3. */
        on_points[on_index].next_is_line = false;
        on_points[on_index].next_cp = (v + 2.0 * c2) / 3.0;

        on_index++;
      }
    }

    /* Create FontForge SplineSet from on_points in REVERSE order to convert
     * from TrueType winding (CW outer) to PostScript winding (CCW outer).
     * Order: [0, n-1, n-2, ..., 1] - first point stays at index 0. */

    for (int i = 0; i < on_count; i++) {
      /* Map to reversed index: 0 stays at 0, then count down from n-1. */
      int src_index = (i == 0) ? 0 : (on_count - i);
      OnCurvePoint *op = &on_points[src_index];

      FFSplinePoint *sp = FF_CreatePoint(op->pos[0], op->pos[1]);
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
         * prev_sp corresponds to prev_src_index, sp corresponds to src_index.
         * In reversed direction: prev_sp's outgoing handle = its original incoming (prev_cp)
         *                        sp's incoming handle = its original outgoing (next_cp) */
        int prev_src_index = (i == 1) ? 0 : (on_count - (i - 1));
        OnCurvePoint *prev_op = &on_points[prev_src_index];

        bool is_line = prev_op->prev_is_line && op->next_is_line;

        if (is_line) {
          FF_SetPointNextCP(prev_sp, FF_PointGetX(prev_sp), FF_PointGetY(prev_sp));
          FF_SetPointPrevCP(sp, FF_PointGetX(sp), FF_PointGetY(sp));
        }
        else {
          /* Swap: prev_op's prev_cp becomes outgoing, op's next_cp becomes incoming. */
          FF_SetPointNextCP(prev_sp, prev_op->prev_cp[0], prev_op->prev_cp[1]);
          FF_SetPointPrevCP(sp, op->next_cp[0], op->next_cp[1]);
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
        FF_SetPointNextCP(prev_sp, last_op->prev_cp[0], last_op->prev_cp[1]);
        FF_SetPointPrevCP(first_sp, first_op->next_cp[0], first_op->next_cp[1]);
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
 * Convert FontForge SplineSets back to Blender Nurbs.
 */
static void fontforge_splinesets_to_nurbs(FFSplineSet *result,
                                          ListBaseT<Nurb> *nurbsbase,
                                          float scale)
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
    nu->resolu = 8;
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
    int index = 0;

    do {
      if (sp == nullptr || index >= num_splines) {
        break;
      }

      BezTriple *bezt = &nu->bezt[index];

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
      int i_next = (index + 1) % num_points;
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
      if (!is_cyclic && index == num_splines - 1) {
        bezt_next->vec[1][0] = float(p3x * double(scale));
        bezt_next->vec[1][1] = float(p3y * double(scale));
        bezt_next->vec[1][2] = 0.0f;
      }

      /* Move to next point */
      sp = FF_PointGetNext(sp, first);
      if (sp == nullptr && is_cyclic && index < num_splines - 1) {
        /* Should not happen for cyclic, but handle it */
        break;
      }
      index++;
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

  FFSplineSet *splinesets = ftoutline_to_splinesets(ftoutline);
  if (splinesets == nullptr) {
    return false;
  }

  FFSplineSet *result = FF_RemoveOverlap(splinesets);
  if (result == nullptr) {
    return false;
  }

  fontforge_splinesets_to_nurbs(result, nurbsbase, scale);
  FF_FreeSplineSets(result);
  return true;
}

/** \} */

#endif /* WITH_FONT_FORGE */

#ifdef WITH_FONT_SKIA

/* -------------------------------------------------------------------- */
/** \name Overlap Removal (Skia PathOps Integration)
 * \{ */

/**
 * Handler for building Skia path from FreeType outline segments.
 */
struct SkiaPathHandler {
  SKPathBuilder *builder;

  void begin_contour(const double2 &p)
  {
    SK_MoveTo(builder, p[0], p[1]);
  }

  void line_to(const double2 &p)
  {
    SK_LineTo(builder, p[0], p[1]);
  }

  void cubic_to(const double2 &cp1, const double2 &cp2, const double2 &p)
  {
    SK_CubicTo(builder, cp1[0], cp1[1], cp2[0], cp2[1], p[0], p[1]);
  }

  void end_contour()
  {
    SK_Close(builder);
  }
};

/**
 * Convert FreeType outline to Skia path and simplify.
 */
static SKPath *ftoutline_to_skpath_simplified(const FT_Outline &ftoutline)
{
  SKPathBuilder *builder = SK_CreatePathBuilder();
  if (builder == nullptr) {
    return nullptr;
  }

  SkiaPathHandler handler{builder};
  iterate_ft_outline(ftoutline, handler);

  SKPath *path = SK_FinishPath(builder);
  if (path == nullptr) {
    return nullptr;
  }

  /* Simplify the path to remove overlaps. */
  return SK_Simplify(path);
}

/**
 * Convert Skia path back to Blender Nurbs.
 *
 * Skia paths use a different representation than Blender curves:
 * - Skia: moveTo, cubicTo(cp1, cp2, endPoint), close
 * - Blender: BezTriple with left handle, vertex, right handle
 *
 * For each segment P0 -> P1 with control points CP1 and CP2:
 * - P0's right handle = CP1
 * - P1's left handle = CP2
 */
static void skia_paths_to_nurbs(SKPath *path, ListBaseT<Nurb> *nurbsbase, float scale)
{
  SKPathIterator *iter = SK_CreateIterator(path);
  if (iter == nullptr) {
    return;
  }

  Vector<SegmentData> segments;

  double2 move(0, 0); /* Initial moveTo point. */
  double2 last(0, 0); /* Current point position. */
  bool in_contour = false;

  auto finalize_contour = [&]() {
    if (segments.is_empty()) {
      return;
    }

    /* Build BezTriples from segments.
     * Each vertex corresponds to the start of a segment. */
    int num_points = int(segments.size());
    if (num_points < 2) {
      segments.clear();
      return;
    }

    Nurb *nu = static_cast<Nurb *>(MEM_callocN(sizeof(Nurb), "skia_nurb"));
    nu->pntsu = num_points;
    nu->pntsv = 1;
    nu->type = CU_BEZIER;
    nu->resolu = 8;
    nu->flagu = CU_NURB_CYCLIC;
    nu->bezt = static_cast<BezTriple *>(
        MEM_calloc_arrayN(size_t(num_points), sizeof(BezTriple), "skia_bezt"));

    for (int i = 0; i < num_points; i++) {
      BezTriple *bezt = &nu->bezt[i];
      SegmentData &seg = segments[size_t(i)];
      SegmentData &prev_seg = segments[size_t((i - 1 + num_points) % num_points)];

      /* Vertex position. */
      bezt->vec[1][0] = float(seg.start[0] * scale);
      bezt->vec[1][1] = float(seg.start[1] * scale);
      bezt->vec[1][2] = 0.0f;

      /* Left handle (incoming from previous segment). */
      if (prev_seg.is_line) {
        /* For lines, place handle at 2/3 from previous point. */
        double2 h = (prev_seg.start + 2.0 * seg.start) / 3.0;
        bezt->vec[0][0] = float(h[0] * scale);
        bezt->vec[0][1] = float(h[1] * scale);
        bezt->h1 = HD_VECT;
      }
      else {
        bezt->vec[0][0] = float(prev_seg.cp2[0] * scale);
        bezt->vec[0][1] = float(prev_seg.cp2[1] * scale);
        bezt->h1 = HD_FREE;
      }
      bezt->vec[0][2] = 0.0f;

      /* Right handle (outgoing to next point). */
      if (seg.is_line) {
        /* For lines, place handle at 1/3 toward next point. */
        double2 h = (2.0 * seg.start + seg.end) / 3.0;
        bezt->vec[2][0] = float(h[0] * scale);
        bezt->vec[2][1] = float(h[1] * scale);
        bezt->h2 = HD_VECT;
      }
      else {
        bezt->vec[2][0] = float(seg.cp1[0] * scale);
        bezt->vec[2][1] = float(seg.cp1[1] * scale);
        bezt->h2 = HD_FREE;
      }
      bezt->vec[2][2] = 0.0f;

      bezt->radius = 1.0f;
    }

    BLI_addtail(nurbsbase, nu);
    segments.clear();
  };

  double points[8];
  SKPathVerb verb;

  while ((verb = SK_IteratorNext(iter, points)) != SK_VERB_DONE) {
    switch (verb) {
      case SK_VERB_MOVE:
        /* Finalize previous contour if any. */
        if (in_contour) {
          finalize_contour();
        }
        move = double2(points[0], points[1]);
        last = move;
        in_contour = true;
        break;

      case SK_VERB_LINE: {
        SegmentData seg;
        seg.start = last;
        seg.end = double2(points[0], points[1]);
        seg.cp1 = seg.start;
        seg.cp2 = seg.end;
        seg.is_line = true;
        segments.append(seg);
        last = seg.end;
        break;
      }

      case SK_VERB_CUBIC: {
        SegmentData seg;
        seg.start = last;
        seg.cp1 = double2(points[0], points[1]);
        seg.cp2 = double2(points[2], points[3]);
        seg.end = double2(points[4], points[5]);
        seg.is_line = false;
        segments.append(seg);
        last = seg.end;
        break;
      }

      case SK_VERB_CLOSE:
        /* Add closing segment if needed (if last point != move point). */
        if (math::distance_squared(last, move) > math::square(1e-6)) {
          SegmentData seg;
          seg.start = last;
          seg.end = move;
          seg.cp1 = seg.start;
          seg.cp2 = seg.end;
          seg.is_line = true;
          segments.append(seg);
        }
        else if (!segments.is_empty()) {
          /* Snap the last segment's endpoint to the move point for precision.
           * This ensures the cyclic curve closes exactly. */
          segments.last().end = move;
          if (segments.last().is_line) {
            segments.last().cp2 = move;
          }
        }
        finalize_contour();
        in_contour = false;
        break;

      case SK_VERB_DONE:
        break;
    }
  }

  /* Finalize any unclosed contour. */
  if (in_contour) {
    finalize_contour();
  }

  SK_FreeIterator(iter);
}

/**
 * Convert FreeType outline to Blender Nurbs using Skia PathOps.
 */
static bool blf_ftoutline_to_curves_with_skia(const FT_Outline &ftoutline,
                                              ListBaseT<Nurb> *nurbsbase,
                                              float scale)
{
  if (ftoutline.n_contours == 0) {
    return true;
  }

  SKPath *result = ftoutline_to_skpath_simplified(ftoutline);
  if (result == nullptr) {
    return false;
  }

  skia_paths_to_nurbs(result, nurbsbase, scale);
  SK_FreePath(result);
  return true;
}

/** \} */

#endif /* WITH_FONT_SKIA */

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
 * Handler for building Blender curves from FreeType outline segments.
 * Collects segments per contour, then converts to BezTriples.
 */
struct BlenderCurveHandler {
  struct Contour {
    Vector<SegmentData> segments;
  };

  Vector<Contour> contours;
  Contour current_contour;
  double2 last = double2(0, 0);

  void begin_contour(const double2 &p)
  {
    current_contour = Contour();
    last = p;
  }

  void line_to(const double2 &p)
  {
    SegmentData seg;
    seg.start = last;
    seg.cp1 = last;
    seg.cp2 = p;
    seg.end = p;
    seg.is_line = true;
    current_contour.segments.append(seg);
    last = p;
  }

  void cubic_to(const double2 &cp1, const double2 &cp2, const double2 &p)
  {
    SegmentData seg;
    seg.start = last;
    seg.cp1 = cp1;
    seg.cp2 = cp2;
    seg.end = p;
    seg.is_line = false;
    current_contour.segments.append(seg);
    last = p;
  }

  void end_contour()
  {
    if (!current_contour.segments.is_empty()) {
      contours.append(std::move(current_contour));
    }
  }
};

/**
 * Check if a BezTriple's handles should be marked as aligned.
 * Aligned handles are on a line through the vertex (collinear) and on opposite sides.
 */
static bool bezt_handles_are_aligned(const BezTriple *bezt, float eps_sq)
{
  /* Handles must not be vector type. */
  if (bezt->h1 == HD_VECT || bezt->h2 == HD_VECT) {
    return false;
  }
  /* Handles must not be at the vertex position. */
  if (len_squared_v2v2(bezt->vec[0], bezt->vec[1]) <= eps_sq) {
    return false;
  }
  if (len_squared_v2v2(bezt->vec[1], bezt->vec[2]) <= eps_sq) {
    return false;
  }
  /* Handles must not be at the same position. */
  if (len_squared_v2v2(bezt->vec[0], bezt->vec[2]) <= eps_sq) {
    return false;
  }
  /* All three points must be collinear. */
  constexpr float collinear_eps_sq = 1e-3f * 1e-3f;
  if (dist_squared_to_line_v2(bezt->vec[0], bezt->vec[1], bezt->vec[2]) >= collinear_eps_sq) {
    return false;
  }
  /* Handles must be on opposite sides of the vertex.
   * The distance between handles must be greater than the distance from either handle to vertex.
   */
  float dist_handles_sq = len_squared_v2v2(bezt->vec[0], bezt->vec[2]);
  if (dist_handles_sq <= len_squared_v2v2(bezt->vec[0], bezt->vec[1])) {
    return false;
  }
  if (dist_handles_sq <= len_squared_v2v2(bezt->vec[1], bezt->vec[2])) {
    return false;
  }
  return true;
}

/**
 * Convert collected contours to Blender Nurbs.
 * Handles proper handle type detection (HD_VECT, HD_FREE, HD_ALIGN).
 */
static void blender_contours_to_nurbs(BlenderCurveHandler &handler,
                                      ListBaseT<Nurb> *nurbsbase,
                                      float scale)
{
  constexpr float eps = 0.0001f;
  constexpr float eps_sq = eps * eps;

  for (BlenderCurveHandler::Contour &contour : handler.contours) {
    int num_points = int(contour.segments.size());
    if (num_points < 2) {
      continue;
    }

    Nurb *nu = MEM_new_for_free<Nurb>("blf_nurb");
    nu->type = CU_BEZIER;
    nu->pntsu = num_points;
    nu->pntsv = 1;
    nu->resolu = 8;
    nu->flagu = CU_NURB_CYCLIC;
    nu->bezt = MEM_calloc_arrayN<BezTriple>(size_t(num_points), "blf_bezt");

    for (int i = 0; i < num_points; i++) {
      BezTriple *bezt = &nu->bezt[i];
      SegmentData &seg = contour.segments[size_t(i)];
      SegmentData &prev_seg = contour.segments[size_t((i - 1 + num_points) % num_points)];

      /* Vertex position (start of this segment). */
      bezt->vec[1][0] = float(seg.start[0]) * scale;
      bezt->vec[1][1] = float(seg.start[1]) * scale;
      bezt->vec[1][2] = 0.0f;

      /* Left handle (incoming from previous segment).
       * For lines, place handle at 2/3 from previous point toward this point. */
      if (prev_seg.is_line) {
        double2 h = prev_seg.start + (seg.start - prev_seg.start) * (2.0 / 3.0);
        bezt->vec[0][0] = float(h[0]) * scale;
        bezt->vec[0][1] = float(h[1]) * scale;
        bezt->h1 = HD_VECT;
      }
      else {
        bezt->vec[0][0] = float(prev_seg.cp2[0]) * scale;
        bezt->vec[0][1] = float(prev_seg.cp2[1]) * scale;
        bezt->h1 = HD_FREE;
      }
      bezt->vec[0][2] = 0.0f;

      /* Right handle (outgoing to next point).
       * For lines, place handle at 1/3 from this point toward next. */
      if (seg.is_line) {
        double2 h = seg.start + (seg.end - seg.start) / 3.0;
        bezt->vec[2][0] = float(h[0]) * scale;
        bezt->vec[2][1] = float(h[1]) * scale;
        bezt->h2 = HD_VECT;
      }
      else {
        bezt->vec[2][0] = float(seg.cp1[0]) * scale;
        bezt->vec[2][1] = float(seg.cp1[1]) * scale;
        bezt->h2 = HD_FREE;
      }
      bezt->vec[2][2] = 0.0f;

      /* Detect aligned handles. */
      if (bezt_handles_are_aligned(bezt, eps_sq)) {
        bezt->h1 = bezt->h2 = HD_ALIGN;
      }

      bezt->radius = 1.0f;
    }

    BLI_addtail(nurbsbase, nu);
  }
}

/**
 * Convert FreeType outline to Blender curves using the unified iterator.
 */
static void blf_glyph_to_curves(const FT_Outline &ftoutline,
                                ListBaseT<Nurb> *nurbsbase,
                                const float scale)
{
  BlenderCurveHandler handler;
  iterate_ft_outline(ftoutline, handler);
  blender_contours_to_nurbs(handler, nurbsbase, scale);
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
                             int simplify_method,
                             float *r_advance)
{
  FT_GlyphSlot glyph = blf_glyphslot_ensure_outline(font, unicode, use_fallback);
  if (!glyph) {
    *r_advance = 0.0f;
    return false;
  }

  if (use_sanitize) {
    bool success = false;

    switch (simplify_method) {
#ifdef WITH_FONT_SKIA
      case DNA_VFONT_SIMPLIFY_SKIA: {
        success = blf_ftoutline_to_curves_with_skia(glyph->outline, nurbsbase, scale);
        break;
      }
#endif
#ifdef WITH_FONT_FORGE
      case DNA_VFONT_SIMPLIFY_FONTFORGE: {
        success = blf_ftoutline_to_curves_with_overlap_removal(glyph->outline, nurbsbase, scale);
        break;
      }
#endif
      default: {
        /* Fallback: no simplification. */
        blf_glyph_to_curves(glyph->outline, nurbsbase, scale);
        success = true;
        break;
      }
    }

    if (!success) {
      *r_advance = 0.0f;
      return false;
    }
  }
  else {
    blf_glyph_to_curves(glyph->outline, nurbsbase, scale);
  }

  *r_advance = float(glyph->advance.x) * scale;
  return true;
}

/** \} */

}  // namespace blender
