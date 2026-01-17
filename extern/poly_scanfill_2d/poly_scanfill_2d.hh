/* SPDX-FileCopyrightText: 2024-2026 Campbell Barton
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Polygon triangulation library - C++20 port. */

#pragma once

#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <tuple>
#include <vector>

namespace poly_fill {

/* -----------------------------------------------------------------------------
 * Public Types */

using Scalar = double;

using Vert = std::array<Scalar, 2>;
using Edge = std::array<int, 2>;
using Face = std::array<int, 3>;
using VertsEdgeMap = std::vector<std::array<int, 2>>;

/* Parameters for polygon filling. */
struct PolyFillParams {
  bool degenerate = false;
};

/* -----------------------------------------------------------------------------
 * Public API */

/**
 * Calculate vertex edge map from edges.
 * Returns a vertex-aligned list of adjacent edges.
 */
[[nodiscard]] VertsEdgeMap verts_edge_map_calc_from_edges(std::span<const Edge> edges,
                                                          int verts_len);

/**
 * Main polygon fill function.
 * Takes vertices and edges, returns triangulated faces.
 */
[[nodiscard]] std::vector<Face> poly_fill(std::span<const Vert> verts,
                                          std::span<const Edge> edges,
                                          bool degenerate = false);

/**
 * Polygon fill with pre-computed edge map.
 * More efficient when edge map is already available.
 */
[[nodiscard]] std::vector<Face> poly_fill_with_edge_map(std::span<const Vert> verts,
                                                        const VertsEdgeMap &verts_edge_map,
                                                        bool degenerate = false);

}  // namespace poly_fill
