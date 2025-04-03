/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Functions for evaluating a Geometry Nodes node group associated to the brush.
 *
 * For each step of the stroke, #prepare_field_eval_data is called to calculate the output field
 * of the node group.
 *
 * Then, for each PBVH node and based on the BPVH type, the various nodes_evaluate_* functions
 * are called to evaluate the output field.
 *
 * The field is of type float and is used as a strength mask for the brush, similarly to how
 * texture masks currently work. The only exception is the Draw brush in Vector Displacement mode:
 * in this case the output is of type float3, and represents a translation.
 */

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

std::unique_ptr<NodeFieldEvalData> prepare_field_eval_data(const Scene &scene,
                                                           const ARegion &region,
                                                           const Depsgraph &depsgraph,
                                                           const Object &object,
                                                           const Brush &brush,
                                                           const StrokeCache &cache);

/* Evaluate the output field for brushes that that interpret the output of the field as a strength
 * mask. */
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

/* Evaluate the field for brushes that interpret the output of the field as a translation.
 * Currently, it's only used by the Draw brush in Vector Displacement mode. */
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
