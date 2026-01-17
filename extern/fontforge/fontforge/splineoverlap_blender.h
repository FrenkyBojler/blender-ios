/* SPDX-FileCopyrightText: 2024 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * C wrapper for FontForge's overlap removal to avoid C++ keyword conflicts.
 */

#ifndef FONTFORGE_SPLINEOVERLAP_BLENDER_H
#define FONTFORGE_SPLINEOVERLAP_BLENDER_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Direct SplineSet API - Creates FontForge SplineSets directly
 * ============================================================================
 * This API allows Blender to create FontForge's native SplineSet structures
 * directly, without an intermediate format. This is closer to how FontForge
 * itself operates.
 */

/**
 * Opaque types wrapping FontForge's internal structures.
 * These are typedef'd to FontForge's actual types in the .c file.
 */
typedef struct splinepointlist FFSplineSet;
typedef struct splinepoint FFSplinePoint;

/**
 * Create a new SplinePoint at the given coordinates.
 * The point is allocated using FontForge's memory system.
 */
FFSplinePoint *FF_CreatePoint(double x, double y);

/**
 * Set the "next" control point (handle going toward next point).
 * If x,y equals the point's position, marks as "no next cp" (line segment).
 */
void FF_SetPointNextCP(FFSplinePoint *sp, double x, double y);

/**
 * Set the "prev" control point (handle coming from previous point).
 * If x,y equals the point's position, marks as "no prev cp" (line segment).
 */
void FF_SetPointPrevCP(FFSplinePoint *sp, double x, double y);

/**
 * Connect two SplinePoints with a cubic spline.
 * This creates the Spline structure and computes the polynomial coefficients.
 * Returns 1 on success, 0 on failure.
 */
int FF_ConnectPoints(FFSplinePoint *from, FFSplinePoint *to);

/**
 * Create a SplineSet (contour) from a linked ring of SplinePoints.
 * @param first The first point in the contour
 * @param last The last point in the contour (may equal first for cyclic)
 * @return New SplineSet, or NULL on failure
 */
FFSplineSet *FF_CreateSplineSet(FFSplinePoint *first, FFSplinePoint *last);

/**
 * Append a SplineSet to a linked list.
 * @param head Pointer to head of list (updated if NULL)
 * @param tail Pointer to tail of list (updated)
 * @param ss SplineSet to append
 */
void FF_AppendSplineSet(FFSplineSet **head, FFSplineSet **tail, FFSplineSet *ss);

/**
 * Remove overlaps from SplineSets using FontForge's algorithm.
 * @param ss Linked list of SplineSets (consumed/freed by this function)
 * @return New SplineSet list with overlaps removed, or NULL on failure
 */
FFSplineSet *FF_RemoveOverlap(FFSplineSet *ss);

/**
 * Get the next SplineSet in a linked list.
 */
FFSplineSet *FF_SplineSetGetNext(FFSplineSet *ss);

/**
 * Check if a SplineSet is cyclic (closed contour).
 */
int FF_SplineSetIsCyclic(FFSplineSet *ss);

/**
 * Count the number of spline segments in a SplineSet.
 */
int FF_SplineSetCountSplines(FFSplineSet *ss);

/**
 * Get the first SplinePoint in a SplineSet.
 */
FFSplinePoint *FF_SplineSetGetFirst(FFSplineSet *ss);

/**
 * Get the next SplinePoint following a spline connection.
 * Returns NULL if there's no next spline or we've wrapped around.
 * @param sp Current point
 * @param first First point in contour (to detect wrap-around)
 */
FFSplinePoint *FF_PointGetNext(FFSplinePoint *sp, FFSplinePoint *first);

/**
 * Get point coordinates.
 */
double FF_PointGetX(FFSplinePoint *sp);
double FF_PointGetY(FFSplinePoint *sp);

/**
 * Get control point coordinates.
 */
double FF_PointGetNextCPX(FFSplinePoint *sp);
double FF_PointGetNextCPY(FFSplinePoint *sp);
double FF_PointGetPrevCPX(FFSplinePoint *sp);
double FF_PointGetPrevCPY(FFSplinePoint *sp);

/**
 * Reverse the winding direction of a SplineSet.
 * This converts between TrueType (CW outer) and PostScript (CCW outer) conventions.
 */
void FF_SplineSetReverse(FFSplineSet *ss);

/**
 * Free a linked list of SplineSets.
 */
void FF_FreeSplineSets(FFSplineSet *ss);


#ifdef __cplusplus
}
#endif

#endif /* FONTFORGE_SPLINEOVERLAP_BLENDER_H */
