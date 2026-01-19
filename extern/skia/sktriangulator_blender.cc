/* SPDX-FileCopyrightText: 2024 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Skia GrTriangulator wrapper for polygon triangulation.
 *
 * This implementation uses Skia's GrTriangulator for polygon tessellation.
 * When WITH_SKIA_PATHOPS is defined, it links against Skia.
 * Otherwise, it provides stub implementations that return failure.
 */

#include "sktriangulator_blender.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef WITH_SKIA_PATHOPS

#  include "include/core/SkPath.h"
#  include "include/core/SkPathBuilder.h"
#  include "include/core/SkRect.h"
#  include "src/gpu/ganesh/GrEagerVertexAllocator.h"
#  include "src/gpu/ganesh/geometry/GrTriangulator.h"

/* Build an SkPath from edges.
 * Edges define contours - we need to trace connected edges to form paths. */
static SkPath build_path_from_edges(const float *coords,
                                    int vertex_count,
                                    const int *edges,
                                    int edge_count)
{
  if (vertex_count < 3 || edge_count < 3) {
    return SkPath();
  }

  /* Build adjacency list for edge tracing. */
  std::vector<std::vector<int>> adjacency(vertex_count);
  for (int i = 0; i < edge_count; i++) {
    int v1 = edges[i * 2];
    int v2 = edges[i * 2 + 1];
    if (v1 >= 0 && v1 < vertex_count && v2 >= 0 && v2 < vertex_count) {
      adjacency[v1].push_back(v2);
      adjacency[v2].push_back(v1);
    }
  }

  /* Track which edges have been used. */
  std::vector<bool> edge_used(edge_count, false);

  auto find_edge_index = [&](int v1, int v2) -> int {
    for (int i = 0; i < edge_count; i++) {
      if (edge_used[i]) {
        continue;
      }
      int e1 = edges[i * 2];
      int e2 = edges[i * 2 + 1];
      if ((e1 == v1 && e2 == v2) || (e1 == v2 && e2 == v1)) {
        return i;
      }
    }
    return -1;
  };

  SkPathBuilder builder;

  /* Trace contours by following connected edges. */
  for (int start_edge = 0; start_edge < edge_count; start_edge++) {
    if (edge_used[start_edge]) {
      continue;
    }

    int start_v = edges[start_edge * 2];
    int current_v = edges[start_edge * 2 + 1];
    edge_used[start_edge] = true;

    /* Start the contour. */
    builder.moveTo(SkScalar(coords[start_v * 2]), SkScalar(coords[start_v * 2 + 1]));
    builder.lineTo(SkScalar(coords[current_v * 2]), SkScalar(coords[current_v * 2 + 1]));

    int prev_v = start_v;

    /* Follow the contour until we return to the start. */
    while (current_v != start_v) {
      int next_v = -1;
      int next_edge = -1;

      /* Find next unused edge from current vertex. */
      for (int neighbor : adjacency[current_v]) {
        if (neighbor == prev_v) {
          continue; /* Don't go back. */
        }
        int edge_idx = find_edge_index(current_v, neighbor);
        if (edge_idx >= 0) {
          next_v = neighbor;
          next_edge = edge_idx;
          break;
        }
      }

      if (next_v < 0) {
        /* No more edges - contour is incomplete, close it. */
        break;
      }

      edge_used[next_edge] = true;
      builder.lineTo(SkScalar(coords[next_v * 2]), SkScalar(coords[next_v * 2 + 1]));
      prev_v = current_v;
      current_v = next_v;
    }

    builder.close();
  }

  return builder.detach();
}

/* Build a simple polygon path from ordered vertices. */
static SkPath build_simple_path(const float *coords, int vertex_count)
{
  SkPathBuilder builder;

  if (vertex_count < 3) {
    return SkPath();
  }

  builder.moveTo(SkScalar(coords[0]), SkScalar(coords[1]));
  for (int i = 1; i < vertex_count; i++) {
    builder.lineTo(SkScalar(coords[i * 2]), SkScalar(coords[i * 2 + 1]));
  }
  builder.close();

  return builder.detach();
}

/* Custom vertex allocator that also maps vertices back to original indices. */
class TriangulatorVertexAllocator : public GrEagerVertexAllocator {
 public:
  TriangulatorVertexAllocator(const float *original_coords, int original_count)
      : original_coords_(original_coords), original_count_(original_count)
  {
  }

  ~TriangulatorVertexAllocator() override
  {
    if (vertices_) {
      free(vertices_);
    }
  }

  void *lock(size_t stride, int eagerCount) override
  {
    stride_ = stride;
    vertices_ = malloc(stride * eagerCount);
    return vertices_;
  }

  void unlock(int actualCount) override
  {
    actual_count_ = actualCount;
  }

  /* Get triangles as indices into original vertex array. */
  bool get_triangles(SKTriangulatorResult *result)
  {
    if (!vertices_ || actual_count_ < 3) {
      return false;
    }

    /* The triangulator outputs raw vertex positions.
     * We need to map them back to original vertex indices. */
    int triangle_count = actual_count_ / 3;

    result->triangles = static_cast<int *>(malloc(triangle_count * 3 * sizeof(int)));
    if (!result->triangles) {
      return false;
    }
    result->triangle_count = triangle_count;

    /* Each vertex in the output is an SkPoint (2 floats). */
    const SkPoint *output_verts = static_cast<const SkPoint *>(vertices_);

    for (int i = 0; i < actual_count_; i++) {
      const SkPoint &pt = output_verts[i];
      int best_idx = 0;
      float best_dist = 1e30f;

      /* Find closest original vertex. */
      for (int j = 0; j < original_count_; j++) {
        float dx = original_coords_[j * 2] - pt.fX;
        float dy = original_coords_[j * 2 + 1] - pt.fY;
        float dist = dx * dx + dy * dy;
        if (dist < best_dist) {
          best_dist = dist;
          best_idx = j;
        }
      }
      result->triangles[i] = best_idx;
    }

    return true;
  }

 private:
  const float *original_coords_;
  int original_count_;
  void *vertices_ = nullptr;
  size_t stride_ = 0;
  int actual_count_ = 0;
};

#endif /* WITH_SKIA_PATHOPS */

/* ============================================================================
 * Public API
 * ============================================================================ */

int SK_Triangulate(const float *coords,
                   int vertex_count,
                   const int *edges,
                   int edge_count,
                   SKTriangulatorResult *result)
{
  if (!result || !coords || vertex_count < 3 || !edges || edge_count < 3) {
    return 0;
  }

  result->triangles = nullptr;
  result->triangle_count = 0;

#ifdef WITH_SKIA_PATHOPS
  SkPath path = build_path_from_edges(coords, vertex_count, edges, edge_count);
  if (path.isEmpty()) {
    return 0;
  }

  /* Set fill type to winding (non-zero). */
  path.setFillType(SkPathFillType::kWinding);

  /* Get path bounds for clipping. */
  SkRect bounds = path.getBounds();
  /* Expand bounds slightly to avoid edge clipping issues. */
  bounds.outset(1.0f, 1.0f);

  /* Tolerance for curve flattening (1.0 is a reasonable default). */
  const SkScalar tolerance = 1.0f;

  TriangulatorVertexAllocator allocator(coords, vertex_count);
  bool isLinear = false;

  int count = GrTriangulator::PathToTriangles(path, tolerance, bounds, &allocator, &isLinear);

  if (count <= 0) {
    return 0;
  }

  if (!allocator.get_triangles(result)) {
    return 0;
  }

  return 1;
#else
  /* Stub: return failure when Skia is not available. */
  return 0;
#endif
}

int SK_TriangulateSimple(const float *coords, int vertex_count, SKTriangulatorResult *result)
{
  if (!result || !coords || vertex_count < 3) {
    return 0;
  }

  result->triangles = nullptr;
  result->triangle_count = 0;

#ifdef WITH_SKIA_PATHOPS
  SkPath path = build_simple_path(coords, vertex_count);
  if (path.isEmpty()) {
    return 0;
  }

  path.setFillType(SkPathFillType::kWinding);

  SkRect bounds = path.getBounds();
  bounds.outset(1.0f, 1.0f);

  const SkScalar tolerance = 1.0f;

  TriangulatorVertexAllocator allocator(coords, vertex_count);
  bool isLinear = false;

  int count = GrTriangulator::PathToTriangles(path, tolerance, bounds, &allocator, &isLinear);

  if (count <= 0) {
    return 0;
  }

  if (!allocator.get_triangles(result)) {
    return 0;
  }

  return 1;
#else
  return 0;
#endif
}

void SK_FreeTriangulatorResult(SKTriangulatorResult *result)
{
  if (result) {
    if (result->triangles) {
      free(result->triangles);
      result->triangles = nullptr;
    }
    result->triangle_count = 0;
  }
}
