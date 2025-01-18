/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"

struct BMVert;
struct Brush;
struct Depsgraph;
struct Object;
struct SubdivCCG;

namespace blender::ed::sculpt_paint {

void nodes_evaluate_factors_mesh(const Depsgraph &depsgraph,
                                 const Object &object,
                                 const StrokeCache &cache,
                                 const Brush &brush,
                                 Span<float3> vert_positions,
                                 Span<int> verts,
                                 MutableSpan<float> factors);

void nodes_evaluate_factors_grids(const Depsgraph &depsgraph,
                                  const Object &object,
                                  const StrokeCache &cache,
                                  const Brush &brush,
                                  const SubdivCCG &subdiv_ccg,
                                  const Span<int> grids,
                                  Span<float3> positions,
                                  MutableSpan<float> factors);

void nodes_evaluate_factors_bmesh(const Depsgraph &depsgraph,
                                  const Object &object,
                                  const StrokeCache &cache,
                                  const Brush &brush,
                                  const Set<BMVert *, 0> &verts,
                                  Span<float3> positions,
                                  MutableSpan<float> factors);

void nodes_evaluate_translations_mesh(const Depsgraph &depsgraph,
                                      const Object &object,
                                      const StrokeCache &cache,
                                      const Brush &brush,
                                      Span<float3> vert_positions,
                                      Span<int> verts,
                                      MutableSpan<float3> translations);

void nodes_evaluate_translations_grids(const Depsgraph &depsgraph,
                                       const Object &object,
                                       const StrokeCache &cache,
                                       const Brush &brush,
                                       const SubdivCCG &subdiv_ccg,
                                       Span<int> grids,
                                       Span<float3> positions,
                                       MutableSpan<float3> translations);

void nodes_evaluate_translations_bmesh(const Depsgraph &depsgraph,
                                       const Object &object,
                                       const StrokeCache &cache,
                                       const Brush &brush,
                                       const Set<BMVert *, 0> &verts,
                                       Span<float3> positions,
                                       MutableSpan<float3> translations);

}  // namespace blender::ed::sculpt_paint
