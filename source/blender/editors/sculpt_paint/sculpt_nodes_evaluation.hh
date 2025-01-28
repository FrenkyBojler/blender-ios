/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_vector_types.hh"
#include "BLI_resource_scope.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"

#include "FN_field.hh"

struct ARegion;
struct BMVert;
struct Brush;
struct Depsgraph;
struct Object;
struct Scene;
struct SubdivCCG;

namespace blender::ed::sculpt_paint {

struct StrokeCache;

struct NodeFieldEvalData {
  ResourceScope scope;
  fn::GField field;
};

std::shared_ptr<NodeFieldEvalData> prepare_field_eval_data(const Scene &scene,
                                                           const ARegion &region,
                                                           const Depsgraph &depsgraph,
                                                           const Object &object,
                                                           const Brush &brush,
                                                           const StrokeCache &cache);
std::shared_ptr<NodeFieldEvalData> prepare_field_eval_data_for_translations(
    const Depsgraph &depsgraph,
    const Object &object,
    const Brush &brush,
    const StrokeCache &cache);

void nodes_evaluate_factors_mesh(const Depsgraph &depsgraph,
                                 const Object &object,
                                 const StrokeCache &cache,
                                 Span<float3> vert_positions,
                                 Span<int> verts,
                                 MutableSpan<float> factors);

void nodes_evaluate_factors_grids(const StrokeCache &cache,
                                  const SubdivCCG &subdiv_ccg,
                                  const Span<int> grids,
                                  Span<float3> positions,
                                  MutableSpan<float> factors);

void nodes_evaluate_factors_bmesh(const StrokeCache &cache,
                                  const Set<BMVert *, 0> &verts,
                                  Span<float3> positions,
                                  MutableSpan<float> factors);

void nodes_evaluate_translations_mesh(const Depsgraph &depsgraph,
                                      const Object &object,
                                      const StrokeCache &cache,
                                      const Span<float3> vert_positions,
                                      const Span<int> verts,
                                      const MutableSpan<float3> translations);

void nodes_evaluate_translations_grids(const StrokeCache &cache,
                                       const SubdivCCG &subdiv_ccg,
                                       Span<int> grids,
                                       Span<float3> positions,
                                       MutableSpan<float3> translations);

void nodes_evaluate_translations_bmesh(const StrokeCache &cache,
                                       const Set<BMVert *, 0> &verts,
                                       Span<float3> positions,
                                       MutableSpan<float3> translations);

}  // namespace blender::ed::sculpt_paint
