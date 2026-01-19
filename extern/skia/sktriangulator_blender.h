/* SPDX-FileCopyrightText: 2024 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Skia GrTriangulator wrapper for polygon triangulation.
 * Provides an alternative to Blender's BLI_scanfill using Skia's triangulator.
 */

#ifndef SKIA_TRIANGULATOR_BLENDER_H
#define SKIA_TRIANGULATOR_BLENDER_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Result structure for triangulation.
 * Contains the triangulated faces as triplets of vertex indices.
 */
typedef struct SKTriangulatorResult {
  /** Array of triangle indices, 3 indices per triangle. */
  int *triangles;
  /** Number of triangles. */
  int triangle_count;
} SKTriangulatorResult;

/**
 * Triangulate a polygon defined by 2D vertices and edges.
 *
 * This uses Skia's GrTriangulator which implements a sweep-line algorithm
 * similar to BLI_scanfill, but with different handling of edge cases.
 *
 * @param coords Array of 2D coordinates, 2 floats per vertex (x, y).
 * @param vertex_count Number of vertices.
 * @param edges Array of edge indices, 2 indices per edge (v1, v2).
 * @param edge_count Number of edges.
 * @param result Output structure to receive triangulation result.
 * @return Non-zero on success, zero on failure.
 */
int SK_Triangulate(const float *coords,
                   int vertex_count,
                   const int *edges,
                   int edge_count,
                   SKTriangulatorResult *result);

/**
 * Free the result of a triangulation.
 *
 * @param result The result to free.
 */
void SK_FreeTriangulatorResult(SKTriangulatorResult *result);

/**
 * Triangulate a simple polygon (vertices in order, no holes).
 *
 * This is a convenience function for simple cases where vertices
 * are provided in contour order (either CW or CCW).
 *
 * @param coords Array of 2D coordinates, 2 floats per vertex (x, y).
 * @param vertex_count Number of vertices (must be >= 3).
 * @param result Output structure to receive triangulation result.
 * @return Non-zero on success, zero on failure.
 */
int SK_TriangulateSimple(const float *coords, int vertex_count, SKTriangulatorResult *result);

#ifdef __cplusplus
}
#endif

#endif /* SKIA_TRIANGULATOR_BLENDER_H */
