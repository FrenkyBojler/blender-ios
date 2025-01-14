/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

namespace blender::ed::sculpt_paint {

template<typename TargetType>
void mesh_sculpt_nodes_evaluate(const Depsgraph &depsgraph,
                                Object &object,
                                const Brush &brush,
                                const Span<float3> position_eval,
                                const Span<int> verts,
                                const MutableSpan<TargetType> output_targets);

template<typename TargetType>
void grids_sculpt_nodes_evaluate(const Depsgraph &depsgraph,
                                 Object &object,
                                 const Brush &brush,
                                 const SubdivCCG &subdiv_ccg,
                                 const Span<int> grids,
                                 const Span<float3> positions,
                                 const MutableSpan<TargetType> output_targets);

template<typename TargetType>
void bmesh_sculpt_nodes_evaluate(const Depsgraph &depsgraph,
                                 Object &object,
                                 const Brush &brush,
                                 const Set<BMVert *, 0> &verts,
                                 const Span<float3> positions,
                                 const MutableSpan<TargetType> output_targets);

}  // namespace blender::ed::sculpt_paint
