/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "BKE_curves.hh"

#include <Eigen/SparseCore>

namespace blender::ed::curves::nurbs {

using WeightMatrix = Eigen::SparseMatrix<float, Eigen::RowMajor>;
using WeightVector = Eigen::VectorXf;
using WeightTriplet = Eigen::Triplet<float, Eigen::SparseMatrix<float>::StorageIndex>;

/**
 * Finds current knot's multiplicity and index of span to insert into.
 */
void find_span_mult(float knot, Span<float> knots, int order, int &r_span, int &r_mult);

/**
 * Calculates knot insertion point weights later used to transform all point attributes to make
 * actual insertion.
 * Also used to preview insertion.
 */
WeightMatrix calc_knot_insertion_weights(Span<float> knots,
                                         int points_num,
                                         int8_t order,
                                         float knot,
                                         int knot_span,
                                         int mult,
                                         int repeat);
/**
 * Prepares curve's knot weights. If `ATTR_NURBS_WEIGHT` is present returns `Span` representing
 * weights for given range. Otherwise fills buffer with 1.0f and returns it's `Span`.
 */
Span<float> prepare_curve_weights(const Span<float> all_weights,
                                  const IndexRange curve_points,
                                  Array<float> &weights_buffer);

IndexMask selection_from_modified(const WeightMatrix &point_weights, IndexMaskMemory &memory);

WeightMatrix roll_matrix_rows(const WeightMatrix &a, const int shift);

void gather_modified_positions(const Span<float3> positions,
                               const Span<float> weights,
                               const WeightMatrix &point_weights,
                               const IndexMask selection,
                               MutableSpan<float3> r_positions);
/**
 * Inserts knot into given curve.
 * \param knot_span: Index of span (interval between two knots) to insert knot.
 * \param knot_multiplicity: Knot's multiplicity before insertion.
 * \param repeat: Times to repeat the knot.
 * \param knots: Curve knots preloaded with `load_curve_knots`.
 */
bke::CurvesGeometry insert_knot(const bke::CurvesGeometry &curves,
                                int curve,
                                float knot,
                                int knot_span,
                                int knot_multiplicity,
                                int repeat,
                                Span<float> knots);
}  // namespace blender::ed::curves::nurbs
