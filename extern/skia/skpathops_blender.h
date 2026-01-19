/* SPDX-FileCopyrightText: 2024 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Skia PathOps wrapper for overlap removal.
 * Provides Simplify functionality as an alternative to FontForge.
 */

#ifndef SKIA_PATHOPS_BLENDER_H
#define SKIA_PATHOPS_BLENDER_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Opaque type for a path being built.
 */
typedef struct SKPathBuilder SKPathBuilder;

/**
 * Opaque type for a completed path.
 */
typedef struct SKPath SKPath;

/**
 * Create a new path builder.
 * @return New path builder, or NULL on failure.
 */
SKPathBuilder *SK_CreatePathBuilder(void);

/**
 * Start a new contour at the given point.
 */
void SK_MoveTo(SKPathBuilder *builder, double x, double y);

/**
 * Add a line segment to the given point.
 */
void SK_LineTo(SKPathBuilder *builder, double x, double y);

/**
 * Add a cubic bezier curve.
 * @param cp1x, cp1y First control point
 * @param cp2x, cp2y Second control point
 * @param x, y End point
 */
void SK_CubicTo(SKPathBuilder *builder,
                double cp1x,
                double cp1y,
                double cp2x,
                double cp2y,
                double x,
                double y);

/**
 * Close the current contour.
 */
void SK_Close(SKPathBuilder *builder);

/**
 * Finish building and get the path.
 * The builder is consumed and should not be used after this call.
 * @return The completed path, or NULL on failure.
 */
SKPath *SK_FinishPath(SKPathBuilder *builder);

/**
 * Free a path builder without finishing it.
 */
void SK_FreePathBuilder(SKPathBuilder *builder);

/**
 * Fill type for path operations.
 */
typedef enum {
  /** Non-zero winding rule (union of all regions). */
  SK_FILL_WINDING,
  /** Even-odd rule (alternating inside/outside). */
  SK_FILL_EVENODD
} SKFillType;

/**
 * Set the fill type for a path.
 * @param path The path to modify
 * @param fill_type The fill type to use
 */
void SK_SetFillType(SKPath *path, SKFillType fill_type);

/**
 * Simplify a path by removing overlapping regions.
 * This is equivalent to FontForge's SplineSetRemoveOverlap.
 * @param path Input path (consumed/freed by this function)
 * @return Simplified path, or NULL on failure.
 */
SKPath *SK_Simplify(SKPath *path);

/**
 * Opaque iterator for traversing path contours.
 */
typedef struct SKPathIterator SKPathIterator;

/**
 * Create an iterator for traversing the path.
 * @return New iterator, or NULL on failure.
 */
SKPathIterator *SK_CreateIterator(SKPath *path);

/**
 * Path segment types returned by the iterator.
 */
typedef enum {
  SK_VERB_MOVE,
  SK_VERB_LINE,
  SK_VERB_CUBIC,
  SK_VERB_CLOSE,
  SK_VERB_DONE
} SKPathVerb;

/**
 * Get the next segment from the iterator.
 * @param iter The iterator
 * @param points Output array for points (must have space for 4 points = 8 doubles)
 *               - MOVE: points[0] = position
 *               - LINE: points[0] = endpoint
 *               - CUBIC: points[0] = cp1, points[1] = cp2, points[2] = endpoint
 *               - CLOSE: no points
 * @return The verb type, or SK_VERB_DONE when finished.
 */
SKPathVerb SK_IteratorNext(SKPathIterator *iter, double points[8]);

/**
 * Free an iterator.
 */
void SK_FreeIterator(SKPathIterator *iter);

/**
 * Free a path.
 */
void SK_FreePath(SKPath *path);

#ifdef __cplusplus
}
#endif

#endif /* SKIA_PATHOPS_BLENDER_H */
