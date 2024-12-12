/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

namespace blender::ed::sculpt_paint {

void mesh_sculpt_nodes_evaluate(const Depsgraph &depsgraph,
                                Object &object,
                                const Brush &brush,
                                StrokeCache &cache,
                                const Span<float3> position_eval,
                                const Span<int> verts,
                                MutableSpan<float3> translations);

void grids_sculpt_nodes_evaluate(const Depsgraph &depsgraph,
                                 Object &object,
                                 const Brush &brush,
                                 StrokeCache &cache,
                                 SubdivCCG &subdiv_ccg,
                                 Span<int> grids,
                                 Span<float3> positions,
                                 MutableSpan<float3> translations);

void bmesh_sculpt_nodes_evaluate(const Depsgraph &depsgraph,
                                 Object &object,
                                 const Brush &brush,
                                 StrokeCache &cache,
                                 const Set<BMVert *, 0> &verts,
                                 Span<float3> positions,
                                 MutableSpan<float3> translations);

void paint_sculpt_nodes_evaluate(const Depsgraph& depsgraph,
  Object& object,
  const Brush& brush,
  StrokeCache& cache,
  Span<float3> positions,
  Span<int> verts,
  MutableSpan<float4> colors);
}  // namespace blender::ed::sculpt_paint
